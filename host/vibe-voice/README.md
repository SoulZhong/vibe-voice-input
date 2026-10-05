**English** · [简体中文](README.zh_CN.md)

# Vibe Voice Companion (macOS)

The **Companion** for Vibe Voice input: it pairs with the AI Passport
(**Device**) over Bluetooth LE, recognizes the streamed speech with Apple
Speech (Mandarin, `zh-CN`, on-device when available), and delivers each
**Segment** to the **Target** — the current chat of a Supported App, or a
live Orca terminal.
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
rebuilding run `tccutil reset Accessibility cn.folotoy.vibevoice` and grant it
again, or bundle with `VV_SIGN_IDENTITY="<keychain signing identity>"` so the
grant survives rebuilds.

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

## Target

The Supported Apps are built in, in this order: Orca, WeChat
(`com.tencent.xinWeChat`), ChatGPT (`/Applications/ChatGPT.app`, bundle id
`com.openai.codex`) and WeCom (`com.tencent.WeWorkMac`); bundle ids were read
from the installed apps' `Info.plist`. Nothing is configured.

The Target is stored in `~/.config/vibe-voice/target.json`, so it survives
restarts of the Device and the Companion:

- **Follows focus.** Whenever a Supported App is frontmost (checked every 2 s
  and again right before each Insert and Submit), the Target becomes that app's
  Current Conversation: for Orca the active leaf terminal of the active tab in
  Orca's active worktree, for the other apps whatever chat they show. Focus on
  any other app leaves the Target unchanged.
- **Default.** With no Target yet, or when the targeted Orca Session closed,
  Orca's Current Conversation is used (and stored) silently. If Orca is not
  running, an Insert or Submit launches it with `orca open` (up to 20 s).
- **Jump.** In the Device picker (long OK) choose an app, then a conversation:
  it becomes the Target and comes to the front. The Orca list shows Orca
  Sessions across worktrees, Current Conversation first; the other apps show
  their one Current Conversation.

Delivery always brings the Target to the front first:

- **Orca Session**: `orca terminal switch` and Orca is activated, then text
  goes through `orca terminal send --text`, Submit through `--enter`, Undo sends
  DEL characters. No clipboard. Line breaks in a Segment become spaces so
  nothing is submitted by accident.
- **WeChat, ChatGPT, WeCom**: never launched. If the app is not running the
  Device gets `TARGET_UNAVAILABLE`. Otherwise the Companion activates it, pastes
  the Segment with Cmd+V and restores the previous clipboard about 0.4 s later.
  Submit presses Return; Undo presses Delete once per character of the last
  Segment. The focused window title is shown on the Device as the Target Title.
- **Undo** goes to where the last Segment went, even if the Target changed since.

Older releases wrote `targets.toml` and `state.toml` in the same directory;
they are no longer read and are left untouched. Optional
`~/.config/vibe-voice/vocabulary.txt` adds one phrase per line to bias
recognition toward your project's terms.

## Voice Notes Recording

Double-pressing OK on the Device starts a meeting recording in the Mac app
Voice Notes (`com.teemo.voice-notes`), or stops the one in progress. It is
recorded by the Mac's microphone and is independent of Dictation: you can
dictate while it records, and a double press while dictating never ends the
Dictation.

- The Companion talks to Voice Notes over its socket
  `~/Library/Application Support/com.teemo.voice-notes/mcp.sock` (or
  `$VN_APP_DATA/mcp.sock`) with `{"op":"status"}`, `{"op":"start"}` and
  `{"op":"stop"}`, on a separate thread, so Dictation audio never waits for it.
- If Voice Notes is not running, a start launches it in the background
  (`open -g -b com.teemo.voice-notes`) and waits up to 20 s for the socket.
- Voice Notes must allow control: enable "allow AI to control recording" on
  its AI page, otherwise the Device shows the control-not-allowed toast.
- Start risks reported by Voice Notes (Bluetooth microphone, Voice Isolation)
  do not stop the recording; the Device shows a warning toast.
- The status is polled every 2 s while the Device is linked, so recordings
  started, paused or stopped in Voice Notes show on the Device too.

## Diagnostics

```bash
target/release/vibe-voice --check        # permission and recognizer status
target/release/vibe-voice --notes-status # Voice Notes recording status (read-only)
target/release/vibe-voice --orca-list    # Orca Sessions as the Device sees them, Current Conversation marked *
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
| Device shows STATUS 2 | The Target is in WeChat, ChatGPT or WeCom: grant Accessibility (re-add after each ad-hoc rebuild). |
| Device shows STATUS 3 | Orca could not be launched or its CLI failed: check `orca status` and the `orca` CLI (`/usr/local/bin/orca`, or set `VIBE_VOICE_ORCA`). |
| Device shows STATUS 4 | `zh-CN` recognizer unavailable: check network or install the dictation language. |
| Text appears in the wrong conversation | Check the Device's top bar before speaking: focusing a Supported App retargets within 2 s; Jump from the picker to pick another. |
| Paste lands but clipboard is lost | Another copy happened within 0.4 s; the newer clipboard is kept on purpose. |

## Source layout

| File | Role |
| --- | --- |
| `src/protocol.rs` | Frame codec, 180-byte limit, UTF-8 tail cut |
| `src/adpcm.rs` | IMA-ADPCM decoder and encoder (golden vector tests) |
| `src/audio.rs` | AUDIO frames to PCM, silence for lost frames |
| `src/session.rs` | Companion state machine behind `Injector` / `Recognizer` / `OrcaApi` traits |
| `src/config.rs` | Supported Apps and the stored Target (`target.json`) |
| `src/voice_notes.rs` | Voice Notes socket client, launch, and the worker thread |
| `src/orca.rs` | Orca CLI client: Current Conversation from `worktree ps` and visual layouts, launch, send |
| `src/ble.rs`, `src/speech.rs`, `src/inject_macos.rs`, `src/ui.rs` | macOS glue |
