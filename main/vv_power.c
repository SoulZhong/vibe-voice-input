// main/vv_power.c -- see vv_power.h.
#include "vv_power.h"

void vv_power_init(vv_power_t *p, uint32_t now_ms) {
    p->active_ms = now_ms;
    p->light = VV_LIGHT_ON;
    p->pace = VV_PACE_FAST;
}

bool vv_power_activity(vv_power_t *p, uint32_t now_ms) {
    bool was_dark = p->light == VV_LIGHT_OFF;
    p->active_ms = now_ms;
    p->light = VV_LIGHT_ON;
    p->pace = VV_PACE_FAST;
    return was_dark;
}

bool vv_power_busy(vv_state_t state) {
    switch (state) {
    case VV_ST_PAIRING:
    case VV_ST_LINKING:
    case VV_ST_DICTATING:
    case VV_ST_WAITING:
    case VV_ST_PICKER:
        return true;
    default:
        return false;
    }
}

void vv_power_update(vv_power_t *p, vv_state_t state, uint32_t now_ms) {
    if (vv_power_busy(state)) p->active_ms = now_ms;
    uint32_t idle = now_ms - p->active_ms;   // wraps safely
    p->light = idle < VV_LIGHT_DIM_MS ? VV_LIGHT_ON
             : idle < VV_LIGHT_OFF_MS ? VV_LIGHT_DIM
                                      : VV_LIGHT_OFF;
    p->pace = idle < VV_PACE_HOLD_MS ? VV_PACE_FAST : VV_PACE_SLOW;
}

uint8_t vv_power_backlight(vv_light_t light) {
    switch (light) {
    case VV_LIGHT_ON: return VV_BACKLIGHT_ON;
    case VV_LIGHT_DIM: return VV_BACKLIGHT_DIM;
    default: return 0;
    }
}
