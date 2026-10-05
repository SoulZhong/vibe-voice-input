//! Alerts: an Orca agent session finished its turn and waits for the user.
//!
//! The Orca CLI has no event stream, so the agent's state is read from the
//! terminal title in `orca terminal list --json`:
//!
//! - Claude Code puts a spinner glyph in front of the title while it works
//!   (`◐ ◑ ◒ ◓`, Braille `⠋…`, `✶ ✻ ✽ ✢ ·` and similar) and `✳` when it is idle,
//!   waiting for the user.
//! - For other agents Orca writes the title itself (its agent table in
//!   `app.asar`): `"<Agent>"` while working, `"<Agent> - action required"` when
//!   it asks for permission and `"<Agent> ready"` when idle — Codex, Cursor
//!   Agent, OpenCode, Pi, OMP, Droid, Hermes, Devin, ZCode. Orca does not
//!   synthesize a working title for Codex (it keeps Codex's own title), so a
//!   Codex turn only alerts when its title was recognizably working before.
//! - Gemini CLI: `✦ Gemini CLI` working, `◇ Gemini CLI` idle.
//!
//! Anything else (plain shells, unrecognized titles) is [`AgentState::Unknown`]
//! and never alerts.

use crate::orca::{OrcaSession, clean_title};
use crate::protocol::utf8_head;
use std::collections::HashMap;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AgentState {
    Working,
    Waiting,
    Unknown,
}

/// Titles Orca writes for agents without their own convention:
/// (working, permission, idle), from Orca's agent table.
const ORCA_AGENT_TITLES: &[(&str, &str, &str)] = &[
    ("Codex", "Codex - action required", "Codex ready"),
    ("Cursor Agent", "Cursor - action required", "Cursor ready"),
    ("OpenCode", "OpenCode - action required", "OpenCode ready"),
    ("Pi", "Pi - action required", "Pi ready"),
    ("OMP", "OMP - action required", "OMP ready"),
    ("Droid", "Droid - action required", "Droid ready"),
    ("Hermes", "Hermes - action required", "Hermes ready"),
    ("Devin", "Devin - action required", "Devin ready"),
    ("ZCode", "ZCode - action required", "ZCode ready"),
];

fn is_spinner(c: char) -> bool {
    matches!(
        c,
        '◐' | '◑'
            | '◒'
            | '◓'
            | '◴'
            | '◵'
            | '◶'
            | '◷'
            | '✶'
            | '✻'
            | '✽'
            | '✢'
            | '·'
            | '✦'
            | '⏳'
    ) || ('\u{2800}'..='\u{28FF}').contains(&c)
}

/// The agent's state as its terminal title shows it.
pub fn title_state(title: &str) -> AgentState {
    let t = title.trim();
    let Some(first) = t.chars().next() else {
        return AgentState::Unknown;
    };
    if first == '✳' || first == '◇' {
        return AgentState::Waiting;
    }
    if is_spinner(first) {
        return AgentState::Working;
    }
    for (working, permission, idle) in ORCA_AGENT_TITLES {
        if t == *working {
            return AgentState::Working;
        }
        if t == *permission || t == *idle {
            return AgentState::Waiting;
        }
    }
    AgentState::Unknown
}

/// Box drawing, block elements and similar TUI chrome.
fn is_chrome_char(c: char) -> bool {
    ('\u{2500}'..='\u{259F}').contains(&c) || matches!(c, '│' | '┃' | '╭' | '╮' | '╯' | '╰')
}

fn strip_ansi(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\u{1b}' {
            match chars.peek() {
                Some('[') => {
                    chars.next();
                    for c in chars.by_ref() {
                        if ('@'..='~').contains(&c) {
                            break;
                        }
                    }
                }
                Some(_) => {
                    chars.next();
                }
                None => {}
            }
        } else if !c.is_control() || c == '\n' {
            out.push(c);
        }
    }
    out
}

/// Whether a preview line is TUI noise rather than the agent's words.
fn is_noise(line: &str) -> bool {
    let t = line.trim();
    // Too short to say anything ("6", ":").
    if t.chars().filter(|c| c.is_alphanumeric()).count() < 2 {
        return true;
    }
    let chrome = t.chars().filter(|c| is_chrome_char(*c)).count();
    if chrome * 2 >= t.chars().count() {
        return true;
    }
    let first = t.chars().next().unwrap_or(' ');
    if is_spinner(first) || matches!(first, '✔' | '✓' | '⎿' | '>' | '❯' | '$' | '%') {
        return true;
    }
    const NOISE: &[&str] = &[
        "new task?",
        "Resume this session",
        "claude --resume",
        "/exit",
        "(base) ",
        "Switching from",
        "for agents",
        "Ran ",
    ];
    if NOISE.iter().any(|n| t.starts_with(n)) || t.starts_with('/') || t.contains(" · done ") {
        return true;
    }
    // Shell prompts such as "user@host dir %".
    (t.contains('@') && (t.ends_with('%') || t.ends_with('$') || t.ends_with('#')))
        || t.contains("--dangerously-skip-permissions")
}

/// Join wrapped terminal lines: a space only between ASCII words.
fn join_lines(a: &str, b: &str) -> String {
    if a.is_empty() {
        return b.to_owned();
    }
    let space = a.chars().last().is_some_and(|c| c.is_ascii_alphanumeric())
        && b.chars().next().is_some_and(|c| c.is_ascii_alphanumeric());
    if space {
        format!("{a} {b}")
    } else {
        format!("{a}{b}")
    }
}

/// The readable end of a terminal preview: the agent's last lines before the
/// prompt box, without status and chrome lines. `None` when nothing is left.
pub fn preview_message(preview: &str) -> Option<String> {
    let clean = strip_ansi(preview);
    let lines: Vec<&str> = clean.lines().collect();
    // Claude Code's prompt box starts at the first full-width rule; what
    // follows is the input line and the status line, not the agent's reply.
    let end = lines
        .iter()
        .position(|l| {
            let t = l.trim();
            t.chars().count() >= 8 && t.chars().all(is_chrome_char)
        })
        .unwrap_or(lines.len());
    // "※ recap: …" is Claude Code's own summary of the turn: prefer it.
    let is_recap = |l: &str| {
        let t = l.trim_start();
        t.starts_with('※') || t.starts_with("recap:")
    };
    if let Some(i) = lines[..end].iter().rposition(|l| is_recap(l)) {
        let mut text = lines[i]
            .trim_start()
            .trim_start_matches('※')
            .trim()
            .to_owned();
        if let Some(rest) = text.strip_prefix("recap:") {
            text = rest.trim().to_owned();
        }
        for l in &lines[i + 1..end] {
            let t = l.trim();
            if is_noise(t) {
                break;
            }
            text = join_lines(&text, t);
        }
        if let Some(cut) = text.find("(disable recaps") {
            text.truncate(cut);
        }
        let text = text.trim().to_owned();
        if !text.is_empty() {
            return Some(text);
        }
    }
    let kept: Vec<String> = lines[..end]
        .iter()
        .map(|l| l.trim())
        .filter(|t| !is_noise(t))
        .map(str::to_owned)
        .collect();
    let tail = &kept[kept.len().saturating_sub(3)..];
    let text = tail
        .iter()
        .fold(String::new(), |acc, l| join_lines(&acc, l));
    let text = text.trim().to_owned();
    (!text.is_empty()).then_some(text)
}

/// Shown when the preview has nothing readable.
pub const DEFAULT_MESSAGE: &str = "等待你的回复";
/// Byte budgets within one 180-byte ALERT frame (4 header bytes).
pub const LABEL_BYTES: usize = 63;
pub const MESSAGE_BYTES: usize = 112;

/// Keep the end of `s` within `max` bytes, with a leading `…` when cut.
fn tail_within(s: &str, max: usize) -> String {
    if s.len() <= max {
        return s.to_owned();
    }
    let budget = max - '…'.len_utf8();
    let mut start = s.len() - budget;
    while !s.is_char_boundary(start) {
        start += 1;
    }
    format!("…{}", &s[start..])
}

/// One Alert as sent to the Device.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Alert {
    pub id: u8,
    pub handle: String,
    pub label: String,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AlertEvent {
    Raise(Alert),
    Clear { id: u8 },
}

/// Tracks each Orca Session's agent state across polls and raises an Alert
/// on a working → waiting transition.
#[derive(Debug, Default)]
pub struct AlertTracker {
    last: HashMap<String, AgentState>,
    ids: HashMap<String, u8>,
    next_id: u8,
    pending: HashMap<String, Alert>,
}

impl AlertTracker {
    /// Stable per-session id (1..=255, reused round-robin).
    fn id_for(&mut self, handle: &str) -> u8 {
        if let Some(id) = self.ids.get(handle) {
            return *id;
        }
        self.next_id = if self.next_id == u8::MAX {
            1
        } else {
            self.next_id + 1
        };
        let id = self.next_id;
        self.ids.retain(|_, v| *v != id);
        self.ids.insert(handle.to_owned(), id);
        id
    }

    /// Feed one poll. `looking_at` is the session the user is looking at
    /// (Orca frontmost and that session its Current Conversation).
    pub fn update(
        &mut self,
        sessions: &[OrcaSession],
        looking_at: Option<&str>,
    ) -> Vec<AlertEvent> {
        let mut events = Vec::new();
        let mut seen = HashMap::new();
        for s in sessions {
            let state = if s.agent.is_some() {
                title_state(&s.raw_title)
            } else {
                AgentState::Unknown
            };
            seen.insert(s.handle.clone(), state);
            let before = self.last.get(&s.handle).copied();
            let watched = looking_at == Some(s.handle.as_str());
            if before == Some(AgentState::Working) && state == AgentState::Waiting && !watched {
                let id = self.id_for(&s.handle);
                let title = clean_title(&s.raw_title);
                let label = if s.worktree.is_empty() {
                    title
                } else if title.is_empty() {
                    s.worktree.clone()
                } else {
                    format!("{} · {title}", s.worktree)
                };
                let message = preview_message(&s.preview).unwrap_or_else(|| DEFAULT_MESSAGE.into());
                let alert = Alert {
                    id,
                    handle: s.handle.clone(),
                    label: utf8_head(&label, LABEL_BYTES).to_owned(),
                    message: tail_within(&message, MESSAGE_BYTES),
                };
                self.pending.insert(s.handle.clone(), alert.clone());
                events.push(AlertEvent::Raise(alert));
            } else if let Some(a) = self.pending.get(&s.handle)
                && (state != AgentState::Waiting || watched)
            {
                events.push(AlertEvent::Clear { id: a.id });
                self.pending.remove(&s.handle);
            }
        }
        // Sessions that disappeared: drop their state and any pending Alert.
        let gone: Vec<String> = self
            .pending
            .keys()
            .filter(|h| !seen.contains_key(*h))
            .cloned()
            .collect();
        for h in gone {
            if let Some(a) = self.pending.remove(&h) {
                events.push(AlertEvent::Clear { id: a.id });
            }
        }
        self.last = seen;
        events
    }

    /// Pending Alerts, oldest id first (to resend after HELLO).
    pub fn pending(&self) -> Vec<Alert> {
        let mut v: Vec<Alert> = self.pending.values().cloned().collect();
        v.sort_by_key(|a| a.id);
        v
    }

    /// Remove a pending Alert by id (opened or dismissed on the Device).
    pub fn take(&mut self, id: u8) -> Option<Alert> {
        let handle = self.pending.iter().find(|(_, a)| a.id == id)?.0.clone();
        self.pending.remove(&handle)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn title_states() {
        for t in [
            "◐ 修复",
            "◑ x",
            "◒ x",
            "◓ x",
            "⠋ Grok",
            "⠹ x",
            "✶ x",
            "✦ Gemini CLI",
        ] {
            assert_eq!(title_state(t), AgentState::Working, "{t}");
        }
        for t in [
            "✳ 任务列表",
            "◇ Gemini CLI",
            "Codex ready",
            "Codex - action required",
            "Cursor ready",
        ] {
            assert_eq!(title_state(t), AgentState::Waiting, "{t}");
        }
        assert_eq!(title_state("Codex"), AgentState::Working);
        for t in [
            "Terminal 1",
            "需要添加的脚本",
            "Setup",
            "",
            "Codex readyish",
            "zsh",
        ] {
            assert_eq!(title_state(t), AgentState::Unknown, "{t}");
        }
    }

    // Previews captured from `orca terminal list --json` (shortened).
    #[test]
    fn preview_messages() {
        let p = "1 项（改动小，直接修掉这次撞到的问题）。第 2 项会先摸清方式，出方案给你确认后再写。\n✻ Churned for 1m 12s · done 8:30 AM\n✔ Update installed · Restart to update\n────────────────────────────────\nPR，更新并合并 #247，打 tag 发版。";
        assert_eq!(
            preview_message(p).as_deref(),
            Some(
                "1 项（改动小，直接修掉这次撞到的问题）。第 2 项会先摸清方式，出方案给你确认后再写。"
            )
        );
        let p = "帮你起草\n12315 投诉文本和给苹果客服的书面说明。\n✻ Cooked for 1m 18s · done 7:16 PM\nnew task? /clear to save 280.4k tokens\n──────────────────────────────\nnew task? /clear to save 368.3k tokens";
        assert_eq!(
            preview_message(p).as_deref(),
            Some("帮你起草12315 投诉文本和给苹果客服的书面说明。")
        );
        let p = "or 33s · done 5:57 PM\n※ recap: 我们在写面试评价，下一步看你是否要重写。 (disable recaps in\n/config)\nnew task? /clear to save 192.3k tokens\n────────────────────────────";
        assert_eq!(
            preview_message(p).as_deref(),
            Some("我们在写面试评价，下一步看你是否要重写。")
        );
        assert_eq!(
            preview_message(
                "new task? /clear to save 1k tokens\n──────────────\n· ← 1 agent\nResume this session with:"
            ),
            None
        );
        assert_eq!(
            preview_message(
                "(base) teemo@localhost nook % claude '--dangerously-skip-permissions'"
            ),
            None
        );
        assert_eq!(
            preview_message("\u{1b}[1mDone\u{1b}[0m: all tests pass").as_deref(),
            Some("Done: all tests pass")
        );
        assert_eq!(preview_message(""), None);
        // The "※" scrolled out of the preview; one-character lines are noise.
        assert_eq!(
            preview_message("recap:\n我们在准备两份合同。\nnew task? /clear to save 1k").as_deref(),
            Some("我们在准备两份合同。")
        );
        assert_eq!(preview_message(":\n6"), None);
    }

    fn sess(h: &str, title: &str, agent: Option<&str>) -> OrcaSession {
        OrcaSession {
            handle: h.into(),
            leaf_id: String::new(),
            worktree_id: String::new(),
            worktree: "my-passport".into(),
            title: clean_title(title),
            raw_title: title.into(),
            agent: agent.map(Into::into),
            preview: "改好了，要我提交吗？\n✻ Baked for 3s".into(),
        }
    }

    fn raise_ids(ev: &[AlertEvent]) -> Vec<u8> {
        ev.iter()
            .filter_map(|e| match e {
                AlertEvent::Raise(a) => Some(a.id),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn transitions_raise_and_clear() {
        let mut t = AlertTracker::default();
        let c = Some("claude");
        // First sight never alerts, even when already waiting.
        assert!(
            t.update(&[sess("a", "◐ 修复", c), sess("b", "✳ 文档", c)], None)
                .is_empty()
        );
        // a: working -> waiting raises; b stays waiting: nothing.
        let ev = t.update(&[sess("a", "✳ 修复", c), sess("b", "✳ 文档", c)], None);
        assert_eq!(
            ev,
            [AlertEvent::Raise(Alert {
                id: 1,
                handle: "a".into(),
                label: "my-passport · 修复".into(),
                message: "改好了，要我提交吗？".into()
            })]
        );
        assert_eq!(t.pending().len(), 1);
        // Still waiting: no repeat.
        assert!(t.update(&[sess("a", "✳ 修复", c)], None).is_empty());
        // Working again: cleared; waiting again: raised with the same id.
        assert_eq!(
            t.update(&[sess("a", "◓ 修复", c)], None),
            [AlertEvent::Clear { id: 1 }]
        );
        assert_eq!(raise_ids(&t.update(&[sess("a", "✳ 修复", c)], None)), [1]);
        // The user opened it on the Mac (Orca frontmost, current): cleared.
        assert_eq!(
            t.update(&[sess("a", "✳ 修复", c)], Some("a")),
            [AlertEvent::Clear { id: 1 }]
        );
        // A transition while the user looks at it: no Alert.
        t.update(&[sess("a", "◐ 修复", c)], Some("a"));
        assert!(t.update(&[sess("a", "✳ 修复", c)], Some("a")).is_empty());
        // Session closed: its pending Alert is cleared, no Alert for it.
        t.update(&[sess("a", "◐ 修复", c)], None);
        assert_eq!(raise_ids(&t.update(&[sess("a", "✳ 修复", c)], None)), [1]);
        assert_eq!(t.update(&[], None), [AlertEvent::Clear { id: 1 }]);
        assert!(t.update(&[sess("a", "✳ 修复", c)], None).is_empty());
    }

    #[test]
    fn unknown_and_agentless_never_alert() {
        let mut t = AlertTracker::default();
        t.update(
            &[
                sess("x", "◐ build", None),
                sess("y", "Terminal 1", Some("claude")),
            ],
            None,
        );
        assert!(
            t.update(
                &[sess("x", "✳ build", None), sess("y", "✳ y", Some("claude"))],
                None
            )
            .is_empty()
        );
        // Codex through Orca's titles.
        let mut t = AlertTracker::default();
        t.update(&[sess("c", "Codex", Some("codex"))], None);
        assert_eq!(
            raise_ids(&t.update(&[sess("c", "Codex ready", Some("codex"))], None)),
            [1]
        );
    }

    #[test]
    fn take_and_limits() {
        let mut t = AlertTracker::default();
        let c = Some("claude");
        let mut long = sess("a", "◐ x", c);
        t.update(&[long.clone()], None);
        long.raw_title = format!("✳ {}", "很长的标题".repeat(20));
        long.preview = "结论".repeat(100);
        let ev = t.update(&[long], None);
        let AlertEvent::Raise(a) = &ev[0] else {
            panic!()
        };
        assert!(a.label.len() <= LABEL_BYTES && a.message.len() <= MESSAGE_BYTES);
        assert!(a.message.starts_with('…') && a.message.ends_with("结论"));
        assert_eq!(t.take(a.id).map(|x| x.handle), Some("a".into()));
        assert_eq!(t.take(a.id), None);
        // Distinct sessions get distinct ids.
        let mut t = AlertTracker::default();
        t.update(&[sess("a", "◐ a", c), sess("b", "◐ b", c)], None);
        assert_eq!(
            raise_ids(&t.update(&[sess("a", "✳ a", c), sess("b", "✳ b", c)], None)),
            [1, 2]
        );
    }
}
