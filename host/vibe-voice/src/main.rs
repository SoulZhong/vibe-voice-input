//! Vibe Voice Companion for macOS.
//!
//! Usage:
//!   vibe-voice                     run the Companion (menu bar + BLE)
//!   vibe-voice --simulate <file>   feed a 16 kHz mono 16-bit WAV/PCM file
//!                                  through ADPCM, the protocol logic and
//!                                  Apple Speech; prints frames, inserts nothing
//!   vibe-voice --orca-list         print Orca Sessions as the Device sees them
//!   vibe-voice --check             print permission and recognizer status

#[cfg(not(target_os = "macos"))]
fn main() {
    eprintln!("vibe-voice runs on macOS only");
    std::process::exit(1);
}

#[cfg(target_os = "macos")]
mod app;

#[cfg(target_os = "macos")]
fn main() {
    app::main();
}
