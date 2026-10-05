//! Process wiring: threads, channels, logging, and the CLI modes.

use objc2::MainThreadMarker;
use std::io::IsTerminal;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::mpsc::{self, RecvTimeoutError};
use std::time::{Duration, Instant, SystemTime};
use vibe_voice::ble::{self, LinkEvent};
use vibe_voice::config::{self, SavedState, TargetsFile};
use vibe_voice::inject_macos::{self, MacInjector};
use vibe_voice::orca::{OrcaApi, OrcaClient, OrcaError, OrcaSession, ProcessRunner};
use vibe_voice::protocol::{AudioFrame, CompanionFrame, DeviceFrame, SAMPLES_PER_FRAME};
use vibe_voice::session::{Companion, InjectError, Injector, RecogEvent};
use vibe_voice::speech::{self, AppleRecognizer};
use vibe_voice::{adpcm, ui};

enum CoreEvent {
    Link(LinkEvent),
    Recog(RecogEvent),
}

fn init_logging(verbose: bool) {
    let mut b =
        env_logger::Builder::from_env(env_logger::Env::default().default_filter_or(if verbose {
            "debug"
        } else {
            "info"
        }));
    b.format_timestamp_millis();
    if !std::io::stderr().is_terminal() {
        // Started from Finder: log to ~/Library/Logs/VibeVoice.log.
        let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
        let path = PathBuf::from(home).join("Library/Logs/VibeVoice.log");
        if let Ok(f) = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
        {
            b.target(env_logger::Target::Pipe(Box::new(f)));
        }
    }
    b.init();
}

pub fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let verbose = args.iter().any(|a| a == "-v" || a == "--verbose");
    init_logging(verbose);
    let mode = args
        .iter()
        .find(|a| !a.starts_with("-v") && *a != "--verbose")
        .map(String::as_str);
    match mode {
        None => run_companion(),
        Some("--simulate") => {
            let Some(file) = args.iter().skip_while(|a| *a != "--simulate").nth(1) else {
                eprintln!("usage: vibe-voice --simulate <16kHz-mono-s16.wav|.pcm>");
                std::process::exit(2);
            };
            std::process::exit(simulate(Path::new(file)));
        }
        Some("--orca-list") => orca_list(),
        Some("--check") => check(),
        Some("-h" | "--help") => {
            println!(
                "vibe-voice [--verbose]            run the Companion\n\
                 vibe-voice --simulate <file>      recognize a 16 kHz mono 16-bit WAV/PCM file (no BLE, no insert)\n\
                 vibe-voice --orca-list            list Orca Sessions (read-only)\n\
                 vibe-voice --check                show permission status"
            );
        }
        Some(other) => {
            eprintln!("unknown argument {other}; see --help");
            std::process::exit(2);
        }
    }
}

// ----- normal operation ----------------------------------------------------

fn file_mtime(p: &Path) -> Option<SystemTime> {
    std::fs::metadata(p).and_then(|m| m.modified()).ok()
}

fn run_companion() {
    let mtm = MainThreadMarker::new().expect("main thread");
    log::info!(
        "Vibe Voice Companion {} starting",
        env!("CARGO_PKG_VERSION")
    );

    // Permission prompts (non-blocking for the rest of the app).
    if !inject_macos::accessibility_trusted(true) {
        log::warn!(
            "Accessibility permission missing: enable Vibe Voice in System Settings > Privacy & Security > Accessibility"
        );
    }
    std::thread::spawn(|| {
        let s = speech::request_authorization(Duration::from_secs(600));
        log::info!("speech recognition permission: {}", speech::status_name(s));
    });

    let (core_tx, core_rx) = mpsc::channel::<CoreEvent>();
    let (out_tx, out_rx) = tokio::sync::mpsc::unbounded_channel::<Vec<u8>>();

    // BLE on a tokio runtime thread.
    let link_tx = core_tx.clone();
    std::thread::Builder::new()
        .name("ble".into())
        .spawn(move || {
            let rt = tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .enable_all()
                .build()
                .expect("tokio runtime");
            let events: ble::EventSink = Arc::new(move |e| {
                let _ = link_tx.send(CoreEvent::Link(e));
            });
            let last = std::sync::Mutex::new(String::new());
            let state: ble::StateSink = Arc::new(move |s: &str| {
                let mut last = last.lock().unwrap_or_else(|e| e.into_inner());
                if *last != s {
                    log::info!("link: {s}");
                    ui::set_state(s);
                    *last = s.to_owned();
                }
            });
            rt.block_on(ble::run(out_rx, events, state));
        })
        .expect("ble thread");

    // Companion logic on its own thread.
    let recog_tx = core_tx.clone();
    std::thread::Builder::new()
        .name("core".into())
        .spawn(move || core_loop(core_rx, recog_tx, out_tx))
        .expect("core thread");

    ui::install(mtm);
    ui::set_state("搜索设备…");
    ui::run(mtm);
}

const HEALTH_INTERVAL: Duration = Duration::from_secs(2);

fn core_loop(
    rx: mpsc::Receiver<CoreEvent>,
    self_tx: mpsc::Sender<CoreEvent>,
    out: tokio::sync::mpsc::UnboundedSender<Vec<u8>>,
) {
    let dir = config::config_dir();
    let targets_path = dir.join("targets.toml");
    let state_path = dir.join("state.toml");
    let targets = TargetsFile::load_or_create(&targets_path);
    let mut targets_mtime = file_mtime(&targets_path);
    let state = SavedState::load(&state_path);
    let recognizer = AppleRecognizer::new(Arc::new(move |e| {
        let _ = self_tx.send(CoreEvent::Recog(e));
    }));
    let orca = OrcaClient::new(ProcessRunner::locate());
    let mut c = Companion::new(
        MacInjector,
        recognizer,
        orca,
        targets,
        state,
        Some(state_path),
    );
    let mut connected = false;
    let mut deadline: Option<Instant> = None;
    let mut next_health = Instant::now() + HEALTH_INTERVAL;
    loop {
        // Permissions can be granted while running; poll them while linked.
        if connected {
            deadline = Some(deadline.map_or(next_health, |d| d.min(next_health)));
        }
        let ev = match deadline {
            Some(d) => match rx.recv_timeout(d.saturating_duration_since(Instant::now())) {
                Ok(ev) => Some(ev),
                Err(RecvTimeoutError::Timeout) => None,
                Err(RecvTimeoutError::Disconnected) => return,
            },
            None => match rx.recv() {
                Ok(ev) => Some(ev),
                Err(_) => return,
            },
        };
        let now = Instant::now();
        match ev {
            Some(CoreEvent::Link(LinkEvent::Connected(name))) => {
                log::info!("Device {name} linked; waiting for HELLO");
                connected = true;
            }
            Some(CoreEvent::Link(LinkEvent::Disconnected)) => {
                connected = false;
                c.on_disconnected();
            }
            Some(CoreEvent::Link(LinkEvent::Frame(bytes))) => match DeviceFrame::decode(&bytes) {
                Ok(frame) => {
                    if matches!(
                        frame,
                        DeviceFrame::Hello { .. } | DeviceFrame::TargetsReq { list: 0 }
                    ) {
                        let m = file_mtime(&targets_path);
                        if m != targets_mtime {
                            targets_mtime = m;
                            log::info!("reloading {}", targets_path.display());
                            c.set_targets(TargetsFile::load_or_create(&targets_path));
                        }
                    }
                    if !matches!(frame, DeviceFrame::Audio(_)) {
                        log::debug!("<- {frame:?}");
                    }
                    c.handle_frame(frame, now);
                }
                Err(e) => log::warn!("bad frame from Device: {e}"),
            },
            Some(CoreEvent::Recog(e)) => c.handle_recog(e, now),
            None => {}
        }
        if connected && now >= next_health {
            next_health = now + HEALTH_INTERVAL;
            c.refresh_health();
        }
        deadline = c.poll(Instant::now());
        for f in c.take_outbox() {
            log::debug!("-> {f:?}");
            if connected {
                let _ = out.send(f.encode());
            }
        }
    }
}

// ----- diagnostics ---------------------------------------------------------

fn check() {
    let s = speech::authorization_status();
    println!("Speech Recognition: {}", speech::status_name(s));
    println!(
        "Accessibility:      {}",
        if inject_macos::accessibility_trusted(false) {
            "trusted"
        } else {
            "missing"
        }
    );
    let mut r = AppleRecognizer::new(Arc::new(|_| {}));
    use vibe_voice::session::Recognizer;
    println!("Recognizer zh-CN:   {:?}", r.health());
    let dir = config::config_dir();
    println!("Config:             {}", dir.join("targets.toml").display());
}

fn orca_list() {
    let mut c = OrcaClient::new(ProcessRunner::locate());
    match c.list() {
        Ok(list) => {
            for (i, s) in list.iter().enumerate() {
                println!("{i:2}  {}  [{}]", s.label(), s.handle);
            }
            println!("{} Orca Sessions", list.len());
        }
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    }
}

// ----- simulate ------------------------------------------------------------

/// Never touches the user's apps: prints what would be inserted.
struct DryRunInjector;

impl Injector for DryRunInjector {
    fn accessibility_trusted(&mut self) -> bool {
        true
    }
    fn is_running(&mut self, _: &str) -> bool {
        true
    }
    fn resolve_bundle_id(&mut self, _: &str) -> Option<String> {
        None
    }
    fn frontmost_bundle_id(&mut self) -> Option<String> {
        None
    }
    fn focused_title(&mut self, _: Option<&str>) -> Option<(String, String)> {
        Some(("simulate".into(), String::new()))
    }
    fn insert(&mut self, _: Option<&str>, text: &str) -> Result<(), InjectError> {
        println!("[dry-run] would insert: {text}");
        Ok(())
    }
    fn submit(&mut self, _: Option<&str>) -> Result<(), InjectError> {
        Ok(())
    }
    fn delete_back(&mut self, _: Option<&str>, _: usize) -> Result<(), InjectError> {
        Ok(())
    }
}

struct NoOrca;

impl OrcaApi for NoOrca {
    fn list(&mut self) -> Result<Vec<OrcaSession>, OrcaError> {
        Ok(Vec::new())
    }
    fn send_text(&mut self, _: &str, _: &str) -> Result<(), OrcaError> {
        Err(OrcaError::Stale)
    }
    fn send_enter(&mut self, _: &str) -> Result<(), OrcaError> {
        Err(OrcaError::Stale)
    }
    fn send_backspaces(&mut self, _: &str, _: usize) -> Result<(), OrcaError> {
        Err(OrcaError::Stale)
    }
    fn switch(&mut self, _: &str) -> Result<(), OrcaError> {
        Err(OrcaError::Stale)
    }
}

/// Read 16 kHz mono signed 16-bit audio from a WAV (PCM) or raw file.
fn read_pcm(path: &Path) -> Result<Vec<i16>, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let data: &[u8] = if bytes.len() >= 12 && &bytes[0..4] == b"RIFF" && &bytes[8..12] == b"WAVE" {
        let mut pos = 12;
        let mut fmt_ok = false;
        let mut data = None;
        while pos + 8 <= bytes.len() {
            let id = &bytes[pos..pos + 4];
            let len = u32::from_le_bytes(bytes[pos + 4..pos + 8].try_into().unwrap()) as usize;
            let body = &bytes[pos + 8..(pos + 8 + len).min(bytes.len())];
            if id == b"fmt " && body.len() >= 16 {
                let format = u16::from_le_bytes([body[0], body[1]]);
                let channels = u16::from_le_bytes([body[2], body[3]]);
                let rate = u32::from_le_bytes(body[4..8].try_into().unwrap());
                let bits = u16::from_le_bytes([body[14], body[15]]);
                if format != 1 || channels != 1 || rate != 16_000 || bits != 16 {
                    return Err(format!(
                        "need 16 kHz mono 16-bit PCM WAV (got format {format}, {channels} ch, {rate} Hz, {bits} bit)"
                    ));
                }
                fmt_ok = true;
            } else if id == b"data" {
                data = Some(body);
            }
            pos += 8 + len + (len & 1);
        }
        if !fmt_ok {
            return Err("WAV without a usable fmt chunk".into());
        }
        data.ok_or("WAV without data")?
    } else {
        &bytes
    };
    Ok(data
        .as_chunks::<2>()
        .0
        .iter()
        .map(|b| i16::from_le_bytes(*b))
        .collect())
}

fn print_frames(frames: Vec<CompanionFrame>) -> Option<CompanionFrame> {
    let mut result = None;
    for f in frames {
        match &f {
            CompanionFrame::Partial { text, .. } => println!("PARTIAL  {text}"),
            CompanionFrame::Result { .. } => {
                println!("RESULT   {f:?}");
                result = Some(f);
            }
            other => println!("FRAME    {other:?}"),
        }
    }
    result
}

fn simulate(path: &Path) -> i32 {
    let pcm = match read_pcm(path) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("{e}");
            return 2;
        }
    };
    println!(
        "audio: {} samples ({:.2} s)",
        pcm.len(),
        pcm.len() as f32 / 16_000.0
    );
    let status = speech::request_authorization(Duration::from_secs(60));
    println!("speech permission: {}", speech::status_name(status));

    let (tx, rx) = mpsc::channel::<RecogEvent>();
    let recognizer = AppleRecognizer::new(Arc::new(move |e| {
        let _ = tx.send(e);
    }));
    let mut c = Companion::new(
        DryRunInjector,
        recognizer,
        NoOrca,
        TargetsFile::defaults(),
        SavedState::default(),
        None,
    );
    let t0 = Instant::now();
    c.handle_frame(
        DeviceFrame::Hello {
            ver: 1,
            fw: "simulate".into(),
        },
        t0,
    );
    c.handle_frame(DeviceFrame::DictStart { dict: 1 }, t0);
    print_frames(c.take_outbox());

    // Encode like the firmware: one ADPCM stream, header carries the state
    // before each frame.
    let mut enc = adpcm::AdpcmState::default();
    for (seq, chunk) in pcm.chunks(SAMPLES_PER_FRAME).enumerate() {
        let mut samples = chunk.to_vec();
        samples.resize(SAMPLES_PER_FRAME, 0);
        let (pred, index) = (enc.predictor, enc.index);
        let data = adpcm::encode(&mut enc, &samples);
        let frame = AudioFrame {
            dict: 1,
            seq: seq as u16,
            pred,
            index,
            adpcm: data,
        };
        c.handle_frame(DeviceFrame::Audio(frame), Instant::now());
        while let Ok(e) = rx.try_recv() {
            c.handle_recog(e, Instant::now());
        }
        c.poll(Instant::now());
        print_frames(c.take_outbox());
        // About 4x real time.
        std::thread::sleep(Duration::from_millis(5));
    }
    c.handle_frame(DeviceFrame::DictStop { dict: 1 }, Instant::now());
    if let Some(CompanionFrame::Result { status, .. }) = print_frames(c.take_outbox()) {
        return if status == vibe_voice::protocol::Status::Ok {
            0
        } else {
            1
        };
    }
    let end = Instant::now() + Duration::from_secs(10);
    loop {
        let wait = c
            .poll(Instant::now())
            .unwrap_or(Instant::now() + Duration::from_millis(200));
        let wait = wait.min(end).saturating_duration_since(Instant::now());
        if let Ok(e) = rx.recv_timeout(wait) {
            c.handle_recog(e, Instant::now());
        }
        c.poll(Instant::now());
        if let Some(CompanionFrame::Result { status, .. }) = print_frames(c.take_outbox()) {
            return if status == vibe_voice::protocol::Status::Ok {
                0
            } else {
                1
            };
        }
        if Instant::now() >= end {
            eprintln!("no RESULT within 10 s");
            return 1;
        }
    }
}
