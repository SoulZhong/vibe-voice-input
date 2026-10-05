// main/vv_adpcm.c -- IMA-ADPCM codec. See vv_adpcm.h.
#include "vv_adpcm.h"

static const int8_t INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

static int clamp_index(int index) {
    return index < 0 ? 0 : (index > 88 ? 88 : index);
}

static int clamp_sample(int value) {
    return value < -32768 ? -32768 : (value > 32767 ? 32767 : value);
}

void vv_adpcm_reset(vv_adpcm_state_t *state) {
    state->predictor = 0;
    state->index = 0;
}

static uint8_t encode_sample(vv_adpcm_state_t *state, int sample) {
    int predictor = state->predictor;
    int step = STEP_TABLE[state->index];
    int diff = sample - predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    int vpdiff = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; vpdiff += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; vpdiff += step; }
    predictor += (code & 8) ? -vpdiff : vpdiff;
    state->predictor = (int16_t)clamp_sample(predictor);
    state->index = (uint8_t)clamp_index(state->index + INDEX_TABLE[code]);
    return code;
}

static int16_t decode_nibble(vv_adpcm_state_t *state, uint8_t code) {
    int step = STEP_TABLE[state->index];
    int vpdiff = step >> 3;
    if (code & 4) vpdiff += step;
    if (code & 2) vpdiff += step >> 1;
    if (code & 1) vpdiff += step >> 2;
    int predictor = state->predictor + ((code & 8) ? -vpdiff : vpdiff);
    state->predictor = (int16_t)clamp_sample(predictor);
    state->index = (uint8_t)clamp_index(state->index + INDEX_TABLE[code & 0x0F]);
    return state->predictor;
}

void vv_adpcm_encode(vv_adpcm_state_t *state, const int16_t *pcm, size_t samples,
                     uint8_t *out) {
    for (size_t i = 0; i < samples; i += 2) {
        uint8_t low = encode_sample(state, pcm[i]);
        uint8_t high = (i + 1 < samples) ? encode_sample(state, pcm[i + 1]) : 0;
        out[i / 2] = (uint8_t)(low | (high << 4));
    }
}

void vv_adpcm_decode(vv_adpcm_state_t *state, const uint8_t *in, size_t samples,
                     int16_t *pcm) {
    for (size_t i = 0; i < samples; i++) {
        uint8_t byte = in[i / 2];
        pcm[i] = decode_nibble(state, (i & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F));
    }
}
