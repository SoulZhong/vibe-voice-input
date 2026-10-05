//! Target configuration (`targets.toml`) and the persisted selection
//! (`state.toml`), both under `~/.config/vibe-voice/`.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TargetType {
    /// Paste into whatever app is frontmost.
    Follow,
    /// A Mac app identified by bundle id (or app path).
    App,
    /// Opens the Orca Session sub-list.
    Orca,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TargetConfig {
    pub id: String,
    pub name: String,
    pub kind: TargetType,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bundle_id: Option<String>,
    /// Used to resolve the bundle id when `bundle_id` is not given.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub app_path: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct TargetsFile {
    #[serde(rename = "target")]
    pub targets: Vec<TargetConfig>,
}

/// Written on first run. Bundle ids were read from the apps' Info.plist.
pub const DEFAULT_TARGETS_TOML: &str = r#"# Vibe Voice Targets, shown on the Device in this order.
# kind = "follow" pastes into the frontmost app, "app" activates the app first,
# "orca" opens the list of live Orca terminals (Orca Sessions).
# An "app" needs bundle_id, or app_path to read the bundle id from.
# Apps that are not running are reported, never launched.

[[target]]
id = "follow"
name = "跟随当前焦点"
kind = "follow"

[[target]]
id = "orca"
name = "Orca"
kind = "orca"
bundle_id = "com.stablyai.orca"

[[target]]
id = "ghostty"
name = "Ghostty"
kind = "app"
bundle_id = "com.mitchellh.ghostty"

[[target]]
id = "cursor"
name = "Cursor"
kind = "app"
bundle_id = "com.todesktop.230313mzl4w4u92"

[[target]]
id = "wechat"
name = "微信"
kind = "app"
bundle_id = "com.tencent.xinWeChat"

[[target]]
id = "wework"
name = "企业微信"
kind = "app"
bundle_id = "com.tencent.WeWorkMac"

# /Applications/ChatGPT.app (its bundle id is com.openai.codex).
[[target]]
id = "chatgpt"
name = "ChatGPT / Codex"
kind = "app"
bundle_id = "com.openai.codex"

# Reserved: a separate Codex desktop app, resolved from its path when installed.
[[target]]
id = "codex-desktop"
name = "Codex 桌面版"
kind = "app"
app_path = "/Applications/Codex.app"
"#;

impl TargetsFile {
    pub fn defaults() -> Self {
        Self::parse(DEFAULT_TARGETS_TOML).expect("built-in defaults are valid")
    }

    pub fn parse(text: &str) -> Result<Self, String> {
        let file: TargetsFile = toml::from_str(text).map_err(|e| e.to_string())?;
        file.validate()?;
        Ok(file)
    }

    fn validate(&self) -> Result<(), String> {
        if self.targets.is_empty() {
            return Err("no [[target]] entries".into());
        }
        if self.targets.len() > 24 {
            return Err("at most 24 targets fit the Device's list".into());
        }
        let mut seen = std::collections::HashSet::new();
        for t in &self.targets {
            if t.id.is_empty() || t.name.is_empty() {
                return Err("every target needs a non-empty id and name".into());
            }
            if !seen.insert(t.id.as_str()) {
                return Err(format!("duplicate target id {:?}", t.id));
            }
            if t.kind == TargetType::App && t.bundle_id.is_none() && t.app_path.is_none() {
                return Err(format!("app target {:?} needs bundle_id or app_path", t.id));
            }
        }
        Ok(())
    }

    /// Load from `path`; create it with defaults if missing. An invalid file is
    /// left untouched and the defaults are used for this run.
    pub fn load_or_create(path: &Path) -> Self {
        match std::fs::read_to_string(path) {
            Ok(text) => match Self::parse(&text) {
                Ok(f) => f,
                Err(e) => {
                    log::error!("{}: {e}; using built-in defaults", path.display());
                    Self::defaults()
                }
            },
            Err(_) => {
                if let Some(dir) = path.parent() {
                    let _ = std::fs::create_dir_all(dir);
                }
                if let Err(e) = std::fs::write(path, DEFAULT_TARGETS_TOML) {
                    log::warn!("cannot write {}: {e}", path.display());
                } else {
                    log::info!("created {}", path.display());
                }
                Self::defaults()
            }
        }
    }
}

/// The selected Target, persisted so it survives Device reboots.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct SavedState {
    /// Target id from `targets.toml` (for Orca Sessions: the Orca entry's id).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub selected: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub orca: Option<SavedOrcaSession>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SavedOrcaSession {
    pub handle: String,
    #[serde(default)]
    pub leaf_id: String,
    #[serde(default)]
    pub label: String,
}

impl SavedState {
    pub fn parse(text: &str) -> Self {
        toml::from_str(text).unwrap_or_else(|e| {
            log::warn!("ignoring unreadable state file: {e}");
            Self::default()
        })
    }

    pub fn load(path: &Path) -> Self {
        std::fs::read_to_string(path)
            .map(|t| Self::parse(&t))
            .unwrap_or_default()
    }

    pub fn to_toml(&self) -> String {
        toml::to_string(self).unwrap_or_default()
    }

    pub fn save(&self, path: &Path) {
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        let tmp = path.with_extension("toml.tmp");
        let body = format!(
            "# Written by Vibe Voice: the Target selected on the Device.\n{}",
            self.to_toml()
        );
        if std::fs::write(&tmp, body)
            .and_then(|_| std::fs::rename(&tmp, path))
            .is_err()
        {
            log::warn!("cannot save {}", path.display());
        }
    }
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
    fn defaults_are_in_decided_order() {
        let f = TargetsFile::defaults();
        let ids: Vec<_> = f.targets.iter().map(|t| t.id.as_str()).collect();
        assert_eq!(
            ids,
            [
                "follow",
                "orca",
                "ghostty",
                "cursor",
                "wechat",
                "wework",
                "chatgpt",
                "codex-desktop"
            ]
        );
        assert_eq!(f.targets[0].kind, TargetType::Follow);
        assert_eq!(f.targets[0].name, "跟随当前焦点");
        assert_eq!(f.targets[1].kind, TargetType::Orca);
        assert_eq!(
            f.targets[4].bundle_id.as_deref(),
            Some("com.tencent.xinWeChat")
        );
        assert_eq!(f.targets[7].bundle_id, None);
        assert_eq!(
            f.targets[7].app_path.as_deref(),
            Some("/Applications/Codex.app")
        );
    }

    #[test]
    fn rejects_invalid_files() {
        assert!(TargetsFile::parse("").is_err());
        assert!(TargetsFile::parse("[[target]]\nid='a'\nname='A'\nkind='app'\n").is_err());
        assert!(TargetsFile::parse(
            "[[target]]\nid='a'\nname='A'\nkind='follow'\n[[target]]\nid='a'\nname='B'\nkind='follow'\n"
        )
        .is_err());
        assert!(TargetsFile::parse("[[target]]\nid='a'\nname='A'\nkind='bogus'\n").is_err());
        assert!(
            TargetsFile::parse("[[target]]\nid='a'\nname='A'\nkind='app'\nbundle_id='x.y'\n")
                .is_ok()
        );
    }

    #[test]
    fn load_or_create_writes_defaults() {
        let dir = std::env::temp_dir().join(format!("vv-cfg-{}", std::process::id()));
        let path = dir.join("targets.toml");
        let _ = std::fs::remove_dir_all(&dir);
        let f = TargetsFile::load_or_create(&path);
        assert_eq!(f, TargetsFile::defaults());
        assert_eq!(
            std::fs::read_to_string(&path).unwrap(),
            DEFAULT_TARGETS_TOML
        );
        // Invalid file is kept, defaults used.
        std::fs::write(&path, "garbage = [").unwrap();
        assert_eq!(TargetsFile::load_or_create(&path), TargetsFile::defaults());
        assert_eq!(std::fs::read_to_string(&path).unwrap(), "garbage = [");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn state_roundtrip() {
        let s = SavedState {
            selected: Some("orca".into()),
            orca: Some(SavedOrcaSession {
                handle: "term_1".into(),
                leaf_id: "l".into(),
                label: "repo · 标题".into(),
            }),
        };
        assert_eq!(SavedState::parse(&s.to_toml()), s);
        assert_eq!(SavedState::parse("not toml ["), SavedState::default());
        let dir = std::env::temp_dir().join(format!("vv-state-{}", std::process::id()));
        let path = dir.join("state.toml");
        s.save(&path);
        assert_eq!(SavedState::load(&path), s);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
