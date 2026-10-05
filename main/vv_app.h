// main/vv_app.h -- Vibe Voice Device state machine (pure C, host tested).
//
// The controller task feeds it button presses, link events, decoded Companion
// frames and periodic ticks. It answers with actions (frames to send, audio
// start/stop) and dirty flags for the UI. It never touches ESP-IDF or LVGL.
//
// States (docs/vibe-voice/firmware.md):
//   NO_LINK   advertising, nobody connected (or connected, not yet secure)
//   PAIRING   showing the 6-digit passkey
//   LINKING   secure link, HELLO sent, waiting for HELLO_ACK
//   IDLE      ready: OK = Dictation, DOWN = Submit, UP = Undo, OK long = picker
//   DICTATING audio streaming, Partial Text shown
//   WAITING   DICT_STOP sent, waiting for RESULT
//   RESULT    transient outcome (Segment preview or error); buttons act as IDLE
//   PICKER    Target list (root list or Orca Sessions)
#pragma once

#include "vv_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VV_TARGET_LABEL_MAX 128
#define VV_TEXT_MAX         192   // a frame carries at most 177 text bytes
#define VV_ITEM_LABEL_MAX   72
#define VV_PICKER_MAX       24
#define VV_STATUS_TEXT_MAX  96

#define VV_DICTATION_LIMIT_MS   (5u * 60u * 1000u)
#define VV_WAIT_RESULT_MS       20000u
#define VV_PICKER_TIMEOUT_MS    5000u
#define VV_TOAST_MS             2500u
#define VV_PENDING_TOAST_MS     5000u
#define VV_RESULT_OK_MS         6000u
#define VV_RESULT_ERR_MS        4000u

typedef enum {
    VV_ST_NO_LINK = 0,
    VV_ST_PAIRING,
    VV_ST_LINKING,
    VV_ST_IDLE,
    VV_ST_DICTATING,
    VV_ST_WAITING,
    VV_ST_RESULT,
    VV_ST_PICKER,
} vv_state_t;

typedef enum { VV_BTN_UP = 0, VV_BTN_DOWN, VV_BTN_OK } vv_btn_t;
typedef enum { VV_PRESS_CLICK = 0, VV_PRESS_DOUBLE, VV_PRESS_LONG } vv_press_t;

typedef enum {
    VV_LINK_ADVERTISING = 0,  // stack up, nobody connected
    VV_LINK_CONNECTED,        // a central connected, link not ready (yet / any more)
    VV_LINK_PASSKEY,          // pairing started: show the passkey
    VV_LINK_SECURE,           // encrypted + authenticated + bonded
    VV_LINK_PAIR_FAILED,
    VV_LINK_READY,            // secure and TX notifications enabled
    VV_LINK_DISCONNECTED,
} vv_link_ev_t;

typedef enum {
    VV_RESULT_INSERTED = 0,
    VV_RESULT_EMPTY,
    VV_RESULT_CANCELLED,
    VV_RESULT_TARGET_DOWN,
    VV_RESULT_RECOGNIZER,
    VV_RESULT_PERMISSION,
    VV_RESULT_TIMEOUT,
    VV_RESULT_FAILED,
} vv_result_t;

typedef enum {
    VV_TOAST_NONE = 0,
    VV_TOAST_SUBMITTING,
    VV_TOAST_SUBMITTED,
    VV_TOAST_UNDOING,
    VV_TOAST_UNDONE,
    VV_TOAST_NOTHING_TO_UNDO,
    VV_TOAST_TARGET_DOWN,
    VV_TOAST_PERMISSION,
    VV_TOAST_FAILED,
    VV_TOAST_PAIR_FAILED,
    VV_TOAST_LIST_FAILED,
} vv_toast_t;

// Dirty flags for the UI.
#define VV_DIRTY_STATE   0x01u
#define VV_DIRTY_TARGET  0x02u
#define VV_DIRTY_PARTIAL 0x04u
#define VV_DIRTY_ELAPSED 0x08u
#define VV_DIRTY_PICKER  0x10u
#define VV_DIRTY_TOAST   0x20u
#define VV_DIRTY_STATUS  0x40u
#define VV_DIRTY_ALL     0x7Fu

// Actions. The controller executes them in this order:
//   1. AUDIO_STOP (capture stopped, every AUDIO frame queued)
//   2. frames[] in order
//   3. AUDIO_START with `dict`
// so DICT_START precedes the first AUDIO frame and DICT_STOP/CANCEL follow
// the last one. AUDIO_STOP without a frame abandons the Dictation.
#define VV_ACT_AUDIO_START 0x01u
#define VV_ACT_AUDIO_STOP  0x02u
#define VV_ACT_MAX_FRAMES  2

typedef struct {
    uint32_t flags;
    uint8_t dict;
    uint8_t frame_count;
    vv_frame_t frames[VV_ACT_MAX_FRAMES];
} vv_actions_t;

typedef struct {
    uint8_t index;   // Companion's index, sent back in TARGET_SELECT
    uint8_t flags;   // VV_ITEM_*
    char label[VV_ITEM_LABEL_MAX];
} vv_item_t;

typedef struct {
    vv_state_t state;
    const char *fw;
    bool link_ready;
    uint32_t passkey;
    bool version_mismatch;

    bool target_known;
    uint8_t target_status;
    uint8_t target_kind;
    char target_label[VV_TARGET_LABEL_MAX];

    uint8_t companion_code;
    char companion_text[VV_STATUS_TEXT_MAX];

    uint8_t dict;
    bool dict_done;          // RESULT for `dict` handled (or cancelled locally)
    uint32_t dict_start_ms;
    uint32_t elapsed_s;
    uint32_t waiting_since_ms;
    char partial[VV_TEXT_MAX];
    bool partial_cut;

    vv_result_t result;
    char segment[VV_TEXT_MAX];
    bool segment_cut;
    uint32_t result_until_ms;

    vv_toast_t toast;
    uint32_t toast_until_ms;

    uint8_t picker_list;
    bool picker_loading;
    uint8_t picker_count;
    uint8_t picker_cursor;
    uint32_t picker_since_ms;
    vv_item_t picker[VV_PICKER_MAX];

    uint32_t dirty;
} vv_app_t;

void vv_app_init(vv_app_t *app, const char *fw);

void vv_app_link(vv_app_t *app, vv_link_ev_t ev, uint32_t passkey, uint32_t now_ms,
                 vv_actions_t *out);
void vv_app_frame(vv_app_t *app, const vv_msg_t *msg, uint32_t now_ms, vv_actions_t *out);
void vv_app_button(vv_app_t *app, vv_btn_t btn, vv_press_t press, uint32_t now_ms,
                   vv_actions_t *out);
void vv_app_tick(vv_app_t *app, uint32_t now_ms, vv_actions_t *out);

// Returns and clears the accumulated dirty flags.
uint32_t vv_app_take_dirty(vv_app_t *app);

// Wrap-safe "a is at or after b" for millisecond timestamps.
static inline bool vv_time_reached(uint32_t now_ms, uint32_t deadline_ms) {
    return (int32_t)(now_ms - deadline_ms) >= 0;
}
