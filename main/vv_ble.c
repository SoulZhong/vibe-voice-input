// main/vv_ble.c -- see vv_ble.h.
#include "vv_ble.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "vv_ble";

#define TX_QUEUE_DEPTH   12
#define TX_TASK_STACK    3072
#define TX_TASK_PRIO     6
#define TX_RETRY_DELAY   pdMS_TO_TICKS(5)
#define TX_RETRIES       20

// ESP-IDF's NimBLE examples use this initializer; its header is not public.
void ble_store_config_init(void);

// 6e400001-b5a3-f393-e0a9-e50e24dcca9e (UUID bytes are little-endian).
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static vv_ble_link_cb_t s_link_cb;
static vv_ble_rx_cb_t s_rx_cb;
static char s_name[20] = "VibeVoice";
static uint8_t s_own_addr_type;
static uint16_t s_tx_handle;
static QueueHandle_t s_tx_queue;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_secure;
static volatile bool s_subscribed;
static volatile bool s_ready;
static volatile uint32_t s_dropped;

static int gap_event(struct ble_gap_event *event, void *arg);

static void emit(vv_link_ev_t ev, uint32_t passkey) {
    if (s_link_cb) s_link_cb(ev, passkey);
}

static void update_ready(void) {
    bool ready = s_conn != BLE_HS_CONN_HANDLE_NONE && s_secure && s_subscribed;
    if (ready && !s_ready) {
        s_ready = true;
        ESP_LOGI(TAG, "link ready (mtu=%u)", ble_att_mtu(s_conn));
        emit(VV_LINK_READY, 0);
    } else if (!ready && s_ready) {
        s_ready = false;
        // Notifications switched off on a live link (e.g. the Companion
        // restarted): report "connected, not ready" so the next READY
        // starts a fresh HELLO exchange.
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) emit(VV_LINK_CONNECTED, 0);
    }
}

// --- GATT ------------------------------------------------------------------

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[VV_FRAME_MAX];
        uint16_t copied = 0;
        if (len == 0 || len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (conn != s_conn || !s_secure) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &copied) != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        if (s_rx_cb) s_rx_cb(buf, copied);
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        // TX is notify-first; a read returns the device name for BLE tools.
        // NimBLE rejects characteristics without an access_cb.
        int rc = os_mbuf_append(ctxt->om, s_name, strlen(s_name));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &RX_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &TX_UUID.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
                .val_handle = &s_tx_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

// --- Advertising -------------------------------------------------------------

static void advertise(void) {
    if (ble_gap_adv_active() || s_conn != BLE_HS_CONN_HANDLE_NONE) return;

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan response rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as %s", s_name);
    emit(VV_LINK_ADVERTISING, 0);
}

static uint32_t random_passkey(void) {
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % 1000000u);
    uint32_t value;
    do {
        value = esp_random();
    } while (value >= limit);
    return value % 1000000u;
}

// `final` is true after an encryption change: an insufficient link is then
// rejected. A CCCD restore may race ahead of ENC_CHANGE, so it only upgrades.
static void check_security(uint16_t conn, bool final) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;
    if (!final && !desc.sec_state.encrypted) return;
    bool secure = desc.sec_state.encrypted && desc.sec_state.authenticated &&
                  desc.sec_state.bonded && desc.sec_state.key_size == 16;
    ESP_LOGI(TAG, "security enc=%d auth=%d bond=%d key=%d",
             desc.sec_state.encrypted, desc.sec_state.authenticated,
             desc.sec_state.bonded, desc.sec_state.key_size);
    if (!secure) {
        ESP_LOGW(TAG, "link is not authenticated+bonded; disconnecting");
        emit(VV_LINK_PAIR_FAILED, 0);
        ble_gap_terminate(conn, BLE_ERR_AUTH_FAIL);
        return;
    }
    if (!s_secure) {
        s_secure = true;
        emit(VV_LINK_SECURE, 0);
        // Short interval for 50 AUDIO frames/s; macOS may pick its own.
        struct ble_gap_upd_params upd = {
            .itvl_min = 12,              // 15 ms
            .itvl_max = 24,              // 30 ms
            .latency = 0,
            .supervision_timeout = 400,  // 4 s
            .min_ce_len = 0,
            .max_ce_len = 0,
        };
        int rc = ble_gap_update_params(conn, &upd);
        if (rc != 0) ESP_LOGW(TAG, "conn param update rc=%d", rc);
    }
    update_ready();
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_secure = false;
        s_subscribed = false;
        s_ready = false;
        ESP_LOGI(TAG, "connected handle=%u", s_conn);
        emit(VV_LINK_CONNECTED, 0);
        (void)ble_gap_set_data_len(s_conn, 251, 2120);
        // Ask the central to pair (new peer) or to encrypt with the bond.
        if (ble_gap_security_initiate(s_conn) != 0) {
            ESP_LOGW(TAG, "security initiate failed");
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected reason=0x%x", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_secure = false;
        s_subscribed = false;
        s_ready = false;
        emit(VV_LINK_DISCONNECTED, 0);
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = random_passkey() };
            emit(VV_LINK_PASSKEY, io.passkey);
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc != 0) ESP_LOGE(TAG, "passkey inject rc=%d", rc);
        } else {
            ESP_LOGW(TAG, "unsupported passkey action %d", event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle != s_conn) return 0;
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "encryption failed status=%d", event->enc_change.status);
            emit(VV_LINK_PAIR_FAILED, 0);
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        check_security(s_conn, true);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The Mac forgot the bond: drop ours and pair again with a new passkey.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn &&
            event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "TX notify %s", s_subscribed ? "on" : "off");
            if (s_subscribed && !s_secure) check_security(s_conn, false);
            update_ready();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u", event->mtu.value);
        if (event->mtu.value < VV_FRAME_MAX + 3) {
            ESP_LOGW(TAG, "MTU below %d: AUDIO frames will not fit", VV_FRAME_MAX + 3);
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "conn interval=%u x1.25ms latency=%u", desc.conn_itvl,
                     desc.conn_latency);
        }
        return 0;
    }

    default:
        return 0;
    }
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "host reset reason=%d", reason);
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address setup rc=%d", rc);
        return;
    }
    advertise();
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// --- TX ----------------------------------------------------------------------

#define MTU_WAIT_MS 1000

static bool wait_for_mtu(uint16_t conn, uint16_t needed) {
    for (int waited = 0; waited < MTU_WAIT_MS; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!s_ready || s_conn != conn) return false;
        if (ble_att_mtu(conn) >= needed) return true;
    }
    return false;
}

static void tx_task(void *arg) {
    (void)arg;
    vv_frame_t frame;
    for (;;) {
        if (xQueueReceive(s_tx_queue, &frame, portMAX_DELAY) != pdTRUE) continue;
        bool sent = false;
        for (int attempt = 0; attempt < TX_RETRIES && s_ready; attempt++) {
            uint16_t conn = s_conn;
            if (conn == BLE_HS_CONN_HANDLE_NONE) break;
            if (ble_att_mtu(conn) < frame.len + 3) {
                // Notifications can be enabled before the central's MTU
                // exchange finishes; give it a moment instead of dropping.
                if (!wait_for_mtu(conn, frame.len + 3)) {
                    ESP_LOGW(TAG, "frame %u bytes exceeds MTU %u", frame.len,
                             ble_att_mtu(conn));
                    break;
                }
            }
            struct os_mbuf *om = ble_hs_mbuf_from_flat(frame.data, frame.len);
            if (om) {
                int rc = ble_gatts_notify_custom(conn, s_tx_handle, om);  // consumes om
                if (rc == 0) {
                    sent = true;
                    break;
                }
                if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                    ESP_LOGW(TAG, "notify rc=%d", rc);
                    break;
                }
            }
            vTaskDelay(TX_RETRY_DELAY);  // controller buffers full: back off
        }
        if (!sent) s_dropped++;
    }
}

bool vv_ble_send(const vv_frame_t *frame, uint32_t wait_ms) {
    if (!s_tx_queue || !s_ready || frame->len == 0) return false;
    if (xQueueSend(s_tx_queue, frame, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        s_dropped++;
        return false;
    }
    return true;
}

bool vv_ble_ready(void) {
    return s_ready;
}

uint32_t vv_ble_dropped(void) {
    return s_dropped;
}

const char *vv_ble_name(void) {
    return s_name;
}

esp_err_t vv_ble_start(vv_ble_link_cb_t link_cb, vv_ble_rx_cb_t rx_cb) {
    s_link_cb = link_cb;
    s_rx_cb = rx_cb;

    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
        snprintf(s_name, sizeof(s_name), "VibeVoice-%02X%02X", mac[4], mac[5]);
    }

    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(vv_frame_t));
    if (!s_tx_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(tx_task, "vv_tx", TX_TASK_STACK, NULL, TX_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_sec_lvl = 4;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(GATT_SERVICES);
    if (rc == 0) rc = ble_gatts_add_svcs(GATT_SERVICES);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup rc=%d", rc);
        return ESP_FAIL;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
