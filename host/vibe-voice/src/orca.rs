//! Orca Sessions through the `orca` CLI.
//!
//! Arguments are always passed as separate argv entries (never through a
//! shell) and values use `--flag=value` so text starting with `-` cannot be
//! mistaken for a flag. The process runner is a trait so tests never touch
//! real terminals.

use serde::Deserialize;
use std::path::PathBuf;

/// One live Orca-managed terminal.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OrcaSession {
    pub handle: String,
    pub leaf_id: String,
    /// Worktree display name (last path component of the worktree path).
    pub worktree: String,
    /// Terminal title with leading status glyphs removed.
    pub title: String,
}

impl OrcaSession {
    /// The Target label: `<worktree> · <title>`.
    pub fn label(&self) -> String {
        match (self.worktree.is_empty(), self.title.is_empty()) {
            (false, false) => format!("{} · {}", self.worktree, self.title),
            (false, true) => self.worktree.clone(),
            (true, false) => self.title.clone(),
            (true, true) => self.handle.clone(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OrcaError {
    /// CLI missing, Orca not running, or unparseable reply.
    Unavailable(String),
    /// The terminal handle no longer exists.
    Stale,
    /// The CLI reported another error.
    Failed(String),
}

impl std::fmt::Display for OrcaError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            OrcaError::Unavailable(m) => write!(f, "Orca unavailable: {m}"),
            OrcaError::Stale => write!(f, "Orca terminal is gone"),
            OrcaError::Failed(m) => write!(f, "Orca error: {m}"),
        }
    }
}

#[derive(Deserialize)]
struct Envelope {
    ok: bool,
    #[serde(default)]
    result: Option<serde_json::Value>,
    #[serde(default)]
    error: Option<ErrorBody>,
}

#[derive(Deserialize)]
struct ErrorBody {
    #[serde(default)]
    code: String,
    #[serde(default)]
    message: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct RawTerminal {
    handle: String,
    #[serde(default)]
    leaf_id: String,
    #[serde(default)]
    worktree_path: String,
    #[serde(default)]
    title: String,
    #[serde(default = "yes")]
    connected: bool,
    #[serde(default = "yes")]
    writable: bool,
    #[serde(default)]
    orphaned: bool,
}

fn yes() -> bool {
    true
}

/// Strip leading spinner/status glyphs such as `✳ ` or `◐ `.
pub fn clean_title(title: &str) -> String {
    title
        .trim_start_matches(|c: char| !c.is_alphanumeric())
        .trim_end()
        .to_owned()
}

fn is_stale_code(code: &str) -> bool {
    code.contains("stale") || code.contains("not_found") || code.contains("unknown_terminal")
}

fn check_envelope(
    stdout: &str,
    exit_ok: bool,
    stderr: &str,
) -> Result<Option<serde_json::Value>, OrcaError> {
    match serde_json::from_str::<Envelope>(stdout.trim()) {
        Ok(env) if env.ok => Ok(env.result),
        Ok(env) => {
            let err = env.error.unwrap_or(ErrorBody {
                code: String::new(),
                message: String::new(),
            });
            if is_stale_code(&err.code) {
                Err(OrcaError::Stale)
            } else if err.code.contains("runtime") || err.code.contains("unavailable") {
                Err(OrcaError::Unavailable(err.code))
            } else {
                Err(OrcaError::Failed(if err.message.is_empty() {
                    err.code
                } else {
                    err.message
                }))
            }
        }
        Err(_) if exit_ok => Ok(None),
        Err(_) => {
            let msg = stderr.trim();
            Err(OrcaError::Unavailable(if msg.is_empty() {
                "no JSON reply".into()
            } else {
                msg.chars().take(200).collect()
            }))
        }
    }
}

/// Parse `orca terminal list --json` output into usable sessions.
pub fn parse_list(stdout: &str) -> Result<Vec<OrcaSession>, OrcaError> {
    let result = check_envelope(stdout, true, "")?
        .ok_or_else(|| OrcaError::Unavailable("unexpected list reply".into()))?;
    let raw: Vec<RawTerminal> = serde_json::from_value(
        result
            .get("terminals")
            .cloned()
            .unwrap_or(serde_json::Value::Array(vec![])),
    )
    .map_err(|e| OrcaError::Unavailable(format!("bad terminal list: {e}")))?;
    Ok(raw
        .into_iter()
        .filter(|t| t.connected && t.writable && !t.orphaned && !t.handle.is_empty())
        .map(|t| OrcaSession {
            worktree: t
                .worktree_path
                .rsplit('/')
                .find(|s| !s.is_empty())
                .unwrap_or("")
                .to_owned(),
            title: clean_title(&t.title),
            handle: t.handle,
            leaf_id: t.leaf_id,
        })
        .collect())
}

/// Text for a terminal: line breaks would submit the prompt, so they become
/// spaces; other control characters are dropped.
pub fn sanitize_terminal_text(text: &str) -> String {
    text.chars()
        .filter_map(|c| match c {
            '\r' | '\n' | '\t' => Some(' '),
            c if c.is_control() => None,
            c => Some(c),
        })
        .collect()
}

pub struct CmdOutput {
    pub success: bool,
    pub stdout: String,
    pub stderr: String,
}

/// Runs the `orca` CLI with the given arguments.
pub trait CommandRunner: Send {
    fn run(&mut self, args: &[String]) -> std::io::Result<CmdOutput>;
}

/// Real runner: spawns the CLI directly (no shell).
pub struct ProcessRunner {
    pub program: PathBuf,
}

impl ProcessRunner {
    /// Locate the CLI: `$VIBE_VOICE_ORCA`, then common install paths, then PATH.
    /// Apps started from Finder get a minimal PATH, hence the fixed paths.
    pub fn locate() -> Self {
        let mut candidates: Vec<PathBuf> = Vec::new();
        if let Ok(p) = std::env::var("VIBE_VOICE_ORCA") {
            candidates.push(p.into());
        }
        candidates.push("/usr/local/bin/orca".into());
        candidates.push("/opt/homebrew/bin/orca".into());
        candidates.push("/Applications/Orca.app/Contents/Resources/bin/orca".into());
        if let Ok(path) = std::env::var("PATH") {
            candidates.extend(path.split(':').map(|d| PathBuf::from(d).join("orca")));
        }
        let program = candidates
            .into_iter()
            .find(|p| p.is_file())
            .unwrap_or_else(|| PathBuf::from("orca"));
        Self { program }
    }
}

impl CommandRunner for ProcessRunner {
    fn run(&mut self, args: &[String]) -> std::io::Result<CmdOutput> {
        let mut cmd = std::process::Command::new(&self.program);
        cmd.args(args).stdin(std::process::Stdio::null());
        // Finder-launched apps lack /usr/local/bin; the CLI script needs `env bash`.
        let path = std::env::var("PATH").unwrap_or_default();
        cmd.env(
            "PATH",
            format!("{path}:/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin"),
        );
        let out = cmd.output()?;
        Ok(CmdOutput {
            success: out.status.success(),
            stdout: String::from_utf8_lossy(&out.stdout).into_owned(),
            stderr: String::from_utf8_lossy(&out.stderr).into_owned(),
        })
    }
}

/// Orca operations the session logic needs.
pub trait OrcaApi {
    fn list(&mut self) -> Result<Vec<OrcaSession>, OrcaError>;
    fn send_text(&mut self, handle: &str, text: &str) -> Result<(), OrcaError>;
    fn send_enter(&mut self, handle: &str) -> Result<(), OrcaError>;
    fn send_backspaces(&mut self, handle: &str, count: usize) -> Result<(), OrcaError>;
    fn switch(&mut self, handle: &str) -> Result<(), OrcaError>;
}

pub struct OrcaClient<R: CommandRunner> {
    runner: R,
}

impl<R: CommandRunner> OrcaClient<R> {
    pub fn new(runner: R) -> Self {
        Self { runner }
    }

    fn call(&mut self, args: Vec<String>) -> Result<Option<serde_json::Value>, OrcaError> {
        log::debug!(
            "orca {}",
            args.iter()
                .filter(|a| !a.starts_with("--text="))
                .cloned()
                .collect::<Vec<_>>()
                .join(" ")
        );
        let out = self
            .runner
            .run(&args)
            .map_err(|e| OrcaError::Unavailable(format!("cannot run orca: {e}")))?;
        check_envelope(&out.stdout, out.success, &out.stderr)
    }

    fn send(&mut self, handle: &str, text: Option<&str>, enter: bool) -> Result<(), OrcaError> {
        let mut args = vec![
            "terminal".to_owned(),
            "send".to_owned(),
            format!("--terminal={handle}"),
        ];
        if let Some(t) = text {
            args.push(format!("--text={t}"));
        }
        if enter {
            args.push("--enter".to_owned());
        }
        args.push("--json".to_owned());
        self.call(args).map(|_| ())
    }
}

impl<R: CommandRunner> OrcaApi for OrcaClient<R> {
    fn list(&mut self) -> Result<Vec<OrcaSession>, OrcaError> {
        let args = vec!["terminal".into(), "list".into(), "--json".into()];
        let out = self
            .runner
            .run(&args)
            .map_err(|e| OrcaError::Unavailable(format!("cannot run orca: {e}")))?;
        if !out.success && serde_json::from_str::<serde_json::Value>(out.stdout.trim()).is_err() {
            return Err(OrcaError::Unavailable(
                out.stderr.trim().chars().take(200).collect(),
            ));
        }
        parse_list(&out.stdout)
    }

    fn send_text(&mut self, handle: &str, text: &str) -> Result<(), OrcaError> {
        let clean = sanitize_terminal_text(text);
        if clean.is_empty() {
            return Ok(());
        }
        self.send(handle, Some(&clean), false)
    }

    fn send_enter(&mut self, handle: &str) -> Result<(), OrcaError> {
        self.send(handle, None, true)
    }

    fn send_backspaces(&mut self, handle: &str, count: usize) -> Result<(), OrcaError> {
        if count == 0 {
            return Ok(());
        }
        self.send(handle, Some(&"\u{7f}".repeat(count)), false)
    }

    fn switch(&mut self, handle: &str) -> Result<(), OrcaError> {
        self.call(vec![
            "terminal".into(),
            "switch".into(),
            format!("--terminal={handle}"),
            "--json".into(),
        ])
        .map(|_| ())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};

    // Shape captured from `orca terminal list --json` (Orca CLI 1.4.218),
    // with paths and titles anonymised.
    const LIST_JSON: &str = r#"{
      "id": "dacda9a8-a16d-411c-9e7f-a6ed35f35350",
      "ok": true,
      "result": {
        "terminals": [
          {
            "handle": "term_aaa",
            "ptyId": "x::/Users/me/src/voice-notes@@1163288f",
            "incarnationId": "d02c",
            "orphaned": false,
            "worktreeId": "x::/Users/me/src/voice-notes",
            "worktreePath": "/Users/me/src/voice-notes",
            "branch": "refs/heads/master",
            "tabId": "6922",
            "leafId": "leaf-a",
            "title": "✳ 提交 PR 合并发布",
            "connected": true,
            "writable": true,
            "lastOutputAt": 1791177614980,
            "preview": "…",
            "executionHostId": "local",
            "agentIdentity": "claude"
          },
          {
            "handle": "term_bbb", "orphaned": false,
            "worktreePath": "/Users/me/src/my-passport", "leafId": "leaf-b",
            "title": "◐ MacOS语音输入集成", "connected": true, "writable": true
          },
          {
            "handle": "term_ccc", "orphaned": false,
            "worktreePath": "/Users/me/src/AxiomOS-rel", "leafId": "leaf-c",
            "title": "Terminal 1", "connected": true, "writable": true, "agentIdentity": null
          },
          {
            "handle": "term_orphan", "orphaned": true, "worktreePath": "",
            "title": "✳ 新手引导设计", "connected": true, "writable": true
          },
          {
            "handle": "term_ro", "orphaned": false, "worktreePath": "/x/y",
            "title": "read only", "connected": true, "writable": false
          }
        ]
      ,
        "totalCount": 5,
        "truncated": false
      }
    }"#;

    #[test]
    fn parses_real_list_shape() {
        let s = parse_list(LIST_JSON).unwrap();
        assert_eq!(s.len(), 3);
        assert_eq!(s[0].handle, "term_aaa");
        assert_eq!(s[0].leaf_id, "leaf-a");
        assert_eq!(s[0].label(), "voice-notes · 提交 PR 合并发布");
        assert_eq!(s[1].label(), "my-passport · MacOS语音输入集成");
        assert_eq!(s[2].label(), "AxiomOS-rel · Terminal 1");
    }

    #[test]
    fn list_errors() {
        assert_eq!(
            parse_list(r#"{"ok":false,"error":{"code":"terminal_handle_stale","message":"x"}}"#),
            Err(OrcaError::Stale)
        );
        assert!(matches!(
            parse_list("not json"),
            Err(OrcaError::Unavailable(_))
        ));
        assert_eq!(
            parse_list(r#"{"ok":true,"result":{"terminals":[]}}"#).unwrap(),
            vec![]
        );
    }

    #[test]
    fn title_cleanup() {
        assert_eq!(clean_title("✳ 任务列表"), "任务列表");
        assert_eq!(clean_title("◐ abc "), "abc");
        assert_eq!(clean_title("Setup"), "Setup");
        assert_eq!(clean_title("✳"), "");
    }

    #[test]
    fn sanitizes_terminal_text() {
        assert_eq!(sanitize_terminal_text("a\nb\r\nc\u{1b}[0m"), "a b  c[0m");
    }

    #[derive(Clone, Default)]
    struct Mock {
        calls: Arc<Mutex<Vec<Vec<String>>>>,
        reply: Arc<Mutex<(bool, String)>>,
    }

    impl CommandRunner for Mock {
        fn run(&mut self, args: &[String]) -> std::io::Result<CmdOutput> {
            self.calls.lock().unwrap().push(args.to_vec());
            let (success, stdout) = self.reply.lock().unwrap().clone();
            Ok(CmdOutput {
                success,
                stdout,
                stderr: String::new(),
            })
        }
    }

    struct Missing;
    impl CommandRunner for Missing {
        fn run(&mut self, _: &[String]) -> std::io::Result<CmdOutput> {
            Err(std::io::Error::new(std::io::ErrorKind::NotFound, "orca"))
        }
    }

    #[test]
    fn builds_argv_without_shell() {
        let mock = Mock::default();
        *mock.reply.lock().unwrap() = (true, r#"{"ok":true,"result":{}}"#.into());
        let mut c = OrcaClient::new(mock.clone());
        c.send_text("term_1", "--help; rm -rf ~ `x` $(y)\n")
            .unwrap();
        c.send_enter("term_1").unwrap();
        c.send_backspaces("term_1", 3).unwrap();
        c.send_backspaces("term_1", 0).unwrap();
        c.switch("term_1").unwrap();
        let calls = mock.calls.lock().unwrap();
        assert_eq!(calls.len(), 4);
        assert_eq!(
            calls[0],
            [
                "terminal",
                "send",
                "--terminal=term_1",
                "--text=--help; rm -rf ~ `x` $(y) ",
                "--json"
            ]
        );
        assert_eq!(
            calls[1],
            ["terminal", "send", "--terminal=term_1", "--enter", "--json"]
        );
        assert_eq!(
            calls[2],
            [
                "terminal",
                "send",
                "--terminal=term_1",
                "--text=\u{7f}\u{7f}\u{7f}",
                "--json"
            ]
        );
        assert_eq!(
            calls[3],
            ["terminal", "switch", "--terminal=term_1", "--json"]
        );
    }

    #[test]
    fn maps_stale_and_missing_cli() {
        let mock = Mock::default();
        *mock.reply.lock().unwrap() =
            (false, r#"{"ok":false,"error":{"code":"terminal_handle_stale","message":"terminal_handle_stale"}}"#.into());
        let mut c = OrcaClient::new(mock);
        assert_eq!(c.send_text("term_x", "hi"), Err(OrcaError::Stale));
        let mut m = OrcaClient::new(Missing);
        assert!(matches!(m.list(), Err(OrcaError::Unavailable(_))));
        assert!(matches!(m.send_enter("t"), Err(OrcaError::Unavailable(_))));
    }
}
