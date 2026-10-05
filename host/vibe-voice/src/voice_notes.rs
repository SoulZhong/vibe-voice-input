//! Voice Notes Recordings through the Voice Notes app's control socket.
//!
//! Voice Notes (`com.teemo.voice-notes`) listens on a Unix socket
//! `<app data>/mcp.sock` (app data = `$VN_APP_DATA`, else
//! `~/Library/Application Support/com.teemo.voice-notes`) for newline-delimited
//! JSON requests `{"op":"status"|"start"|"stop"}` and answers
//! `{"ok":bool,"data":…,"error":…}` (see voice-notes `src-tauri/src/mcp/uds.rs`).
//!
//! `start` can block up to 20 s while Voice Notes loads its model and `stop`
//! until the note is finalized, so these calls must run on their own thread,
//! never on the Companion's core loop (Dictation audio must keep flowing).
//! [`NotesWorker`] does that; the session only queues [`NotesOp`]s and handles
//! [`NotesReply`]s.

use serde::Deserialize;
use std::io::{BufRead, BufReader, Write};
use std::os::unix::net::UnixStream;
use std::path::PathBuf;
use std::sync::mpsc;
use std::time::{Duration, Instant};

pub const BUNDLE_ID: &str = "com.teemo.voice-notes";
/// How long a launch may take until the socket answers.
pub const LAUNCH_TIMEOUT: Duration = Duration::from_secs(20);
/// Reply timeouts: status is a cheap poll; start waits for the model.
pub const STATUS_TIMEOUT: Duration = Duration::from_millis(800);
pub const START_TIMEOUT: Duration = Duration::from_secs(30);
pub const STOP_TIMEOUT: Duration = Duration::from_secs(120);

/// Risk kinds Voice Notes reports when a recording starts (`precheck.rs`).
pub const RISK_VOICE_ISOLATION: &str = "voice_isolation";
pub const RISK_BLUETOOTH_MIC: &str = "bluetooth_mic";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NotesPhase {
    Idle,
    Recording,
    Paused,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NotesStatus {
    pub phase: NotesPhase,
    pub elapsed_ms: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NotesError {
    /// Voice Notes is not running (no socket).
    NotRunning,
    /// Voice Notes is not installed.
    NotInstalled,
    /// Launched, but the socket did not answer in time.
    LaunchFailed,
    /// The user has not allowed control ("允许 AI 控制录制" is off).
    ControlDisabled,
    /// Voice Notes refused or failed.
    Failed(String),
}

impl std::fmt::Display for NotesError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            NotesError::NotRunning => write!(f, "Voice Notes is not running"),
            NotesError::NotInstalled => write!(f, "Voice Notes is not installed"),
            NotesError::LaunchFailed => write!(f, "Voice Notes did not start in time"),
            NotesError::ControlDisabled => write!(f, "Voice Notes control is disabled"),
            NotesError::Failed(m) => write!(f, "Voice Notes: {m}"),
        }
    }
}

/// One request for the worker.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NotesOp {
    Status,
    Start,
    Stop,
}

/// The worker's answer to a [`NotesOp`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NotesReply {
    Status(Result<NotesStatus, NotesError>),
    /// Started; the pre-record risk kinds (may be empty).
    Started(Result<Vec<String>, NotesError>),
    Stopped(Result<(), NotesError>),
}

/// Voice Notes control, behind a trait so tests never touch the real app.
pub trait VoiceNotesApi: Send {
    fn status(&mut self) -> Result<NotesStatus, NotesError>;
    /// Start a recording, launching Voice Notes first when needed.
    fn start(&mut self) -> Result<Vec<String>, NotesError>;
    fn stop(&mut self) -> Result<(), NotesError>;
}

#[derive(Deserialize)]
struct Envelope {
    ok: bool,
    #[serde(default)]
    data: Option<serde_json::Value>,
    #[serde(default)]
    error: Option<String>,
}

fn envelope(line: &str) -> Result<serde_json::Value, NotesError> {
    let env: Envelope = serde_json::from_str(line.trim())
        .map_err(|e| NotesError::Failed(format!("bad reply: {e}")))?;
    if env.ok {
        Ok(env.data.unwrap_or(serde_json::Value::Null))
    } else {
        let msg = env.error.unwrap_or_default();
        // CONTROL_DENIED in uds.rs: "已被用户禁用:请在 voice-notes …开启「允许 AI 控制录制」"
        if msg.contains("已被用户禁用") || msg.contains("允许 AI 控制") {
            Err(NotesError::ControlDisabled)
        } else {
            Err(NotesError::Failed(msg))
        }
    }
}

/// Parse the reply to `{"op":"status"}`.
pub fn parse_status(line: &str) -> Result<NotesStatus, NotesError> {
    let data = envelope(line)?;
    let phase = match data.get("state").and_then(|s| s.as_str()) {
        Some("recording") => NotesPhase::Recording,
        Some("paused") => NotesPhase::Paused,
        _ => NotesPhase::Idle,
    };
    let elapsed_ms = data.get("elapsed_ms").and_then(|v| v.as_u64()).unwrap_or(0);
    Ok(NotesStatus { phase, elapsed_ms })
}

/// Parse the reply to `{"op":"start"}`: the risk kinds.
pub fn parse_start(line: &str) -> Result<Vec<String>, NotesError> {
    let data = envelope(line)?;
    Ok(data
        .get("risks")
        .and_then(|r| r.as_array())
        .into_iter()
        .flatten()
        .filter_map(|r| r.get("kind").and_then(|k| k.as_str()).map(str::to_owned))
        .collect())
}

/// Parse the reply to `{"op":"stop"}`. Nothing to stop counts as stopped.
pub fn parse_stop(line: &str) -> Result<(), NotesError> {
    match envelope(line) {
        Ok(_) => Ok(()),
        Err(NotesError::Failed(m)) if m.contains("没有正在进行的录制") => Ok(()),
        Err(e) => Err(e),
    }
}

/// The socket path, honouring `$VN_APP_DATA`.
pub fn socket_path() -> PathBuf {
    let dir = std::env::var_os("VN_APP_DATA")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
            PathBuf::from(home).join("Library/Application Support/com.teemo.voice-notes")
        });
    dir.join("mcp.sock")
}

/// Launches Voice Notes (a trait so the client logic is testable).
pub trait Launcher: Send {
    /// Start the app in the background. `NotInstalled` when it is missing.
    fn launch(&mut self) -> Result<(), NotesError>;
}

/// `open -g -b com.teemo.voice-notes`: launch without taking focus.
pub struct OpenLauncher;

impl Launcher for OpenLauncher {
    fn launch(&mut self) -> Result<(), NotesError> {
        log::info!("launching Voice Notes (open -g -b {BUNDLE_ID})");
        let out = std::process::Command::new("/usr/bin/open")
            .args(["-g", "-b", BUNDLE_ID])
            .stdin(std::process::Stdio::null())
            .output()
            .map_err(|e| NotesError::Failed(format!("open: {e}")))?;
        if out.status.success() {
            Ok(())
        } else {
            log::warn!(
                "open -b {BUNDLE_ID}: {}",
                String::from_utf8_lossy(&out.stderr).trim()
            );
            Err(NotesError::NotInstalled)
        }
    }
}

/// Sends one request over a fresh connection and reads one line.
pub trait Transport: Send {
    fn request(&mut self, json: &str, timeout: Duration) -> Result<String, NotesError>;
}

/// The real Unix-socket transport.
pub struct UdsTransport {
    pub path: PathBuf,
}

impl Transport for UdsTransport {
    fn request(&mut self, json: &str, timeout: Duration) -> Result<String, NotesError> {
        let stream = UnixStream::connect(&self.path).map_err(|_| NotesError::NotRunning)?;
        let io = |e: std::io::Error| NotesError::Failed(format!("socket: {e}"));
        stream.set_read_timeout(Some(timeout)).map_err(io)?;
        stream.set_write_timeout(Some(STATUS_TIMEOUT)).map_err(io)?;
        let mut w = &stream;
        w.write_all(json.as_bytes()).map_err(io)?;
        w.write_all(b"\n").map_err(io)?;
        w.flush().map_err(io)?;
        let mut line = String::new();
        BufReader::new(&stream).read_line(&mut line).map_err(io)?;
        if line.trim().is_empty() {
            return Err(NotesError::Failed("empty reply".into()));
        }
        Ok(line)
    }
}

/// Voice Notes client: launches the app when a start needs it.
pub struct VoiceNotesClient<T: Transport, L: Launcher> {
    pub transport: T,
    pub launcher: L,
    pub launch_timeout: Duration,
    pub retry_every: Duration,
}

impl VoiceNotesClient<UdsTransport, OpenLauncher> {
    pub fn system() -> Self {
        Self {
            transport: UdsTransport {
                path: socket_path(),
            },
            launcher: OpenLauncher,
            launch_timeout: LAUNCH_TIMEOUT,
            retry_every: Duration::from_millis(250),
        }
    }
}

impl<T: Transport, L: Launcher> VoiceNotesClient<T, L> {
    fn call(&mut self, op: &str, timeout: Duration) -> Result<String, NotesError> {
        self.transport
            .request(&format!("{{\"op\":\"{op}\"}}"), timeout)
    }

    /// Launch Voice Notes and wait until its socket answers a status.
    fn launch_and_wait(&mut self) -> Result<NotesStatus, NotesError> {
        self.launcher.launch()?;
        let deadline = Instant::now() + self.launch_timeout;
        loop {
            match self.call("status", STATUS_TIMEOUT) {
                Ok(line) => return parse_status(&line),
                Err(NotesError::NotRunning) if Instant::now() < deadline => {
                    std::thread::sleep(self.retry_every);
                }
                Err(NotesError::NotRunning) => return Err(NotesError::LaunchFailed),
                Err(e) => return Err(e),
            }
        }
    }
}

impl<T: Transport, L: Launcher> VoiceNotesApi for VoiceNotesClient<T, L> {
    fn status(&mut self) -> Result<NotesStatus, NotesError> {
        parse_status(&self.call("status", STATUS_TIMEOUT)?)
    }

    fn start(&mut self) -> Result<Vec<String>, NotesError> {
        let status = match self.status() {
            Err(NotesError::NotRunning) => self.launch_and_wait()?,
            other => other?,
        };
        if status.phase != NotesPhase::Idle {
            return Ok(Vec::new()); // already recording: nothing to start
        }
        parse_start(&self.call("start", START_TIMEOUT)?)
    }

    fn stop(&mut self) -> Result<(), NotesError> {
        match self.call("stop", STOP_TIMEOUT) {
            Err(NotesError::NotRunning) => Ok(()),
            Err(e) => Err(e),
            Ok(line) => parse_stop(&line),
        }
    }
}

/// Runs Voice Notes calls on a dedicated thread: ops in, replies out.
pub struct NotesWorker {
    tx: mpsc::Sender<NotesOp>,
}

impl NotesWorker {
    /// `reply` is called on the worker thread for every op, in order.
    pub fn spawn<A, F>(mut api: A, reply: F) -> Self
    where
        A: VoiceNotesApi + 'static,
        F: Fn(NotesReply) + Send + 'static,
    {
        let (tx, rx) = mpsc::channel::<NotesOp>();
        std::thread::Builder::new()
            .name("voice-notes".into())
            .spawn(move || {
                for op in rx {
                    let r = match op {
                        NotesOp::Status => NotesReply::Status(api.status()),
                        NotesOp::Start => NotesReply::Started(api.start()),
                        NotesOp::Stop => NotesReply::Stopped(api.stop()),
                    };
                    reply(r);
                }
            })
            .expect("voice-notes thread");
        Self { tx }
    }

    pub fn send(&self, op: NotesOp) {
        let _ = self.tx.send(op);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};

    #[test]
    fn parses_status_shapes() {
        // Shapes of status_json() in voice-notes uds.rs.
        let rec = r#"{"ok":true,"data":{"state":"recording","note_id":"N1","elapsed_ms":125400,"system_audio":"on","diarization":"off"}}"#;
        assert_eq!(
            parse_status(rec),
            Ok(NotesStatus {
                phase: NotesPhase::Recording,
                elapsed_ms: 125_400
            })
        );
        let paused = r#"{"ok":true,"data":{"state":"paused","note_id":"N1","elapsed_ms":9000}}"#;
        assert_eq!(parse_status(paused).unwrap().phase, NotesPhase::Paused);
        let idle = r#"{"ok":true,"data":{"state":"idle","note_id":"","elapsed_ms":0,"system_audio":"","diarization":""}}"#;
        assert_eq!(parse_status(idle).unwrap().phase, NotesPhase::Idle);
        assert!(matches!(
            parse_status("garbage"),
            Err(NotesError::Failed(_))
        ));
    }

    #[test]
    fn parses_start_risks_and_errors() {
        let ok = r#"{"ok":true,"data":{"note_id":"N2","risks":[{"kind":"voice_isolation","detail":""},{"kind":"bluetooth_mic","detail":"AirPods"}]}}"#;
        assert_eq!(
            parse_start(ok),
            Ok(vec![RISK_VOICE_ISOLATION.into(), RISK_BLUETOOTH_MIC.into()])
        );
        assert_eq!(
            parse_start(r#"{"ok":true,"data":{"note_id":"N2","risks":[]}}"#),
            Ok(vec![])
        );
        let denied = r#"{"ok":false,"error":"已被用户禁用:请在 voice-notes 左侧「AI」页开启「允许 AI 控制录制」"}"#;
        assert_eq!(parse_start(denied), Err(NotesError::ControlDisabled));
        assert_eq!(
            parse_start(r#"{"ok":false,"error":"录制启动超时"}"#),
            Err(NotesError::Failed("录制启动超时".into()))
        );
        assert_eq!(
            parse_stop(r#"{"ok":false,"error":"没有正在进行的录制"}"#),
            Ok(())
        );
    }

    #[derive(Clone, Default)]
    struct FakeTransport {
        /// Replies in order; `None` = connection refused (not running).
        replies: Arc<Mutex<VecDeque<Option<String>>>>,
        sent: Arc<Mutex<Vec<String>>>,
    }

    impl Transport for FakeTransport {
        fn request(&mut self, json: &str, _: Duration) -> Result<String, NotesError> {
            self.sent.lock().unwrap().push(json.to_owned());
            match self.replies.lock().unwrap().pop_front() {
                Some(Some(r)) => Ok(r),
                _ => Err(NotesError::NotRunning),
            }
        }
    }

    struct FakeLauncher {
        result: Result<(), NotesError>,
        launched: Arc<Mutex<usize>>,
    }

    impl Launcher for FakeLauncher {
        fn launch(&mut self) -> Result<(), NotesError> {
            *self.launched.lock().unwrap() += 1;
            self.result.clone()
        }
    }

    const IDLE: &str = r#"{"ok":true,"data":{"state":"idle","elapsed_ms":0}}"#;

    fn client(
        replies: Vec<Option<&str>>,
        launch: Result<(), NotesError>,
    ) -> (
        VoiceNotesClient<FakeTransport, FakeLauncher>,
        FakeTransport,
        Arc<Mutex<usize>>,
    ) {
        let t = FakeTransport::default();
        *t.replies.lock().unwrap() = replies.into_iter().map(|r| r.map(str::to_owned)).collect();
        let launched = Arc::new(Mutex::new(0));
        let c = VoiceNotesClient {
            transport: t.clone(),
            launcher: FakeLauncher {
                result: launch,
                launched: launched.clone(),
            },
            launch_timeout: Duration::from_millis(30),
            retry_every: Duration::from_millis(1),
        };
        (c, t, launched)
    }

    #[test]
    fn start_launches_when_not_running() {
        let started =
            r#"{"ok":true,"data":{"note_id":"N1","risks":[{"kind":"bluetooth_mic","detail":""}]}}"#;
        let (mut c, t, launched) = client(vec![None, None, Some(IDLE), Some(started)], Ok(()));
        assert_eq!(c.start(), Ok(vec![RISK_BLUETOOTH_MIC.into()]));
        assert_eq!(*launched.lock().unwrap(), 1);
        let sent = t.sent.lock().unwrap();
        assert_eq!(sent.last().unwrap(), r#"{"op":"start"}"#);
        assert_eq!(sent.len(), 4);
    }

    #[test]
    fn start_errors() {
        let (mut c, _, _) = client(vec![None], Err(NotesError::NotInstalled));
        assert_eq!(c.start(), Err(NotesError::NotInstalled));
        // Launched but the socket never answers.
        let (mut c, _, _) = client(vec![None; 200], Ok(()));
        assert_eq!(c.start(), Err(NotesError::LaunchFailed));
        // Already recording: nothing sent.
        let rec = r#"{"ok":true,"data":{"state":"recording","elapsed_ms":5}}"#;
        let (mut c, t, launched) = client(vec![Some(rec)], Ok(()));
        assert_eq!(c.start(), Ok(vec![]));
        assert_eq!(t.sent.lock().unwrap().len(), 1);
        assert_eq!(*launched.lock().unwrap(), 0);
    }

    #[test]
    fn status_and_stop_never_launch() {
        let (mut c, _, launched) = client(vec![None, None], Ok(()));
        assert_eq!(c.status(), Err(NotesError::NotRunning));
        assert_eq!(c.stop(), Ok(()));
        assert_eq!(*launched.lock().unwrap(), 0);
    }

    struct Slow;
    impl VoiceNotesApi for Slow {
        fn status(&mut self) -> Result<NotesStatus, NotesError> {
            Ok(NotesStatus {
                phase: NotesPhase::Idle,
                elapsed_ms: 0,
            })
        }
        fn start(&mut self) -> Result<Vec<String>, NotesError> {
            std::thread::sleep(Duration::from_millis(300));
            Ok(vec![])
        }
        fn stop(&mut self) -> Result<(), NotesError> {
            Ok(())
        }
    }

    #[test]
    fn worker_runs_off_the_calling_thread() {
        let (tx, rx) = mpsc::channel();
        let w = NotesWorker::spawn(Slow, move |r| {
            let _ = tx.send(r);
        });
        let t0 = Instant::now();
        w.send(NotesOp::Start);
        w.send(NotesOp::Status);
        // Sending never blocks on the slow start.
        assert!(t0.elapsed() < Duration::from_millis(100));
        assert_eq!(
            rx.recv_timeout(Duration::from_secs(2)),
            Ok(NotesReply::Started(Ok(vec![])))
        );
        assert!(matches!(
            rx.recv_timeout(Duration::from_secs(2)),
            Ok(NotesReply::Status(Ok(_)))
        ));
    }
}
