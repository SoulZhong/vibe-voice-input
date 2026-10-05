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
    const uint8_t ack[] = { 0x81, VV_PROTO_VERSION };
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
    const uint8_t target[] = { 0xB2, 0, 1, VV_APP_WECHAT, 'W', 'e', 'C', 'h', 'a', 't' };
    frame(target, sizeof(target));
    assert(app.target_known && strcmp(app.target_label, "WeChat") == 0);
    assert(app.target_app == VV_APP_WECHAT);
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

    // DOUBLE never Submits or Undoes; a double OK only toggles Voice Notes.
    press(VV_BTN_DOWN, VV_PRESS_DOUBLE);
    assert(act.frame_count == 0);
    press(VV_BTN_UP, VV_PRESS_DOUBLE);
    assert(act.frame_count == 0);
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_NOTES_TOGGLE);
    assert(act.flags == 0 && app.state == VV_ST_IDLE);

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
    const uint8_t ack2[] = { 0x81, 1 };   // a v1 Companion
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

static void end_list(uint8_t list, uint8_t count) {
    const uint8_t end[] = { 0xB1, list, count };
    frame(end, sizeof(end));
}

static void target_state(uint8_t status, uint8_t kind, uint8_t app_index, const char *label) {
    uint8_t data[96] = { 0xB2, status, kind, app_index };
    size_t n = strlen(label);
    memcpy(&data[4], label, n);
    frame(data, 4 + n);
}

// Root list: the four Supported Apps, each opening a sub-list.
static void root_list(uint8_t current) {
    static const char *const names[] = { "Orca", "WeChat", "ChatGPT", "WeCom" };
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t flags = VV_ITEM_SUBLIST | (i == current ? VV_ITEM_CURRENT : 0) |
                        (i >= 2 ? VV_ITEM_NOT_RUNNING : 0);
        item(0, i, 4, flags, names[i]);
    }
    end_list(0, 4);
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
    root_list(VV_APP_WECHAT);
    assert(!app.picker_loading && app.picker_count == 4 && app.picker_cursor == 1);
    // Root rows map to the Supported App logos; sub-lists have none.
    assert(vv_app_picker_logo(&app, 0) == VV_APP_ORCA);
    assert(vv_app_picker_logo(&app, 3) == VV_APP_WECOM);
    assert(vv_app_picker_logo(&app, 4) == -1);

    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(app.picker_cursor == 2);
    press(VV_BTN_DOWN, VV_PRESS_DOUBLE);
    assert(app.picker_cursor == 0);              // wraps
    press(VV_BTN_UP, VV_PRESS_CLICK);
    assert(app.picker_cursor == 3);

    // OK on root row n opens list n + 1 (here 1 = Orca, which may launch:
    // the longer timeout applies).
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.picker_list == 1 && app.picker_loading);
    assert(act.frames[0].data[0] == VV_MSG_TARGETS_REQ && act.frames[0].data[1] == 1);
    assert(vv_app_picker_logo(&app, 0) == -1);
    tick(VV_PICKER_TIMEOUT_MS);
    assert(app.state == VV_ST_PICKER && app.picker_loading);
    item(1, 0, 3, 0, "wt-a · build");
    item(1, 1, 3, VV_ITEM_CURRENT, "wt-b · voice");
    item(1, 2, 3, 0, "wt-c · docs");
    end_list(1, 3);
    assert(app.picker_cursor == 1);              // cursor on the Current Conversation
    assert(vv_app_picker_logo(&app, 0) == -1);

    // Jump: TARGET_SELECT, wait, close on TARGET_STATE with a toast.
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_TARGET_SELECT);
    assert(act.frames[0].data[1] == 1 && act.frames[0].data[2] == 2);
    assert(app.state == VV_ST_PICKER && app.picker_jumping);
    press(VV_BTN_DOWN, VV_PRESS_CLICK);          // ignored while jumping
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.frame_count == 0 && app.picker_cursor == 2);
    target_state(0, VV_KIND_ORCA, VV_APP_ORCA, "Orca · wt-c · docs");
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_JUMPED);
    assert(vv_app_target_logo(&app) == VV_APP_ORCA);
    assert(strcmp(vv_app_target_title(&app), "wt-c · docs") == 0);

    // A failed Jump closes with the matching toast.
    press(VV_BTN_OK, VV_PRESS_LONG);
    root_list(VV_APP_ORCA);
    press(VV_BTN_DOWN, VV_PRESS_CLICK);          // WeChat
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.picker_list == 2 && act.frames[0].data[1] == 2);
    item(2, 0, 1, VV_ITEM_CURRENT, "Current · Zhang");
    end_list(2, 1);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.frames[0].data[0] == VV_MSG_TARGET_SELECT && act.frames[0].data[1] == 2 &&
           act.frames[0].data[2] == 0);
    target_state(3, VV_KIND_APP, VV_APP_WECHAT, "WeChat");
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_TARGET_DOWN);

    // No TARGET_STATE: the Jump times out.
    press(VV_BTN_OK, VV_PRESS_LONG);
    root_list(VV_APP_WECHAT);
    press(VV_BTN_OK, VV_PRESS_CLICK);            // cursor on WeChat (current)
    assert(app.picker_list == 2);
    item(2, 0, 1, VV_ITEM_CURRENT, "Current");
    end_list(2, 1);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    tick(VV_JUMP_TIMEOUT_MS);
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_FAILED);

    // A TARGET_STATE without a pending Jump leaves the picker open.
    press(VV_BTN_OK, VV_PRESS_LONG);
    root_list(VV_APP_WECHAT);
    target_state(0, VV_KIND_APP, VV_APP_WECHAT, "WeChat · Li");
    assert(app.state == VV_ST_PICKER);

    // Empty sub-list of an app that is not running; long OK backs out, then closes.
    press(VV_BTN_DOWN, VV_PRESS_CLICK);          // ChatGPT (not running)
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.picker_list == 3 && (app.picker_parent_flags & VV_ITEM_NOT_RUNNING));
    end_list(3, 0);
    assert(!app.picker_loading && app.picker_count == 0);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(act.frame_count == 0 && app.state == VV_ST_PICKER);
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_PICKER && app.picker_list == 0 && app.picker_loading);
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_IDLE);

    // A list that never ends times out (root: 5 s; Orca: longer, it may launch).
    press(VV_BTN_OK, VV_PRESS_LONG);
    tick(VV_PICKER_TIMEOUT_MS);
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_LIST_FAILED);
    press(VV_BTN_OK, VV_PRESS_LONG);
    root_list(VV_APP_ORCA);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.picker_list == 1);
    tick(VV_PICKER_LAUNCH_MS - 1);
    assert(app.state == VV_ST_PICKER);
    tick(1);
    assert(app.state == VV_ST_IDLE && app.toast == VV_TOAST_LIST_FAILED);

    // More items than fit are ignored safely.
    press(VV_BTN_OK, VV_PRESS_LONG);
    for (int i = 0; i < VV_PICKER_MAX + 5; i++) item(0, (uint8_t)i, 40, 0, "x");
    assert(app.picker_count == VV_PICKER_MAX);
    assert(vv_app_picker_logo(&app, 10) == -1);  // index beyond the Supported Apps

    // Link loss drops a pending Jump.
    end_list(0, 40);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.picker_jumping);
    link_event(VV_LINK_DISCONNECTED, 0);
    assert(app.state == VV_ST_NO_LINK && !app.picker_jumping);
}

static void notes_state(uint8_t state, uint32_t elapsed_s, uint8_t notice) {
    const uint8_t data[] = { 0xC0, state, (uint8_t)elapsed_s, (uint8_t)(elapsed_s >> 8),
                             (uint8_t)(elapsed_s >> 16), (uint8_t)(elapsed_s >> 24), notice };
    frame(data, sizeof(data));
}

static bool toggled(void) {
    return act.frame_count == 1 && act.frames[0].data[0] == VV_MSG_NOTES_TOGGLE &&
           act.frames[0].len == 1 && act.flags == 0;
}

static void test_voice_notes(void) {
    connect_ready();
    // Not linked: a double press does nothing.
    vv_app_t saved = app;
    link_event(VV_LINK_DISCONNECTED, 0);
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(act.frame_count == 0);
    app = saved;

    // Idle: double OK toggles; the Companion answers with NOTES_STATE.
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(toggled() && app.state == VV_ST_IDLE);
    notes_state(VV_NOTES_STARTING, 0, VV_NOTICE_NONE);
    assert(vv_app_notes_active(&app) && (vv_app_take_dirty(&app) & VV_DIRTY_NOTES));
    notes_state(VV_NOTES_RECORDING, 0, VV_NOTICE_RISK_BLUETOOTH);
    assert(app.toast == VV_TOAST_NOTES_RISK_BLUETOOTH);
    // Counted locally while recording.
    tick(999);
    assert(app.notes_elapsed_s == 0);
    tick(1);
    assert(app.notes_elapsed_s == 1 && (vv_app_take_dirty(&app) & VV_DIRTY_NOTES));

    // Dictating: a double OK toggles Voice Notes and leaves the Dictation alone.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.state == VV_ST_DICTATING);
    uint8_t dict = app.dict;
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(toggled() && app.state == VV_ST_DICTATING && app.dict == dict && !app.dict_done);
    notes_state(VV_NOTES_STOPPING, 61, VV_NOTICE_NONE);
    tick(5000);
    assert(app.state == VV_ST_DICTATING && app.notes_elapsed_s == 61);   // not counting
    notes_state(VV_NOTES_IDLE, 0, VV_NOTICE_STOPPED);
    assert(!vv_app_notes_active(&app) && app.toast == VV_TOAST_NOTES_STOPPED);
    assert(app.state == VV_ST_DICTATING);
    const uint8_t partial[] = { 0x90, dict, 'o', 'k' };
    frame(partial, sizeof(partial));
    assert(strcmp(app.partial, "ok") == 0);

    // Waiting and Result: still toggles; the RESULT is unaffected.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.state == VV_ST_WAITING);
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(toggled() && app.state == VV_ST_WAITING);
    const uint8_t result[] = { 0x91, dict, 0, 'o', 'k' };
    frame(result, sizeof(result));
    assert(app.state == VV_ST_RESULT && app.result == VV_RESULT_INSERTED);
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(toggled() && app.state == VV_ST_RESULT);

    // Picker: a double OK does nothing (UP/DOWN double still move two rows).
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_PICKER);
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(act.frame_count == 0 && app.state == VV_ST_PICKER);
    press(VV_BTN_OK, VV_PRESS_LONG);

    // Notices map to toasts; unknown notices and states are safe.
    static const struct { uint8_t notice; vv_toast_t toast; } cases[] = {
        { VV_NOTICE_STARTED, VV_TOAST_NOTES_STARTED },
        { VV_NOTICE_LAUNCH_FAILED, VV_TOAST_NOTES_LAUNCH_FAILED },
        { VV_NOTICE_START_FAILED, VV_TOAST_NOTES_START_FAILED },
        { VV_NOTICE_RISK_OTHER, VV_TOAST_NOTES_RISK_OTHER },
        { VV_NOTICE_NOT_INSTALLED, VV_TOAST_NOTES_NOT_INSTALLED },
        { VV_NOTICE_RISK_VOICE_ISOLATION, VV_TOAST_NOTES_RISK_VOICE_ISOLATION },
        { VV_NOTICE_CONTROL_DISABLED, VV_TOAST_NOTES_CONTROL_DISABLED },
        { VV_NOTICE_STOP_FAILED, VV_TOAST_NOTES_STOP_FAILED },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        notes_state(VV_NOTES_IDLE, 0, cases[i].notice);
        assert(app.toast == cases[i].toast);
    }
    vv_toast_t before = app.toast;
    notes_state(9, 7, 99);
    assert(app.notes_state == VV_NOTES_IDLE && app.toast == before);

    // Link loss forgets the recording state until the next HELLO.
    notes_state(VV_NOTES_PAUSED, 3600, VV_NOTICE_NONE);
    assert(vv_app_notes_active(&app));
    link_event(VV_LINK_DISCONNECTED, 0);
    assert(!vv_app_notes_active(&app));
}

static void alert(uint8_t id, const char *label, const char *message) {
    uint8_t data[VV_FRAME_MAX] = { 0xD0, id, VV_APP_ORCA, (uint8_t)strlen(label) };
    size_t n = strlen(label), m = strlen(message);
    memcpy(&data[4], label, n);
    memcpy(&data[4 + n], message, m);
    frame(data, 4 + n + m);
}

static bool sent_alert(uint8_t type, uint8_t id) {
    return act.frame_count == 1 && act.frames[0].data[0] == type &&
           act.frames[0].data[1] == id && act.frames[0].len == 2;
}

static void test_alerts(void) {
    connect_ready();
    assert(!vv_app_alert_card(&app) && vv_app_alert_badge(&app) == 0);
    alert(1, "wt-a · fix", "Tests pass. Commit?");
    assert(app.alert_count == 1 && vv_app_alert_card(&app));
    assert(strcmp(app.alerts[0].label, "wt-a · fix") == 0);
    assert(strcmp(app.alerts[0].message, "Tests pass. Commit?") == 0);
    assert(vv_app_take_dirty(&app) & VV_DIRTY_ALERTS);
    alert(2, "wt-b · docs", "Done");
    alert(3, "wt-c · pr", "Merge?");
    // Every new Alert pops up: the card shows the newest, numbered 1/3.
    assert(app.alert_count == 3 && app.alerts[app.alert_cursor].id == 3);
    assert(vv_app_alert_position(&app) == 1);

    // DOWN goes to older ones and wraps to the newest.
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(app.alerts[app.alert_cursor].id == 2 && vv_app_alert_position(&app) == 2);
    assert(act.frame_count == 0);
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(app.alerts[app.alert_cursor].id == 3);
    press(VV_BTN_DOWN, VV_PRESS_CLICK);              // back on 2
    // A clear of another Alert keeps the shown one.
    const uint8_t clear3[] = { 0xD1, 3 };
    frame(clear3, sizeof(clear3));
    assert(app.alert_count == 2 && app.alerts[app.alert_cursor].id == 2);
    // A replaced Alert moves to the end with new text and pops up again.
    alert(3, "wt-c · pr", "Merge?");
    alert(1, "wt-a · fix", "Still waiting");
    assert(app.alert_count == 3 && app.alerts[2].id == 1 &&
           strcmp(app.alerts[2].message, "Still waiting") == 0);
    assert(app.alerts[app.alert_cursor].id == 1);

    // UP dismisses and shows the newest remaining; OK opens it.
    press(VV_BTN_UP, VV_PRESS_CLICK);
    assert(sent_alert(VV_MSG_ALERT_DISMISS, 1) && app.alert_count == 2);
    assert(app.state == VV_ST_IDLE && act.flags == 0);
    assert(app.alerts[app.alert_cursor].id == 3);    // queue [2, 3]
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(sent_alert(VV_MSG_ALERT_OPEN, 3) && act.flags == 0);
    assert(app.alert_count == 1 && app.alerts[0].id == 2 && app.state == VV_ST_IDLE);

    // Double OK on the card toggles Voice Notes, never opens the Alert.
    press(VV_BTN_OK, VV_PRESS_DOUBLE);
    assert(toggled() && app.alert_count == 1);
    // DOWN with one Alert does nothing; long OK still opens the picker.
    press(VV_BTN_DOWN, VV_PRESS_CLICK);
    assert(act.frame_count == 0 && app.alert_count == 1);
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_PICKER && !vv_app_alert_card(&app));
    assert(vv_app_alert_badge(&app) == 1);
    press(VV_BTN_OK, VV_PRESS_LONG);
    assert(app.state == VV_ST_IDLE && vv_app_alert_card(&app));

    // ALERT_CLEAR removes it; unknown ids are ignored.
    uint8_t id = app.alerts[0].id;
    const uint8_t stale[] = { 0xD1, 99 };
    frame(stale, sizeof(stale));
    assert(app.alert_count == 1);
    const uint8_t clear[] = { 0xD1, id };
    frame(clear, sizeof(clear));
    assert(app.alert_count == 0 && !vv_app_alert_card(&app));
    // With no Alert, Idle buttons are normal again.
    press(VV_BTN_UP, VV_PRESS_CLICK);
    assert(act.frames[0].data[0] == VV_MSG_UNDO);

    // Dictating: no card, OK still stops the Dictation; a badge counts.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.state == VV_ST_DICTATING);
    alert(5, "wt · x", "y");
    assert(!vv_app_alert_card(&app) && vv_app_alert_badge(&app) == 1);
    press(VV_BTN_UP, VV_PRESS_CLICK);          // UP cancels the Dictation
    assert(act.frames[0].data[0] == VV_MSG_DICT_CANCEL && app.alert_count == 1);
    assert(app.state == VV_ST_RESULT && vv_app_alert_card(&app));
    // Back in RESULT the card shows again and takes the click.
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(sent_alert(VV_MSG_ALERT_OPEN, 5) && app.state == VV_ST_IDLE);
}

static void alert_more(uint8_t id, uint16_t offset, const char *text) {
    uint8_t data[VV_FRAME_MAX] = { 0xD2, id, (uint8_t)offset, (uint8_t)(offset >> 8) };
    size_t n = strlen(text);
    memcpy(&data[4], text, n);
    frame(data, 4 + n);
}

static void test_alert_more(void) {
    connect_ready();
    alert(4, "wt · t", "Hello ");
    alert_more(4, 6, "world, ");
    alert_more(4, 13, "again.");
    assert(strcmp(app.alerts[0].message, "Hello world, again.") == 0);
    assert(app.alerts[0].msg_rx == 19);
    // Out of order, duplicate or unknown: ignored.
    alert_more(4, 6, "dup");
    alert_more(4, 40, "gap");
    alert_more(9, 19, "other");
    assert(strcmp(app.alerts[0].message, "Hello world, again.") == 0);
    // Up to ~360 bytes arrive over several frames.
    char part[121];
    memset(part, 'a', 120);
    part[120] = '\0';
    alert(5, "wt · long", part);
    alert_more(5, 120, part);
    alert_more(5, 240, part);
    assert(strlen(app.alerts[1].message) == 360 && app.alerts[1].msg_rx == 360);
    alert_more(5, 360, "zzzz");                     // would not fit: ignored
    assert(strlen(app.alerts[1].message) == 360);
    vv_msg_t m;
    const uint8_t short_more[] = { 0xD2, 1, 0 };
    assert(!vv_proto_decode(short_more, sizeof(short_more), &m));
}

static void test_alert_queue_cap_and_states(void) {
    connect_ready();
    for (uint8_t i = 1; i <= VV_ALERT_MAX + 2; i++) alert(i, "s", "m");
    assert(app.alert_count == VV_ALERT_MAX && app.alerts[0].id == 3);
    assert(app.alerts[VV_ALERT_MAX - 1].id == VV_ALERT_MAX + 2);
    // Over-long and invalid text is cut and sanitized.
    char big[VV_FRAME_MAX - 4 - 3 + 1];   // fills the frame
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    alert(50, "lbl", big);
    assert(strlen(app.alerts[VV_ALERT_MAX - 1].message) < VV_ALERT_MSG_MAX);
    const uint8_t bad[] = { 0xD0, 1, 0, 9, 'a' };   // label longer than the frame
    vv_msg_t m;
    assert(!vv_proto_decode(bad, sizeof(bad), &m));
    // Dismiss them all, then: Waiting shows no card, only the badge.
    while (app.alert_count) press(VV_BTN_UP, VV_PRESS_CLICK);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    press(VV_BTN_OK, VV_PRESS_CLICK);
    assert(app.state == VV_ST_WAITING);
    alert(60, "s", "m");
    assert(!vv_app_alert_card(&app) && vv_app_alert_badge(&app) == 1);
    press(VV_BTN_UP, VV_PRESS_CLICK);
    assert(act.frame_count == 0 && app.alert_count == 1);
    // Link loss clears the queue.
    link_event(VV_LINK_DISCONNECTED, 0);
    assert(app.alert_count == 0 && vv_app_alert_badge(&app) == 0);
}

static void test_target_logo_and_title(void) {
    vv_app_init(&app, "fw");
    assert(vv_app_target_logo(&app) == -1 && strcmp(vv_app_target_title(&app), "") == 0);
    connect_ready();
    target_state(0, VV_KIND_APP, VV_APP_CHATGPT, "ChatGPT · Fix · tests");
    assert(vv_app_target_logo(&app) == VV_APP_CHATGPT);
    assert(strcmp(vv_app_target_title(&app), "Fix · tests") == 0);
    target_state(0, VV_KIND_ORCA, VV_APP_NONE, "Orca");
    assert(vv_app_target_logo(&app) == -1 && strcmp(vv_app_target_title(&app), "Orca") == 0);
    target_state(0, VV_KIND_APP, 7, "x");
    assert(vv_app_target_logo(&app) == -1);
    assert(vv_app_take_dirty(&app) & VV_DIRTY_TARGET);
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
    const uint8_t target[] = { 0xB2, 3, 1, VV_APP_WECOM, 'Q', (uint8_t)0xFF, 'Q' };
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
    test_target_logo_and_title();
    test_voice_notes();
    test_alerts();
    test_alert_queue_cap_and_states();
    test_alert_more();
    test_status_and_target();
    puts("test_vv_app: PASS");
    return 0;
}
