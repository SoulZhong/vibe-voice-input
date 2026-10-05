//! Vibe Voice Companion: voice input for vibe coding with the FoloToy AI Passport.
//!
//! Pure, unit-tested modules (`protocol`, `adpcm`, `audio`, `config`, `orca`,
//! `session`) are kept apart from the macOS glue (`ble`, `speech`,
//! `inject_macos`, `ui`); `orca` keeps its process runner behind a trait.

pub mod adpcm;
pub mod alerts;
pub mod audio;
pub mod config;
pub mod login_item;
pub mod orca;
pub mod protocol;
pub mod session;
#[cfg(unix)]
pub mod voice_notes;

#[cfg(target_os = "macos")]
pub mod ble;
#[cfg(target_os = "macos")]
pub mod inject_macos;
#[cfg(target_os = "macos")]
pub mod login_macos;
#[cfg(target_os = "macos")]
pub mod speech;
#[cfg(target_os = "macos")]
pub mod ui;
