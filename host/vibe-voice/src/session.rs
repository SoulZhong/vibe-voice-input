//! Companion behaviour: the protocol state machine, independent of BLE,
//! Apple Speech, and macOS input injection (those sit behind traits).
//!
//! The driver feeds Device frames, recognizer events and time in; outgoing
//! frames collect in an outbox the driver drains and writes to the Device.

use crate::audio::AudioAssembler;
use crate::config::{SavedOrcaSession, SavedState, TargetConfig, TargetType, TargetsFile};
use crate::orca::{OrcaApi, OrcaError, OrcaSession};
use crate::protocol::{
    Action, CompanionFrame, DeviceFrame, FLAG_CURRENT, FLAG_NOT_RUNNING, FLAG_SUBLIST, LIST_ORCA,
    LIST_ROOT, PROTOCOL_VERSION, Status, StatusCode, TargetKind, utf8_head,
};
use std::path::PathBuf;
use std::time::{Duration, Instant};
use unicode_segmentation::UnicodeSegmentation;

/// Minimum spacing of PARTIAL frames (about 5 per second).
pub const PARTIAL_INTERVAL: Duration = Duration::from_millis(200);
/// How long to wait for the final result after DICT_STOP.
pub const FINAL_TIMEOUT: Duration = Duration::from_secs(3);
/// Longest Orca Session list sent to the Device (its picker holds 24 rows).
pub const MAX_ORCA_ITEMS: usize = 24;
/// Label budgets that fit the Device's buffers (and always a frame):
/// list rows hold 71 bytes, the Target label 127.
pub const ITEM_LABEL_BYTES: usize = 71;
pub const STATE_LABEL_BYTES: usize = 127;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum InjectError {
    NotRunning,
    Permission,
    Failed(String),
}

/// macOS input injection for App Targets (`None` bundle id = follow focus).
pub trait Injector {
    fn accessibility_trusted(&mut self) -> bool;
    fn is_running(&mut self, bundle_id: &str) -> bool;
    /// Read the bundle id of the app at `path`, if installed.
    fn resolve_bundle_id(&mut self, path: &str) -> Option<String>;
    /// Bundle id of the frontmost app.
    fn frontmost_bundle_id(&mut self) -> Option<String>;
    /// (app name, focused window title) of the app, or of the frontmost app.
    fn focused_title(&mut self, bundle_id: Option<&str>) -> Option<(String, String)>;
    /// Activate (unless following focus) and paste `text` without Enter.
    fn insert(&mut self, bundle_id: Option<&str>, text: &str) -> Result<(), InjectError>;
    /// Activate (unless following focus) and press Return.
    fn submit(&mut self, bundle_id: Option<&str>) -> Result<(), InjectError>;
    /// Activate (unless following focus) and press Delete `count` times.
    fn delete_back(&mut self, bundle_id: Option<&str>, count: usize) -> Result<(), InjectError>;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecognizerHealth {
    Ready,
    NoPermission,
    Unavailable,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RecognizerStartError {
    NoPermission,
    Unavailable,
    Failed(String),
}

/// Streaming speech recognition. Results arrive later as [`RecogEvent`]s.
pub trait Recognizer {
    fn health(&mut self) -> RecognizerHealth;
    fn start(&mut self, dict: u8) -> Result<(), RecognizerStartError>;
    fn push(&mut self, pcm: &[i16]);
    /// No more audio (DICT_STOP); a final result should follow.
    fn finish(&mut self);
    fn cancel(&mut self);
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RecogEvent {
    Partial { dict: u8, text: String },
    Final { dict: u8, text: String },
    Error { dict: u8, message: String },
}

/// Where a Segment went, so UNDO hits the same place.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Dest {
    Follow,
    App { bundle_id: String },
    Orca { handle: String },
}

#[derive(Debug, Clone)]
struct Segment {
    dest: Dest,
    /// User-perceived characters (grapheme clusters) = Delete presses.
    chars: usize,
}

#[derive(Debug, PartialEq, Eq)]
enum Phase {
    Recording,
    Finalizing { deadline: Instant },
}

#[derive(Debug)]
struct Dictation {
    id: u8,
    audio: AudioAssembler,
    phase: Phase,
    /// Recognizer could not start or failed: the status to report.
    failure: Option<Status>,
    best: String,
    sent_partial: String,
    last_partial_at: Option<Instant>,
    partial_pending: bool,
}

/// The current Target, resolved against config and live state.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TargetView {
    pub status: Status,
    pub kind: TargetKind,
    pub label: String,
}

/// Count of user-perceived characters (Chinese characters count 1 each).
pub fn undo_count(text: &str) -> usize {
    text.graphemes(true).count()
}

fn cap_chars(s: &str, max: usize) -> String {
    if s.chars().count() <= max {
        s.to_owned()
    } else {
        let mut out: String = s.chars().take(max.saturating_sub(1)).collect();
        out.push('…');
        out
    }
}

/// `"<name> · <title>"` within `max_bytes`, keeping the name and cutting the
/// title (with `…`) so the Device never has to drop the start of a label.
pub fn compose_label(name: &str, title: Option<&str>, max_bytes: usize) -> String {
    let name = cap_chars(name, 16);
    let name = utf8_head(&name, max_bytes).to_owned();
    let Some(title) = title.map(str::trim).filter(|t| !t.is_empty()) else {
        return name;
    };
    const SEP: &str = " · ";
    let room = max_bytes.saturating_sub(name.len() + SEP.len());
    if room < 7 {
        return name;
    }
    let title = if title.len() <= room {
        title.to_owned()
    } else {
        format!("{}…", utf8_head(title, room - '…'.len_utf8()))
    };
    format!("{name}{SEP}{title}")
}

pub struct Companion<I: Injector, R: Recognizer, O: OrcaApi> {
    pub injector: I,
    pub recognizer: R,
    pub orca: O,
    targets: TargetsFile,
    state: SavedState,
    state_path: Option<PathBuf>,
    orca_list: Vec<OrcaSession>,
    dictation: Option<Dictation>,
    last_segment: Option<Segment>,
    orca_failed: bool,
    last_status: Option<StatusCode>,
    outbox: Vec<CompanionFrame>,
}

impl<I: Injector, R: Recognizer, O: OrcaApi> Companion<I, R, O> {
    pub fn new(
        injector: I,
        recognizer: R,
        orca: O,
        targets: TargetsFile,
        state: SavedState,
        state_path: Option<PathBuf>,
    ) -> Self {
        Self {
            injector,
            recognizer,
            orca,
            targets,
            state,
            state_path,
            orca_list: Vec::new(),
            dictation: None,
            last_segment: None,
            orca_failed: false,
            last_status: None,
            outbox: Vec::new(),
        }
    }

    /// Frames to write to the Device, in order.
    pub fn take_outbox(&mut self) -> Vec<CompanionFrame> {
        std::mem::take(&mut self.outbox)
    }

    pub fn saved_state(&self) -> &SavedState {
        &self.state
    }

    /// Replace the Target configuration (after `targets.toml` changed).
    pub fn set_targets(&mut self, targets: TargetsFile) {
        self.targets = targets;
    }

    pub fn dictation_active(&self) -> bool {
        self.dictation.is_some()
    }

    fn send(&mut self, f: CompanionFrame) {
        self.outbox.push(f);
    }

    // ----- link -----------------------------------------------------------

    /// The BLE link dropped: abandon any Dictation without inserting.
    pub fn on_disconnected(&mut self) {
        if let Some(d) = self.dictation.take() {
            log::info!("link lost during dictation {}; abandoned", d.id);
            self.recognizer.cancel();
        }
        self.outbox.clear();
        self.last_status = None;
    }

    // ----- frames ---------------------------------------------------------

    pub fn handle_frame(&mut self, frame: DeviceFrame, now: Instant) {
        match frame {
            DeviceFrame::Hello { ver, fw } => self.on_hello(ver, &fw),
            DeviceFrame::DictStart { dict } => self.on_dict_start(dict, now),
            DeviceFrame::Audio(a) => {
                if let Some(d) = self.dictation.as_mut()
                    && d.phase == Phase::Recording
                    && d.failure.is_none()
                    && let Some(pcm) = d.audio.push(&a)
                {
                    self.recognizer.push(&pcm);
                }
            }
            DeviceFrame::DictStop { dict } => self.on_dict_stop(dict, now),
            DeviceFrame::DictCancel { dict } => self.on_dict_cancel(dict),
            DeviceFrame::Submit => self.on_submit(),
            DeviceFrame::Undo => self.on_undo(),
            DeviceFrame::TargetsReq { list } => self.on_targets_req(list),
            DeviceFrame::TargetSelect { list, index } => self.on_target_select(list, index),
        }
    }

    fn on_hello(&mut self, ver: u8, fw: &str) {
        log::info!("Device HELLO ver={ver} fw={fw:?}");
        if ver != PROTOCOL_VERSION {
            log::warn!("Device protocol version {ver}, Companion speaks {PROTOCOL_VERSION}");
        }
        // A new HELLO means the Device (re)started: any old Dictation is gone.
        if let Some(d) = self.dictation.take() {
            log::info!("dictation {} abandoned by HELLO", d.id);
            self.recognizer.cancel();
        }
        self.send(CompanionFrame::HelloAck {
            ver: PROTOCOL_VERSION,
        });
        let view = self.current_view();
        self.send_state(view);
        self.last_status = None;
        self.report_health();
    }

    fn on_dict_start(&mut self, dict: u8, now: Instant) {
        if let Some(old) = self.dictation.take() {
            log::warn!("dictation {} replaced by {dict}", old.id);
            self.recognizer.cancel();
        }
        // After a new Dictation starts there is nothing to undo.
        self.last_segment = None;
        let view = self.current_view();
        self.send_state(view);
        let failure = match self.recognizer.start(dict) {
            Ok(()) => None,
            Err(e) => {
                log::error!("recognizer start failed: {e:?}");
                Some(match e {
                    RecognizerStartError::NoPermission => Status::Permission,
                    _ => Status::RecognizerError,
                })
            }
        };
        if failure.is_some() {
            self.report_health();
        }
        let _ = now;
        self.dictation = Some(Dictation {
            id: dict,
            audio: AudioAssembler::new(dict),
            phase: Phase::Recording,
            failure,
            best: String::new(),
            sent_partial: String::new(),
            last_partial_at: None,
            partial_pending: false,
        });
    }

    fn on_dict_stop(&mut self, dict: u8, now: Instant) {
        let Some(d) = self.dictation.as_mut().filter(|d| d.id == dict) else {
            log::warn!("DICT_STOP for unknown dictation {dict}");
            self.send(CompanionFrame::Result {
                dict,
                status: Status::Empty,
                text: String::new(),
            });
            return;
        };
        if d.phase != Phase::Recording {
            return;
        }
        log::info!(
            "dictation {dict} stopped: {} frames, {} lost, {} dropped",
            d.audio.frames,
            d.audio.lost_frames,
            d.audio.dropped_frames
        );
        if let Some(status) = d.failure {
            self.dictation = None;
            self.send(CompanionFrame::Result {
                dict,
                status,
                text: String::new(),
            });
            return;
        }
        d.phase = Phase::Finalizing {
            deadline: now + FINAL_TIMEOUT,
        };
        self.recognizer.finish();
    }

    fn on_dict_cancel(&mut self, dict: u8) {
        if self.dictation.as_ref().is_some_and(|d| d.id == dict) {
            self.dictation = None;
            self.recognizer.cancel();
        }
        self.send(CompanionFrame::Result {
            dict,
            status: Status::Cancelled,
            text: String::new(),
        });
    }

    // ----- recognizer -----------------------------------------------------

    pub fn handle_recog(&mut self, ev: RecogEvent, now: Instant) {
        let id = match &ev {
            RecogEvent::Partial { dict, .. }
            | RecogEvent::Final { dict, .. }
            | RecogEvent::Error { dict, .. } => *dict,
        };
        let Some(d) = self.dictation.as_mut().filter(|d| d.id == id) else {
            return;
        };
        match ev {
            RecogEvent::Partial { text, .. } => {
                d.best = text;
                d.partial_pending = d.best != d.sent_partial;
                self.flush_partial(now);
            }
            RecogEvent::Final { text, .. } => {
                d.best = text.clone();
                if matches!(d.phase, Phase::Finalizing { .. }) {
                    self.complete(text);
                } else {
                    // Apple may finalize early (for example after a long
                    // pause); keep it as the best text and wait for DICT_STOP.
                    d.partial_pending = d.best != d.sent_partial;
                    self.flush_partial(now);
                }
            }
            RecogEvent::Error { message, .. } => {
                log::warn!("recognizer error in dictation {id}: {message}");
                if matches!(d.phase, Phase::Finalizing { .. }) {
                    if d.best.trim().is_empty() {
                        // "No speech detected" ends here too: report EMPTY
                        // unless nothing at all could be recognized.
                        let dict = d.id;
                        self.dictation = None;
                        let status = if message.contains("1110")
                            || message.to_lowercase().contains("no speech")
                        {
                            Status::Empty
                        } else {
                            Status::RecognizerError
                        };
                        self.send(CompanionFrame::Result {
                            dict,
                            status,
                            text: String::new(),
                        });
                    } else {
                        let best = d.best.clone();
                        self.complete(best);
                    }
                } else if d.best.trim().is_empty() {
                    d.failure = Some(Status::RecognizerError);
                }
            }
        }
    }

    fn flush_partial(&mut self, now: Instant) {
        let Some(d) = self.dictation.as_mut() else {
            return;
        };
        if !d.partial_pending {
            return;
        }
        if let Some(t) = d.last_partial_at
            && now < t + PARTIAL_INTERVAL
        {
            return;
        }
        d.partial_pending = false;
        d.last_partial_at = Some(now);
        d.sent_partial = d.best.clone();
        let f = CompanionFrame::Partial {
            dict: d.id,
            text: d.best.clone(),
        };
        self.send(f);
    }

    /// Run timers. Returns when to call again.
    pub fn poll(&mut self, now: Instant) -> Option<Instant> {
        self.flush_partial(now);
        let d = self.dictation.as_ref()?;
        if let Phase::Finalizing { deadline } = d.phase
            && now >= deadline
        {
            log::warn!("final result timed out; using best partial");
            let best = d.best.clone();
            self.recognizer.cancel();
            self.complete(best);
            return None;
        }
        let d = self.dictation.as_ref()?;
        let partial_due = if d.partial_pending {
            d.last_partial_at.map(|t| t + PARTIAL_INTERVAL)
        } else {
            None
        };
        let final_due = match d.phase {
            Phase::Finalizing { deadline } => Some(deadline),
            Phase::Recording => None,
        };
        match (partial_due, final_due) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (a, b) => a.or(b),
        }
    }

    /// Finish the Dictation: Insert the Segment and reply RESULT.
    fn complete(&mut self, text: String) {
        let Some(d) = self.dictation.take() else {
            return;
        };
        let dict = d.id;
        let text = text.trim().to_owned();
        if text.is_empty() {
            self.send(CompanionFrame::Result {
                dict,
                status: Status::Empty,
                text: String::new(),
            });
            return;
        }
        let status = match self.current_dest() {
            Err(status) => status,
            Ok(dest) => {
                let r = self.deliver(&dest, &text);
                if r == Status::Ok {
                    // Follow focus: UNDO must hit the app that got the text,
                    // even if focus moves on.
                    let dest = match dest {
                        Dest::Follow => self
                            .injector
                            .frontmost_bundle_id()
                            .map_or(Dest::Follow, |bundle_id| Dest::App { bundle_id }),
                        d => d,
                    };
                    self.last_segment = Some(Segment {
                        dest,
                        chars: undo_count(&text),
                    });
                }
                r
            }
        };
        log::info!(
            "dictation {dict}: {status:?} ({} chars)",
            text.chars().count()
        );
        let sent = if status == Status::Ok {
            text
        } else {
            String::new()
        };
        self.send(CompanionFrame::Result {
            dict,
            status,
            text: sent,
        });
    }

    fn deliver(&mut self, dest: &Dest, text: &str) -> Status {
        match dest {
            Dest::Follow => self.inject(|i| i.insert(None, text)),
            Dest::App { bundle_id } => {
                let b = bundle_id.clone();
                self.inject(|i| i.insert(Some(&b), text))
            }
            Dest::Orca { handle } => {
                let h = handle.clone();
                let r = self.orca.send_text(&h, text);
                if r.is_ok()
                    && let Err(e) = self.orca.switch(&h)
                {
                    log::debug!("orca switch: {e}");
                }
                self.orca_status(r)
            }
        }
    }

    fn inject(&mut self, f: impl FnOnce(&mut I) -> Result<(), InjectError>) -> Status {
        match f(&mut self.injector) {
            Ok(()) => Status::Ok,
            Err(InjectError::NotRunning) => Status::TargetUnavailable,
            Err(InjectError::Permission) => {
                self.report_health();
                Status::Permission
            }
            Err(InjectError::Failed(m)) => {
                log::error!("inject failed: {m}");
                Status::TargetUnavailable
            }
        }
    }

    fn orca_status(&mut self, r: Result<(), OrcaError>) -> Status {
        match r {
            Ok(()) => {
                if self.orca_failed {
                    self.orca_failed = false;
                    self.report_health();
                }
                Status::Ok
            }
            Err(OrcaError::Stale) => Status::TargetUnavailable,
            Err(e) => {
                log::error!("{e}");
                if matches!(e, OrcaError::Unavailable(_)) {
                    self.orca_failed = true;
                    self.report_health();
                }
                Status::TargetUnavailable
            }
        }
    }

    // ----- actions --------------------------------------------------------

    fn on_submit(&mut self) {
        let status = match self.current_dest() {
            Err(s) => s,
            Ok(Dest::Follow) => self.inject(|i| i.submit(None)),
            Ok(Dest::App { bundle_id }) => self.inject(|i| i.submit(Some(&bundle_id))),
            Ok(Dest::Orca { handle }) => {
                let r = self.orca.send_enter(&handle);
                self.orca_status(r)
            }
        };
        // The submitted Segment has left the input, so backspaces would hit new text.
        if status == Status::Ok {
            self.last_segment = None;
        }
        self.send(CompanionFrame::ActionResult {
            action: Action::Submit,
            status,
        });
    }

    fn on_undo(&mut self) {
        let Some(seg) = self.last_segment.clone() else {
            self.send(CompanionFrame::ActionResult {
                action: Action::Undo,
                status: Status::NothingToUndo,
            });
            return;
        };
        let n = seg.chars;
        let status = match &seg.dest {
            Dest::Follow => self.inject(|i| i.delete_back(None, n)),
            Dest::App { bundle_id } => {
                let b = bundle_id.clone();
                if self.injector.is_running(&b) {
                    self.inject(|i| i.delete_back(Some(&b), n))
                } else {
                    Status::TargetUnavailable
                }
            }
            Dest::Orca { handle } => {
                let r = self.orca.send_backspaces(handle, n);
                self.orca_status(r)
            }
        };
        if status == Status::Ok {
            self.last_segment = None;
        }
        self.send(CompanionFrame::ActionResult {
            action: Action::Undo,
            status,
        });
    }

    // ----- targets --------------------------------------------------------

    fn selected_index(&self) -> usize {
        self.state
            .selected
            .as_deref()
            .and_then(|id| self.targets.targets.iter().position(|t| t.id == id))
            .filter(|&i| {
                self.targets.targets[i].kind != TargetType::Orca || self.state.orca.is_some()
            })
            .unwrap_or(0)
    }

    fn bundle_of(&mut self, t: &TargetConfig) -> Option<String> {
        if let Some(b) = &t.bundle_id {
            return Some(b.clone());
        }
        t.app_path
            .as_deref()
            .and_then(|p| self.injector.resolve_bundle_id(p))
    }

    fn app_running(&mut self, t: &TargetConfig) -> bool {
        match self.bundle_of(t) {
            Some(b) => self.injector.is_running(&b),
            None => false,
        }
    }

    /// Find the saved Orca Session among live terminals (by handle, then by
    /// pane id after an Orca restart re-issued handles).
    fn find_orca(&mut self) -> Result<Option<OrcaSession>, OrcaError> {
        let Some(saved) = self.state.orca.clone() else {
            return Ok(None);
        };
        let list = self.orca.list()?;
        let found = list
            .iter()
            .find(|s| s.handle == saved.handle)
            .or_else(|| {
                list.iter()
                    .find(|s| !saved.leaf_id.is_empty() && s.leaf_id == saved.leaf_id)
            })
            .cloned();
        if let Some(s) = &found
            && s.handle != saved.handle
        {
            log::info!("Orca handle changed {} -> {}", saved.handle, s.handle);
            self.state.orca = Some(SavedOrcaSession {
                handle: s.handle.clone(),
                leaf_id: s.leaf_id.clone(),
                label: s.label(),
            });
            self.persist();
        }
        Ok(found)
    }

    fn current_dest(&mut self) -> Result<Dest, Status> {
        let t = self.targets.targets[self.selected_index()].clone();
        match t.kind {
            TargetType::Follow => Ok(Dest::Follow),
            TargetType::App => match self.bundle_of(&t) {
                Some(b) if self.injector.is_running(&b) => Ok(Dest::App { bundle_id: b }),
                _ => Err(Status::TargetUnavailable),
            },
            TargetType::Orca => match self.state.orca.clone() {
                Some(s) => Ok(Dest::Orca { handle: s.handle }),
                None => Err(Status::TargetUnavailable),
            },
        }
    }

    /// Resolve the current Target and its Target Title.
    pub fn current_view(&mut self) -> TargetView {
        let t = self.targets.targets[self.selected_index()].clone();
        let ax = |s: &mut Self| {
            if s.injector.accessibility_trusted() {
                Status::Ok
            } else {
                Status::Permission
            }
        };
        match t.kind {
            TargetType::Follow => {
                let status = ax(self);
                let front = self.injector.focused_title(None).map(|(app, _)| app);
                TargetView {
                    status,
                    kind: TargetKind::FollowFocus,
                    label: compose_label(&t.name, front.as_deref(), STATE_LABEL_BYTES),
                }
            }
            TargetType::App => {
                let bundle = self.bundle_of(&t);
                match bundle {
                    Some(b) if self.injector.is_running(&b) => {
                        let status = ax(self);
                        let title = self.injector.focused_title(Some(&b)).map(|(_, w)| w);
                        TargetView {
                            status,
                            kind: TargetKind::App,
                            label: compose_label(&t.name, title.as_deref(), STATE_LABEL_BYTES),
                        }
                    }
                    _ => TargetView {
                        status: Status::TargetUnavailable,
                        kind: TargetKind::App,
                        label: compose_label(&t.name, None, STATE_LABEL_BYTES),
                    },
                }
            }
            TargetType::Orca => {
                let saved_label = self
                    .state
                    .orca
                    .as_ref()
                    .map(|s| s.label.clone())
                    .unwrap_or_default();
                match self.find_orca() {
                    Ok(Some(s)) => {
                        if self.orca_failed {
                            self.orca_failed = false;
                            self.report_health();
                        }
                        TargetView {
                            status: Status::Ok,
                            kind: TargetKind::OrcaSession,
                            label: compose_label(&s.worktree, Some(&s.title), STATE_LABEL_BYTES),
                        }
                    }
                    Ok(None) => TargetView {
                        status: Status::TargetUnavailable,
                        kind: TargetKind::OrcaSession,
                        label: utf8_head(&saved_label, STATE_LABEL_BYTES).to_owned(),
                    },
                    Err(e) => {
                        log::warn!("{e}");
                        if matches!(e, OrcaError::Unavailable(_)) && !self.orca_failed {
                            self.orca_failed = true;
                            self.report_health();
                        }
                        TargetView {
                            status: Status::TargetUnavailable,
                            kind: TargetKind::OrcaSession,
                            label: utf8_head(&saved_label, STATE_LABEL_BYTES).to_owned(),
                        }
                    }
                }
            }
        }
    }

    fn send_state(&mut self, v: TargetView) {
        log::info!("Target: {} ({:?})", v.label, v.status);
        self.send(CompanionFrame::TargetState {
            status: v.status,
            kind: v.kind,
            label: v.label,
        });
    }

    fn on_targets_req(&mut self, list: u8) {
        match list {
            LIST_ROOT => {
                let current = self.selected_index();
                let targets = self.targets.targets.clone();
                let count = targets.len() as u8;
                for (i, t) in targets.iter().enumerate() {
                    let mut flags = 0;
                    if i == current {
                        flags |= FLAG_CURRENT;
                    }
                    match t.kind {
                        TargetType::Follow => {}
                        TargetType::App => {
                            if !self.app_running(t) {
                                flags |= FLAG_NOT_RUNNING;
                            }
                        }
                        TargetType::Orca => {
                            flags |= FLAG_SUBLIST;
                            if t.bundle_id.is_some() && !self.app_running(t) {
                                flags |= FLAG_NOT_RUNNING;
                            }
                        }
                    }
                    let label = compose_label(&t.name, None, ITEM_LABEL_BYTES);
                    self.send(CompanionFrame::TargetItem {
                        list,
                        index: i as u8,
                        count,
                        flags,
                        label,
                    });
                }
                self.send(CompanionFrame::TargetEnd { list, count });
            }
            LIST_ORCA => {
                let sessions = match self.orca.list() {
                    Ok(s) => {
                        if self.orca_failed {
                            self.orca_failed = false;
                            self.report_health();
                        }
                        s
                    }
                    Err(e) => {
                        log::warn!("{e}");
                        self.orca_failed = true;
                        self.report_health();
                        Vec::new()
                    }
                };
                let sessions: Vec<_> = sessions.into_iter().take(MAX_ORCA_ITEMS).collect();
                let count = sessions.len() as u8;
                let selected_orca =
                    self.targets.targets[self.selected_index()].kind == TargetType::Orca;
                let current = self
                    .state
                    .orca
                    .as_ref()
                    .map(|s| s.handle.clone())
                    .filter(|_| selected_orca);
                for (i, s) in sessions.iter().enumerate() {
                    let flags = if current.as_deref() == Some(s.handle.as_str()) {
                        FLAG_CURRENT
                    } else {
                        0
                    };
                    let label = compose_label(&s.worktree, Some(&s.title), ITEM_LABEL_BYTES);
                    self.send(CompanionFrame::TargetItem {
                        list,
                        index: i as u8,
                        count,
                        flags,
                        label,
                    });
                }
                self.orca_list = sessions;
                self.send(CompanionFrame::TargetEnd { list, count });
            }
            other => {
                log::warn!("TARGETS_REQ for unknown list {other}");
                self.send(CompanionFrame::TargetEnd {
                    list: other,
                    count: 0,
                });
            }
        }
    }

    fn on_target_select(&mut self, list: u8, index: u8) {
        let i = index as usize;
        match list {
            LIST_ROOT if i < self.targets.targets.len() => {
                let t = self.targets.targets[i].clone();
                if t.kind == TargetType::Orca {
                    // The Orca row opens the Orca Session list; the Device
                    // should send TARGETS_REQ 1. Keep the current Target.
                    log::info!("TARGET_SELECT on the Orca row; Target unchanged");
                } else {
                    self.state.selected = Some(t.id.clone());
                    self.persist();
                }
            }
            LIST_ORCA if i < self.orca_list.len() => {
                let s = self.orca_list[i].clone();
                let orca_id = self
                    .targets
                    .targets
                    .iter()
                    .find(|t| t.kind == TargetType::Orca)
                    .map(|t| t.id.clone());
                if let Some(id) = orca_id {
                    self.state.selected = Some(id);
                    self.state.orca = Some(SavedOrcaSession {
                        handle: s.handle.clone(),
                        leaf_id: s.leaf_id.clone(),
                        label: s.label(),
                    });
                    self.persist();
                    if let Err(e) = self.orca.switch(&s.handle) {
                        log::debug!("orca switch: {e}");
                    }
                }
            }
            _ => log::warn!("TARGET_SELECT {list}/{index} out of range; Target unchanged"),
        }
        let view = self.current_view();
        self.send_state(view);
    }

    fn persist(&self) {
        if let Some(p) = &self.state_path {
            self.state.save(p);
        }
    }

    // ----- STATUS ---------------------------------------------------------

    /// The most important Companion-level problem right now.
    pub fn health_code(&mut self) -> (StatusCode, &'static str) {
        match self.recognizer.health() {
            RecognizerHealth::NoPermission => {
                return (StatusCode::SpeechPermission, "需要语音识别权限");
            }
            RecognizerHealth::Unavailable => {
                return (StatusCode::RecognizerUnavailable, "中文语音识别不可用");
            }
            RecognizerHealth::Ready => {}
        }
        let kind = self.targets.targets[self.selected_index()].kind;
        if kind != TargetType::Orca && !self.injector.accessibility_trusted() {
            return (StatusCode::AccessibilityPermission, "需要辅助功能权限");
        }
        if kind == TargetType::Orca && self.orca_failed {
            return (StatusCode::OrcaUnavailable, "Orca 不可用");
        }
        (StatusCode::Clear, "")
    }

    /// Send STATUS if it changed since last sent.
    pub fn report_health(&mut self) {
        let (code, text) = self.health_code();
        if self.last_status != Some(code) {
            self.last_status = Some(code);
            self.send(CompanionFrame::Status {
                code,
                text: text.to_owned(),
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::adpcm::{AdpcmState, encode};
    use crate::protocol::AudioFrame;
    use std::collections::HashSet;

    #[derive(Default)]
    struct FakeInjector {
        trusted: bool,
        running: HashSet<String>,
        log: Vec<String>,
        fail_insert: Option<InjectError>,
        frontmost: Option<String>,
    }

    impl Injector for FakeInjector {
        fn accessibility_trusted(&mut self) -> bool {
            self.trusted
        }
        fn is_running(&mut self, b: &str) -> bool {
            self.running.contains(b)
        }
        fn resolve_bundle_id(&mut self, path: &str) -> Option<String> {
            (path == "/Applications/Codex.app" && self.running.contains("codex.desktop"))
                .then(|| "codex.desktop".into())
        }
        fn frontmost_bundle_id(&mut self) -> Option<String> {
            self.frontmost.clone()
        }
        fn focused_title(&mut self, b: Option<&str>) -> Option<(String, String)> {
            Some(match b {
                None => ("Ghostty".into(), "zsh".into()),
                Some(b) => (b.into(), format!("{b}-chat")),
            })
        }
        fn insert(&mut self, b: Option<&str>, text: &str) -> Result<(), InjectError> {
            if let Some(e) = self.fail_insert.clone() {
                return Err(e);
            }
            self.log.push(format!("insert {b:?} {text}"));
            Ok(())
        }
        fn submit(&mut self, b: Option<&str>) -> Result<(), InjectError> {
            self.log.push(format!("submit {b:?}"));
            Ok(())
        }
        fn delete_back(&mut self, b: Option<&str>, n: usize) -> Result<(), InjectError> {
            self.log.push(format!("delete {b:?} {n}"));
            Ok(())
        }
    }

    #[derive(Default)]
    struct FakeRecognizer {
        health: Option<RecognizerHealth>,
        started: Vec<u8>,
        samples: usize,
        finished: usize,
        cancelled: usize,
    }

    impl Recognizer for FakeRecognizer {
        fn health(&mut self) -> RecognizerHealth {
            self.health.unwrap_or(RecognizerHealth::Ready)
        }
        fn start(&mut self, dict: u8) -> Result<(), RecognizerStartError> {
            match self.health() {
                RecognizerHealth::NoPermission => Err(RecognizerStartError::NoPermission),
                RecognizerHealth::Unavailable => Err(RecognizerStartError::Unavailable),
                RecognizerHealth::Ready => {
                    self.started.push(dict);
                    Ok(())
                }
            }
        }
        fn push(&mut self, pcm: &[i16]) {
            self.samples += pcm.len();
        }
        fn finish(&mut self) {
            self.finished += 1;
        }
        fn cancel(&mut self) {
            self.cancelled += 1;
        }
    }

    #[derive(Default)]
    struct FakeOrca {
        sessions: Vec<OrcaSession>,
        unavailable: bool,
        log: Vec<String>,
    }

    impl OrcaApi for FakeOrca {
        fn list(&mut self) -> Result<Vec<OrcaSession>, OrcaError> {
            if self.unavailable {
                return Err(OrcaError::Unavailable("down".into()));
            }
            Ok(self.sessions.clone())
        }
        fn send_text(&mut self, h: &str, t: &str) -> Result<(), OrcaError> {
            self.check(h)?;
            self.log.push(format!("text {h} {t}"));
            Ok(())
        }
        fn send_enter(&mut self, h: &str) -> Result<(), OrcaError> {
            self.check(h)?;
            self.log.push(format!("enter {h}"));
            Ok(())
        }
        fn send_backspaces(&mut self, h: &str, n: usize) -> Result<(), OrcaError> {
            self.check(h)?;
            self.log.push(format!("bs {h} {n}"));
            Ok(())
        }
        fn switch(&mut self, h: &str) -> Result<(), OrcaError> {
            self.check(h)?;
            self.log.push(format!("switch {h}"));
            Ok(())
        }
    }

    impl FakeOrca {
        fn check(&self, h: &str) -> Result<(), OrcaError> {
            if self.unavailable {
                return Err(OrcaError::Unavailable("down".into()));
            }
            if self.sessions.iter().any(|s| s.handle == h) {
                Ok(())
            } else {
                Err(OrcaError::Stale)
            }
        }
    }

    type C = Companion<FakeInjector, FakeRecognizer, FakeOrca>;

    fn session(h: &str, leaf: &str, wt: &str, title: &str) -> OrcaSession {
        OrcaSession {
            handle: h.into(),
            leaf_id: leaf.into(),
            worktree: wt.into(),
            title: title.into(),
        }
    }

    fn companion() -> C {
        let mut inj = FakeInjector {
            trusted: true,
            ..Default::default()
        };
        inj.running.insert("com.mitchellh.ghostty".into());
        inj.running.insert("com.tencent.xinWeChat".into());
        let orca = FakeOrca {
            sessions: vec![
                session("term_a", "leaf_a", "my-passport", "语音输入"),
                session("term_b", "leaf_b", "voice-notes", "PR"),
            ],
            ..Default::default()
        };
        Companion::new(
            inj,
            FakeRecognizer::default(),
            orca,
            TargetsFile::defaults(),
            SavedState::default(),
            None,
        )
    }

    fn audio(dict: u8, seq: u16) -> DeviceFrame {
        let mut st = AdpcmState::default();
        let pcm: Vec<i16> = (0..320)
            .map(|n| ((n as f32 * 0.3).sin() * 3000.0) as i16)
            .collect();
        DeviceFrame::Audio(AudioFrame {
            dict,
            seq,
            pred: 0,
            index: 0,
            adpcm: encode(&mut st, &pcm),
        })
    }

    fn select(c: &mut C, list: u8, index: u8, now: Instant) {
        c.handle_frame(DeviceFrame::TargetsReq { list }, now);
        c.handle_frame(DeviceFrame::TargetSelect { list, index }, now);
        c.take_outbox();
    }

    /// Run a whole Dictation that recognizes `text`.
    fn dictate(c: &mut C, dict: u8, text: &str, t0: Instant) -> Vec<CompanionFrame> {
        c.handle_frame(DeviceFrame::DictStart { dict }, t0);
        c.handle_frame(audio(dict, 0), t0);
        c.handle_frame(DeviceFrame::DictStop { dict }, t0);
        c.handle_recog(
            RecogEvent::Final {
                dict,
                text: text.into(),
            },
            t0,
        );
        c.take_outbox()
    }

    #[test]
    fn hello_gets_ack_then_target_state() {
        let mut c = companion();
        let now = Instant::now();
        c.handle_frame(
            DeviceFrame::Hello {
                ver: 1,
                fw: "0.1".into(),
            },
            now,
        );
        let out = c.take_outbox();
        assert_eq!(out[0], CompanionFrame::HelloAck { ver: 1 });
        assert_eq!(
            out[1],
            CompanionFrame::TargetState {
                status: Status::Ok,
                kind: TargetKind::FollowFocus,
                label: "跟随当前焦点 · Ghostty".into()
            }
        );
        assert_eq!(
            out[2],
            CompanionFrame::Status {
                code: StatusCode::Clear,
                text: String::new()
            }
        );
    }

    #[test]
    fn hello_reports_missing_permissions() {
        let mut c = companion();
        c.injector.trusted = false;
        c.handle_frame(
            DeviceFrame::Hello {
                ver: 1,
                fw: String::new(),
            },
            Instant::now(),
        );
        let out = c.take_outbox();
        assert!(matches!(
            out[1],
            CompanionFrame::TargetState {
                status: Status::Permission,
                ..
            }
        ));
        assert!(matches!(
            out[2],
            CompanionFrame::Status {
                code: StatusCode::AccessibilityPermission,
                ..
            }
        ));
        c.recognizer.health = Some(RecognizerHealth::NoPermission);
        c.handle_frame(
            DeviceFrame::Hello {
                ver: 1,
                fw: String::new(),
            },
            Instant::now(),
        );
        let out = c.take_outbox();
        assert!(matches!(
            out[2],
            CompanionFrame::Status {
                code: StatusCode::SpeechPermission,
                ..
            }
        ));
    }

    #[test]
    fn dictation_inserts_and_replies_result() {
        let mut c = companion();
        let out = dictate(&mut c, 4, " 把这个函数改成异步 ", Instant::now());
        assert!(matches!(out[0], CompanionFrame::TargetState { .. }));
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::Result {
                dict: 4,
                status: Status::Ok,
                text: "把这个函数改成异步".into()
            }
        );
        assert_eq!(c.injector.log, ["insert None 把这个函数改成异步"]);
        assert_eq!(c.recognizer.started, [4]);
        assert_eq!(c.recognizer.samples, 320);
        assert_eq!(c.recognizer.finished, 1);
        assert!(!c.dictation_active());
    }

    #[test]
    fn audio_gaps_reach_recognizer_as_silence() {
        let mut c = companion();
        let now = Instant::now();
        c.handle_frame(DeviceFrame::DictStart { dict: 1 }, now);
        c.handle_frame(audio(1, 0), now);
        c.handle_frame(audio(1, 2), now);
        c.handle_frame(audio(2, 3), now); // wrong dictation
        assert_eq!(c.recognizer.samples, 320 * 3);
    }

    #[test]
    fn partials_are_throttled() {
        let mut c = companion();
        let t0 = Instant::now();
        c.handle_frame(DeviceFrame::DictStart { dict: 1 }, t0);
        c.take_outbox();
        c.handle_recog(
            RecogEvent::Partial {
                dict: 1,
                text: "把".into(),
            },
            t0,
        );
        c.handle_recog(
            RecogEvent::Partial {
                dict: 1,
                text: "把这".into(),
            },
            t0 + Duration::from_millis(50),
        );
        c.handle_recog(
            RecogEvent::Partial {
                dict: 1,
                text: "把这个".into(),
            },
            t0 + Duration::from_millis(100),
        );
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::Partial {
                dict: 1,
                text: "把".into()
            }]
        );
        let due = c.poll(t0 + Duration::from_millis(120)).unwrap();
        assert_eq!(due, t0 + PARTIAL_INTERVAL);
        assert!(c.take_outbox().is_empty());
        assert_eq!(c.poll(due), None);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::Partial {
                dict: 1,
                text: "把这个".into()
            }]
        );
        // Stale dictation ids are ignored.
        c.handle_recog(
            RecogEvent::Partial {
                dict: 9,
                text: "x".into(),
            },
            t0 + Duration::from_secs(1),
        );
        assert!(c.take_outbox().is_empty());
    }

    #[test]
    fn final_timeout_uses_best_partial() {
        let mut c = companion();
        let t0 = Instant::now();
        c.handle_frame(DeviceFrame::DictStart { dict: 2 }, t0);
        c.handle_recog(
            RecogEvent::Partial {
                dict: 2,
                text: "你好".into(),
            },
            t0,
        );
        c.handle_frame(DeviceFrame::DictStop { dict: 2 }, t0);
        c.take_outbox();
        assert_eq!(
            c.poll(t0 + Duration::from_secs(1)),
            Some(t0 + FINAL_TIMEOUT)
        );
        c.poll(t0 + FINAL_TIMEOUT);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::Result {
                dict: 2,
                status: Status::Ok,
                text: "你好".into()
            }]
        );
        assert_eq!(c.recognizer.cancelled, 1);
        // A late final for a finished dictation is ignored.
        c.handle_recog(
            RecogEvent::Final {
                dict: 2,
                text: "你好。".into(),
            },
            t0 + FINAL_TIMEOUT,
        );
        assert!(c.take_outbox().is_empty());
    }

    #[test]
    fn empty_and_no_speech() {
        let mut c = companion();
        let out = dictate(&mut c, 1, "  ", Instant::now());
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::Result {
                dict: 1,
                status: Status::Empty,
                text: String::new()
            }
        );
        let t0 = Instant::now();
        c.handle_frame(DeviceFrame::DictStart { dict: 2 }, t0);
        c.handle_frame(DeviceFrame::DictStop { dict: 2 }, t0);
        c.handle_recog(
            RecogEvent::Error {
                dict: 2,
                message: "kAFAssistantErrorDomain 1110 No speech detected".into(),
            },
            t0,
        );
        assert_eq!(
            c.take_outbox().last().unwrap(),
            &CompanionFrame::Result {
                dict: 2,
                status: Status::Empty,
                text: String::new()
            }
        );
        assert!(c.injector.log.is_empty());
    }

    #[test]
    fn recognizer_error_reported() {
        let mut c = companion();
        let t0 = Instant::now();
        c.handle_frame(DeviceFrame::DictStart { dict: 3 }, t0);
        c.handle_frame(DeviceFrame::DictStop { dict: 3 }, t0);
        c.handle_recog(
            RecogEvent::Error {
                dict: 3,
                message: "boom".into(),
            },
            t0,
        );
        assert_eq!(
            c.take_outbox().last().unwrap(),
            &CompanionFrame::Result {
                dict: 3,
                status: Status::RecognizerError,
                text: String::new()
            }
        );
        c.recognizer.health = Some(RecognizerHealth::NoPermission);
        c.handle_frame(DeviceFrame::DictStart { dict: 4 }, t0);
        c.handle_frame(audio(4, 0), t0);
        c.handle_frame(DeviceFrame::DictStop { dict: 4 }, t0);
        let out = c.take_outbox();
        assert!(out.contains(&CompanionFrame::Status {
            code: StatusCode::SpeechPermission,
            text: "需要语音识别权限".into()
        }));
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::Result {
                dict: 4,
                status: Status::Permission,
                text: String::new()
            }
        );
        assert_eq!(c.recognizer.samples, 0);
    }

    #[test]
    fn cancel_discards() {
        let mut c = companion();
        let t0 = Instant::now();
        c.handle_frame(DeviceFrame::DictStart { dict: 5 }, t0);
        c.handle_recog(
            RecogEvent::Partial {
                dict: 5,
                text: "abc".into(),
            },
            t0,
        );
        c.take_outbox();
        c.handle_frame(DeviceFrame::DictCancel { dict: 5 }, t0);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::Result {
                dict: 5,
                status: Status::Cancelled,
                text: String::new()
            }]
        );
        assert_eq!(c.recognizer.cancelled, 1);
        c.handle_recog(
            RecogEvent::Final {
                dict: 5,
                text: "abc".into(),
            },
            t0,
        );
        assert!(c.take_outbox().is_empty());
        assert!(c.injector.log.is_empty());
    }

    #[test]
    fn disconnect_abandons_without_insert() {
        let mut c = companion();
        let t0 = Instant::now();
        c.handle_frame(DeviceFrame::DictStart { dict: 6 }, t0);
        c.handle_frame(DeviceFrame::DictStop { dict: 6 }, t0);
        c.on_disconnected();
        c.handle_recog(
            RecogEvent::Final {
                dict: 6,
                text: "x".into(),
            },
            t0,
        );
        assert!(c.take_outbox().is_empty());
        assert!(c.injector.log.is_empty());
        assert_eq!(c.recognizer.cancelled, 1);
    }

    #[test]
    fn undo_once_counts_graphemes() {
        let mut c = companion();
        let t0 = Instant::now();
        dictate(&mut c, 1, "改成异步👍🏽ok", t0);
        c.handle_frame(DeviceFrame::Undo, t0);
        c.handle_frame(DeviceFrame::Undo, t0);
        assert_eq!(
            c.take_outbox(),
            [
                CompanionFrame::ActionResult {
                    action: Action::Undo,
                    status: Status::Ok
                },
                CompanionFrame::ActionResult {
                    action: Action::Undo,
                    status: Status::NothingToUndo
                },
            ]
        );
        assert_eq!(c.injector.log.last().unwrap(), "delete None 7");
        assert_eq!(undo_count("é"), 1);
    }

    #[test]
    fn follow_focus_undo_targets_app_that_got_text() {
        let mut c = companion();
        c.injector.frontmost = Some("com.mitchellh.ghostty".into());
        let t0 = Instant::now();
        dictate(&mut c, 1, "你好", t0);
        c.injector.frontmost = Some("com.tencent.xinWeChat".into());
        c.handle_frame(DeviceFrame::Undo, t0);
        assert_eq!(
            c.injector.log.last().unwrap(),
            "delete Some(\"com.mitchellh.ghostty\") 2"
        );
    }

    #[test]
    fn new_dictation_clears_undo() {
        let mut c = companion();
        let t0 = Instant::now();
        dictate(&mut c, 1, "你好", t0);
        c.handle_frame(DeviceFrame::DictStart { dict: 2 }, t0);
        c.handle_frame(DeviceFrame::DictCancel { dict: 2 }, t0);
        c.take_outbox();
        c.handle_frame(DeviceFrame::Undo, t0);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::ActionResult {
                action: Action::Undo,
                status: Status::NothingToUndo
            }]
        );
    }

    #[test]
    fn submit_clears_undo() {
        let mut c = companion();
        let t0 = Instant::now();
        dictate(&mut c, 1, "你好", t0);
        c.handle_frame(DeviceFrame::Submit, t0);
        c.take_outbox();
        c.handle_frame(DeviceFrame::Undo, t0);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::ActionResult {
                action: Action::Undo,
                status: Status::NothingToUndo
            }]
        );
    }

    #[test]
    fn submit_follow_focus() {
        let mut c = companion();
        c.handle_frame(DeviceFrame::Submit, Instant::now());
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::ActionResult {
                action: Action::Submit,
                status: Status::Ok
            }]
        );
        assert_eq!(c.injector.log, ["submit None"]);
    }

    #[test]
    fn root_list_flags_and_app_selection() {
        let mut c = companion();
        let now = Instant::now();
        c.handle_frame(DeviceFrame::TargetsReq { list: 0 }, now);
        let out = c.take_outbox();
        assert_eq!(out.len(), 9);
        let flags: Vec<u8> = out[..8]
            .iter()
            .map(|f| match f {
                CompanionFrame::TargetItem { flags, count, .. } => {
                    assert_eq!(*count, 8);
                    *flags
                }
                _ => panic!(),
            })
            .collect();
        // follow(current), orca(sublist, Orca app not running), ghostty, cursor(nr),
        // wechat, wework(nr), chatgpt(nr), codex(nr)
        assert_eq!(flags, [1, 2 | 4, 0, 4, 0, 4, 4, 4]);
        assert_eq!(out[8], CompanionFrame::TargetEnd { list: 0, count: 8 });

        c.handle_frame(DeviceFrame::TargetSelect { list: 0, index: 4 }, now);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::TargetState {
                status: Status::Ok,
                kind: TargetKind::App,
                label: "微信 · com.tencent.xinWeChat-chat".into()
            }]
        );
        assert_eq!(c.saved_state().selected.as_deref(), Some("wechat"));
        dictate(&mut c, 1, "晚上吃什么", now);
        c.handle_frame(DeviceFrame::Undo, now);
        c.handle_frame(DeviceFrame::Submit, now);
        assert_eq!(
            c.injector.log,
            [
                "insert Some(\"com.tencent.xinWeChat\") 晚上吃什么",
                "delete Some(\"com.tencent.xinWeChat\") 5",
                "submit Some(\"com.tencent.xinWeChat\")"
            ]
        );
    }

    #[test]
    fn app_not_running_is_unavailable_never_launched() {
        let mut c = companion();
        let now = Instant::now();
        select(&mut c, 0, 3, now); // Cursor, not running
        c.handle_frame(
            DeviceFrame::Hello {
                ver: 1,
                fw: String::new(),
            },
            now,
        );
        let out = c.take_outbox();
        assert_eq!(
            out[1],
            CompanionFrame::TargetState {
                status: Status::TargetUnavailable,
                kind: TargetKind::App,
                label: "Cursor".into()
            }
        );
        let out = dictate(&mut c, 1, "你好", now);
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::Result {
                dict: 1,
                status: Status::TargetUnavailable,
                text: String::new()
            }
        );
        c.handle_frame(DeviceFrame::Submit, now);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::ActionResult {
                action: Action::Submit,
                status: Status::TargetUnavailable
            }]
        );
        assert!(c.injector.log.is_empty());
    }

    #[test]
    fn app_path_resolution() {
        let mut c = companion();
        let now = Instant::now();
        select(&mut c, 0, 7, now);
        c.handle_frame(DeviceFrame::Submit, now);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::ActionResult {
                action: Action::Submit,
                status: Status::TargetUnavailable
            }]
        );
        c.injector.running.insert("codex.desktop".into());
        c.handle_frame(DeviceFrame::Submit, now);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::ActionResult {
                action: Action::Submit,
                status: Status::Ok
            }]
        );
    }

    #[test]
    fn selecting_orca_row_keeps_target() {
        let mut c = companion();
        let now = Instant::now();
        c.handle_frame(DeviceFrame::TargetsReq { list: 0 }, now);
        c.take_outbox();
        c.handle_frame(DeviceFrame::TargetSelect { list: 0, index: 1 }, now);
        assert!(matches!(
            &c.take_outbox()[0],
            CompanionFrame::TargetState {
                kind: TargetKind::FollowFocus,
                ..
            }
        ));
        c.handle_frame(DeviceFrame::TargetSelect { list: 0, index: 99 }, now);
        assert!(matches!(
            &c.take_outbox()[0],
            CompanionFrame::TargetState {
                kind: TargetKind::FollowFocus,
                ..
            }
        ));
    }

    #[test]
    fn orca_session_flow() {
        let mut c = companion();
        let now = Instant::now();
        c.handle_frame(DeviceFrame::TargetsReq { list: 1 }, now);
        assert_eq!(
            c.take_outbox(),
            [
                CompanionFrame::TargetItem {
                    list: 1,
                    index: 0,
                    count: 2,
                    flags: 0,
                    label: "my-passport · 语音输入".into()
                },
                CompanionFrame::TargetItem {
                    list: 1,
                    index: 1,
                    count: 2,
                    flags: 0,
                    label: "voice-notes · PR".into()
                },
                CompanionFrame::TargetEnd { list: 1, count: 2 },
            ]
        );
        c.handle_frame(DeviceFrame::TargetSelect { list: 1, index: 1 }, now);
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::TargetState {
                status: Status::Ok,
                kind: TargetKind::OrcaSession,
                label: "voice-notes · PR".into()
            }]
        );
        assert_eq!(c.saved_state().selected.as_deref(), Some("orca"));
        assert_eq!(c.saved_state().orca.as_ref().unwrap().handle, "term_b");
        let out = dictate(&mut c, 1, "运行测试", now);
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::Result {
                dict: 1,
                status: Status::Ok,
                text: "运行测试".into()
            }
        );
        c.handle_frame(DeviceFrame::Undo, now);
        c.handle_frame(DeviceFrame::Submit, now);
        assert_eq!(
            c.orca.log,
            [
                "switch term_b",
                "text term_b 运行测试",
                "switch term_b",
                "bs term_b 4",
                "enter term_b"
            ]
        );
        assert!(c.injector.log.is_empty());
        // Current flag in the sub-list.
        c.take_outbox();
        c.handle_frame(DeviceFrame::TargetsReq { list: 1 }, now);
        assert!(matches!(
            c.take_outbox()[1],
            CompanionFrame::TargetItem {
                flags: FLAG_CURRENT,
                ..
            }
        ));
    }

    #[test]
    fn orca_session_gone_or_down() {
        let mut c = companion();
        let now = Instant::now();
        select(&mut c, 1, 0, now);
        // Orca restarted: new handle, same pane.
        c.orca.sessions[0].handle = "term_new".into();
        let out = dictate(&mut c, 1, "继续", now);
        assert!(matches!(
            out[0],
            CompanionFrame::TargetState {
                status: Status::Ok,
                ..
            }
        ));
        assert_eq!(c.saved_state().orca.as_ref().unwrap().handle, "term_new");
        // Pane closed.
        c.orca.sessions.remove(0);
        let out = dictate(&mut c, 2, "继续", now);
        assert!(matches!(
            out[0],
            CompanionFrame::TargetState {
                status: Status::TargetUnavailable,
                kind: TargetKind::OrcaSession,
                ..
            }
        ));
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::Result {
                dict: 2,
                status: Status::TargetUnavailable,
                text: String::new()
            }
        );
        // Orca down: STATUS 3 and an empty list.
        c.orca.unavailable = true;
        c.handle_frame(DeviceFrame::TargetsReq { list: 1 }, now);
        let out = c.take_outbox();
        assert!(out.contains(&CompanionFrame::Status {
            code: StatusCode::OrcaUnavailable,
            text: "Orca 不可用".into()
        }));
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::TargetEnd { list: 1, count: 0 }
        );
    }

    #[test]
    fn persisted_selection_survives_restart() {
        let dir = std::env::temp_dir().join(format!("vv-sess-{}", std::process::id()));
        let path = dir.join("state.toml");
        let _ = std::fs::remove_dir_all(&dir);
        let mut c = companion();
        c.state_path = Some(path.clone());
        select(&mut c, 0, 2, Instant::now());
        let state = SavedState::load(&path);
        assert_eq!(state.selected.as_deref(), Some("ghostty"));
        let mut c2 = companion();
        c2.state = state;
        c2.handle_frame(
            DeviceFrame::Hello {
                ver: 1,
                fw: String::new(),
            },
            Instant::now(),
        );
        assert!(
            matches!(&c2.take_outbox()[1], CompanionFrame::TargetState { kind: TargetKind::App, label, .. } if label.starts_with("Ghostty"))
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn insert_permission_failure() {
        let mut c = companion();
        c.injector.fail_insert = Some(InjectError::Permission);
        let out = dictate(&mut c, 1, "你好", Instant::now());
        assert_eq!(
            out.last().unwrap(),
            &CompanionFrame::Result {
                dict: 1,
                status: Status::Permission,
                text: String::new()
            }
        );
        c.handle_frame(DeviceFrame::Undo, Instant::now());
        assert_eq!(
            c.take_outbox(),
            [CompanionFrame::ActionResult {
                action: Action::Undo,
                status: Status::NothingToUndo
            }]
        );
    }

    #[test]
    fn labels_fit_and_keep_name() {
        for max in [ITEM_LABEL_BYTES, STATE_LABEL_BYTES] {
            let l = compose_label("企业微信", Some(&"很长的会话标题".repeat(20)), max);
            assert!(l.starts_with("企业微信 · "));
            assert!(l.len() <= max, "{} > {max}", l.len());
            assert!(l.ends_with('…'));
        }
        assert_eq!(compose_label("Orca", Some("  "), 71), "Orca");
        assert_eq!(
            compose_label("my-passport", Some("语音输入"), 71),
            "my-passport · 语音输入"
        );
        let long_name = compose_label(&"名".repeat(40), Some("t"), 71);
        assert!(long_name.starts_with(&"名".repeat(15)) && long_name.len() <= 71);
    }
}
