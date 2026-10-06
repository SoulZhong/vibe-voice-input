// Host test: screen light and link pace (main/vv_power.c).
#include "vv_power.h"

#include <assert.h>
#include <stdio.h>

static void idle_dims_then_darkens(void) {
    vv_power_t p;
    vv_power_init(&p, 1000);
    vv_power_update(&p, VV_ST_IDLE, 1000);
    assert(p.light == VV_LIGHT_ON && p.pace == VV_PACE_FAST);

    vv_power_update(&p, VV_ST_IDLE, 1000 + VV_PACE_HOLD_MS);
    assert(p.light == VV_LIGHT_ON && p.pace == VV_PACE_SLOW);

    vv_power_update(&p, VV_ST_IDLE, 1000 + VV_LIGHT_DIM_MS);
    assert(p.light == VV_LIGHT_DIM);
    assert(vv_power_backlight(p.light) == VV_BACKLIGHT_DIM);

    vv_power_update(&p, VV_ST_IDLE, 1000 + VV_LIGHT_OFF_MS);
    assert(p.light == VV_LIGHT_OFF);
    assert(vv_power_backlight(p.light) == 0);
}

static void press_in_the_dark_only_wakes(void) {
    vv_power_t p;
    vv_power_init(&p, 0);
    vv_power_update(&p, VV_ST_IDLE, VV_LIGHT_OFF_MS);
    assert(vv_power_activity(&p, VV_LIGHT_OFF_MS));    // was dark
    assert(p.light == VV_LIGHT_ON && p.pace == VV_PACE_FAST);
    assert(!vv_power_activity(&p, VV_LIGHT_OFF_MS + 5)); // now lit

    // A press while dimmed acts normally.
    vv_power_update(&p, VV_ST_IDLE, VV_LIGHT_OFF_MS + 5 + VV_LIGHT_DIM_MS);
    assert(p.light == VV_LIGHT_DIM);
    assert(!vv_power_activity(&p, VV_LIGHT_OFF_MS + 5 + VV_LIGHT_DIM_MS));
}

static void busy_states_stay_lit_and_fast(void) {
    const vv_state_t busy[] = {VV_ST_PAIRING, VV_ST_LINKING, VV_ST_DICTATING,
                               VV_ST_WAITING, VV_ST_PICKER};
    for (unsigned i = 0; i < sizeof(busy) / sizeof(busy[0]); i++) {
        vv_power_t p;
        vv_power_init(&p, 0);
        // A five-minute Dictation never dims.
        for (uint32_t t = 0; t <= 5u * 60u * 1000u; t += 100) vv_power_update(&p, busy[i], t);
        assert(p.light == VV_LIGHT_ON && p.pace == VV_PACE_FAST);
    }
    assert(!vv_power_busy(VV_ST_IDLE));
    assert(!vv_power_busy(VV_ST_RESULT));
    assert(!vv_power_busy(VV_ST_NO_LINK));
}

static void timers_count_from_the_end_of_busy(void) {
    vv_power_t p;
    vv_power_init(&p, 0);
    vv_power_update(&p, VV_ST_DICTATING, 100000);
    vv_power_update(&p, VV_ST_IDLE, 100000 + VV_LIGHT_DIM_MS - 1);
    assert(p.light == VV_LIGHT_ON);
    vv_power_update(&p, VV_ST_IDLE, 100000 + VV_PACE_HOLD_MS - 1);
    assert(p.pace == VV_PACE_FAST);
}

static void survives_clock_wrap(void) {
    vv_power_t p;
    vv_power_init(&p, 0xFFFFFF00u);
    vv_power_update(&p, VV_ST_IDLE, 0x00000100u);   // 512 ms later
    assert(p.light == VV_LIGHT_ON);
}

int main(void) {
    idle_dims_then_darkens();
    press_in_the_dark_only_wakes();
    busy_states_stay_lit_and_fast();
    timers_count_from_the_end_of_busy();
    survives_clock_wrap();
    printf("test_vv_power: ok\n");
    return 0;
}
