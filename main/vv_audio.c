// main/vv_audio.c -- see vv_audio.h.
#include "vv_audio.h"

#include "vv_adpcm.h"
#include "vv_ble.h"
#include "vv_proto.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <stdbool.h>

static const char *TAG = "vv_audio";

#define SAMPLE_RATE   16000
#define WORKER_STACK  3584
// Above LVGL (4) and the BLE TX task (6): a missed read overflows I2S DMA.
#define WORKER_PRIO   7
#define STOP_WAIT_MS  300

static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped;
static volatile bool s_run;
static volatile uint8_t s_dict;
static volatile uint8_t s_level;
static bool s_session;   // touched only by the controller task

static uint8_t level_of(const int16_t *pcm, size_t n) {
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    // Square-root scale so quiet speech still moves the meter.
    float level = sqrtf((float)peak / 32768.0f) * 100.0f;
    return (uint8_t)(level > 100.0f ? 100.0f : level);
}

static void stream(uint8_t dict) {
    static int16_t pcm[VV_AUDIO_SAMPLES];
    static uint8_t adpcm[VV_AUDIO_ADPCM_BYTES];
    static vv_frame_t frame;
    vv_adpcm_state_t state;
    uint16_t seq = 0;
    uint32_t read_errors = 0, dropped = 0;

    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "set_format failed; Dictation %u has no audio", dict);
        return;
    }
    vv_adpcm_reset(&state);
    // The RX DMA ring holds up to ~90 ms captured before this Dictation.
    (void)bsp_audio_read(pcm, sizeof(pcm));

    while (s_run) {
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            if (++read_errors % 50 == 1) ESP_LOGW(TAG, "read failed (%u)", (unsigned)read_errors);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        vv_adpcm_state_t before = state;
        vv_adpcm_encode(&state, pcm, VV_AUDIO_SAMPLES, adpcm);
        vv_proto_audio(&frame, dict, seq++, before.predictor, before.index, adpcm);
        if (!vv_ble_send(&frame, 0)) dropped++;
        s_level = level_of(pcm, VV_AUDIO_SAMPLES);
    }
    ESP_LOGI(TAG, "Dictation %u: %u frames, %u dropped", dict, seq, (unsigned)dropped);
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_run) stream(s_dict);
        s_level = 0;
        xSemaphoreGive(s_stopped);
    }
}

esp_err_t vv_audio_init(void) {
    s_stopped = xSemaphoreCreateBinary();
    if (!s_stopped) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker, "vv_audio", WORKER_STACK, NULL, WORKER_PRIO, &s_task) != pdPASS) {
        vSemaphoreDelete(s_stopped);
        s_stopped = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void vv_audio_start(uint8_t dict) {
    if (!s_task) return;
    if (s_session) vv_audio_stop();
    (void)xSemaphoreTake(s_stopped, 0);  // drop a late give from a timed-out stop
    s_dict = dict;
    s_run = true;
    s_session = true;
    xTaskNotifyGive(s_task);
}

void vv_audio_stop(void) {
    if (!s_task || !s_session) return;
    s_run = false;
    s_session = false;
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(STOP_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "worker did not stop within %d ms", STOP_WAIT_MS);
    }
}

uint8_t vv_audio_level(void) {
    return s_level;
}
