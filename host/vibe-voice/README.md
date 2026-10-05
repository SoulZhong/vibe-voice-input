**English** · [简体中文](README.zh_CN.md)

# Vibe Voice Companion (macOS)

The **Companion** for Vibe Voice input: it pairs with the AI Passport
(**Device**) over Bluetooth LE, recognizes the streamed speech with Apple
Speech (Mandarin, `zh-CN`, on-device when available), and delivers each
**Segment** to the chosen **Target** — a Mac app or a live Orca terminal.
Terms follow [`docs/vibe-voice/CONTEXT.md`](../../docs/vibe-voice/CONTEXT.md);
the wire format is [`docs/vibe-voice/protocol.md`](../../docs/vibe-voice/protocol.md).

## Requirements

- macOS 13 or later (Apple Silicon or Intel), Bluetooth on.
- Rust 1.88 or later (`cargo`).
- The Mandarin on-device speech model (System Settings > Keyboard > Dictation,
  add Chinese) for offline recognition. Without it Apple's server is used.

## Build and test

```bash
cd host/vibe-voice
cargo test                       # protocol, ADPCM golden vector, session logic
cargo clippy --all-targets -- -D warnings
scripts/bundle.sh                # release build -> target/bundle/VibeVoice.app
```

`bundle.sh` writes `VibeVoice.app` (bundle id `cn.folotoy.vibevoice`, menu bar
agent without a Dock icon) and signs it ad hoc. Copy it to `/Applications` if
you like and open it from Finder or with `open`.

## Permissions

Start the app with `open target/bundle/VibeVoice.app` (or from Finder). macOS
asks for each permission once:

| Permission | Why | Where to fix |
| --- | --- | --- |
| Bluetooth | Talk to the Device | Privacy & Security > Bluetooth |
| Speech Recognition | Turn audio into text | Privacy & Security > Speech Recognition |
| Accessibility | Activate apps, read window titles, send Cmd+V / Return / Delete | Privacy & Security > Accessibility |

No microphone permission is needed: audio comes from the Device. Missing
permissions are reported to the Device as STATUS codes (1 = speech,
2 = Accessibility). The ad-hoc signature changes on every rebuild, so after
rebuilding remove and re-add Vibe Voice under Accessibility.

Run the app through `open` or Finder, not by executing the binary from a
terminal: macOS asks the *launching* app (your terminal) for the Speech usage
description, and a terminal without one gets the process killed. The binary
therefore refuses to request Speech permission outside the app bundle.

## Pairing

1. Turn on the Device and start Vibe Voice; the menu bar shows `VV ○` while it
   scans for a Device advertising `VibeVoice-XXXX`.
2. On first connection macOS shows a pairing prompt. Type the 6-digit passkey
   shown on the Device screen. The Companion retries for up to two minutes
   while you type.
3. The menu bar turns to `VV ●`. The bond is remembered by both sides;
   reconnection after sleep, reboot or range loss is automatic.

To pin one Device when several are near, start with
`VIBE_VOICE_DEVICE=VibeVoice-XXXX`. To re-pair, remove the Device in
System Settings > Bluetooth and erase its bond on the Device.

## Targets

`~/.config/vibe-voice/targets.toml` is created on first run. Targets appear on
the Device in file order. The defaults are follow focus, Orca, Ghostty, Cursor,
WeChat, WeCom, ChatGPT / Codex (`/Applications/ChatGPT.app`, bundle id
`com.openai.codex`), and a reserved Codex desktop entry resolved from
`/Applications/Codex.app` when installed. Bundle ids were read from the
installed apps' `Info.plist`.

```toml
[[target]]
id = "ghostty"          # stable id, stored in state.toml
name = "Ghostty"        # label on the Device
kind = "app"            # "follow", "app" or "orca"
bundle_id = "com.mitchellh.ghostty"
# app_path = "/Applications/Foo.app"   # alternative to bundle_id
```

The file is reloaded when the Device asks for the Target list. The selected
Target is saved in `~/.config/vibe-voice/state.toml`, so it survives Device
reboots. Optional `~/.config/vibe-voice/vocabulary.txt` adds one phrase per
line to bias recognition toward your project's terms.

Behaviour per Target:

- **App Target**: never launched. If the app is not running the Device gets
  `TARGET_UNAVAILABLE`. Otherwise the Companion activates it, reads the focused
  window title (shown on the Device as the Target Title), pastes the Segment
  with Cmd+V and restores the previous clipboard about 0.4 s later. Submit
  presses Return; Undo presses Delete once per character of the last Segment.
  Focus stays in the app.
- **Follow focus**: the same, in whatever app is frontmost. Undo goes to the
  app that received the Segment.
- **Orca Session**: chosen from the Orca sub-list (`orca terminal list`).
  Text goes through `orca terminal send --text`, Submit through `--enter`,
  Undo sends DEL characters; `orca terminal switch` brings the tab forward.
  No clipboard is used. Line breaks in a Segment become spaces so nothing is
  submitted by accident.

## Diagnostics

```bash
target/release/vibe-voice --check        # permission and recognizer status
target/release/vibe-voice --orca-list    # Orca Sessions as the Device sees them
# Recognize a 16 kHz mono 16-bit WAV through ADPCM, protocol and Apple Speech.
# Nothing is inserted; frames are printed.
say -v Tingting "<Chinese sentence>" -o /tmp/clip.wav --data-format=LEI16@16000 --file-format=WAVE
open -W -n --stdout /tmp/sim.txt target/bundle/VibeVoice.app --args --simulate /tmp/clip.wav
cat /tmp/sim.txt
```

Logs go to `~/Library/Logs/VibeVoice.log` when started from Finder (add
`--verbose` for frame-level detail).

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Stays at "searching" | Device advertising? Bluetooth permission granted? |
| Pairing prompt never appears | Remove the old bond in Bluetooth settings and on the Device, then reconnect. |
| Device shows STATUS 1 or RESULT 5 | Grant Speech Recognition, then restart the app. |
| Device shows STATUS 2 | Grant Accessibility (re-add after each rebuild). |
| Device shows STATUS 3 | Orca not running or `orca` CLI missing (`/usr/local/bin/orca`, or set `VIBE_VOICE_ORCA`). |
| Device shows STATUS 4 | `zh-CN` recognizer unavailable: check network or install the dictation language. |
| Text appears in the wrong app | Choose an App Target instead of follow focus. |
| Paste lands but clipboard is lost | Another copy happened within 0.4 s; the newer clipboard is kept on purpose. |

## Source layout

| File | Role |
| --- | --- |
| `src/protocol.rs` | Frame codec, 180-byte limit, UTF-8 tail cut |
| `src/adpcm.rs` | IMA-ADPCM decoder and encoder (golden vector tests) |
| `src/audio.rs` | AUDIO frames to PCM, silence for lost frames |
| `src/session.rs` | Companion state machine behind `Injector` / `Recognizer` / `OrcaApi` traits |
| `src/config.rs` | `targets.toml` and `state.toml` |
| `src/orca.rs` | Orca CLI client and JSON parsing |
| `src/ble.rs`, `src/speech.rs`, `src/inject_macos.rs`, `src/ui.rs` | macOS glue |
