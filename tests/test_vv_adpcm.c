// Host test: the firmware IMA-ADPCM encoder must reproduce the shared golden
// vector (tests/vectors/adpcm_golden.txt) that the Companion decoder also uses.
#include "vv_adpcm.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SAMPLES 512

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    assert(text);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    text[size] = '\0';
    fclose(file);
    return text;
}

static const char *find_line(const char *text, const char *key) {
    size_t key_len = strlen(key);
    for (const char *line = text; line && *line; ) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == ' ') return line + key_len + 1;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

static size_t parse_ints(const char *line, int *out, size_t max) {
    size_t count = 0;
    char *end;
    while (*line && *line != '\n' && count < max) {
        long value = strtol(line, &end, 10);
        if (end == line) break;
        out[count++] = (int)value;
        line = end;
    }
    return count;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    char *text = read_file(argv[1]);

    int input[MAX_SAMPLES], expected_pcm[MAX_SAMPLES], final_pred, final_index;
    size_t samples = parse_ints(find_line(text, "input"), input, MAX_SAMPLES);
    assert(samples == 320);
    assert(parse_ints(find_line(text, "decoded"), expected_pcm, MAX_SAMPLES) == samples);
    assert(parse_ints(find_line(text, "final_predictor"), &final_pred, 1) == 1);
    assert(parse_ints(find_line(text, "final_index"), &final_index, 1) == 1);

    uint8_t expected_adpcm[MAX_SAMPLES / 2];
    const char *hex = find_line(text, "encoded");
    for (size_t i = 0; i < samples / 2; i++) {
        int high = hex_value(hex[2 * i]), low = hex_value(hex[2 * i + 1]);
        assert(high >= 0 && low >= 0);
        expected_adpcm[i] = (uint8_t)(high << 4 | low);
    }

    int16_t pcm[MAX_SAMPLES];
    for (size_t i = 0; i < samples; i++) pcm[i] = (int16_t)input[i];

    // Encode in two 20 ms halves to prove state carries across AUDIO frames.
    vv_adpcm_state_t enc;
    vv_adpcm_reset(&enc);
    uint8_t adpcm[MAX_SAMPLES / 2];
    vv_adpcm_encode(&enc, pcm, 160, adpcm);
    vv_adpcm_state_t mid = enc;
    vv_adpcm_encode(&enc, pcm + 160, samples - 160, adpcm + 80);
    assert(memcmp(adpcm, expected_adpcm, samples / 2) == 0);
    assert(enc.predictor == final_pred && enc.index == final_index);

    vv_adpcm_state_t dec;
    vv_adpcm_reset(&dec);
    int16_t decoded[MAX_SAMPLES];
    vv_adpcm_decode(&dec, adpcm, samples, decoded);
    for (size_t i = 0; i < samples; i++) assert(decoded[i] == expected_pcm[i]);
    assert(dec.predictor == final_pred && dec.index == final_index);

    // A frame decodes on its own from the state carried in its header.
    vv_adpcm_state_t independent = mid;
    vv_adpcm_decode(&independent, adpcm + 80, samples - 160, decoded);
    for (size_t i = 0; i < samples - 160; i++) assert(decoded[i] == expected_pcm[160 + i]);

    // Extremes clamp instead of overflowing.
    int16_t extremes[8] = { 32767, 32767, -32768, -32768, 32767, -32768, 0, 0 };
    vv_adpcm_state_t clamp;
    vv_adpcm_reset(&clamp);
    for (int round = 0; round < 200; round++) vv_adpcm_encode(&clamp, extremes, 8, adpcm);
    assert(clamp.index <= 88);

    free(text);
    puts("test_vv_adpcm: PASS");
    return 0;
}
