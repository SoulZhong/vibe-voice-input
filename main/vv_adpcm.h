// main/vv_adpcm.h -- IMA-ADPCM (4 bit, standard 89-step table, low nibble first).
// Pure C, no ESP-IDF dependency. Must reproduce tests/vectors/adpcm_golden.txt.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t predictor;
    uint8_t index;   // 0..88
} vv_adpcm_state_t;

void vv_adpcm_reset(vv_adpcm_state_t *state);

// Encodes `samples` 16-bit samples into samples/2 bytes (rounded up; a final
// odd sample occupies the low nibble of the last byte). Updates `state`.
void vv_adpcm_encode(vv_adpcm_state_t *state, const int16_t *pcm, size_t samples,
                     uint8_t *out);

// Reference decoder (used by host tests; mirrors what the Companion does).
void vv_adpcm_decode(vv_adpcm_state_t *state, const uint8_t *in, size_t samples,
                     int16_t *pcm);
