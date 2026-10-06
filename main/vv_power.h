// main/vv_power.h -- when to light the screen and how fast to run the link
// (pure C, host tested).
//
// Activity (a button press, a new Alert, a state the user must see) lights
// the screen; it dims after VV_LIGHT_DIM_MS and goes dark after
// VV_LIGHT_OFF_MS without activity. Busy states (Dictation, waiting for a
// RESULT, the picker, pairing) count as continuous activity.
//
// The BLE link runs FAST (short connection interval) while busy and for
// VV_PACE_HOLD_MS after activity, so presses that follow each other and the
// first AUDIO frames see low latency; otherwise SLOW (long interval with
// peripheral latency).
#pragma once

#include "vv_app.h"

#include <stdbool.h>
#include <stdint.h>

#define VV_LIGHT_DIM_MS  15000u
#define VV_LIGHT_OFF_MS  60000u   // since the last activity
#define VV_PACE_HOLD_MS  10000u

#define VV_BACKLIGHT_ON   90u     // percent
#define VV_BACKLIGHT_DIM  15u

typedef enum { VV_LIGHT_ON = 0, VV_LIGHT_DIM, VV_LIGHT_OFF } vv_light_t;
typedef enum { VV_PACE_FAST = 0, VV_PACE_SLOW } vv_pace_t;

typedef struct {
    uint32_t active_ms;   // last activity
    vv_light_t light;
    vv_pace_t pace;
} vv_power_t;

void vv_power_init(vv_power_t *p, uint32_t now_ms);

// Something the user should notice. Returns true when the screen was dark:
// a button press that only wakes the screen must not act.
bool vv_power_activity(vv_power_t *p, uint32_t now_ms);

// Re-evaluate light and pace for the current state.
void vv_power_update(vv_power_t *p, vv_state_t state, uint32_t now_ms);

// States that keep the screen on and the link fast.
bool vv_power_busy(vv_state_t state);

// Backlight percent for a light level.
uint8_t vv_power_backlight(vv_light_t light);
