// main/vv_audio.h -- Dictation audio worker: 16 kHz/16-bit/mono capture in
// 20 ms chunks, IMA-ADPCM, one AUDIO frame per chunk via vv_ble_send().
// Nothing is buffered beyond one chunk; frames the BLE queue cannot take are
// dropped (the Companion sees a gap in `seq`).
#pragma once

#include "esp_err.h"

#include <stdint.h>

// bsp_audio_init() must have succeeded. Creates the worker task.
esp_err_t vv_audio_init(void);

// Starts streaming Dictation `dict` (seq restarts at 0, encoder state reset).
void vv_audio_start(uint8_t dict);

// Stops streaming and waits (bounded) until the worker has queued its last
// AUDIO frame, so a DICT_STOP/DICT_CANCEL sent afterwards follows it.
void vv_audio_stop(void);

// Recent input level 0..100 for the UI meter.
uint8_t vv_audio_level(void);
