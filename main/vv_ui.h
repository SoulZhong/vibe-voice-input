// main/vv_ui.h -- Vibe Voice screen (240x320 portrait, LVGL).
// Every function must run in the LVGL task or with bsp_lvgl_lock() held.
#pragma once

#include "vv_app.h"

#include <stdint.h>

// Builds and loads the screen. `device_name` is the advertised BLE name.
void vv_ui_init(const char *device_name);

// Applies the model to the widgets touched by `dirty` (VV_DIRTY_*).
void vv_ui_render(const vv_app_t *app, uint32_t dirty);

// Battery percentage for the top-right indicator; -1 = unavailable.
void vv_ui_set_battery(int soc);

// Pushes one microphone level sample (0..100) into the Dictation meter.
void vv_ui_push_level(uint8_t level);
