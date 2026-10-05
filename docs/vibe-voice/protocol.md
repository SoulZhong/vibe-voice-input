**English** · [简体中文](protocol.zh_CN.md)

# Vibe Voice BLE protocol (v2)

The contract between the **Device** (AI Passport firmware in `main/`) and the
**Companion** (macOS app in `host/vibe-voice/`). Terms follow
[`CONTEXT.md`](CONTEXT.md).

Version 2 replaced v1's configurable Target list with the stored, focus-following
Target and the Jump picker: TARGETS_REQ list ids, TARGET_ITEM flags and the
TARGET_STATE layout changed. A v1 peer sees a version mismatch.

## Transport

- BLE GATT, Nordic UART Service layout. The Device is the peripheral.
  - Service `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
  - RX `6e400002-…` write / write-without-response: Companion → Device
  - TX `6e400003-…` notify: Device → Companion
- Advertised name: `VibeVoice-XXXX` (last two MAC bytes, uppercase hex).
- Security: LE Secure Connections, bonding, MITM. The Device has DisplayOnly IO
  capability and shows a 6-digit passkey; macOS asks the user to type it. RX, TX
  and the TX CCCD require an encrypted link. Bonds are stored in NVS.
- One protocol frame per GATT write or notification. Every frame is at most
  **180 bytes** (fits the ATT MTU macOS negotiates). There is no fragmentation;
  long text is sent as its **tail**, cut on a UTF-8 character boundary.
- Integers are little-endian. Text is UTF-8 without a terminator; its length is
  the rest of the frame.

## Frame layout

```text
byte 0   type
byte 1.. payload (type specific)
```

### Device → Companion

| Type | Name | Payload | Meaning |
| --- | --- | --- | --- |
| `0x01` | HELLO | `ver u8` (=2), `fw text` | Sent when the TX CCCD is enabled, then repeated every 1 s until HELLO_ACK (a bonded reconnect can restore the CCCD before the Companion listens). |
| `0x10` | DICT_START | `dict u8` | A Dictation began. `dict` increments per Dictation (wraps). |
| `0x11` | AUDIO | `dict u8`, `seq u16`, `pred i16`, `index u8`, `adpcm[160]` | 20 ms of audio = 320 samples. `pred`/`index` are the encoder state **before** this frame, so every frame decodes on its own; a gap in `seq` means lost frames. |
| `0x12` | DICT_STOP | `dict u8` | User ended the Dictation (OK) or the 5-minute limit hit. Companion finalizes, Inserts and replies RESULT. |
| `0x13` | DICT_CANCEL | `dict u8` | Cancel: discard, insert nothing, reply RESULT status `CANCELLED`. |
| `0x20` | SUBMIT | — | Press Enter in the Target. Reply ACTION_RESULT. |
| `0x21` | UNDO | — | Remove the latest Segment from the Target. Reply ACTION_RESULT. |
| `0x30` | TARGETS_REQ | `list u8` | Ask for a picker list. `0` = the Supported Apps; `n` (1–4) = the conversations of root row `n - 1`: `1` Orca Sessions, `2` WeChat, `3` ChatGPT, `4` WeCom. |
| `0x31` | TARGET_SELECT | `list u8`, `index u8` | Jump to a row of the last list sent. Reply TARGET_STATE. |

### Companion → Device

| Type | Name | Payload | Meaning |
| --- | --- | --- | --- |
| `0x81` | HELLO_ACK | `ver u8` (=2) | Followed by TARGET_STATE. |
| `0x82` | STATUS | `code u8`, `text` | Companion-level problem shown on the Device (codes below, `0` clears). |
| `0x90` | PARTIAL | `dict u8`, `text` | Latest Partial Text (tail). Replaces the previous one. |
| `0x91` | RESULT | `dict u8`, `status u8`, `text` | Dictation outcome; `text` is the Segment (tail) when inserted. |
| `0xA0` | ACTION_RESULT | `action u8` (`0x20`/`0x21`), `status u8` | Outcome of SUBMIT/UNDO. |
| `0xB0` | TARGET_ITEM | `list u8`, `index u8`, `count u8`, `flags u8`, `label text` | One list row. `flags`: bit0 current (root list: the app the Target is in; sub-list: the app's Current Conversation), bit1 opens a sub-list, bit2 app not running. |
| `0xB1` | TARGET_END | `list u8`, `count u8` | List complete. |
| `0xB2` | TARGET_STATE | `status u8`, `kind u8`, `app u8`, `label text` | The Target. `kind`: 1 a Supported App's Current Conversation, 2 an Orca Session (0 reserved). `app`: the Supported App it is in, `0` Orca, `1` WeChat, `2` ChatGPT, `3` WeCom, `0xFF` none; the Device shows that app's logo. `label` is `"<app> · <Target Title>"` when a title is known. |

### Status codes

RESULT / ACTION_RESULT / TARGET_STATE `status`:

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | OK | Inserted / done / Target usable (an Orca Target is usable while Orca is not running: it is launched). |
| 1 | EMPTY | Nothing recognized; nothing inserted. |
| 2 | CANCELLED | Dictation cancelled. |
| 3 | TARGET_UNAVAILABLE | The targeted app is not running (only Orca is launched), Orca could not be launched or has no session, or a Jump failed. |
| 4 | RECOGNIZER_ERROR | Speech recognition failed. |
| 5 | PERMISSION | A macOS permission is missing (Speech, Accessibility). |
| 6 | NOTHING_TO_UNDO | UNDO with no remembered Segment. |

STATUS `code`: 0 clear, 1 speech permission missing, 2 accessibility permission
missing, 3 Orca CLI unavailable, 4 recognizer unavailable for `zh-CN`.

## Audio

- Capture 16 kHz, 16-bit, mono. IMA-ADPCM, 4 bits per sample, low nibble first,
  standard 89-entry step table. 320 samples → 160 bytes per frame, 50 frames/s
  (≈ 8.4 kB/s on air).
- The shared golden vector [`tests/vectors/adpcm_golden.txt`](../../tests/vectors/adpcm_golden.txt)
  must round-trip identically in the firmware encoder and the Companion decoder.

## Limits

- A Target list has at most **24** items. TARGET_ITEM labels are at most **71**
  bytes and TARGET_STATE labels at most **127** bytes. The Companion fits them
  by keeping the name and shortening the title with "…" (labels are not cut to
  their tail).
- The Device closes the picker if TARGET_END does not arrive within 5 s (25 s
  for list 1, whose request may launch Orca) and if a Jump's TARGET_STATE does
  not arrive within 5 s. It gives up on RESULT after 30 s (an Insert may wait
  up to 20 s for Orca to launch).

## Behaviour rules

- An Insert never presses Enter. Only SUBMIT does.
- UNDO deletes as many characters (grapheme clusters) as the latest Segment had,
  once, where that Segment went (even if the Target changed since). After UNDO, a successful SUBMIT, or the start of a new Dictation, there is
  nothing to undo until the next Segment.
- **Target.** The Companion stores the Target (an Orca Session, or a non-Orca
  Supported App whose Current Conversation is whatever it shows) and keeps it
  across restarts. Whenever a Supported App is frontmost on the Mac, checked
  every 2 s and again right before each Insert and Submit, the Target becomes
  that app's Current Conversation (for Orca: the active leaf terminal of the
  active tab in Orca's active worktree). Focus on any other app leaves the
  Target unchanged. With no Target yet, or when the targeted Orca Session no
  longer exists, Orca's Current Conversation silently takes its place.
- **Delivery.** Insert, Submit and Undo first bring the destination to the
  front. For an Orca Session the Companion runs `orca terminal switch` and
  activates Orca, then uses `orca terminal send` (text, Enter, or backspaces);
  no clipboard. Line breaks become spaces. For another app it activates the app,
  pastes with ⌘V (restoring the clipboard) or presses Return / Delete.
- **Launching.** Only Orca is launched (`orca open`, bounded to 20 s): for an
  Insert or Submit whose Target is in Orca, and for TARGETS_REQ 1. Other apps
  are never launched; their sub-list is empty and Insert, Submit and Undo
  report TARGET_UNAVAILABLE.
- **Picker.** Root rows are always the four Supported Apps in the fixed order
  above (row `i` = `app` `i`), each with bit1 and with bit0 on the app the Target
  is in. OK on root row `i` sends TARGETS_REQ `i + 1`. List 1 holds up to 24
  Orca Sessions across worktrees, the Current Conversation first (bit0), then the
  rest of the active worktree, then the others; labels are
  `"<worktree> · <title>"`. Lists 2–4 hold exactly one row, the Current
  Conversation (bit0), labelled with the Chinese for "current conversation" and
  the focused window title, or none when the app is not running. The Device opens a list with the cursor on its bit0 row.
- **Jump.** TARGET_SELECT on a sub-list row makes it the Target and brings it to
  the front (Orca: `orca terminal switch` and activate Orca; other apps:
  activate). The reply is TARGET_STATE; its status is the failure when the Jump
  failed. The Device closes the picker when that TARGET_STATE arrives. A
  TARGET_SELECT on the root list or out of range leaves the Target unchanged and
  is answered with TARGET_STATE.
- **TARGET_STATE** is sent after HELLO_ACK, at DICT_START, after each Insert and
  Submit, after a Jump, and whenever the Target or its view changes (the 2 s
  refresh runs while linked and not dictating).
- RESULT text is empty when nothing was inserted. DICT_STOP for an unknown
  Dictation is answered with RESULT `EMPTY`.
- STATUS carries one code at a time, by priority: 1, 4, 2 (when the Target is
  in an app other than Orca), 3 (when the Target is in Orca and the CLI or the
  launch failed).
- The Companion writes PARTIAL without response and every other frame with
  response.
- On (re)connect the Device sends HELLO; the Companion answers HELLO_ACK, then
  TARGET_STATE. The Target lives in the Companion's
  `~/.config/vibe-voice/target.json`, so it survives Device and Companion
  restarts.
- If the link drops during a Dictation, both sides abandon it without inserting.
