**English** · [简体中文](README.zh_CN.md)

# Vibe Voice Input

Voice input for vibe coding. Speak into a wearable FoloToy AI Passport (ESP32-C3),
and the recognized text lands in the conversation you are working in on your Mac:
an Orca terminal session running a coding agent, or the open chat in WeChat,
ChatGPT or WeCom.

- **Device firmware** (`main/`): Bluetooth LE remote with a 240×320 Chinese UI.
  It streams 16 kHz ADPCM audio while you speak and shows live text, the current
  Target and its app logo.
- **macOS Companion** (`host/vibe-voice/`, Rust): pairs with the device, recognizes
  speech on-device with Apple Speech (Chinese and English mixed, streaming), and
  inserts, submits or undoes text in the Target.

## Controls

| Button | Idle | While dictating |
| --- | --- | --- |
| OK (click) | Start dictation | Stop and insert the text (never presses Enter) |
| DOWN | Submit (Enter) | — |
| UP | Undo the last inserted text | Cancel the dictation |
| OK (long press) | Open the picker to jump to Orca / WeChat / ChatGPT / WeCom conversations | — |
| OK (double press) | Start or stop a Voice Notes recording on the Mac | Same, without touching the dictation |

The Target follows Mac focus: whenever a supported app is frontmost it becomes
that app's current conversation; otherwise the last Target is kept. With no Target
yet, Orca's current conversation is used.

## Quick start

1. **Flash the firmware** (ESP-IDF 5.5.3 activated): run `./tools/validate.sh`,
   then write `build/vibe-voice-input-full.bin` at `0x0`. Flashing the merged
   image resets stored pairing data.
2. **Build the Companion**: `cd host/vibe-voice && ./scripts/bundle.sh`, then open
   `target/bundle/VibeVoice.app`. Grant Bluetooth, Speech Recognition and
   Accessibility when asked.
3. **Pair**: the device shows a 6-digit passkey; type it into the macOS prompt.

Details: [Companion README](host/vibe-voice/README.md) ·
[firmware](docs/vibe-voice/firmware.md) · [BLE protocol](docs/vibe-voice/protocol.md) ·
[glossary](docs/vibe-voice/CONTEXT.md).

## Development

Read [`AGENTS.md`](AGENTS.md) first. Run `./tools/validate.sh --static` while
iterating and the full `./tools/validate.sh` before delivery; run `cargo test` in
`host/vibe-voice`. The board baseline, BSP and hardware guide come from the
upstream project and are documented under [`docs/`](docs/README.md).

## License and credits

- This project is licensed under the [GNU Affero General Public License v3.0 or
  later](LICENSE).
- It is derived from [FoloToy AI Passport](https://gitee.com/FoloToy/ai-passport),
  Copyright (c) 2026 FoloToy, released under the
  [MIT License](LICENSES/MIT-FoloToy.txt). That notice applies to the inherited code.
- The Chinese UI fonts are generated from Noto Sans SC under the SIL Open Font
  License 1.1 ([details](assets/fonts/vibe-voice/README.md)).
- The app logos in `assets/icons/vibe-voice/` are trademarks of their respective
  owners, extracted from locally installed apps for identification only
  ([details](assets/icons/vibe-voice/README.md)).
