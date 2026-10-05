// main/vv_proto.c -- see vv_proto.h.
#include "vv_proto.h"

#include "vv_text.h"

#include <string.h>

size_t vv_proto_hello(vv_frame_t *f, const char *fw) {
    size_t fw_len = fw ? strlen(fw) : 0;
    size_t start = vv_utf8_tail_start(fw, fw_len, VV_FRAME_MAX - 2);
    f->data[0] = VV_MSG_HELLO;
    f->data[1] = VV_PROTO_VERSION;
    memcpy(&f->data[2], fw + start, fw_len - start);
    f->len = (uint8_t)(2 + fw_len - start);
    return f->len;
}

size_t vv_proto_dict(vv_frame_t *f, uint8_t type, uint8_t dict) {
    if (type != VV_MSG_DICT_START && type != VV_MSG_DICT_STOP && type != VV_MSG_DICT_CANCEL) {
        return f->len = 0;
    }
    f->data[0] = type;
    f->data[1] = dict;
    return f->len = 2;
}

size_t vv_proto_audio(vv_frame_t *f, uint8_t dict, uint16_t seq, int16_t pred,
                      uint8_t index, const uint8_t adpcm[VV_AUDIO_ADPCM_BYTES]) {
    uint16_t p = (uint16_t)pred;
    f->data[0] = VV_MSG_AUDIO;
    f->data[1] = dict;
    f->data[2] = (uint8_t)(seq & 0xFF);
    f->data[3] = (uint8_t)(seq >> 8);
    f->data[4] = (uint8_t)(p & 0xFF);
    f->data[5] = (uint8_t)(p >> 8);
    f->data[6] = index;
    memcpy(&f->data[7], adpcm, VV_AUDIO_ADPCM_BYTES);
    return f->len = VV_AUDIO_FRAME_LEN;
}

size_t vv_proto_simple(vv_frame_t *f, uint8_t type) {
    if (type != VV_MSG_SUBMIT && type != VV_MSG_UNDO && type != VV_MSG_NOTES_TOGGLE) {
        return f->len = 0;
    }
    f->data[0] = type;
    return f->len = 1;
}

size_t vv_proto_alert_id(vv_frame_t *f, uint8_t type, uint8_t id) {
    if (type != VV_MSG_ALERT_OPEN && type != VV_MSG_ALERT_DISMISS) return f->len = 0;
    f->data[0] = type;
    f->data[1] = id;
    return f->len = 2;
}

size_t vv_proto_targets_req(vv_frame_t *f, uint8_t list) {
    f->data[0] = VV_MSG_TARGETS_REQ;
    f->data[1] = list;
    return f->len = 2;
}

size_t vv_proto_target_select(vv_frame_t *f, uint8_t list, uint8_t index) {
    f->data[0] = VV_MSG_TARGET_SELECT;
    f->data[1] = list;
    f->data[2] = index;
    return f->len = 3;
}

static bool with_text(const uint8_t *data, size_t len, size_t fixed, vv_msg_t *msg) {
    if (len < fixed) return false;
    msg->text = (const char *)data + fixed;
    msg->text_len = len - fixed;
    return true;
}

bool vv_proto_decode(const uint8_t *data, size_t len, vv_msg_t *msg) {
    memset(msg, 0, sizeof(*msg));
    if (!data || len == 0 || len > VV_FRAME_MAX) return false;
    msg->type = data[0];
    msg->text = "";
    msg->text2 = "";
    switch (msg->type) {
    case VV_MSG_HELLO_ACK:
        if (len < 2) return false;
        msg->a = data[1];
        return true;
    case VV_MSG_STATUS:
    case VV_MSG_PARTIAL:
        if (len < 2) return false;
        msg->a = data[1];
        return with_text(data, len, 2, msg);
    case VV_MSG_RESULT:
        if (len < 3) return false;
        msg->a = data[1];
        msg->b = data[2];
        return with_text(data, len, 3, msg);
    case VV_MSG_ACTION_RESULT:
    case VV_MSG_TARGET_END:
        if (len < 3) return false;
        msg->a = data[1];
        msg->b = data[2];
        return true;
    case VV_MSG_TARGET_ITEM:
        if (len < 5) return false;
        msg->a = data[1];
        msg->b = data[2];
        msg->c = data[3];
        msg->d = data[4];
        return with_text(data, len, 5, msg);
    case VV_MSG_ALERT:
        // id, app, label_len, label, message (rest of the frame)
        if (len < 4 || (size_t)4 + data[3] > len) return false;
        msg->a = data[1];
        msg->b = data[2];
        msg->text = (const char *)data + 4;
        msg->text_len = data[3];
        msg->text2 = (const char *)data + 4 + data[3];
        msg->text2_len = len - 4 - data[3];
        return true;
    case VV_MSG_ALERT_MORE:
        // id, offset u16, text (rest)
        if (len < 4) return false;
        msg->a = data[1];
        msg->u32 = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
        return with_text(data, len, 4, msg);
    case VV_MSG_ALERT_CLEAR:
        if (len < 2) return false;
        msg->a = data[1];
        return true;
    case VV_MSG_NOTES_STATE:
        if (len < 7) return false;
        msg->a = data[1];
        msg->u32 = (uint32_t)data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) |
                   ((uint32_t)data[5] << 24);
        msg->c = data[6];
        return true;
    case VV_MSG_TARGET_STATE:
        if (len < 4) return false;
        msg->a = data[1];
        msg->b = data[2];
        msg->c = data[3];
        return with_text(data, len, 4, msg);
    default:
        return false;
    }
}
