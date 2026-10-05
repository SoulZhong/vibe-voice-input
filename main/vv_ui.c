// main/vv_ui.c -- Vibe Voice "voice remote" screen. See vv_ui.h.
//
// Layout (240x320, the BSP masks the corners with a 30 px radius):
//   y  10..36   top bar: 20 px Target app logo and Target pill (left),
//               battery percentage and gauge (right)
//   y  44..262  page for the current state (centre / dictation / result / picker)
//   y 268..308  control hints, or a toast that temporarily replaces them
#include "vv_ui.h"

#include "vv_icons.h"
#include "vv_strings.h"
#include "vv_text.h"

#include "lvgl.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(vv_font_body_16);
LV_FONT_DECLARE(vv_font_title_24);
LV_FONT_DECLARE(vv_font_digits_48);

#define FONT_BODY   (&vv_font_body_16)
#define FONT_TITLE  (&vv_font_title_24)
#define FONT_DIGITS (&vv_font_digits_48)

// Palette: calm ink background, warm white text, mint for "ready",
// coral for "recording", amber for warnings, soft blue for pairing.
#define C_BG       0x0E1116
#define C_SURFACE  0x1A1F27
#define C_RAISED   0x252C36
#define C_LINE     0x2C343F
#define C_TEXT     0xF2EEE6
#define C_MUTED    0x8B95A3
#define C_DIM      0x4A5361
#define C_MINT     0x4FD1B5
#define C_CORAL    0xFF5F57
#define C_AMBER    0xF4B54D
#define C_BLUE     0x8AB4FF
#define C_ON_MINT  0x08201B

#define SCREEN_W   240
#define SIDE       24
#define CONTENT_W  (SCREEN_W - 2 * SIDE)

#define METER_BARS  16
#define PICKER_ROWS 5
#define ROW_H       34

static const vv_wrap_t PARTIAL_WRAP = { .line_px = CONTENT_W, .max_lines = 6,
                                        .narrow_px = 9, .wide_px = 16 };
static const vv_wrap_t SEGMENT_WRAP = { .line_px = CONTENT_W - 24, .max_lines = 4,
                                        .narrow_px = 9, .wide_px = 16 };
static const vv_wrap_t WAITING_WRAP = { .line_px = CONTENT_W, .max_lines = 2,
                                        .narrow_px = 9, .wide_px = 16 };

static struct {
    lv_obj_t *scr;
    // Top bar
    lv_obj_t *pill, *pill_dot, *pill_label, *pill_logo;
    lv_obj_t *batt_label, *batt_body, *batt_fill, *batt_nub;
    // Centre page: rings + title + body, or passkey; in IDLE the Target app
    // logo and conversation title replace the rings
    lv_obj_t *center, *ring_outer, *ring_mid, *core, *spinner, *title, *body, *passkey;
    lv_obj_t *logo, *conv;
    // Dictation page
    lv_obj_t *dict, *rec_dot, *dict_title, *elapsed, *bars[METER_BARS], *partial;
    // Result page
    lv_obj_t *result, *result_dot, *result_title, *card, *card_bar, *segment, *result_body;
    // Picker page
    lv_obj_t *picker, *picker_title, *picker_pos, *picker_msg;
    lv_obj_t *rows[PICKER_ROWS], *row_label[PICKER_ROWS], *row_tag[PICKER_ROWS];
    lv_obj_t *row_logo[PICKER_ROWS];
    // Alert card (over the page) and the top-bar count badge
    lv_obj_t *alert, *alert_logo, *alert_label, *alert_pos, *alert_msg;
    lv_obj_t *badge, *badge_label;
    // Bottom
    lv_obj_t *hint1, *hint2, *toast, *toast_label;

    char device_line[48];
    uint8_t levels[METER_BARS];
} ui;

// ---------------------------------------------------------------------------
// Widget helpers

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static lv_obj_t *page(void) {
    lv_obj_t *obj = lv_obj_create(ui.scr);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_size(obj, SCREEN_W, 268);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

static lv_obj_t *ring(lv_obj_t *parent, int cx, int cy, int d, int width, uint32_t color) {
    lv_obj_t *obj = box(parent, cx - d / 2, cy - d / 2, d, d, C_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, width, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x, int y,
                      int w, lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *image(lv_obj_t *parent, int x, int y) {
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_set_pos(img, x, y);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

static void show(lv_obj_t *obj, bool visible) {
    if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_color(lv_obj_t *label, uint32_t color) {
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static void set_bg(lv_obj_t *obj, uint32_t color) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
}

// ---------------------------------------------------------------------------
// Construction

static void build_top_bar(void) {
    ui.pill = box(ui.scr, 20, 10, 124, 26, C_SURFACE, 13);
    lv_obj_set_style_border_color(ui.pill, lv_color_hex(C_AMBER), 0);
    ui.pill_dot = box(ui.pill, 10, 9, 8, 8, C_DIM, LV_RADIUS_CIRCLE);
    ui.pill_label = text(ui.pill, FONT_BODY, C_TEXT, 24, 2, 92, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_height(ui.pill_label, 22);
    lv_label_set_long_mode(ui.pill_label, LV_LABEL_LONG_MODE_DOTS);

    ui.pill_logo = image(ui.scr, 18, 13);

    ui.batt_label = text(ui.scr, FONT_BODY, C_MUTED, 146, 12, 50, LV_TEXT_ALIGN_RIGHT);
    lv_label_set_long_mode(ui.batt_label, LV_LABEL_LONG_MODE_CLIP);
    ui.batt_body = box(ui.scr, 199, 17, 18, 11, C_BG, 3);
    lv_obj_set_style_border_width(ui.batt_body, 1, 0);
    lv_obj_set_style_border_color(ui.batt_body, lv_color_hex(C_MUTED), 0);
    ui.batt_fill = box(ui.batt_body, 2, 2, 12, 5, C_TEXT, 1);
    ui.batt_nub = box(ui.scr, 217, 20, 2, 5, C_MUTED, 1);
}

static void build_center(void) {
    ui.center = page();
    const int cx = SCREEN_W / 2, cy = 114;
    ui.ring_outer = ring(ui.center, cx, cy, 120, 1, C_LINE);
    ui.ring_mid = ring(ui.center, cx, cy, 90, 2, C_MINT);
    ui.core = box(ui.center, cx - 22, cy - 22, 44, 44, C_MINT, LV_RADIUS_CIRCLE);

    ui.spinner = lv_spinner_create(ui.center);
    lv_obj_set_size(ui.spinner, 100, 100);
    lv_obj_set_pos(ui.spinner, cx - 50, cy - 50);
    lv_spinner_set_anim_params(ui.spinner, 1400, 90);
    lv_obj_set_style_arc_width(ui.spinner, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.spinner, lv_color_hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.spinner, lv_color_hex(C_MINT), LV_PART_INDICATOR);
    show(ui.spinner, false);

    ui.logo = image(ui.center, cx - 48, cy - 48);
    ui.conv = text(ui.center, FONT_BODY, C_TEXT, SIDE, 176, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(ui.conv, 22);
    lv_label_set_long_mode(ui.conv, LV_LABEL_LONG_MODE_DOTS);
    show(ui.conv, false);

    ui.title = text(ui.center, FONT_TITLE, C_TEXT, SIDE, 182, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    ui.body = text(ui.center, FONT_BODY, C_MUTED, SIDE, 216, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(ui.body, 2 * 21);
    ui.passkey = text(ui.center, FONT_DIGITS, C_TEXT, 0, 104, SCREEN_W, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_style_text_letter_space(ui.passkey, 2, 0);
}

static void build_dictation(void) {
    ui.dict = page();
    ui.rec_dot = box(ui.dict, SIDE, 56, 12, 12, C_CORAL, LV_RADIUS_CIRCLE);
    ui.dict_title = text(ui.dict, FONT_TITLE, C_TEXT, SIDE + 20, 46, 110, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(ui.dict_title, VV_H_DICTATING);
    ui.elapsed = text(ui.dict, FONT_TITLE, C_CORAL, 136, 46, 80, LV_TEXT_ALIGN_RIGHT);
    for (int i = 0; i < METER_BARS; i++) {
        ui.bars[i] = box(ui.dict, SIDE + i * 12, 96, 6, 4, C_CORAL, 3);
    }
    ui.partial = text(ui.dict, FONT_BODY, C_TEXT, SIDE, 126, CONTENT_W, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_height(ui.partial, PARTIAL_WRAP.max_lines * 21);
    lv_obj_set_style_text_line_space(ui.partial, 0, 0);
}

static void build_result(void) {
    ui.result = page();
    ui.result_dot = box(ui.result, SIDE, 56, 12, 12, C_MINT, LV_RADIUS_CIRCLE);
    ui.result_title = text(ui.result, FONT_TITLE, C_TEXT, SIDE + 20, 46, CONTENT_W - 20,
                           LV_TEXT_ALIGN_LEFT);
    ui.card = box(ui.result, 20, 92, SCREEN_W - 40, 116, C_SURFACE, 14);
    ui.card_bar = box(ui.card, 0, 14, 4, 88, C_MINT, 2);
    ui.segment = text(ui.card, FONT_BODY, C_TEXT, 16, 12, CONTENT_W - 24, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_height(ui.segment, SEGMENT_WRAP.max_lines * 21);
    ui.result_body = text(ui.result, FONT_BODY, C_MUTED, SIDE, 96, CONTENT_W, LV_TEXT_ALIGN_LEFT);
}

static void build_picker(void) {
    ui.picker = page();
    ui.picker_title = text(ui.picker, FONT_TITLE, C_TEXT, SIDE, 46, 140, LV_TEXT_ALIGN_LEFT);
    ui.picker_pos = text(ui.picker, FONT_BODY, C_MUTED, 156, 52, 60, LV_TEXT_ALIGN_RIGHT);
    ui.picker_msg = text(ui.picker, FONT_BODY, C_MUTED, SIDE, 140, CONTENT_W, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < PICKER_ROWS; i++) {
        ui.rows[i] = box(ui.picker, 16, 84 + i * ROW_H, SCREEN_W - 32, ROW_H - 4, C_BG, 10);
        ui.row_logo[i] = image(ui.rows[i], 8, 5);
        ui.row_label[i] = text(ui.rows[i], FONT_BODY, C_TEXT, 12, 5, 132, LV_TEXT_ALIGN_LEFT);
        lv_obj_set_height(ui.row_label[i], 22);
        lv_label_set_long_mode(ui.row_label[i], LV_LABEL_LONG_MODE_DOTS);
        ui.row_tag[i] = text(ui.rows[i], FONT_BODY, C_MUTED, 146, 5, 52, LV_TEXT_ALIGN_RIGHT);
    }
}

static void build_alert(void) {
    // Card: logo + "提醒 n/N" (amber), the session, then the agent's words.
    ui.alert = box(ui.scr, 16, 44, SCREEN_W - 32, 220, C_SURFACE, 14);
    lv_obj_set_style_border_width(ui.alert, 1, 0);
    lv_obj_set_style_border_color(ui.alert, lv_color_hex(C_AMBER), 0);
    ui.alert_logo = image(ui.alert, 12, 10);
    ui.alert_pos = text(ui.alert, FONT_BODY, C_AMBER, 40, 9, 150, LV_TEXT_ALIGN_LEFT);
    ui.alert_label = text(ui.alert, FONT_BODY, C_MUTED, 12, 36, SCREEN_W - 56, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_height(ui.alert_label, 22);
    lv_label_set_long_mode(ui.alert_label, LV_LABEL_LONG_MODE_DOTS);
    ui.alert_msg = text(ui.alert, FONT_BODY, C_TEXT, 12, 62, SCREEN_W - 56, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_height(ui.alert_msg, 7 * 21);   // as many lines as fit
    lv_label_set_long_mode(ui.alert_msg, LV_LABEL_LONG_MODE_DOTS);
    show(ui.alert, false);

    // Badge on the top-left logo's corner.
    ui.badge = box(ui.scr, 30, 4, 16, 16, C_AMBER, LV_RADIUS_CIRCLE);
    ui.badge_label = text(ui.badge, FONT_BODY, C_ON_MINT, 0, -3, 16, LV_TEXT_ALIGN_CENTER);
    show(ui.badge, false);
}

static void render_alerts(const vv_app_t *app) {
    bool card = vv_app_alert_card(app);
    show(ui.alert, card);
    if (card) {
        const vv_alert_t *a = &app->alerts[app->alert_cursor];
        bool logo = a->app < VV_APP_COUNT;
        if (logo) lv_image_set_src(ui.alert_logo, vv_icons_20[a->app]);
        show(ui.alert_logo, logo);
        if (app->alert_count > 1) {
            lv_label_set_text_fmt(ui.alert_pos, VV_T_ALERT_TITLE " %u/%u",
                                  (unsigned)vv_app_alert_position(app),
                                  (unsigned)app->alert_count);
        } else {
            lv_label_set_text(ui.alert_pos, VV_T_ALERT_TITLE);
        }
        lv_label_set_text(ui.alert_label, a->label);
        lv_label_set_text(ui.alert_msg, a->message);
    }
    uint8_t badge = vv_app_alert_badge(app);
    show(ui.badge, badge > 0);
    if (badge) lv_label_set_text_fmt(ui.badge_label, "%u", (unsigned)badge);
}

static void build_bottom(void) {
    ui.hint1 = text(ui.scr, FONT_BODY, C_MUTED, 16, 266, SCREEN_W - 32, LV_TEXT_ALIGN_CENTER);
    ui.hint2 = text(ui.scr, FONT_BODY, C_MUTED, 16, 287, SCREEN_W - 32, LV_TEXT_ALIGN_CENTER);
    ui.toast = box(ui.scr, 28, 270, SCREEN_W - 56, 32, C_RAISED, 16);
    ui.toast_label = text(ui.toast, FONT_BODY, C_TEXT, 8, 5, SCREEN_W - 72, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(ui.toast_label, 22);
    lv_label_set_long_mode(ui.toast_label, LV_LABEL_LONG_MODE_DOTS);
    show(ui.toast, false);
}

void vv_ui_init(const char *device_name) {
    memset(&ui, 0, sizeof(ui));
    snprintf(ui.device_line, sizeof(ui.device_line), VV_T_DEVICE_NAME " %s",
             device_name ? device_name : "");

    ui.scr = lv_obj_create(NULL);
    lv_obj_remove_flag(ui.scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui.scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ui.scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(ui.scr, FONT_BODY, 0);

    build_center();
    build_dictation();
    build_result();
    build_picker();
    build_alert();
    build_bottom();
    build_top_bar();
    lv_obj_move_foreground(ui.badge);
    vv_ui_set_battery(-1);
    lv_screen_load(ui.scr);
}

// ---------------------------------------------------------------------------
// Rendering

// Small Target app logo at the top left whenever the Target is known. The
// pill ends at x=144 so "100%" fits beside the battery gauge.
static void render_pill_logo(const vv_app_t *app) {
    int logo = vv_app_target_logo(app);
    bool on = logo >= 0;
    if (on) lv_image_set_src(ui.pill_logo, vv_icons_20[logo]);
    show(ui.pill_logo, on);
    lv_obj_set_x(ui.pill, on ? 44 : 20);
    lv_obj_set_width(ui.pill, on ? 100 : 124);
    lv_obj_set_width(ui.pill_label, on ? 68 : 92);
}

// While a Voice Notes Recording is active the pill shows it ("录音 12:34",
// coral dot) instead of the Target label; the Target logo stays beside it and
// the Idle page still shows the conversation. 68 px fits "录音 MM:SS" and
// "录 H:MM:SS" in the 16 px body font.
static void render_notes_pill(const vv_app_t *app) {
    char time[9];
    vv_format_notes_elapsed(app->notes_elapsed_s, time);
    bool long_time = app->notes_elapsed_s >= 3600u;
    switch (app->notes_state) {
    case VV_NOTES_RECORDING:
        lv_label_set_text_fmt(ui.pill_label, "%s %s",
                              long_time ? VV_T_NOTES_REC_SHORT : VV_T_NOTES_REC, time);
        break;
    case VV_NOTES_PAUSED:
        lv_label_set_text_fmt(ui.pill_label, "%s %s", VV_T_NOTES_PAUSED, time);
        break;
    case VV_NOTES_STARTING:
        lv_label_set_text(ui.pill_label, VV_T_NOTES_STARTING);
        break;
    default:
        lv_label_set_text(ui.pill_label, VV_T_NOTES_STOPPING);
        break;
    }
    bool live = app->notes_state == VV_NOTES_RECORDING;
    set_bg(ui.pill_dot, live ? C_CORAL : C_AMBER);
    lv_obj_set_style_border_color(ui.pill, lv_color_hex(live ? C_CORAL : C_AMBER), 0);
    lv_obj_set_style_border_width(ui.pill, 1, 0);
    set_color(ui.pill_label, live ? C_CORAL : C_AMBER);
}

static void render_target(const vv_app_t *app) {
    if (vv_app_notes_active(app)) {
        render_notes_pill(app);
        return;
    }
    const char *label = app->target_known && app->target_label[0] ? app->target_label
                                                                  : VV_T_NO_TARGET;
    lv_label_set_text(ui.pill_label, label);
    bool down = app->target_known && app->target_status != VV_STATUS_OK;
    uint32_t dot = !app->target_known ? C_DIM : (down ? C_AMBER : C_MINT);
    set_bg(ui.pill_dot, dot);
    lv_obj_set_style_border_color(ui.pill, lv_color_hex(C_AMBER), 0);
    lv_obj_set_style_border_width(ui.pill, down ? 1 : 0, 0);
    set_color(ui.pill_label, app->target_known ? C_TEXT : C_MUTED);
}

static void set_orb(uint32_t ring_color, uint32_t core_color, bool core_filled, bool spinning) {
    lv_obj_set_style_border_color(ui.ring_mid, lv_color_hex(ring_color), 0);
    set_bg(ui.core, core_color);
    lv_obj_set_style_bg_opa(ui.core, core_filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui.core, core_filled ? 0 : 2, 0);
    lv_obj_set_style_border_color(ui.core, lv_color_hex(core_color), 0);
    show(ui.spinner, spinning);
    show(ui.ring_mid, !spinning);
}

static void center_mode(bool passkey) {
    show(ui.ring_outer, !passkey);
    show(ui.ring_mid, !passkey);
    show(ui.core, !passkey);
    show(ui.logo, false);
    show(ui.conv, false);
    show(ui.title, true);
    show(ui.passkey, passkey);
    if (passkey) show(ui.spinner, false);
    lv_obj_set_y(ui.title, passkey ? 58 : 182);
    lv_obj_set_y(ui.body, passkey ? 172 : 216);
}

// Subtitle for IDLE: the most important Companion-side problem, else the hint.
static void idle_body(const vv_app_t *app) {
    const char *msg = NULL;
    if (app->version_mismatch) {
        msg = VV_T_STATUS_VERSION;
    } else if (app->companion_code != VV_COMPANION_OK) {
        switch (app->companion_code) {
        case VV_COMPANION_NO_SPEECH_PERMISSION: msg = VV_T_STATUS_SPEECH; break;
        case VV_COMPANION_NO_AX_PERMISSION: msg = VV_T_STATUS_AX; break;
        case VV_COMPANION_NO_ORCA: msg = VV_T_STATUS_ORCA; break;
        case VV_COMPANION_NO_ZH_RECOGNIZER: msg = VV_T_STATUS_ZH; break;
        default:
            msg = app->companion_text[0] ? app->companion_text : VV_T_STATUS_OTHER;
            break;
        }
    } else if (app->target_known && app->target_status != VV_STATUS_OK) {
        msg = VV_T_ACTION_TARGET;
    }
    lv_label_set_text(ui.body, msg ? msg : VV_T_IDLE_HINT);
    set_color(ui.body, msg ? C_AMBER : C_MUTED);
}

static void set_tail(lv_obj_t *label, const char *s, bool already_cut, const vv_wrap_t *wrap) {
    size_t len = strlen(s);
    size_t start = vv_text_fit_tail(s, len, wrap);
    if (start == 0 && !already_cut) {
        lv_label_set_text(label, s);
        return;
    }
    // Leave room for the leading ellipsis on the first line.
    vv_wrap_t tighter = *wrap;
    tighter.line_px -= wrap->wide_px;
    start = vv_text_fit_tail(s, len, &tighter);
    lv_label_set_text_fmt(label, "…%s", s + start);
}

static void render_waiting_body(const vv_app_t *app) {
    if (app->partial[0]) {
        set_tail(ui.body, app->partial, app->partial_cut, &WAITING_WRAP);
        set_color(ui.body, C_TEXT);
    } else {
        lv_label_set_text(ui.body, VV_T_WAITING_HINT);
        set_color(ui.body, C_MUTED);
    }
}

static void render_result(const vv_app_t *app) {
    const char *title = VV_H_FAILED, *body = VV_T_FAILED_BODY;
    uint32_t color = C_AMBER;
    switch (app->result) {
    case VV_RESULT_INSERTED: title = VV_H_INSERTED; body = NULL; color = C_MINT; break;
    case VV_RESULT_EMPTY: title = VV_H_EMPTY; body = VV_T_EMPTY_BODY; color = C_MUTED; break;
    case VV_RESULT_CANCELLED:
        title = VV_H_CANCELLED; body = VV_T_CANCELLED_BODY; color = C_MUTED; break;
    case VV_RESULT_TARGET_DOWN: title = VV_H_TARGET_DOWN; body = VV_T_TARGET_DOWN_BODY; break;
    case VV_RESULT_RECOGNIZER:
        title = VV_H_RECOGNIZER; body = VV_T_RECOGNIZER_BODY; color = C_CORAL; break;
    case VV_RESULT_PERMISSION: title = VV_H_PERMISSION; body = VV_T_PERMISSION_BODY; break;
    case VV_RESULT_TIMEOUT: title = VV_H_TIMEOUT; body = VV_T_TIMEOUT_BODY; break;
    case VV_RESULT_FAILED: break;
    }
    lv_label_set_text(ui.result_title, title);
    set_bg(ui.result_dot, color);
    show(ui.card, body == NULL);
    show(ui.result_body, body != NULL);
    if (body) {
        lv_label_set_text(ui.result_body, body);
    } else {
        set_tail(ui.segment, app->segment, app->segment_cut, &SEGMENT_WRAP);
    }
}

static const char *picker_title(uint8_t list) {
    switch (list) {
    case VV_LIST_ORCA: return VV_H_PICKER_ORCA;
    case VV_APP_WECHAT + 1: return VV_H_PICKER_WECHAT;
    case VV_APP_CHATGPT + 1: return VV_H_PICKER_CHATGPT;
    case VV_APP_WECOM + 1: return VV_H_PICKER_WECOM;
    default: return VV_H_PICKER_ROOT;
    }
}

static void render_picker(const vv_app_t *app) {
    lv_label_set_text(ui.picker_title, picker_title(app->picker_list));
    bool empty = !app->picker_loading && app->picker_count == 0;
    bool busy = app->picker_loading || app->picker_jumping;
    const char *msg = VV_T_PICKER_LOADING;
    if (app->picker_jumping) {
        msg = VV_T_PICKER_JUMPING;
    } else if (empty) {
        msg = app->picker_list == VV_LIST_ROOT ? VV_T_PICKER_EMPTY
              : (app->picker_parent_flags & VV_ITEM_NOT_RUNNING) ? VV_T_PICKER_NOT_RUN
                                                                 : VV_T_PICKER_NO_CONV;
    }
    show(ui.picker_msg, busy || empty);
    lv_label_set_text(ui.picker_msg, msg);
    if (busy || empty) {
        lv_label_set_text(ui.picker_pos, "");
        for (int i = 0; i < PICKER_ROWS; i++) show(ui.rows[i], false);
        return;
    }
    lv_label_set_text_fmt(ui.picker_pos, "%u/%u", (unsigned)app->picker_cursor + 1,
                          (unsigned)app->picker_count);
    int first = (int)app->picker_cursor - PICKER_ROWS / 2;
    if (first > (int)app->picker_count - PICKER_ROWS) first = app->picker_count - PICKER_ROWS;
    if (first < 0) first = 0;
    for (int i = 0; i < PICKER_ROWS; i++) {
        int index = first + i;
        if (index >= app->picker_count) {
            show(ui.rows[i], false);
            continue;
        }
        const vv_item_t *item = &app->picker[index];
        bool selected = index == app->picker_cursor;
        show(ui.rows[i], true);
        set_bg(ui.rows[i], selected ? C_MINT : C_BG);
        lv_obj_set_style_border_width(ui.rows[i], selected ? 0 : 1, 0);
        lv_obj_set_style_border_color(ui.rows[i], lv_color_hex(C_SURFACE), 0);
        int logo = vv_app_picker_logo(app, (uint8_t)index);
        if (logo >= 0) lv_image_set_src(ui.row_logo[i], vv_icons_20[logo]);
        show(ui.row_logo[i], logo >= 0);
        lv_obj_set_x(ui.row_label[i], logo >= 0 ? 34 : 12);
        lv_obj_set_width(ui.row_label[i], logo >= 0 ? 110 : 132);
        lv_label_set_text(ui.row_label[i], item->label);
        set_color(ui.row_label[i], selected ? C_ON_MINT : C_TEXT);
        const char *tag = "";
        uint32_t tag_color = selected ? C_ON_MINT : C_MUTED;
        if (item->flags & VV_ITEM_NOT_RUNNING) {
            tag = VV_T_NOT_RUNNING;
            if (!selected) tag_color = C_AMBER;
        } else if (item->flags & VV_ITEM_CURRENT) {
            tag = "●";   // the Target's app / the Current Conversation
            if (!selected) tag_color = C_MINT;
        } else if (item->flags & VV_ITEM_SUBLIST) {
            tag = "→";
        }
        lv_label_set_text(ui.row_tag[i], tag);
        set_color(ui.row_tag[i], tag_color);
    }
}

static const char *toast_text(vv_toast_t toast, uint32_t *color) {
    *color = C_TEXT;
    switch (toast) {
    case VV_TOAST_SUBMITTING: return VV_T_SUBMITTING;
    case VV_TOAST_SUBMITTED: *color = C_MINT; return VV_T_SUBMITTED;
    case VV_TOAST_UNDOING: return VV_T_UNDOING;
    case VV_TOAST_UNDONE: *color = C_MINT; return VV_T_UNDONE;
    case VV_TOAST_NOTHING_TO_UNDO: return VV_T_NOTHING_TO_UNDO;
    case VV_TOAST_TARGET_DOWN: *color = C_AMBER; return VV_T_ACTION_TARGET;
    case VV_TOAST_PERMISSION: *color = C_AMBER; return VV_T_ACTION_PERM;
    case VV_TOAST_FAILED: *color = C_CORAL; return VV_T_ACTION_FAILED;
    case VV_TOAST_PAIR_FAILED: *color = C_CORAL; return VV_T_PAIR_FAILED;
    case VV_TOAST_LIST_FAILED: *color = C_AMBER; return VV_T_LIST_FAILED;
    case VV_TOAST_JUMPED: *color = C_MINT; return VV_T_JUMPED;
    case VV_TOAST_NOTES_STARTED: *color = C_CORAL; return VV_T_NOTES_STARTED;
    case VV_TOAST_NOTES_STOPPED: *color = C_MINT; return VV_T_NOTES_STOPPED;
    case VV_TOAST_NOTES_LAUNCH_FAILED: *color = C_CORAL; return VV_T_NOTES_LAUNCH;
    case VV_TOAST_NOTES_START_FAILED: *color = C_CORAL; return VV_T_NOTES_FAILED;
    case VV_TOAST_NOTES_RISK_BLUETOOTH: *color = C_AMBER; return VV_T_NOTES_RISK_BT;
    case VV_TOAST_NOTES_RISK_VOICE_ISOLATION: *color = C_AMBER; return VV_T_NOTES_RISK_VI;
    case VV_TOAST_NOTES_RISK_OTHER: *color = C_AMBER; return VV_T_NOTES_RISK;
    case VV_TOAST_NOTES_NOT_INSTALLED: *color = C_AMBER; return VV_T_NOTES_MISSING;
    case VV_TOAST_NOTES_CONTROL_DISABLED: *color = C_AMBER; return VV_T_NOTES_DENIED;
    case VV_TOAST_NOTES_STOP_FAILED: *color = C_CORAL; return VV_T_NOTES_STOP_FAIL;
    case VV_TOAST_NONE: break;
    }
    return NULL;
}

static void render_bottom(const vv_app_t *app) {
    const char *h1 = "", *h2 = "";
    switch (app->state) {
    case VV_ST_NO_LINK:
    case VV_ST_PAIRING: h1 = ui.device_line; break;
    case VV_ST_IDLE:
    case VV_ST_RESULT:
        if (vv_app_alert_card(app)) {
            static char next[40];
            snprintf(next, sizeof(next), VV_T_ALERT_NEXT_FMT, (unsigned)app->alert_count);
            h1 = VV_T_ALERT_HINT;
            h2 = app->alert_count > 1 ? next : "";
        } else {
            h1 = VV_T_HINT_IDLE_1;
            h2 = VV_T_HINT_IDLE_2;
        }
        break;
    case VV_ST_DICTATING: h2 = VV_T_HINT_DICT; break;
    case VV_ST_PICKER: h1 = VV_T_HINT_PICKER_1; h2 = VV_T_HINT_PICKER_2; break;
    default: break;
    }
    uint32_t color;
    const char *toast = toast_text(app->toast, &color);
    show(ui.toast, toast != NULL);
    show(ui.hint1, toast == NULL);
    show(ui.hint2, toast == NULL);
    if (toast) {
        lv_label_set_text(ui.toast_label, toast);
        set_color(ui.toast_label, color);
    }
    lv_label_set_text(ui.hint1, h1);
    lv_label_set_text(ui.hint2, h2);
}

static void render_elapsed(const vv_app_t *app) {
    char buf[6];
    vv_format_elapsed(app->elapsed_s * 1000u, buf);
    lv_label_set_text(ui.elapsed, buf);
    // Amber during the last 30 s before the 5-minute auto-stop.
    bool near_limit = app->elapsed_s * 1000u + 30000u >= VV_DICTATION_LIMIT_MS;
    set_color(ui.elapsed, near_limit ? C_AMBER : C_CORAL);
}

static void render_partial(const vv_app_t *app) {
    if (app->state == VV_ST_DICTATING) {
        if (app->partial[0]) {
            set_tail(ui.partial, app->partial, app->partial_cut, &PARTIAL_WRAP);
            set_color(ui.partial, C_TEXT);
        } else {
            lv_label_set_text(ui.partial, VV_T_LISTEN_HINT);
            set_color(ui.partial, C_DIM);
        }
    } else if (app->state == VV_ST_WAITING) {
        render_waiting_body(app);
    }
}

static void render_state(const vv_app_t *app) {
    vv_state_t st = app->state;
    show(ui.center, st == VV_ST_NO_LINK || st == VV_ST_PAIRING || st == VV_ST_LINKING ||
                    st == VV_ST_IDLE || st == VV_ST_WAITING);
    show(ui.dict, st == VV_ST_DICTATING);
    show(ui.result, st == VV_ST_RESULT);
    show(ui.picker, st == VV_ST_PICKER);

    switch (st) {
    case VV_ST_NO_LINK:
        center_mode(false);
        set_orb(C_DIM, C_DIM, false, false);
        lv_label_set_text(ui.title, VV_H_NO_LINK);
        lv_label_set_text(ui.body, VV_T_NO_LINK_HINT);
        set_color(ui.body, C_MUTED);
        break;
    case VV_ST_PAIRING: {
        char digits[8];
        vv_format_passkey(app->passkey, digits);
        center_mode(true);
        lv_label_set_text(ui.passkey, digits);
        set_color(ui.passkey, C_BLUE);
        lv_label_set_text(ui.title, VV_H_PAIRING);
        lv_label_set_text(ui.body, VV_T_PAIRING_HINT);
        set_color(ui.body, C_MUTED);
        break;
    }
    case VV_ST_LINKING:
        center_mode(false);
        set_orb(C_BLUE, C_BLUE, false, true);
        lv_obj_set_style_arc_color(ui.spinner, lv_color_hex(C_BLUE), LV_PART_INDICATOR);
        lv_label_set_text(ui.title, VV_H_LINKING);
        lv_label_set_text(ui.body, VV_T_LINKING_HINT);
        set_color(ui.body, C_MUTED);
        break;
    case VV_ST_IDLE: {
        center_mode(false);
        set_orb(C_MINT, C_MINT, true, false);
        lv_label_set_text(ui.title, VV_H_IDLE);
        int logo = vv_app_target_logo(app);
        if (logo >= 0) {
            // The Target app's logo and conversation replace the orb.
            show(ui.ring_outer, false);
            show(ui.ring_mid, false);
            show(ui.core, false);
            show(ui.title, false);
            lv_image_set_src(ui.logo, vv_icons_96[logo]);
            bool usable = app->target_status == VV_STATUS_OK;
            lv_obj_set_style_image_opa(ui.logo, usable ? LV_OPA_COVER : LV_OPA_50, 0);
            show(ui.logo, true);
            lv_label_set_text(ui.conv, vv_app_target_title(app));
            show(ui.conv, true);
        }
        idle_body(app);
        break;
    }
    case VV_ST_WAITING:
        center_mode(false);
        set_orb(C_MINT, C_MINT, false, true);
        lv_obj_set_style_arc_color(ui.spinner, lv_color_hex(C_MINT), LV_PART_INDICATOR);
        lv_label_set_text(ui.title, VV_H_WAITING);
        render_waiting_body(app);
        break;
    case VV_ST_DICTATING:
        memset(ui.levels, 0, sizeof(ui.levels));
        for (int i = 0; i < METER_BARS; i++) {
            lv_obj_set_height(ui.bars[i], 4);
            lv_obj_set_y(ui.bars[i], 96);
        }
        render_elapsed(app);
        render_partial(app);
        break;
    case VV_ST_RESULT:
        render_result(app);
        break;
    case VV_ST_PICKER:
        render_picker(app);
        break;
    }
}

void vv_ui_render(const vv_app_t *app, uint32_t dirty) {
    if (dirty & (VV_DIRTY_TARGET | VV_DIRTY_NOTES)) render_target(app);
    if (dirty & (VV_DIRTY_STATE | VV_DIRTY_TARGET)) render_pill_logo(app);
    if (dirty & (VV_DIRTY_STATE | VV_DIRTY_STATUS | VV_DIRTY_TARGET)) {
        render_state(app);
    } else {
        if (dirty & VV_DIRTY_PARTIAL) render_partial(app);
        if ((dirty & VV_DIRTY_ELAPSED) && app->state == VV_ST_DICTATING) render_elapsed(app);
        if ((dirty & VV_DIRTY_PICKER) && app->state == VV_ST_PICKER) render_picker(app);
    }
    if (dirty & (VV_DIRTY_STATE | VV_DIRTY_ALERTS)) render_alerts(app);
    if (dirty & (VV_DIRTY_STATE | VV_DIRTY_TOAST | VV_DIRTY_ALERTS)) render_bottom(app);
}

void vv_ui_set_battery(int soc) {
    if (soc < 0 || soc > 100) {
        lv_label_set_text(ui.batt_label, "--");
        show(ui.batt_fill, false);
        return;
    }
    lv_label_set_text_fmt(ui.batt_label, "%d%%", soc);
    show(ui.batt_fill, true);
    lv_obj_set_width(ui.batt_fill, soc * 12 / 100 < 1 ? 1 : soc * 12 / 100);
    set_bg(ui.batt_fill, soc <= 15 ? C_CORAL : C_TEXT);
}

void vv_ui_push_level(uint8_t level) {
    if (!ui.dict || lv_obj_has_flag(ui.dict, LV_OBJ_FLAG_HIDDEN)) return;
    memmove(ui.levels, ui.levels + 1, METER_BARS - 1);
    ui.levels[METER_BARS - 1] = level;
    for (int i = 0; i < METER_BARS; i++) {
        int h = 4 + ui.levels[i] * 24 / 100;   // 4..28 px, centred on y = 98
        lv_obj_set_height(ui.bars[i], h);
        lv_obj_set_y(ui.bars[i], 98 - h / 2);
    }
}
