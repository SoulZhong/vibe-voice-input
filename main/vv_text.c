// main/vv_text.c -- see vv_text.h.
#include "vv_text.h"

#include <stdio.h>
#include <string.h>

static bool is_cont(uint8_t byte) {
    return (byte & 0xC0) == 0x80;
}

size_t vv_utf8_tail_start(const char *s, size_t len, size_t max_bytes) {
    if (!s || len <= max_bytes) return 0;
    size_t start = len - max_bytes;
    while (start < len && is_cont((uint8_t)s[start])) start++;
    return start;
}

bool vv_utf8_next(const char *s, size_t len, size_t *pos, uint32_t *cp) {
    if (*pos >= len) return false;
    const uint8_t *p = (const uint8_t *)s + *pos;
    size_t left = len - *pos;
    uint8_t b0 = p[0];
    uint32_t value;
    size_t need;
    uint32_t min;
    if (b0 < 0x80) {
        *cp = b0;
        *pos += 1;
        return true;
    } else if ((b0 & 0xE0) == 0xC0) {
        value = b0 & 0x1F; need = 1; min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        value = b0 & 0x0F; need = 2; min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        value = b0 & 0x07; need = 3; min = 0x10000;
    } else {
        goto invalid;
    }
    if (left <= need) goto invalid;
    for (size_t i = 1; i <= need; i++) {
        if (!is_cont(p[i])) goto invalid;
        value = (value << 6) | (p[i] & 0x3F);
    }
    if (value < min || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) goto invalid;
    *cp = value;
    *pos += need + 1;
    return true;
invalid:
    *cp = 0xFFFD;
    *pos += 1;
    return true;
}

size_t vv_utf8_sanitize_tail(const char *in, size_t len, char *out, size_t out_size,
                             bool *cut) {
    if (cut) *cut = false;
    if (!out || out_size == 0) return 0;
    if (!in) len = 0;
    size_t start = vv_utf8_tail_start(in, len, out_size - 1);
    if (cut) *cut = start > 0;
    size_t pos = start, n = 0;
    uint32_t cp;
    while (vv_utf8_next(in, len, &pos, &cp)) {
        if (cp == '\r') continue;
        if (cp == 0xFFFD || (cp < 0x20 && cp != '\n') || cp == 0x7F) {
            out[n++] = '?';
            continue;
        }
        // Valid sequence: copy its bytes verbatim (never longer than the input).
        size_t width = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        memcpy(&out[n], &in[pos - width], width);
        n += width;
    }
    out[n] = '\0';
    return n;
}

static int glyph_px(uint32_t cp, const vv_wrap_t *wrap) {
    return (cp >= 0x20 && cp < 0x7F) ? wrap->narrow_px : wrap->wide_px;
}

int vv_text_lines(const char *s, size_t len, const vv_wrap_t *wrap) {
    if (!s || len == 0) return 0;
    int lines = 1, x = 0;
    size_t pos = 0;
    uint32_t cp;
    while (vv_utf8_next(s, len, &pos, &cp)) {
        if (cp == '\n') {
            lines++;
            x = 0;
            continue;
        }
        int w = glyph_px(cp, wrap);
        if (x > 0 && x + w > wrap->line_px) {
            lines++;
            x = 0;
        }
        x += w;
    }
    return lines;
}

size_t vv_text_fit_tail(const char *s, size_t len, const vv_wrap_t *wrap) {
    size_t start = 0;
    uint32_t cp;
    while (start < len && vv_text_lines(s + start, len - start, wrap) > wrap->max_lines) {
        if (!vv_utf8_next(s, len, &start, &cp)) break;
    }
    return start;
}

void vv_format_elapsed(uint32_t ms, char out[6]) {
    uint32_t seconds = ms / 1000;
    uint32_t minutes = seconds / 60;
    if (minutes > 99) minutes = 99;
    snprintf(out, 6, "%02u:%02u", (unsigned)minutes, (unsigned)(seconds % 60));
}

void vv_format_passkey(uint32_t passkey, char out[8]) {
    passkey %= 1000000;
    snprintf(out, 8, "%03u %03u", (unsigned)(passkey / 1000), (unsigned)(passkey % 1000));
}
