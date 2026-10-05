**English** · [简体中文](protocol.zh_CN.md)

# Vibe Voice BLE protocol (v1)

The contract between the **Device** (AI Passport firmware in `main/`) and the
**Companion** (macOS app in `host/vibe-voice/`). Terms follow
[`CONTEXT.md`](CONTEXT.md).

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
| `0x01` | HELLO | `ver u8` (=1), `fw text` | Sent once after the TX CCCD is enabled. |
| `0x10` | DICT_START | `dict u8` | A Dictation began. `dict` increments per Dictation (wraps). |
| `0x11` | AUDIO | `dict u8`, `seq u16`, `pred i16`, `index u8`, `adpcm[160]` | 20 ms of audio = 320 samples. `pred`/`index` are the encoder state **before** this frame, so every frame decodes on its own; a gap in `seq` means lost frames. |
| `0x12` | DICT_STOP | `dict u8` | User ended the Dictation (OK) or the 5-minute limit hit. Companion finalizes, Inserts and replies RESULT. |
| `0x13` | DICT_CANCEL | `dict u8` | Cancel: discard, insert nothing, reply RESULT status `CANCELLED`. |
| `0x20` | SUBMIT | — | Press Enter in the Target. Reply ACTION_RESULT. |
| `0x21` | UNDO | — | Remove the latest Segment from the Target. Reply ACTION_RESULT. |
| `0x30` | TARGETS_REQ | `list u8` | Ask for a Target list. `0` = root list, `1` = Orca Sessions. |
| `0x31` | TARGET_SELECT | `list u8`, `index u8` | Choose an item from the last list sent. Reply TARGET_STATE. |

### Companion → Device

| Type | Name | Payload | Meaning |
| --- | --- | --- | --- |
| `0x81` | HELLO_ACK | `ver u8` | Followed by TARGET_STATE for the current Target. |
| `0x82` | STATUS | `code u8`, `text` | Companion-level problem shown on the Device (codes below, `0` clears). |
| `0x90` | PARTIAL | `dict u8`, `text` | Latest Partial Text (tail). Replaces the previous one. |
| `0x91` | RESULT | `dict u8`, `status u8`, `text` | Dictation outcome; `text` is the Segment (tail) when inserted. |
| `0xA0` | ACTION_RESULT | `action u8` (`0x20`/`0x21`), `status u8` | Outcome of SUBMIT/UNDO. |
| `0xB0` | TARGET_ITEM | `list u8`, `index u8`, `count u8`, `flags u8`, `label text` | One list row. `flags`: bit0 current selection, bit1 opens a sub-list, bit2 app not running. |
| `0xB1` | TARGET_END | `list u8`, `count u8` | List complete. |
| `0xB2` | TARGET_STATE | `status u8`, `kind u8`, `label text` | Current Target. `kind`: 0 follow focus, 1 App Target, 2 Orca Session. `label` is `"<name> · <Target Title>"` when a title is known. |

### Status codes

RESULT / ACTION_RESULT / TARGET_STATE `status`:

| Code | Name | Meaning |
| --- | --- | --- |
| 0 | OK | Inserted / done / Target usable. |
| 1 | EMPTY | Nothing recognized; nothing inserted. |
| 2 | CANCELLED | Dictation cancelled. |
| 3 | TARGET_UNAVAILABLE | App not running or Orca Session gone; never auto-launched. |
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
- The Device closes the picker if TARGET_END does not arrive within 5 s, and
  gives up on RESULT after 20 s.

## Behaviour rules

- An Insert never presses Enter. Only SUBMIT does.
- UNDO deletes as many characters (grapheme clusters) as the latest Segment had,
  once. After UNDO, a successful SUBMIT, or the start of a new Dictation, there is
  nothing to undo until the next Segment.
- A row whose flags have bit1 set opens its sub-list: the Device sends TARGETS_REQ
  for it rather than TARGET_SELECT. A TARGET_SELECT on such a row leaves the Target
  unchanged and is answered with TARGET_STATE.
- RESULT text is empty when nothing was inserted. DICT_STOP for an unknown
  Dictation is answered with RESULT `EMPTY`.
- STATUS carries one code at a time, by priority: 1, 4, 2 (App Targets only),
  3 (Orca Sessions only).
- The Companion writes PARTIAL without response and every other frame with
  response.
- Before inserting into an App Target the Companion activates the app, reads its
  focused window title (Target Title), pastes with ⌘V, and restores the clipboard.
  It never launches an app that is not running.
- For an Orca Session the Companion uses `orca terminal send` (text, Enter, or
  backspaces) and `orca terminal switch` to bring it forward; no clipboard.
- On (re)connect the Device sends HELLO; the Companion answers HELLO_ACK, then
  TARGET_STATE. The selected Target lives in the Companion's config, so it
  survives Device reboots.
- If the link drops during a Dictation, both sides abandon it without inserting.
