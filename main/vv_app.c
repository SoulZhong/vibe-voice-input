// main/vv_app.c -- see vv_app.h.
#include "vv_app.h"

#include "vv_text.h"

#include <string.h>

static void clear_actions(vv_actions_t *out) {
    out->flags = 0;
    out->dict = 0;
    out->frame_count = 0;
}

static vv_frame_t *push_frame(vv_actions_t *out) {
    if (out->frame_count >= VV_ACT_MAX_FRAMES) return NULL;
    return &out->frames[out->frame_count++];
}

static void set_state(vv_app_t *app, vv_state_t state) {
    if (app->state != state) {
        app->state = state;
        app->dirty |= VV_DIRTY_STATE;
    }
}

static void set_toast(vv_app_t *app, vv_toast_t toast, uint32_t now_ms, uint32_t duration) {
    app->toast = toast;
    app->toast_until_ms = now_ms + duration;
    app->dirty |= VV_DIRTY_TOAST;
}

static void clear_toast(vv_app_t *app) {
    if (app->toast != VV_TOAST_NONE) {
        app->toast = VV_TOAST_NONE;
        app->dirty |= VV_DIRTY_TOAST;
    }
}

static void copy_text(char *dst, size_t size, const char *src, size_t len, bool *cut) {
    vv_utf8_sanitize_tail(src, len, dst, size, cut);
}

// HELLO is sent exactly once per link, after TX notifications are enabled.
static void send_hello(vv_app_t *app, vv_actions_t *out) {
    vv_frame_t *f = push_frame(out);
    if (f) vv_proto_hello(f, app->fw);
}

void vv_app_init(vv_app_t *app, const char *fw) {
    memset(app, 0, sizeof(*app));
    app->fw = fw ? fw : "";
    app->state = VV_ST_NO_LINK;
    app->dict_done = true;
    app->dirty = VV_DIRTY_ALL;
}

uint32_t vv_app_take_dirty(vv_app_t *app) {
    uint32_t dirty = app->dirty;
    app->dirty = 0;
    return dirty;
}

// ---------------------------------------------------------------------------
// Dictation

static void start_dictation(vv_app_t *app, uint32_t now_ms, vv_actions_t *out) {
    app->dict = (uint8_t)(app->dict + 1);
    app->dict_done = false;
    app->dict_start_ms = now_ms;
    app->elapsed_s = 0;
    app->partial[0] = '\0';
    app->partial_cut = false;
    vv_frame_t *f = push_frame(out);
    if (f) vv_proto_dict(f, VV_MSG_DICT_START, app->dict);
    out->flags |= VV_ACT_AUDIO_START;
    out->dict = app->dict;
    clear_toast(app);
    app->dirty |= VV_DIRTY_PARTIAL | VV_DIRTY_ELAPSED;
    set_state(app, VV_ST_DICTATING);
}

static void stop_dictation(vv_app_t *app, uint32_t now_ms, vv_actions_t *out) {
    out->flags |= VV_ACT_AUDIO_STOP;
    vv_frame_t *f = push_frame(out);
    if (f) vv_proto_dict(f, VV_MSG_DICT_STOP, app->dict);
    app->waiting_since_ms = now_ms;
    set_state(app, VV_ST_WAITING);
}

static void show_result(vv_app_t *app, vv_result_t result, uint32_t now_ms) {
    app->result = result;
    app->result_until_ms = now_ms + (result == VV_RESULT_INSERTED ? VV_RESULT_OK_MS
                                                                  : VV_RESULT_ERR_MS);
    app->dirty |= VV_DIRTY_STATE;
    set_state(app, VV_ST_RESULT);
}

static void cancel_dictation(vv_app_t *app, uint32_t now_ms, vv_actions_t *out) {
    out->flags |= VV_ACT_AUDIO_STOP;
    vv_frame_t *f = push_frame(out);
    if (f) vv_proto_dict(f, VV_MSG_DICT_CANCEL, app->dict);
    app->dict_done = true;  // the Companion's CANCELLED RESULT needs no display
    show_result(app, VV_RESULT_CANCELLED, now_ms);
}

static vv_result_t result_from_status(uint8_t status) {
    switch (status) {
    case VV_STATUS_OK: return VV_RESULT_INSERTED;
    case VV_STATUS_EMPTY: return VV_RESULT_EMPTY;
    case VV_STATUS_CANCELLED: return VV_RESULT_CANCELLED;
    case VV_STATUS_TARGET_UNAVAILABLE: return VV_RESULT_TARGET_DOWN;
    case VV_STATUS_RECOGNIZER_ERROR: return VV_RESULT_RECOGNIZER;
    case VV_STATUS_PERMISSION: return VV_RESULT_PERMISSION;
    default: return VV_RESULT_FAILED;
    }
}

// ---------------------------------------------------------------------------
// Picker

static void picker_request(vv_app_t *app, uint8_t list, uint32_t now_ms, vv_actions_t *out) {
    vv_frame_t *f = push_frame(out);
    if (f) vv_proto_targets_req(f, list);
    app->picker_list = list;
    app->picker_loading = true;
    app->picker_count = 0;
    app->picker_cursor = 0;
    app->picker_since_ms = now_ms;
    app->dirty |= VV_DIRTY_PICKER;
    clear_toast(app);
    set_state(app, VV_ST_PICKER);
}

static void picker_close(vv_app_t *app) {
    app->picker_loading = false;
    app->picker_count = 0;
    app->dirty |= VV_DIRTY_PICKER;
    set_state(app, VV_ST_IDLE);
}

static void picker_move(vv_app_t *app, int delta) {
    if (app->picker_loading || app->picker_count == 0) return;
    int count = app->picker_count;
    int cursor = ((int)app->picker_cursor + delta) % count;
    if (cursor < 0) cursor += count;
    app->picker_cursor = (uint8_t)cursor;
    app->dirty |= VV_DIRTY_PICKER;
}

static void picker_button(vv_app_t *app, vv_btn_t btn, vv_press_t press, uint32_t now_ms,
                          vv_actions_t *out) {
    if (btn == VV_BTN_OK && press == VV_PRESS_LONG) {
        if (app->picker_list != VV_LIST_ROOT) {
            picker_request(app, VV_LIST_ROOT, now_ms, out);
        } else {
            picker_close(app);
        }
        return;
    }
    int step = press == VV_PRESS_DOUBLE ? 2 : (press == VV_PRESS_CLICK ? 1 : 0);
    if (btn == VV_BTN_UP) {
        picker_move(app, -step);
    } else if (btn == VV_BTN_DOWN) {
        picker_move(app, step);
    } else if (btn == VV_BTN_OK && press == VV_PRESS_CLICK) {
        if (app->picker_loading || app->picker_count == 0) return;
        const vv_item_t *item = &app->picker[app->picker_cursor];
        if (item->flags & VV_ITEM_SUBLIST) {
            picker_request(app, VV_LIST_ORCA, now_ms, out);
            return;
        }
        vv_frame_t *f = push_frame(out);
        if (f) vv_proto_target_select(f, app->picker_list, item->index);
        picker_close(app);
    }
}

static void picker_item(vv_app_t *app, const vv_msg_t *msg) {
    if (app->state != VV_ST_PICKER || !app->picker_loading || msg->a != app->picker_list) return;
    if (app->picker_count >= VV_PICKER_MAX) return;
    vv_item_t *item = &app->picker[app->picker_count++];
    item->index = msg->b;
    item->flags = msg->d;
    copy_text(item->label, sizeof(item->label), msg->text, msg->text_len, NULL);
    if (item->flags & VV_ITEM_CURRENT) app->picker_cursor = (uint8_t)(app->picker_count - 1);
}

static void picker_end(vv_app_t *app, const vv_msg_t *msg) {
    if (app->state != VV_ST_PICKER || !app->picker_loading || msg->a != app->picker_list) return;
    app->picker_loading = false;
    if (app->picker_cursor >= app->picker_count) app->picker_cursor = 0;
    app->dirty |= VV_DIRTY_PICKER;
}

// ---------------------------------------------------------------------------
// Events

static void drop_link(vv_app_t *app, vv_actions_t *out) {
    if (app->state == VV_ST_DICTATING) {
        out->flags |= VV_ACT_AUDIO_STOP;   // abandon: no DICT_STOP on a dead link
    }
    app->dict_done = true;
    app->link_ready = false;
    app->picker_loading = false;
    app->picker_count = 0;
    app->passkey = 0;
    app->dirty |= VV_DIRTY_PICKER;
    set_state(app, VV_ST_NO_LINK);
}

void vv_app_link(vv_app_t *app, vv_link_ev_t ev, uint32_t passkey, uint32_t now_ms,
                 vv_actions_t *out) {
    clear_actions(out);
    switch (ev) {
    case VV_LINK_ADVERTISING:
    case VV_LINK_DISCONNECTED:
        drop_link(app, out);
        break;
    case VV_LINK_CONNECTED:
        // Also reported when the Companion disables TX notifications on a
        // live link: the next READY starts over with a new HELLO.
        drop_link(app, out);
        break;
    case VV_LINK_PASSKEY:
        app->passkey = passkey;
        app->dirty |= VV_DIRTY_STATE;
        set_state(app, VV_ST_PAIRING);
        break;
    case VV_LINK_SECURE:
        if (app->state == VV_ST_NO_LINK || app->state == VV_ST_PAIRING) {
            app->passkey = 0;
            set_state(app, VV_ST_LINKING);
        }
        break;
    case VV_LINK_PAIR_FAILED:
        set_toast(app, VV_TOAST_PAIR_FAILED, now_ms, VV_RESULT_ERR_MS);
        drop_link(app, out);
        break;
    case VV_LINK_READY:
        if (app->link_ready) break;
        app->link_ready = true;
        set_state(app, VV_ST_LINKING);
        send_hello(app, out);
        break;
    }
}

static void on_result(vv_app_t *app, const vv_msg_t *msg, uint32_t now_ms) {
    if (msg->a != app->dict || app->dict_done) return;
    // A RESULT only follows DICT_STOP. One arriving after the wait timed out
    // is dropped by dict_done above.
    if (app->state != VV_ST_WAITING && app->state != VV_ST_IDLE &&
        app->state != VV_ST_RESULT) {
        return;
    }
    app->dict_done = true;
    vv_result_t result = result_from_status(msg->b);
    if (result == VV_RESULT_INSERTED) {
        copy_text(app->segment, sizeof(app->segment), msg->text, msg->text_len,
                  &app->segment_cut);
    } else {
        app->segment[0] = '\0';
        app->segment_cut = false;
    }
    show_result(app, result, now_ms);
}

static void on_action_result(vv_app_t *app, const vv_msg_t *msg, uint32_t now_ms) {
    vv_toast_t toast;
    switch (msg->b) {
    case VV_STATUS_OK:
        toast = msg->a == VV_MSG_UNDO ? VV_TOAST_UNDONE : VV_TOAST_SUBMITTED;
        break;
    case VV_STATUS_NOTHING_TO_UNDO: toast = VV_TOAST_NOTHING_TO_UNDO; break;
    case VV_STATUS_TARGET_UNAVAILABLE: toast = VV_TOAST_TARGET_DOWN; break;
    case VV_STATUS_PERMISSION: toast = VV_TOAST_PERMISSION; break;
    default: toast = VV_TOAST_FAILED; break;
    }
    if (msg->a == VV_MSG_UNDO && msg->b == VV_STATUS_OK && app->state == VV_ST_RESULT) {
        set_state(app, VV_ST_IDLE);  // the previewed Segment is gone
    }
    if (app->state == VV_ST_IDLE || app->state == VV_ST_RESULT) {
        set_toast(app, toast, now_ms, VV_TOAST_MS);
    }
}

void vv_app_frame(vv_app_t *app, const vv_msg_t *msg, uint32_t now_ms, vv_actions_t *out) {
    clear_actions(out);
    if (!app->link_ready) return;
    switch (msg->type) {
    case VV_MSG_HELLO_ACK:
        app->version_mismatch = msg->a != VV_PROTO_VERSION;
        app->dirty |= VV_DIRTY_STATUS;
        if (app->state == VV_ST_LINKING) set_state(app, VV_ST_IDLE);
        break;
    case VV_MSG_STATUS:
        app->companion_code = msg->a;
        copy_text(app->companion_text, sizeof(app->companion_text), msg->text, msg->text_len,
                  NULL);
        app->dirty |= VV_DIRTY_STATUS;
        break;
    case VV_MSG_PARTIAL:
        if (msg->a == app->dict && !app->dict_done &&
            (app->state == VV_ST_DICTATING || app->state == VV_ST_WAITING)) {
            copy_text(app->partial, sizeof(app->partial), msg->text, msg->text_len,
                      &app->partial_cut);
            app->dirty |= VV_DIRTY_PARTIAL;
        }
        break;
    case VV_MSG_RESULT:
        on_result(app, msg, now_ms);
        break;
    case VV_MSG_ACTION_RESULT:
        on_action_result(app, msg, now_ms);
        break;
    case VV_MSG_TARGET_ITEM:
        picker_item(app, msg);
        break;
    case VV_MSG_TARGET_END:
        picker_end(app, msg);
        break;
    case VV_MSG_TARGET_STATE:
        app->target_known = true;
        app->target_status = msg->a;
        app->target_kind = msg->b;
        copy_text(app->target_label, sizeof(app->target_label), msg->text, msg->text_len, NULL);
        app->dirty |= VV_DIRTY_TARGET;
        break;
    default:
        break;
    }
}

void vv_app_button(vv_app_t *app, vv_btn_t btn, vv_press_t press, uint32_t now_ms,
                   vv_actions_t *out) {
    clear_actions(out);
    switch (app->state) {
    case VV_ST_IDLE:
    case VV_ST_RESULT:
        if (press == VV_PRESS_DOUBLE) return;  // never Submit/Undo twice by accident
        if (btn == VV_BTN_OK && press == VV_PRESS_CLICK) {
            start_dictation(app, now_ms, out);
        } else if (btn == VV_BTN_OK && press == VV_PRESS_LONG) {
            picker_request(app, VV_LIST_ROOT, now_ms, out);
        } else if (btn == VV_BTN_DOWN && press == VV_PRESS_CLICK) {
            vv_frame_t *f = push_frame(out);
            if (f) vv_proto_simple(f, VV_MSG_SUBMIT);
            if (app->state == VV_ST_RESULT) set_state(app, VV_ST_IDLE);
            set_toast(app, VV_TOAST_SUBMITTING, now_ms, VV_PENDING_TOAST_MS);
        } else if (btn == VV_BTN_UP && press == VV_PRESS_CLICK) {
            vv_frame_t *f = push_frame(out);
            if (f) vv_proto_simple(f, VV_MSG_UNDO);
            set_toast(app, VV_TOAST_UNDOING, now_ms, VV_PENDING_TOAST_MS);
        }
        break;
    case VV_ST_DICTATING:
        if (btn == VV_BTN_OK && (press == VV_PRESS_CLICK || press == VV_PRESS_LONG)) {
            stop_dictation(app, now_ms, out);
        } else if (btn == VV_BTN_UP && press == VV_PRESS_CLICK) {
            cancel_dictation(app, now_ms, out);
        }
        break;
    case VV_ST_PICKER:
        picker_button(app, btn, press, now_ms, out);
        break;
    default:
        break;  // no link / pairing / linking / waiting: nothing to do
    }
}

void vv_app_tick(vv_app_t *app, uint32_t now_ms, vv_actions_t *out) {
    clear_actions(out);
    if (app->toast != VV_TOAST_NONE && vv_time_reached(now_ms, app->toast_until_ms)) {
        clear_toast(app);
    }
    switch (app->state) {
    case VV_ST_DICTATING: {
        uint32_t elapsed_ms = now_ms - app->dict_start_ms;
        uint32_t seconds = elapsed_ms / 1000;
        if (seconds != app->elapsed_s) {
            app->elapsed_s = seconds;
            app->dirty |= VV_DIRTY_ELAPSED;
        }
        if (elapsed_ms >= VV_DICTATION_LIMIT_MS) stop_dictation(app, now_ms, out);
        break;
    }
    case VV_ST_WAITING:
        if (vv_time_reached(now_ms, app->waiting_since_ms + VV_WAIT_RESULT_MS)) {
            app->dict_done = true;
            app->segment[0] = '\0';
            show_result(app, VV_RESULT_TIMEOUT, now_ms);
        }
        break;
    case VV_ST_RESULT:
        if (vv_time_reached(now_ms, app->result_until_ms)) set_state(app, VV_ST_IDLE);
        break;
    case VV_ST_PICKER:
        if (app->picker_loading &&
            vv_time_reached(now_ms, app->picker_since_ms + VV_PICKER_TIMEOUT_MS)) {
            picker_close(app);
            set_toast(app, VV_TOAST_LIST_FAILED, now_ms, VV_RESULT_ERR_MS);
        }
        break;
    default:
        break;
    }
}
