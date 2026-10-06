// main/main.c -- Vibe Voice Device: voice input for vibe coding.
//
// Tasks and ownership:
//   esp_timer (button callbacks)  -> enqueue only
//   NimBLE host (link/RX callbacks) -> enqueue only
//   vv_ctl (this file)            owns vv_app_t, executes actions, renders UI
//                                 under bsp_lvgl_lock(), polls the battery
//   vv_audio                      capture + ADPCM + AUDIO frames
//   vv_tx                         BLE notifications in FIFO order
// The pure state machine lives in vv_app.c; see docs/vibe-voice/firmware.md.
// Power (vv_power.c): the controller dims and darkens the screen when idle,
// slows the BLE link, and lets the chip light-sleep only while the screen is
// dark and nothing is busy.
#include "vv_app.h"
#include "vv_audio.h"
#include "vv_ble.h"
#include "vv_power.h"
#include "vv_proto.h"
#include "vv_ui.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "vibe_voice";

#define CTL_QUEUE_DEPTH  16
#define CTL_STACK        6144
#define CTL_PRIO         5
#define TICK_MS          100
#define BATTERY_POLL_MS  30000
#define LVGL_LOCK_MS     100
#define FRAME_SEND_MS    200

typedef enum { CTL_BUTTON = 0, CTL_LINK, CTL_RX } ctl_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t a;        // button / link event
    uint8_t b;        // press kind
    uint8_t len;      // RX length
    uint32_t passkey;
    uint8_t data[VV_FRAME_MAX];
} ctl_event_t;

static QueueHandle_t s_queue;
static vv_app_t s_app;
static vv_power_t s_power;
static esp_pm_lock_handle_t s_awake_lock;   // no light sleep: screen lit or busy
static esp_pm_lock_handle_t s_cpu_lock;     // full CPU speed while capturing
static bool s_awake_held, s_cpu_held;
static char s_fw[48];
static bool s_battery_ok;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void log_heap(const char *when) {
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u min=%u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

// Caller holds bsp_lvgl_lock().
static void log_lvgl_pool(const char *when) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL pool %s: used=%u%% max_used=%u free=%u largest=%u", when,
             (unsigned)mon.used_pct, (unsigned)mon.max_used, (unsigned)mon.free_size,
             (unsigned)mon.free_biggest_size);
}

// --- Producers (must not block) -----------------------------------------------

static void post(const ctl_event_t *ev) {
    if (s_queue && xQueueSend(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "control queue full; event %u dropped", ev->kind);
    }
}

static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    vv_press_t press;
    switch (ev) {
    case BSP_BTN_CLICK: press = VV_PRESS_CLICK; break;
    case BSP_BTN_DOUBLE: press = VV_PRESS_DOUBLE; break;
    case BSP_BTN_LONG: press = VV_PRESS_LONG; break;
    default: return;  // PRESS: act on CLICK so long-press can be told apart
    }
    vv_btn_t b = btn == BSP_BTN_UP ? VV_BTN_UP : (btn == BSP_BTN_DOWN ? VV_BTN_DOWN : VV_BTN_OK);
    ctl_event_t e = { .kind = CTL_BUTTON, .a = (uint8_t)b, .b = (uint8_t)press };
    post(&e);
}

static void on_link(vv_link_ev_t ev, uint32_t passkey) {
    ctl_event_t e = { .kind = CTL_LINK, .a = (uint8_t)ev, .passkey = passkey };
    post(&e);
}

static void on_rx(const uint8_t *data, size_t len) {
    if (len == 0 || len > VV_FRAME_MAX) return;
    ctl_event_t e = { .kind = CTL_RX, .len = (uint8_t)len };
    memcpy(e.data, data, len);
    post(&e);
}

// --- Controller ---------------------------------------------------------------

static void execute(const vv_actions_t *act) {
    if (act->flags & VV_ACT_AUDIO_STOP) vv_audio_stop();
    for (int i = 0; i < act->frame_count; i++) {
        if (!vv_ble_send(&act->frames[i], FRAME_SEND_MS)) {
            ESP_LOGW(TAG, "frame 0x%02x not sent", act->frames[i].data[0]);
        }
    }
    if (act->flags & VV_ACT_AUDIO_START) vv_audio_start(act->dict);
}

static void handle(const ctl_event_t *ev, uint32_t now, vv_actions_t *act) {
    switch (ev->kind) {
    case CTL_BUTTON:
        vv_app_button(&s_app, (vv_btn_t)ev->a, (vv_press_t)ev->b, now, act);
        break;
    case CTL_LINK: {
        vv_link_ev_t link = (vv_link_ev_t)ev->a;
        vv_app_link(&s_app, link, ev->passkey, now, act);
        if (link == VV_LINK_READY) {
            log_heap("link ready");
            if (bsp_lvgl_lock(LVGL_LOCK_MS)) {
                log_lvgl_pool("link ready");
                bsp_lvgl_unlock();
            }
        }
        break;
    }
    case CTL_RX: {
        vv_msg_t msg;
        if (vv_proto_decode(ev->data, ev->len, &msg)) {
            vv_app_frame(&s_app, &msg, now, act);
        } else {
            ESP_LOGW(TAG, "ignored frame type 0x%02x len %u", ev->data[0], ev->len);
            act->flags = 0;
            act->frame_count = 0;
        }
        break;
    }
    default:
        act->flags = 0;
        act->frame_count = 0;
        break;
    }
}

// Light the screen for what the user should notice: a new state (a RESULT,
// the passkey, a lost link), a new Alert or a toast. Target updates from Mac
// focus changes and the Voice Notes clock do not light it.
static void note_activity(uint32_t now) {
    static vv_state_t last_state = VV_ST_NO_LINK;
    static uint8_t last_alerts;
    static vv_toast_t last_toast;
    bool seen = s_app.state != last_state || s_app.alert_count > last_alerts ||
                (s_app.toast != VV_TOAST_NONE && s_app.toast != last_toast);
    last_state = s_app.state;
    last_alerts = s_app.alert_count;
    last_toast = s_app.toast;
    if (seen) (void)vv_power_activity(&s_power, now);
}

static void hold(esp_pm_lock_handle_t lock, bool *held, bool want) {
    if (!lock || *held == want) return;
    if ((want ? esp_pm_lock_acquire(lock) : esp_pm_lock_release(lock)) == ESP_OK) *held = want;
}

static void apply_power(uint32_t now) {
    static int backlight = -1;
    note_activity(now);
    vv_power_update(&s_power, s_app.state, now);
    int level = vv_power_backlight(s_power.light);
    if (level != backlight) {
        backlight = level;
        bsp_display_backlight((uint8_t)level);
        ESP_LOGI(TAG, "backlight %d%%", level);
    }
    // LEDC dimming, LCD flushes and the button-to-UI path need the clocks.
    hold(s_awake_lock, &s_awake_held,
         s_power.light != VV_LIGHT_OFF || vv_power_busy(s_app.state));
    hold(s_cpu_lock, &s_cpu_held,
         s_app.state == VV_ST_DICTATING || s_app.state == VV_ST_WAITING);
    vv_ble_set_pace(s_power.pace == VV_PACE_FAST);
}

// Dynamic frequency scaling, plus automatic light sleep whenever no lock
// forbids it. BLE keeps time on the main crystal during light sleep (the
// board has no 32 kHz crystal); USB stays awake while a host is attached.
static void power_init(void) {
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power management unavailable: %s", esp_err_to_name(err));
        return;
    }
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "vv_awake", &s_awake_lock) != ESP_OK ||
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "vv_cpu", &s_cpu_lock) != ESP_OK) {
        ESP_LOGW(TAG, "power locks unavailable");
        return;
    }
    // Stay awake through startup; the controller decides from then on.
    hold(s_awake_lock, &s_awake_held, true);
}

static void controller_task(void *arg) {
    (void)arg;
    static ctl_event_t ev;
    static vv_actions_t act;
    uint32_t pending_dirty = 0;
    uint32_t last_tick = now_ms();
    uint32_t last_battery = last_tick - BATTERY_POLL_MS;
    int battery = -1;
    bool battery_dirty = false;

    for (;;) {
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            uint32_t now = now_ms();
            // A press on a dark screen only lights it: the user cannot see
            // what OK or DOWN would do.
            bool wake_only = ev.kind == CTL_BUTTON && vv_power_activity(&s_power, now);
            if (!wake_only) {
                handle(&ev, now, &act);
                execute(&act);
            }
            if (ev.kind == CTL_LINK && ev.a == VV_LINK_DISCONNECTED) {
                ESP_LOGI(TAG, "BLE frames dropped so far: %u", (unsigned)vv_ble_dropped());
            }
        }

        uint32_t now = now_ms();
        bool ticked = now - last_tick >= TICK_MS;
        if (ticked) {
            last_tick = now;
            vv_app_tick(&s_app, now, &act);
            execute(&act);
        }
        if (s_battery_ok && now - last_battery >= BATTERY_POLL_MS) {
            last_battery = now;
            int soc = bsp_battery_soc();
            if (soc != battery) {
                battery = soc;
                battery_dirty = true;
            }
        }

        apply_power(now);

        pending_dirty |= vv_app_take_dirty(&s_app);
        bool meter = ticked && s_app.state == VV_ST_DICTATING;
        if (!pending_dirty && !battery_dirty && !meter) continue;
        if (!bsp_lvgl_lock(LVGL_LOCK_MS)) continue;  // retry on the next loop
        if (pending_dirty) vv_ui_render(&s_app, pending_dirty);
        if (battery_dirty) vv_ui_set_battery(battery);
        if (meter) vv_ui_push_level(vv_audio_level());
        bsp_lvgl_unlock();
        pending_dirty = 0;
        battery_dirty = false;
    }
}

// --- Startup ------------------------------------------------------------------

void app_main(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(s_fw, sizeof(s_fw), "VibeVoice/%s", desc->version);
    ESP_LOGI(TAG, "%s starting", s_fw);
    log_heap("boot");

    // Bonds live in NVS. Never erase it here: that would destroy bonds and
    // any other stored data just to hide a partition problem.
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init: %s (bonds will not persist)",
                                esp_err_to_name(err));

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    power_init();
    vv_power_init(&s_power, now_ms());
    bsp_display_backlight(vv_power_backlight(VV_LIGHT_ON));

    s_queue = xQueueCreate(CTL_QUEUE_DEPTH, sizeof(ctl_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "control queue allocation failed");
        return;
    }
    vv_app_init(&s_app, s_fw);

    if (bsp_audio_init() != ESP_OK || vv_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio unavailable: Dictations will carry no audio");
    }
    s_battery_ok = bsp_battery_init() == ESP_OK;
    if (!s_battery_ok) ESP_LOGW(TAG, "battery gauge unavailable");
    log_heap("before BLE");

    err = vv_ble_start(on_link, on_rx);
    if (err != ESP_OK) ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
    log_heap("after BLE start");

    if (bsp_lvgl_lock(1000)) {
        vv_ui_init(vv_ble_name());
        vv_ui_render(&s_app, vv_app_take_dirty(&s_app));
        log_lvgl_pool("after UI");
        bsp_lvgl_unlock();
    }

    if (xTaskCreate(controller_task, "vv_ctl", CTL_STACK, NULL, CTL_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "controller task creation failed");
        return;
    }
    err = bsp_button_init(on_button, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "buttons unavailable: %s", esp_err_to_name(err));
    log_heap("ready");
}
