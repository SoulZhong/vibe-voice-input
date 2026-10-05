// Host test: Vibe Voice Device state machine (main/vv_app.c).
#include "vv_app.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static vv_app_t app;
static vv_actions_t act;
static uint32_t now;

static void frame(const uint8_t *data, size_t len) {
    vv_msg_t msg;
    assert(vv_proto_decode(data, len, &msg));
    vv_app_frame(&app, &msg, now, &act);
}

static void press(vv_btn_t btn, vv_press_t p) {
    vv_app_button(&app, btn, p, now, &act);
}

static void tick(uint32_t advance) {
    now += advance;
    vv_app_tick(&app, now, &act);
}

static void link_event(vv_link_ev_t ev, uint32_t passkey) {
    vv_app_link(&app, ev, passkey, now, &act);
}

static bool sent(uint8_t type) {
    for (int i = 0; i < act.frame_count; i++) {
        if (act.frames[i].data[0] == type) return true;
    }
    return false;
}

static void connect_ready(void) {
    vv_app_init(&app, "fw-test");
    now = 1000;
    assert(app.state == VV_ST_NO_LINK);
    link_event(VV_LINK_ADVERTISING, 0);
    link_event(VV_LINK_CONNECTED, 0);
    assert(app.state == VV_ST_NO_LINK);
    link_event(VV_LINK_PASSKEY, 123456);
    assert(app.state == VV_ST_PAIRING && app.passkey == 123456);
    link_event(VV_LINK_SECURE, 0);
    assert(app.state == VV_ST_LINKING && app.passkey == 0);
    // Frames before READY (notifications not enabled) are ignored.
    const uint8_t ack[] = { 0x81, 1 };
    frame(ack, sizeof(ack));
    assert(app.state == VV_ST_LINKING);
    link_event(VV_LINK_READY, 0);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_HELLO);
    assert(memcmp(&act.frames[0].data[2], "fw-test", 7) == 0);
    // HELLO repeats every second until HELLO_ACK (first copy may be lost).
    tick(500);
    assert(act.frame_count == 0 && app.state == VV_ST_LINKING);
    tick(1000);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_HELLO);
    frame(ack, sizeof(ack));
    assert(app.state == VV_ST_IDLE && !app.version_mismatch);
    const uint8_t target[] = { 0xB2, 0, 1, 'W', 'e', 'C', 'h', 'a', 't' };
    frame(target, sizeof(target));
    assert(app.target_known && strcmp(app.target_label, "WeChat") == 0);
    (void)vv_app_take_dirty(&app);
}

static void test_dictation_happy_path(void) {
    connect_ready();
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.state == VV_ST_DICTATING);
    assert(act.flags == VV_ACT_AUDIO_START && act.dict == 1);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_DICT_START &&
           act.frames[0].data[1] == 1);

    // DOWN does nothing while dictating.
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(act.flags == 0 && act.frame_count == 0 && app.state == VV_ST_DICTATING);

    const uint8_t partial[] = { 0x90, 1, 'h', 'e', 'l', 'l', 'o' };
    frame(partial, sizeof(partial));
    assert(strcmp(app.partial, "hello") == 0);
    assert(vv_app_take_dirty(&app) & VV_DIRTY_PARTIAL);
    const uint8_t stale[] = { 0x90, 9, 'x' };
    frame(stale, sizeof(stale));
    assert(strcmp(app.partial, "hello") == 0);

    tick(1500);
    assert(app.elapsed_s == 1 && (vv_app_take_dirty(&app) & VV_DIRTY_ELAPSED));

    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.state == VV_ST_WAITING);
    assert(act.flags == VV_ACT_AUDIO_STOP && act.frame_count == 1);
    assert(act.frames[0].data[0] == VV_MSG_DICT_STOP && act.frames[0].data[1] == 1);

    // Buttons are ignored while waiting.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.frame_count == 0 && app.state == VV_ST_WAITING);

    const uint8_t result[] = { 0x91, 1, 0, 'h', 'i' };
    frame(result, sizeof(result));
    assert(app.state == VV_ST_RESULT && app.result == VV_RESULT_INSERTED);
    assert(strcmp(app.segment, "hi") == 0);

    // DOWN in RESULT submits immediately.
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_SUBMIT);
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_SUBMITTING);
    const uint8_t submitted[] = { 0xA0, 0x20, 0 };
    frame(submitted, sizeof(submitted));
    assert(app.toast == VV_TOAST_SUBMITTED);
    tick(VV_TOAST_MS);
    assert(app.toast == VV_TOAST_NONE);
}

static void test_cancel_and_undo(void) {
    connect_ready();
    press(VV_BTN_OK, VV_PRESS_CLICK);
    press(VV_BTN_UP, VV_PRESS_CLICK);
    assert(act.flags == VV_ACT_AUDIO_STOP && act.frame_count == 1);
    assert(act.frames[0].data[0] == VV_MSG_DICT_CANCEL);
    assert(app.state == VV_ST_RESULT && app.result == VV_RESULT_CANCELLED);
    // The Companion's CANCELLED reply is not shown again.
    const uint8_t cancelled[] = { 0x91, 1, 2 };
    frame(cancelled, sizeof(cancelled));
    assert(app.result == VV_RESULT_CANCELLED);
    tick(VV_RESULT_ERR_MS);
    assert(app.state == VV_ST_IDLE);

    press(VV_BTN_UP, VV_PRESS_CLICK);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_UNDO);
    const uint8_t nothing[] = { 0xA0, 0x21, 6 };
    frame(nothing, sizeof(nothing));
    assert(app.toast == VV_TOAST_NOTHING_TO_UNDO);

    // DOUBLE never Submits or Undoes.
    press(VV_BTN_DOWN, VV_PRESS_DOUBLE);
    assert(act.frame_count == 0);
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(act.frame_count == 0 && app.state == VV_ST_IDLE);

    // Undo after an Insert leaves the result preview.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.dict == 2);
    press(VV_BTN_OK, VV_PRESS_LONG);   // long OK also stops
    assert(app.state == VV_ST_WAITING);
    const uint8_t ok[] = { 0x91, 2, 0, 'x' };
    frame(ok, sizeof(ok));
    assert(app.state == VV_ST_RESULT);
    press(VV_BTN_UP, VV_PRESS_CLICK);
    const uint8_t undone[] = { 0xA0, 0x21, 0 };
    frame(undone, sizeof(undone));
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_UNDONE);
}

static void test_result_statuses_and_timeouts(void) {
    connect_ready();
    static const struct { uint8_t status; vv_result_t result; } cases[] = {
        { 1, VV_RESULT_EMPTY }, { 3, VV_RESULT_TARGET_DOWN }, { 4, VV_RESULT_RECOGNIZER },
        { 5, VV_RESULT_PERMISSION }, { 99, VV_RESULT_FAILED },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        press(VV_BTN_OK, VV_PRESS_CLICK);
        uint8_t dict = act.dict;
        press(VV_BTN_OK, VV_PRESS_CLICK);
        const uint8_t wrong[] = { 0x91, (uint8_t)(dict + 1), 0 };
        frame(wrong, sizeof(wrong));
        assert(app.state == VV_ST_WAITING);
        const uint8_t r[] = { 0x91, dict, cases[i].status };
        frame(r, sizeof(r));
        assert(app.state == VV_ST_RESULT && app.result == cases[i].result);
        assert(app.segment[0] == '\0');
    }

    // No RESULT: time out to an error, and a late RESULT is dropped.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    uint8_t dict = act.dict;
    press(VV_BTN_OK, VV_PRESS_CLICK);
    tick(VV_WAIT_RESULT_MS - 1);
    assert(app.state == VV_ST_WAITING);
    tick(1);
    assert(app.state == VV_ST_RESULT && app.result == VV_RESULT_TIMEOUT);
    const uint8_t late[] = { 0x91, dict, 0, 'z' };
    frame(late, sizeof(late));
    assert(app.result == VV_RESULT_TIMEOUT);

    // Five-minute limit stops the Dictation by itself.
    tick(VV_RESULT_ERR_MS);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    dict = act.dict;
    for (uint32_t t = 0; t + 1000 < VV_DICTATION_LIMIT_MS; t += 1000) {
        tick(1000);
        assert(app.state == VV_ST_DICTATING && act.frame_count == 0);
    }
    tick(1000);
    assert(app.state == VV_ST_WAITING && act.flags == VV_ACT_AUDIO_STOP);
    assert(act.frames[0].data[0] == VV_MSG_DICT_STOP && act.frames[0].data[1] == dict);
}

static void test_disconnect_abandons(void) {
    connect_ready();
    press(VV_BTN_OK, VV_PRESS_CLICK);
    link_event(VV_LINK_DISCONNECTED, 0);
    assert(app.state == VV_ST_NO_LINK);
    assert(act.flags == VV_ACT_AUDIO_STOP && act.frame_count == 0);  // no DICT_STOP
    // Buttons do nothing without a link.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.flags == 0 && act.frame_count == 0);
    // Reconnect with existing bond: no passkey.
    link_event(VV_LINK_CONNECTED, 0);
    link_event(VV_LINK_SECURE, 0);
    link_event(VV_LINK_READY, 0);
    assert(sent(VV_MSG_HELLO));
    link_event(VV_LINK_READY, 0);
    assert(act.frame_count == 0);   // duplicate READY does not re-send
    const uint8_t ack2[] = { 0x81, 2 };
    frame(ack2, sizeof(ack2));
    assert(app.state == VV_ST_IDLE && app.version_mismatch);

    // Companion turns notifications off and on again on the same link.
    link_event(VV_LINK_CONNECTED, 0);
    assert(app.state == VV_ST_NO_LINK && !app.link_ready);
    link_event(VV_LINK_READY, 0);
    assert(sent(VV_MSG_HELLO) && app.state == VV_ST_LINKING);

    link_event(VV_LINK_PAIR_FAILED, 0);
    assert(app.state == VV_ST_NO_LINK && app.toast == VV_TOAST_PAIR_FAILED);
}

static void item(uint8_t list, uint8_t index, uint8_t count, uint8_t flags, const char *label) {
    uint8_t data[64] = { 0xB0, list, index, count, flags };
    size_t n = strlen(label);
    memcpy(&data[5], label, n);
    frame(data, 5 + n);
}

static void test_picker(void) {
    connect_ready();
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_PICKER && app.picker_loading && app.picker_list == 0);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_TARGETS_REQ &&
           act.frames[0].data[1] == 0);
    press(VV_BTN_OK, VV_PRESS_CLICK);           // nothing to select yet
    assert(act.frame_count == 0);
    item(1, 0, 3, 0, "wrong list");
    assert(app.picker_count == 0);
    item(0, 0, 3, 0, "Follow");
    item(0, 1, 3, VV_ITEM_CURRENT | VV_ITEM_NOT_RUNNING, "WeChat");
    item(0, 2, 3, VV_ITEM_SUBLIST, "Orca");
    const uint8_t end[] = { 0xB1, 0, 3 };
    frame(end, sizeof(end));
    assert(!app.picker_loading && app.picker_count == 3 && app.picker_cursor == 1);

    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(app.picker_cursor == 2);
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(app.picker_cursor == 0);              // wraps
    press(VV_BTN_UP, VV_PRESS_CLICK);
    assert(app.picker_cursor == 2);
    press(VV_BTN_UP, VV_PRESS_DOUBLE);
    assert(app.picker_cursor == 0);

    // Item with a sub-list opens the Orca Sessions list.
    press(VV_BTN_UP, VV_PRESS_CLICK);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.picker_list == 1 && app.picker_loading);
    assert(act.frames[0].data[0] == VV_MSG_TARGETS_REQ && act.frames[0].data[1] == 1);
    item(1, 0, 2, 0, "shell-1");
    item(1, 1, 2, 0, "shell-2");
    const uint8_t end1[] = { 0xB1, 1, 2 };
    frame(end1, sizeof(end1));
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.state == VV_ST_IDLE);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_TARGET_SELECT);
    assert(act.frames[0].data[1] == 1 && act.frames[0].data[2] == 1);

    // Long OK backs out of the sub-list, then closes the picker.
    press(VV_BTN_OK, VV_PRESS_LONG);
    item(0, 0, 1, VV_ITEM_SUBLIST, "Orca");
    frame(end, sizeof(end));
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.picker_list == 1);
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_PICKER && app.picker_list == 0 && app.picker_loading);
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_IDLE);

    // An empty list can be closed; a list that never ends times out.
    press(VV_BTN_OK, VV_PRESS_LONG);
    const uint8_t empty[] = { 0xB1, 0, 0 };
    frame(empty, sizeof(empty));
    assert(!app.picker_loading && app.picker_count == 0);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.frame_count == 0 && app.state == VV_ST_PICKER);
    press(VV_BTN_OK, VV_PRESS_LONG);
    press(VV_BTN_OK, VV_PRESS_LONG);
    tick(VV_PICKER_TIMEOUT_MS);
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_LIST_FAILED);

    // More items than fit are ignored safely.
    press(VV_BTN_OK, VV_PRESS_LONG);
    for (int i = 0; i < VV_PICKER_MAX + 5; i++) item(0, (uint8_t)i, 40, 0, "x");
    assert(app.picker_count == VV_PICKER_MAX);
}

static void test_status_and_target(void) {
    connect_ready();
    const uint8_t status[] = { 0x82, 2, 'A', 'X' };
    frame(status, sizeof(status));
    assert(app.companion_code == 2 && strcmp(app.companion_text, "AX") == 0);
    assert(vv_app_take_dirty(&app) & VV_DIRTY_STATUS);
    const uint8_t clear[] = { 0x82, 0 };
    frame(clear, sizeof(clear));
    assert(app.companion_code == 0);

    // Not-running Target and invalid UTF-8 in the label.
    const uint8_t target[] = { 0xB2, 3, 1, 'Q', (uint8_t)0xFF, 'Q' };
    frame(target, sizeof(target));
    assert(app.target_status == 3 && strcmp(app.target_label, "Q?Q") == 0);

    // Over-long Partial Text keeps its tail and flags the cut.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    uint8_t partial[VV_FRAME_MAX] = { 0x90, act.dict };
    memset(&partial[2], 'a', sizeof(partial) - 2);
    partial[VV_FRAME_MAX - 1] = 'z';
    frame(partial, sizeof(partial));
    size_t n = strlen(app.partial);
    assert(n == VV_FRAME_MAX - 2 && app.partial[n - 1] == 'z' && !app.partial_cut);
}

int main(void) {
    test_dictation_happy_path();
    test_cancel_and_undo();
    test_result_statuses_and_timeouts();
    test_disconnect_abandons();
    test_picker();
    test_status_and_target();
    puts("test_vv_app: PASS");
    return 0;
}
