// main/vv_proto.h -- Vibe Voice BLE protocol v2 frames (docs/vibe-voice/protocol.md).
// Pure C: encoders for Device -> Companion frames, a decoder for Companion ->
// Device frames. One frame per GATT write/notification, at most 180 bytes.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VV_PROTO_VERSION     2
#define VV_FRAME_MAX         180
#define VV_AUDIO_SAMPLES     320   // 20 ms at 16 kHz
#define VV_AUDIO_ADPCM_BYTES 160
#define VV_AUDIO_FRAME_LEN   (1 + 1 + 2 + 2 + 1 + VV_AUDIO_ADPCM_BYTES)

// Device -> Companion
enum {
    VV_MSG_HELLO = 0x01,
    VV_MSG_DICT_START = 0x10,
    VV_MSG_AUDIO = 0x11,
    VV_MSG_DICT_STOP = 0x12,
    VV_MSG_DICT_CANCEL = 0x13,
    VV_MSG_SUBMIT = 0x20,
    VV_MSG_UNDO = 0x21,
    VV_MSG_TARGETS_REQ = 0x30,
    VV_MSG_TARGET_SELECT = 0x31,
    VV_MSG_NOTES_TOGGLE = 0x40,
    VV_MSG_ALERT_OPEN = 0x50,
    VV_MSG_ALERT_DISMISS = 0x51,
};

// Companion -> Device
enum {
    VV_MSG_HELLO_ACK = 0x81,
    VV_MSG_STATUS = 0x82,
    VV_MSG_PARTIAL = 0x90,
    VV_MSG_RESULT = 0x91,
    VV_MSG_ACTION_RESULT = 0xA0,
    VV_MSG_TARGET_ITEM = 0xB0,
    VV_MSG_TARGET_END = 0xB1,
    VV_MSG_TARGET_STATE = 0xB2,
    VV_MSG_NOTES_STATE = 0xC0,
    VV_MSG_ALERT = 0xD0,
    VV_MSG_ALERT_CLEAR = 0xD1,
};

// NOTES_STATE state: the Voice Notes Recording on the Mac.
enum {
    VV_NOTES_IDLE = 0,
    VV_NOTES_RECORDING = 1,
    VV_NOTES_PAUSED = 2,
    VV_NOTES_STARTING = 3,
    VV_NOTES_STOPPING = 4,
};

// NOTES_STATE notice: a one-off event shown as a toast.
enum {
    VV_NOTICE_NONE = 0,
    VV_NOTICE_STARTED = 1,
    VV_NOTICE_STOPPED = 2,
    VV_NOTICE_LAUNCH_FAILED = 3,
    VV_NOTICE_START_FAILED = 4,
    VV_NOTICE_RISK_BLUETOOTH = 5,
    VV_NOTICE_RISK_OTHER = 6,
    VV_NOTICE_NOT_INSTALLED = 7,
    VV_NOTICE_RISK_VOICE_ISOLATION = 8,
    VV_NOTICE_CONTROL_DISABLED = 9,
    VV_NOTICE_STOP_FAILED = 10,
};

// RESULT / ACTION_RESULT / TARGET_STATE status codes.
enum {
    VV_STATUS_OK = 0,
    VV_STATUS_EMPTY = 1,
    VV_STATUS_CANCELLED = 2,
    VV_STATUS_TARGET_UNAVAILABLE = 3,
    VV_STATUS_RECOGNIZER_ERROR = 4,
    VV_STATUS_PERMISSION = 5,
    VV_STATUS_NOTHING_TO_UNDO = 6,
};

// STATUS codes.
enum {
    VV_COMPANION_OK = 0,
    VV_COMPANION_NO_SPEECH_PERMISSION = 1,
    VV_COMPANION_NO_AX_PERMISSION = 2,
    VV_COMPANION_NO_ORCA = 3,
    VV_COMPANION_NO_ZH_RECOGNIZER = 4,
};

// TARGET_ITEM flags. CURRENT: in the root list the app the Target is in, in
// a sub-list the app's Current Conversation.
#define VV_ITEM_CURRENT     0x01
#define VV_ITEM_SUBLIST     0x02
#define VV_ITEM_NOT_RUNNING 0x04

// TARGET_STATE kind (0 is reserved).
enum { VV_KIND_NONE = 0, VV_KIND_APP = 1, VV_KIND_ORCA = 2 };

// TARGET_STATE app: Supported App index (root row order), or none.
enum { VV_APP_ORCA = 0, VV_APP_WECHAT = 1, VV_APP_CHATGPT = 2, VV_APP_WECOM = 3, VV_APP_COUNT = 4 };
#define VV_APP_NONE 0xFF

// TARGETS_REQ list ids: 0 = the Supported Apps, n = conversations of root
// row n - 1 (1 Orca, 2 WeChat, 3 ChatGPT, 4 WeCom).
enum { VV_LIST_ROOT = 0, VV_LIST_ORCA = 1, VV_LIST_LAST = 4 };

typedef struct {
    uint8_t len;
    uint8_t data[VV_FRAME_MAX];
} vv_frame_t;

// Encoders return the frame length (0 if it does not fit). Text is cut to its
// tail on a UTF-8 boundary when longer than the frame allows.
size_t vv_proto_hello(vv_frame_t *f, const char *fw);
size_t vv_proto_dict(vv_frame_t *f, uint8_t type, uint8_t dict); // START/STOP/CANCEL
size_t vv_proto_audio(vv_frame_t *f, uint8_t dict, uint16_t seq, int16_t pred,
                      uint8_t index, const uint8_t adpcm[VV_AUDIO_ADPCM_BYTES]);
size_t vv_proto_simple(vv_frame_t *f, uint8_t type);              // SUBMIT/UNDO/NOTES_TOGGLE
size_t vv_proto_targets_req(vv_frame_t *f, uint8_t list);
size_t vv_proto_target_select(vv_frame_t *f, uint8_t list, uint8_t index);
size_t vv_proto_alert_id(vv_frame_t *f, uint8_t type, uint8_t id);  // ALERT_OPEN/DISMISS

// A decoded Companion frame. `text` points into the input buffer (not
// terminated); `text_len` may be 0.
typedef struct {
    uint8_t type;
    uint8_t a;       // ver / code / dict / action / list / status / NOTES state
    uint8_t b;       // RESULT status / ACTION status / ITEM index / END count / STATE kind
    uint8_t c;       // ITEM count / STATE app / NOTES notice
    uint8_t d;       // ITEM flags
    uint32_t u32;    // NOTES elapsed seconds
    const char *text;   // ALERT: the label
    size_t text_len;
    const char *text2;  // ALERT: the message
    size_t text2_len;
} vv_msg_t;

// Returns false for an unknown type or a payload shorter than its fixed part.
bool vv_proto_decode(const uint8_t *data, size_t len, vv_msg_t *msg);
