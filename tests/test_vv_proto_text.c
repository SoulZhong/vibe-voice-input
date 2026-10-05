// Host test: Vibe Voice frame codec and UTF-8 / wrap helpers.
#include "vv_proto.h"
#include "vv_text.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_encoders(void) {
    vv_frame_t f;
    assert(vv_proto_hello(&f, "vibe-voice/1.0") == 2 + 14);
    assert(f.data[0] == 0x01 && f.data[1] == 1 && memcmp(&f.data[2], "vibe-voice/1.0", 14) == 0);

    assert(vv_proto_dict(&f, VV_MSG_DICT_START, 7) == 2 && f.data[0] == 0x10 && f.data[1] == 7);
    assert(vv_proto_dict(&f, VV_MSG_DICT_STOP, 255) == 2 && f.data[0] == 0x12);
    assert(vv_proto_dict(&f, VV_MSG_DICT_CANCEL, 0) == 2 && f.data[0] == 0x13);
    assert(vv_proto_dict(&f, VV_MSG_SUBMIT, 0) == 0);

    uint8_t adpcm[VV_AUDIO_ADPCM_BYTES];
    for (int i = 0; i < VV_AUDIO_ADPCM_BYTES; i++) adpcm[i] = (uint8_t)i;
    assert(vv_proto_audio(&f, 3, 0x1234, -2, 88, adpcm) == 167);
    assert(f.len == 167 && f.len <= VV_FRAME_MAX);
    assert(f.data[0] == 0x11 && f.data[1] == 3);
    assert(f.data[2] == 0x34 && f.data[3] == 0x12);           // seq little-endian
    assert(f.data[4] == 0xFE && f.data[5] == 0xFF);           // pred -2 little-endian
    assert(f.data[6] == 88 && f.data[7] == 0 && f.data[166] == 159);

    assert(vv_proto_simple(&f, VV_MSG_SUBMIT) == 1 && f.data[0] == 0x20);
    assert(vv_proto_simple(&f, VV_MSG_UNDO) == 1 && f.data[0] == 0x21);
    assert(vv_proto_simple(&f, VV_MSG_HELLO) == 0);
    assert(vv_proto_targets_req(&f, 1) == 2 && f.data[0] == 0x30 && f.data[1] == 1);
    assert(vv_proto_target_select(&f, 0, 4) == 3 && f.data[0] == 0x31 && f.data[2] == 4);

    // An over-long firmware string is cut to its tail on a character boundary.
    char fw[400];
    memset(fw, 'a', sizeof(fw));
    memcpy(fw + 300, "\xE4\xB8\xAD", 3);
    fw[399] = '\0';
    assert(vv_proto_hello(&f, fw) <= VV_FRAME_MAX);
}

static void test_decoder(void) {
    vv_msg_t m;
    const uint8_t ack[] = { 0x81, 1 };
    assert(vv_proto_decode(ack, sizeof(ack), &m) && m.type == 0x81 && m.a == 1);
    const uint8_t ack_short[] = { 0x81 };
    assert(!vv_proto_decode(ack_short, 1, &m));

    const uint8_t partial[] = { 0x90, 5, 0xE4, 0xBD, 0xA0, 'o', 'k' };
    assert(vv_proto_decode(partial, sizeof(partial), &m));
    assert(m.a == 5 && m.text_len == 5 && memcmp(m.text, "\xE4\xBD\xA0ok", 5) == 0);

    const uint8_t result[] = { 0x91, 5, 0, 'h', 'i' };
    assert(vv_proto_decode(result, sizeof(result), &m) && m.a == 5 && m.b == 0 && m.text_len == 2);
    const uint8_t result_empty[] = { 0x91, 5, 1 };
    assert(vv_proto_decode(result_empty, 3, &m) && m.b == 1 && m.text_len == 0);

    const uint8_t action[] = { 0xA0, 0x21, 6 };
    assert(vv_proto_decode(action, 3, &m) && m.a == 0x21 && m.b == 6);
    const uint8_t item[] = { 0xB0, 0, 2, 5, 0x06, 'O', 'r', 'c', 'a' };
    assert(vv_proto_decode(item, sizeof(item), &m));
    assert(m.a == 0 && m.b == 2 && m.c == 5 && m.d == 6 && m.text_len == 4);
    const uint8_t item_short[] = { 0xB0, 0, 2, 5 };
    assert(!vv_proto_decode(item_short, 4, &m));
    const uint8_t end[] = { 0xB1, 1, 3 };
    assert(vv_proto_decode(end, 3, &m) && m.a == 1 && m.b == 3);
    const uint8_t state[] = { 0xB2, 3, 1, 'W' };
    assert(vv_proto_decode(state, 4, &m) && m.a == 3 && m.b == 1 && m.text_len == 1);
    const uint8_t status[] = { 0x82, 2 };
    assert(vv_proto_decode(status, 2, &m) && m.a == 2 && m.text_len == 0);

    const uint8_t unknown[] = { 0x55, 0 };
    assert(!vv_proto_decode(unknown, 2, &m));
    assert(!vv_proto_decode(NULL, 0, &m));
    uint8_t big[VV_FRAME_MAX + 1] = { 0x90, 1 };
    assert(!vv_proto_decode(big, sizeof(big), &m));
    assert(vv_proto_decode(big, VV_FRAME_MAX, &m) && m.text_len == VV_FRAME_MAX - 2);
}

static void test_utf8(void) {
    const char *s = "ab\xE4\xB8\xAD\xE6\x96\x87";  // "ab中文", 8 bytes
    assert(vv_utf8_tail_start(s, 8, 8) == 0);
    assert(vv_utf8_tail_start(s, 8, 7) == 1);   // "b中文"
    assert(vv_utf8_tail_start(s, 8, 6) == 2);   // "中文"
    assert(vv_utf8_tail_start(s, 8, 5) == 5);   // cannot split 中 -> "文"
    assert(vv_utf8_tail_start(s, 8, 2) == 8);   // nothing fits
    assert(vv_utf8_tail_start(s, 8, 0) == 8);

    size_t pos = 0;
    uint32_t cp;
    assert(vv_utf8_next(s, 8, &pos, &cp) && cp == 'a');
    pos = 2;
    assert(vv_utf8_next(s, 8, &pos, &cp) && cp == 0x4E2D && pos == 5);
    const char bad[] = { (char)0xC0, (char)0x80, (char)0xE4, (char)0xB8 };
    pos = 0;
    assert(vv_utf8_next(bad, 4, &pos, &cp) && cp == 0xFFFD && pos == 1);  // overlong
    pos = 2;
    assert(vv_utf8_next(bad, 4, &pos, &cp) && cp == 0xFFFD && pos == 3);  // truncated
    const char surrogate[] = "\xED\xA0\x80";
    pos = 0;
    assert(vv_utf8_next(surrogate, 3, &pos, &cp) && cp == 0xFFFD);
    const char emoji[] = "\xF0\x9F\x98\x80";
    pos = 0;
    assert(vv_utf8_next(emoji, 4, &pos, &cp) && cp == 0x1F600 && pos == 4);

    char out[7];
    bool cut;
    assert(vv_utf8_sanitize_tail(s, 8, out, sizeof(out), &cut) == 6 && cut);
    assert(strcmp(out, "\xE4\xB8\xAD\xE6\x96\x87") == 0);
    char big[32];
    assert(vv_utf8_sanitize_tail("a\rb\x01" "c\n", 6, big, sizeof(big), &cut) == 5 && !cut);
    assert(strcmp(big, "ab?c\n") == 0);
    vv_utf8_sanitize_tail(bad, 4, big, sizeof(big), NULL);
    assert(strcmp(big, "????") == 0);
    vv_utf8_sanitize_tail(NULL, 5, big, sizeof(big), NULL);
    assert(big[0] == '\0');
}

static void test_wrap(void) {
    const vv_wrap_t wrap = { .line_px = 64, .max_lines = 2, .narrow_px = 8, .wide_px = 16 };
    assert(vv_text_lines("", 0, &wrap) == 0);
    assert(vv_text_lines("abcdefgh", 8, &wrap) == 1);     // 64 px exactly
    assert(vv_text_lines("abcdefghi", 9, &wrap) == 2);
    assert(vv_text_lines("a\nb", 3, &wrap) == 2);
    // Five hanzi (16 px each): 4 per line.
    const char *zh = "\xE4\xB8\x80\xE4\xBA\x8C\xE4\xB8\x89\xE5\x9B\x9B\xE4\xBA\x94";
    assert(vv_text_lines(zh, 15, &wrap) == 2);

    // 12 hanzi need 3 lines; the tail that fits two lines drops the first 4.
    char twelve[64] = "";
    for (int i = 0; i < 12; i++) strcat(twelve, "\xE4\xB8\xAD");
    assert(vv_text_lines(twelve, 36, &wrap) == 3);
    assert(vv_text_fit_tail(twelve, 36, &wrap) == 12);
    assert(vv_text_fit_tail(zh, 15, &wrap) == 0);
    assert(vv_text_fit_tail("", 0, &wrap) == 0);
}

static void test_format(void) {
    char t[6];
    vv_format_elapsed(0, t);
    assert(strcmp(t, "00:00") == 0);
    vv_format_elapsed(299999, t);
    assert(strcmp(t, "04:59") == 0);
    vv_format_elapsed(300000, t);
    assert(strcmp(t, "05:00") == 0);
    vv_format_elapsed(200u * 60u * 1000u, t);
    assert(strcmp(t, "99:00") == 0);
    char p[8];
    vv_format_passkey(42, p);
    assert(strcmp(p, "000 042") == 0);
    vv_format_passkey(987654, p);
    assert(strcmp(p, "987 654") == 0);
}

int main(void) {
    test_encoders();
    test_decoder();
    test_utf8();
    test_wrap();
    test_format();
    puts("test_vv_proto_text: PASS");
    return 0;
}
