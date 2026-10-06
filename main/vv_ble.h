// main/vv_ble.h -- NimBLE peripheral transport for the Vibe Voice protocol.
//
// Nordic UART Service layout, LE Secure Connections with MITM and bonding
// (DisplayOnly: the Device shows a 6-digit passkey), bonds kept in NVS.
// RX/TX/CCCD require an encrypted, authenticated link.
//
// Callbacks run in the NimBLE host task: they must only enqueue.
// vv_ble_send() may be called from any task; frames are notified in FIFO
// order by one TX task, so the order of all sends from one task is preserved.
#pragma once

#include "vv_app.h"
#include "vv_proto.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*vv_ble_link_cb_t)(vv_link_ev_t ev, uint32_t passkey);
typedef void (*vv_ble_rx_cb_t)(const uint8_t *data, size_t len);

// NVS must already be initialized. Starts advertising as VibeVoice-XXXX.
esp_err_t vv_ble_start(vv_ble_link_cb_t link_cb, vv_ble_rx_cb_t rx_cb);

// Advertised name ("VibeVoice-XXXX"); valid after vv_ble_start().
const char *vv_ble_name(void);

// True while a secure link has TX notifications enabled.
bool vv_ble_ready(void);

// Queues a frame for notification. Waits up to `wait_ms` for queue space.
// Returns false when the link is not ready or the queue stayed full.
bool vv_ble_send(const vv_frame_t *frame, uint32_t wait_ms);

// Frames dropped because the queue was full or the stack ran out of buffers.
uint32_t vv_ble_dropped(void);

// Connection pace: fast (15-30 ms interval) for Dictation and quick follow-up
// presses, slow (60-90 ms, peripheral latency 4) when idle. Call it whenever
// the wanted pace may have changed (controller task): it asks the central
// only when the pace differs from the last accepted request, retrying a
// request that could not be sent yet.
void vv_ble_set_pace(bool fast);
