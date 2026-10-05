//! Launch at login: the user's choice (`settings.json`) and where the app runs.
//!
//! The Companion registers itself as a login item (SMAppService, see
//! `login_macos`) only when it runs from an installed copy in `/Applications`
//! or `~/Applications`, so development builds in `target/` never create login
//! items pointing at build directories.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// `~/.config/vibe-voice/settings.json`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Settings {
    #[serde(default = "default_true")]
    pub launch_at_login: bool,
}

fn default_true() -> bool {
    true
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            launch_at_login: true,
        }
    }
}

impl Settings {
    /// Defaults for a missing or unreadable file.
    pub fn load(path: &Path) -> Self {
        let Ok(text) = std::fs::read_to_string(path) else {
            return Self::default();
        };
        serde_json::from_str(&text).unwrap_or_else(|e| {
            log::warn!("ignoring {}: {e}", path.display());
            Self::default()
        })
    }

    pub fn save(&self, path: &Path) {
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let tmp = path.with_extension("json.tmp");
        let body = serde_json::to_string_pretty(self).unwrap_or_default() + "\n";
        if std::fs::write(&tmp, body)
            .and_then(|_| std::fs::rename(&tmp, path))
            .is_err()
        {
            log::warn!("cannot save {}", path.display());
        }
    }
}

pub fn settings_path() -> PathBuf {
    crate::config::config_dir().join("settings.json")
}

/// The `.app` bundle containing `exe` (`X.app/Contents/MacOS/<binary>`).
pub fn bundle_of(exe: &Path) -> Option<&Path> {
    let macos = exe.parent()?;
    let contents = macos.parent()?;
    let app = contents.parent()?;
    let ok = macos.file_name()? == "MacOS"
        && contents.file_name()? == "Contents"
        && app.extension()? == "app";
    ok.then_some(app)
}

/// Whether `exe` runs from an app bundle installed directly in
/// `/Applications` or `<home>/Applications`.
pub fn is_installed_location(exe: &Path, home: &Path) -> bool {
    let Some(app) = bundle_of(exe) else {
        return false;
    };
    let Some(dir) = app.parent() else {
        return false;
    };
    dir == Path::new("/Applications") || dir == home.join("Applications")
}

/// Whether the running binary is an installed copy.
pub fn running_installed() -> bool {
    let Ok(exe) = std::env::current_exe() else {
        return false;
    };
    let exe = exe.canonicalize().unwrap_or(exe);
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_default();
    is_installed_location(&exe, &home)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn settings_default_load_save() {
        let dir = std::env::temp_dir().join(format!("vv-settings-{}", std::process::id()));
        let path = dir.join("settings.json");
        let _ = std::fs::remove_dir_all(&dir);
        assert!(Settings::load(&path).launch_at_login, "missing file: on");
        let off = Settings {
            launch_at_login: false,
        };
        off.save(&path);
        assert_eq!(Settings::load(&path), off);
        std::fs::write(&path, "{}").unwrap();
        assert!(Settings::load(&path).launch_at_login, "missing key: on");
        std::fs::write(&path, "not json").unwrap();
        assert!(Settings::load(&path).launch_at_login, "garbage: on");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn installed_location() {
        let home = Path::new("/Users/me");
        let yes = [
            "/Applications/VibeVoice.app/Contents/MacOS/vibe-voice",
            "/Users/me/Applications/VibeVoice.app/Contents/MacOS/vibe-voice",
        ];
        for p in yes {
            assert!(is_installed_location(Path::new(p), home), "{p}");
        }
        let no = [
            // Development builds: bundle in target/, or the bare binary.
            "/Users/me/src/vv/host/vibe-voice/target/bundle/VibeVoice.app/Contents/MacOS/vibe-voice",
            "/Users/me/src/vv/host/vibe-voice/target/release/vibe-voice",
            "/Users/me/src/vv/host/vibe-voice/target/debug/vibe-voice",
            // Nested deeper or another user's folder.
            "/Applications/Tools/VibeVoice.app/Contents/MacOS/vibe-voice",
            "/Users/other/Applications/VibeVoice.app/Contents/MacOS/vibe-voice",
            "/Applications/vibe-voice",
            "/Applications/VibeVoice.app/Contents/Resources/vibe-voice",
        ];
        for p in no {
            assert!(!is_installed_location(Path::new(p), home), "{p}");
        }
        assert_eq!(
            bundle_of(Path::new(
                "/Applications/VibeVoice.app/Contents/MacOS/vibe-voice"
            )),
            Some(Path::new("/Applications/VibeVoice.app"))
        );
    }
}
