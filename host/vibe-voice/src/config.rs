//! Built-in Supported Apps, the stored Target (`target.json`) and the
//! configuration directory `~/.config/vibe-voice/`.
//!
//! Older releases wrote `targets.toml` and `state.toml` there; they are left
//! on disk untouched and no longer read. `vocabulary.txt` in the same
//! directory is still used by `speech`.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// One app Vibe Voice can deliver to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SupportedApp {
    /// Name shown on the Device.
    pub name: &'static str,
    /// Bundle id, read from the installed app's `Info.plist`.
    pub bundle_id: &'static str,
}

/// Index of Orca in [`SUPPORTED_APPS`].
pub const ORCA: usize = 0;
pub const ORCA_BUNDLE_ID: &str = "com.stablyai.orca";

/// The Supported Apps in Device order. Root row `i` opens list `i + 1`.
pub const SUPPORTED_APPS: [SupportedApp; 4] = [
    SupportedApp {
        name: "Orca",
        bundle_id: ORCA_BUNDLE_ID,
    },
    SupportedApp {
        name: "微信",
        bundle_id: "com.tencent.xinWeChat",
    },
    // /Applications/ChatGPT.app; its bundle id is com.openai.codex.
    SupportedApp {
        name: "ChatGPT",
        bundle_id: "com.openai.codex",
    },
    SupportedApp {
        name: "企业微信",
        bundle_id: "com.tencent.WeWorkMac",
    },
];

/// Index of the Supported App with `bundle_id`.
pub fn supported_app(bundle_id: &str) -> Option<usize> {
    SUPPORTED_APPS.iter().position(|a| a.bundle_id == bundle_id)
}

/// The Target, persisted so it survives restarts.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum StoredTarget {
    /// A Supported App other than Orca: whatever chat it shows at Insert time.
    App { bundle_id: String },
    /// One Orca Session. `leaf_id` finds it again after Orca re-issued
    /// handles; `worktree` and `title` label it while Orca is not running.
    Orca {
        handle: String,
        #[serde(default)]
        leaf_id: String,
        #[serde(default)]
        worktree: String,
        #[serde(default)]
        title: String,
    },
}

impl StoredTarget {
    /// `None` for a missing or unreadable file, or an app no longer supported.
    pub fn load(path: &Path) -> Option<Self> {
        let text = std::fs::read_to_string(path).ok()?;
        let t: Self = match serde_json::from_str(&text) {
            Ok(t) => t,
            Err(e) => {
                log::warn!("ignoring {}: {e}", path.display());
                return None;
            }
        };
        match &t {
            StoredTarget::App { bundle_id }
                if supported_app(bundle_id).is_none_or(|i| i == ORCA) =>
            {
                None
            }
            _ => Some(t),
        }
    }

    pub fn save(&self, path: &Path) {
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let tmp = path.with_extension("json.tmp");
        let body = serde_json::to_string_pretty(self).unwrap_or_default();
        if std::fs::write(&tmp, body + "\n")
            .and_then(|_| std::fs::rename(&tmp, path))
            .is_err()
        {
            log::warn!("cannot save {}", path.display());
        }
    }
}

/// Where the Target is stored.
pub fn target_path() -> PathBuf {
    config_dir().join("target.json")
}

pub fn config_dir() -> PathBuf {
    if let Ok(dir) = std::env::var("VIBE_VOICE_CONFIG_DIR") {
        return PathBuf::from(dir);
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    PathBuf::from(home).join(".config").join("vibe-voice")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn supported_apps_in_decided_order() {
        let names: Vec<_> = SUPPORTED_APPS.iter().map(|a| a.name).collect();
        assert_eq!(names, ["Orca", "微信", "ChatGPT", "企业微信"]);
        assert_eq!(SUPPORTED_APPS[ORCA].bundle_id, ORCA_BUNDLE_ID);
        assert_eq!(supported_app("com.tencent.xinWeChat"), Some(1));
        assert_eq!(supported_app("com.openai.codex"), Some(2));
        assert_eq!(supported_app("com.tencent.WeWorkMac"), Some(3));
        assert_eq!(supported_app("com.mitchellh.ghostty"), None);
    }

    #[test]
    fn stored_target_roundtrip() {
        let dir = std::env::temp_dir().join(format!("vv-target-{}", std::process::id()));
        let path = dir.join("target.json");
        let _ = std::fs::remove_dir_all(&dir);
        assert_eq!(StoredTarget::load(&path), None);
        let orca = StoredTarget::Orca {
            handle: "term_1".into(),
            leaf_id: "leaf".into(),
            worktree: "my-passport".into(),
            title: "语音输入".into(),
        };
        orca.save(&path);
        assert_eq!(StoredTarget::load(&path), Some(orca));
        let app = StoredTarget::App {
            bundle_id: "com.tencent.xinWeChat".into(),
        };
        app.save(&path);
        assert_eq!(StoredTarget::load(&path), Some(app));
        // Unsupported apps and garbage are ignored.
        std::fs::write(&path, r#"{"app":{"bundle_id":"com.mitchellh.ghostty"}}"#).unwrap();
        assert_eq!(StoredTarget::load(&path), None);
        std::fs::write(&path, "not json").unwrap();
        assert_eq!(StoredTarget::load(&path), None);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
