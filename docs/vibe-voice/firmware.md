**English** · [简体中文](firmware.zh_CN.md)

# Vibe Voice Device firmware

The AI Passport firmware in `main/` for Vibe Voice: voice input for vibe coding.
It captures speech, streams it to the **Companion** over BLE, shows Partial Text
and results, and sends button intents. Terms follow [`CONTEXT.md`](CONTEXT.md);
the wire contract is [`protocol.md`](protocol.md) (v2), implemented without
extensions.

This application replaces the baseline hardware-test menu: startup goes
straight to the Vibe Voice screen. The `demo_*.c` and `ui_pixel*.c` sources stay
in `main/` for their host tests and as reference, but `main/CMakeLists.txt` no
longer compiles them into the firmware.

## Module map

| File | Role | ESP-IDF / LVGL? |
| --- | --- | --- |
| `main/vv_app.c` | State machine: buttons, link events, Companion frames, ticks → actions + UI dirty flags | No (host tested) |
| `main/vv_proto.c` | Frame encoders (Device → Companion) and decoder (Companion → Device) | No (host tested) |
| `main/vv_adpcm.c` | IMA-ADPCM encoder/decoder, matches `tests/vectors/adpcm_golden.txt` | No (host tested) |
| `main/vv_text.c` | UTF-8 tail cut, sanitizing, wrap estimate, time/passkey formatting | No (host tested) |
| `main/vv_strings.h` | Every fixed Chinese UI string | No |
| `assets/icons/vibe-voice/vv_icons.c` | Supported App logos, 96 px and 20 px (generated, [README](../../assets/icons/vibe-voice/README.md)) | LVGL data only |
| `main/vv_ble.c` | NimBLE peripheral, security, advertising, FIFO TX task | Yes |
| `main/vv_audio.c` | Capture worker: 20 ms chunks → ADPCM → AUDIO frames | Yes |
| `main/vv_ui.c` | LVGL screen | Yes |
| `main/main.c` | Startup and the controller task that owns the state machine | Yes |

Tasks: button callbacks (shared `esp_timer` task) and NimBLE callbacks only
enqueue into the controller queue. The controller (`vv_ctl`, priority 5) runs the
state machine, executes actions and renders the UI while holding
`bsp_lvgl_lock()`. The audio worker (`vv_audio`, priority 7) and the BLE TX task
(`vv_tx`, priority 6) never touch LVGL. The LVGL task (priority 4) only renders.

## States and screens

The screen is 240 × 320 portrait with a dark ink background. A persistent top
bar shows the Target label from TARGET_STATE (for example
"WeChat · Zhang San", ellipsized when long) with a status dot — grey when unknown, mint
when usable, amber with an amber outline when the Target is not usable — and
the battery percentage with a small battery glyph at top right (`--` and an
empty glyph when the gauge returns `-1`). Whenever the Target is known, the 20 px
logo of the Target's app (TARGET_STATE `app`) sits at the top left before the
pill. While a Voice Notes Recording is active the pill shows it instead of the
Target label, on every page: a coral dot and outline with `VV_T_NOTES_REC` and
the time (`MM:SS`, or `VV_T_NOTES_REC_SHORT` with `H:MM:SS` from one hour), in
amber `VV_T_NOTES_PAUSED` with the time when paused, `VV_T_NOTES_STARTING` or
`VV_T_NOTES_STOPPING` while Voice Notes works; the Target logo stays beside it
and Idle still shows the conversation. The time comes from NOTES_STATE and is
counted on locally each second while recording. An amber badge with the number
of queued Alerts sits on the logo's corner whenever the Alert card cannot show.
The bottom two lines show short
control hints, or a toast that temporarily replaces them.

| State | Screen |
| --- | --- |
| No link (advertising) | Grey concentric rings, title `VV_H_NO_LINK` ("not connected"), `VV_T_NO_LINK_HINT` ("open Vibe Voice on the Mac and connect"), bottom line with the device name `VibeVoice-XXXX` |
| Pairing | Title `VV_H_PAIRING` ("passkey"), the 6-digit passkey in 48 px blue digits (`482 913`), `VV_T_PAIRING_HINT` ("type this on the Mac"), device name |
| Linking | Blue spinner, `VV_H_LINKING` ("connecting"), `VV_T_LINKING_HINT` ("waiting for the Mac"): secure link, HELLO sent, waiting for HELLO_ACK |
| Idle | The Target app's 96 px logo (half transparent when the Target is not usable) with the conversation title below it (the label after `" · "`); without a known app, mint rings with a filled core and `VV_H_IDLE` ("ready"). Subtitle `VV_T_IDLE_HINT` ("press OK to speak"), or in amber the most important problem: protocol version mismatch, a Companion STATUS message (speech permission, accessibility permission, Orca CLI, zh-CN recognizer, or the Companion's text for unknown codes), or "Target not running" |
| Dictating | Coral dot + `VV_H_DICTATING`, elapsed `MM:SS` (turns amber in the last 30 s), a 16-bar live microphone meter, and the latest Partial Text wrapped over up to 6 lines, showing its tail with a leading `…` when it does not fit |
| Waiting for result | Mint spinner, `VV_H_WAITING` ("recognizing…"), the last two lines of Partial Text |
| Result (transient) | `VV_H_INSERTED` ("inserted") with a card previewing the Segment tail (6 s), or an error title and explanation (4 s) for EMPTY, CANCELLED, TARGET_UNAVAILABLE, RECOGNIZER_ERROR, PERMISSION, an unknown status, or a 30 s timeout. Then back to Idle. |
| Alert card (Idle / Result with an Alert queued) | A card over the page with an amber outline: the app logo and `VV_T_ALERT_TITLE` in amber with `n/N` when several are queued (1 = newest), the session label (`"<worktree> · <title>"`), and the agent's message (up to 360 bytes) wrapped over as many lines as fit (7); hints `VV_T_ALERT_HINT` and, with several, `VV_T_ALERT_NEXT_FMT` with the total. Not shown while dictating, waiting or in the picker; the badge counts instead |
| Picker | `VV_H_PICKER_ROOT` (the four Supported Apps, each row starting with its 20 px logo) or the sub-list title (`VV_H_PICKER_ORCA`, `VV_H_PICKER_WECHAT`, `VV_H_PICKER_CHATGPT`, `VV_H_PICKER_WECOM`), position `n/N`, a 5-row window with the cursor highlighted in mint; tags: amber `VV_T_NOT_RUNNING` for not running, a dot for current (the Target's app, or the Current Conversation), an arrow for a row that opens a sub-list; messages for loading, jumping (`VV_T_PICKER_JUMPING`), and an empty list (`VV_T_PICKER_NOT_RUN` when the app is not running, else `VV_T_PICKER_NO_CONV`) |

Toasts: submitting → submitted, undoing → undone, nothing to undo, Target not
running, macOS permission missing, failed (all from ACTION_RESULT), pairing
failed, list failed to load, jumped (`VV_T_JUMPED`, or the failure toast when a
Jump failed or timed out), and the Voice Notes notices from NOTES_STATE:
started, stopped, launch failed, start failed, not installed, control not
allowed, stop failed, and the start risks (Bluetooth microphone, Voice
Isolation, other); the failure and risk toasts stay 4 s. The exact Chinese text of every string is in
`main/vv_strings.h` and in the [Chinese version](firmware.zh_CN.md) of this page.

## Controls

Short presses act on the button component's CLICK event; it fires about 180 ms
after release, because the component waits that long to rule out a double click.
A double press of OK toggles a Voice Notes Recording (NOTES_TOGGLE) in Idle,
Result, Dictating and Waiting; other DOUBLE presses are ignored except UP/DOWN
in the picker, so a quick double tap can never Submit or Undo twice. The
button component (`espressif/button` 4.2.0) decides between SINGLE_CLICK and
DOUBLE_CLICK only after the short-press window following the last release, so a
double press reports DOUBLE alone, never a CLICK first: double-pressing during a
Dictation toggles Voice Notes and leaves the Dictation running. Two presses
slower than that window are two clicks.

| State | UP | DOWN | OK click | OK long (500 ms) |
| --- | --- | --- | --- | --- |
| Idle / Result | Undo | Submit | Start Dictation; double: toggle Voice Notes | Open the Jump picker |
| Alert card (Idle / Result) | Dismiss the Alert | Next older Alert (wraps; if several) | Open the Alert (Jump to its session); double: toggle Voice Notes, the Alert stays | Open the Jump picker |
| Dictating | Cancel | — | Stop (Companion Inserts); double: toggle Voice Notes, the Dictation continues | Stop (same as click) |
| Waiting | — | — | Double: toggle Voice Notes | — |
| Picker | Move up (wraps; double = 2) | Move down (wraps; double = 2) | Root row `i`: open list `i + 1` (that app's conversations); sub-list row: Jump | Back to the root list from a sub-list; close from the root list or while a Jump is pending |
| No link, Pairing, Linking | — | — | — | — |

A Dictation stops by itself after 5 minutes (sends DICT_STOP as if OK was pressed).

## Behaviour details

- **Dictation.** OK sends DICT_START, then the audio worker streams one AUDIO
  frame per 20 ms. Stop and Cancel first stop the worker and wait (bounded,
  300 ms) until its last frame is queued, then send DICT_STOP / DICT_CANCEL, so
  the Companion always sees them after the final AUDIO frame. `dict` increments
  per Dictation and wraps at 255.
- **Waiting.** A RESULT is matched by `dict`. If none arrives within 30 s (the
  Companion may first launch Orca, up to 20 s) the
  Device shows the timeout result and ignores a later RESULT for that Dictation. After a
  local Cancel the Companion's CANCELLED RESULT is not shown again.
- **Picker (Jump).** OK long sends TARGETS_REQ(0); TARGET_ITEM rows for the
  requested list are stored (at most 24, labels cut to 71 bytes on a UTF-8
  boundary) until TARGET_END. The cursor starts on the row flagged current. The
  root list is always the four Supported Apps in protocol order, so root row `i`
  shows logo `i`. OK on root row `i` sends TARGETS_REQ(`i + 1`); OK on a sub-list
  row sends TARGET_SELECT(list, index) and shows the jumping message until the
  Companion's TARGET_STATE arrives, which closes the picker with the jumped toast
  (or the failure toast for a non-OK status). A list that does not finish within
  5 s (25 s for list 1, whose request may launch Orca) closes the picker with
  the list-failed toast; a Jump without a reply within 5 s closes it with the
  failed toast. A TARGET_STATE that is not a Jump reply only updates the top bar.
- **Alerts.** ALERT frames queue up to 8 Alerts, oldest first; an ALERT with a
  known `id` replaces that Alert (moved to the end), beyond 8 the oldest is
  dropped, and ALERT_CLEAR removes one. ALERT_MORE frames extend a message (up
  to 360 bytes) when their offset matches what has arrived. Every new Alert
  pops up: the card switches to it and numbers it 1/N; DOWN steps to older
  ones and wraps. A clear of another Alert keeps the shown one. The card shows
  in Idle and Result while any are queued; it takes OK, UP and DOWN clicks only
  there, so it never takes OK from a Dictation. Opening sends ALERT_OPEN and
  removes it (the Companion's TARGET_STATE follows); dismissing sends
  ALERT_DISMISS; either way the newest remaining Alert shows next. A double press is a separate DOUBLE event, so
  double OK on the card toggles Voice Notes and leaves the Alert alone. Link
  loss clears the queue; the Companion resends pending Alerts after HELLO.
- **Voice Notes Recording.** Independent of Dictation: it records with the
  Mac's microphone, and NOTES_STATE never changes the state, the Dictation, the
  Target or the picker; it only updates the top bar and may show a toast.
- **Link loss.** A disconnect during a Dictation stops capture and sends
  nothing; both sides abandon it (protocol rule). Any pending list or Jump is
  dropped, and the Voice Notes state is cleared until the next HELLO.
- **Text safety.** All Companion text is sanitized before display: invalid
  UTF-8 bytes and control characters become `?`, `\r` is dropped, and long text
  keeps its tail on a character boundary.

## BLE and pairing

- Advertises the NUS service UUID; the scan response carries
  `VibeVoice-XXXX` (last two bytes of the BT MAC, uppercase hex).
- One connection. RX: write / write-without-response, requires an encrypted,
  authenticated link. TX: notify plus read (returns the device name, for BLE
  tools), read and CCCD write require an encrypted, authenticated link. Both
  characteristics have an `access_cb`.
- Security: LE Secure Connections only (`CONFIG_BT_NIMBLE_SM_LEGACY=n`,
  `SM_SC_ONLY`), MITM, bonding, DisplayOnly IO capability. On connect the Device
  requests security. For a new Mac, the Device generates an unbiased random
  6-digit passkey and shows it; the user types it on the Mac. A link that ends
  up unencrypted, unauthenticated, unbonded, or with a key shorter than 16 bytes
  is disconnected and shows the pairing-failed toast.
- Bonds persist in NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST=y`, up to 3). A bonded
  Mac reconnects without a passkey. If the Mac forgot the Device, the Device
  deletes the old bond and pairs again (`REPEAT_PAIRING`). If the Device lost its
  bond (for example after flashing the merged image, which can reset NVS), the
  Mac's encryption fails and the Device shows the pairing error: remove
  `VibeVoice-XXXX` from the Mac's Bluetooth settings and pair again.
- Link ready = secure + TX notifications enabled. Only then does the Device send
  HELLO, repeating it every second until HELLO_ACK moves to Idle. If the Companion turns
  notifications off and on again on a live link, the Device starts over with a
  new HELLO.
- Throughput: the Device requests a 15–30 ms connection interval and 251-byte
  data length; the preferred ATT MTU is 247 (an AUDIO frame is 167 bytes and
  needs an MTU of at least 170). One TX task notifies frames in FIFO order from a
  12-frame queue and backs off while the controller is out of buffers. The audio
  worker never waits for that queue: when it is full the frame is dropped and
  the Companion sees a gap in `seq`.

## Audio

16 kHz, 16-bit, mono from the ES8311 via `bsp_audio_read()` in 320-sample
(20 ms) chunks. The first chunk after start is discarded (stale DMA data). Each
chunk is IMA-ADPCM encoded (low nibble first) with the encoder state before the
chunk in the frame header. Only one chunk is buffered; nothing accumulates a
whole recording. Mic gain stays at the BSP's 30 dB. `CONFIG_I2S_ISR_IRAM_SAFE=y`
keeps the I2S interrupt running during Flash writes; the firmware writes NVS
only while pairing, never during a Dictation.

## Chinese text and fonts

Fonts are generated from Noto Sans SC (OFL 1.1); see
[`assets/fonts/vibe-voice/README.md`](../../assets/fonts/vibe-voice/README.md).

- Body 16 px: all of GB2312 (6763 hanzi and its symbols) plus ASCII, Latin-1,
  general punctuation, CJK punctuation and fullwidth forms — 7648 glyphs. This
  is the supported character set for Partial Text, Segments, Target labels and
  picker items.
- Title 24 px: ASCII plus the characters of the `VV_H_*` titles. Digits 48 px:
  the passkey.
- Conversation titles under the Idle logo use the body font (they are arbitrary
  text).
- Characters outside the set (emoji, rare hanzi, vertical marks) render as
  LVGL's placeholder box; they are never silently dropped.
- `python3 tools/vibe_fonts.py check` (part of the static gate) fails if any
  string literal in `main/vv_*` or any `VV_H_*` title is not covered.

## Resources

Measured from the firmware build (ESP-IDF 5.5.3):

| Item | Size |
| --- | --- |
| Application image | ≈ 2.0 MB of the 8 MB factory partition (≈ 75 % free) |
| Fonts (Flash, `.rodata`) | body 958 KiB, title 23 KiB, digits 4.4 KiB |
| App logos (Flash, `.rodata`) | 113 KiB: 4 × 96 px (27 KiB each) and 4 × 20 px (1.2 KiB each), RGB565 + 8-bit alpha |
| Static DRAM | 167 KiB of 321 KiB (includes the 48 KiB LVGL pool); ≈ 150 KiB left for heap before NimBLE (≈ 60 KiB), the 19 KiB LCD DMA buffer and task stacks |
| LVGL pool | ≈ 31 KiB peak in a 64-bit host simulation of all screens (smaller on the 32-bit target) |

At runtime the log reports free heap and largest free block at boot, before and
after BLE start, at `ready` and when the link becomes ready, plus LVGL pool use
after the UI is built and on link ready.

## Build and test

```bash
./tools/validate.sh --static    # host tests: vv_adpcm (golden vector), vv_proto/vv_text, vv_app, font coverage
./tools/validate.sh --firmware  # ESP-IDF build + merged image build/vibe-voice-input-full.bin
```

## On-device acceptance checklist

Flash and observe with the serial log. Report the firmware hash with results.

1. Boot: the Vibe Voice screen appears (no demo menu); top bar "no Target", battery
   percentage or `--`; log shows heap and LVGL pool lines.
2. Advertising: a BLE scanner sees `VibeVoice-XXXX` with the NUS UUID.
3. First pairing from the Mac: the 6-digit passkey appears large and readable;
   entering it pairs; the screen moves through Linking to Idle, which shows the
   Target app's logo and conversation title, and the top bar shows the Target
   label.
4. Wrong passkey or cancel on the Mac: pairing-failed toast, then No link.
5. Reboot the Device: the Mac reconnects without a passkey.
6. Dictation: OK → Dictating, timer runs, meter moves with speech, Chinese and
   English Partial Text wraps and shows the tail; OK → Waiting → Inserted with
   the Segment preview; text appears in the Target without Enter.
7. DOWN → "submitted" toast and the Target receives Enter; UP → "undone"; UP
   again → "nothing to undo".
8. UP while dictating → Cancelled result, nothing inserted.
9. Long OK opens the picker: four app rows with logos, the cursor on the
   Target's app; UP/DOWN move and wrap. OK on Orca lists Orca Sessions with the
   cursor on the Current Conversation (Orca launches first if it was not
   running); OK on a session switches Orca to it, the picker closes with
   "switched" and the logo and top bar follow. OK on WeChat shows one
   "current conversation" row, or "app not running"; long OK goes back, then
   closes. Not-running apps show the amber tag.
10. Focus follows the Mac: bring WeChat or Orca to the front and within about
    2 s the Idle logo, title and top bar change; focusing another app (for
    example a browser) leaves them unchanged.
11. Companion STATUS codes 1–4 show the matching amber line in Idle; code 0 clears it.
12. Turn Bluetooth off on the Mac during a Dictation: No link, audio stops, no
    insert; reconnect works.
13. Five-minute Dictation stops by itself (timer turns amber at 04:30).
14. Audio quality: no crackles or gaps in the Companion's decoded audio; log
    `Dictation N: X frames, Y dropped` with Y near 0 at the negotiated interval.
15. Glyphs: titles, hints, toasts, picker labels, mixed Chinese/English/digits
    and fullwidth punctuation render without boxes; an emoji renders as a box.
16. Repeat 20 Dictations: no heap decline in the `heap` log lines.
17. Voice Notes: double OK in Idle starts a recording in Voice Notes (launched
    in the background if needed); the top bar shows the coral recording time on
    every page and it keeps counting; a Bluetooth microphone shows the risk
    toast. Dictate meanwhile: text still lands in the Target. Double OK while
    dictating stops the recording and the Dictation continues. Starting or
    stopping in Voice Notes on the Mac shows on the Device within about 2 s.
18. Alerts: let a Claude Code session in Orca finish a turn while another app
    is frontmost: within about 2 s the Alert card shows its label and last
    words; UP dismisses it, OK switches Orca to that session and the top bar
    follows. Each new Alert pops up as 1/N; DOWN pages to older ones and the
    hint shows the total. While dictating only the badge counts; the card
    appears after the result. A session that starts working again drops its
    Alert; a turn that ends while Orca shows that session still alerts, like
    Orca's own notification.
