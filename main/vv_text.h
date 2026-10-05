// main/vv_text.h -- UTF-8 and text-layout helpers (pure C, host tested).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Smallest character-boundary offset `start` such that len - start <= max_bytes.
// Text that already fits returns 0.
size_t vv_utf8_tail_start(const char *s, size_t len, size_t max_bytes);

// Decodes one code point at *pos. Returns false at the end. Invalid or
// overlong sequences decode as U+FFFD and consume one byte.
bool vv_utf8_next(const char *s, size_t len, size_t *pos, uint32_t *cp);

// Copies the tail of `in` that fits `out_size - 1` bytes into `out` and
// NUL-terminates. Invalid UTF-8 bytes and control characters other than
// '\n' become '?'; '\r' is dropped. Returns the output length.
// `*cut` (optional) is set when the input had to be shortened.
size_t vv_utf8_sanitize_tail(const char *in, size_t len, char *out, size_t out_size,
                             bool *cut);

// Estimated width model for wrapping in the 16 px body font: printable ASCII
// is `narrow_px` wide, every other code point `wide_px`.
typedef struct {
    int line_px;
    int max_lines;
    int narrow_px;
    int wide_px;
} vv_wrap_t;

// Number of wrapped lines `s` needs under `wrap` (0 for empty text).
int vv_text_lines(const char *s, size_t len, const vv_wrap_t *wrap);

// Smallest character-boundary start such that s[start..] fits within
// wrap->max_lines. Used to show the newest part of long Partial Text.
size_t vv_text_fit_tail(const char *s, size_t len, const vv_wrap_t *wrap);

// "MM:SS" (minutes saturate at 99). `out` must hold 6 bytes.
void vv_format_elapsed(uint32_t ms, char out[6]);

// Voice Notes Recording time: "MM:SS" below one hour, else "H:MM:SS"
// (hours capped at 99).
void vv_format_notes_elapsed(uint32_t s, char out[9]);

// "123 456". `out` must hold 8 bytes.
void vv_format_passkey(uint32_t passkey, char out[8]);
