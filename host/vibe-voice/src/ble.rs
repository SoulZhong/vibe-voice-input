//! BLE central: find a `VibeVoice-` Device, connect, pair (via the macOS
//! passkey prompt), subscribe to NUS TX and carry frames both ways.
//! Reconnects forever with backoff.

use crate::protocol::{MAX_FRAME, NAME_PREFIX, NUS_RX, NUS_SERVICE, NUS_TX, ty};
use btleplug::api::{
    Central, CentralEvent, Characteristic, Manager as _, Peripheral as _, ScanFilter, WriteType,
};
use btleplug::platform::{Adapter, Manager, Peripheral};
use futures::StreamExt;
use std::sync::Arc;
use std::time::Duration;
use tokio::sync::mpsc::UnboundedReceiver;
use tokio::time::{sleep, timeout};
use uuid::Uuid;

/// A healthy write with response completes within one or two connection
/// intervals; far longer means the link is gone.
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);

/// Link events delivered to the Companion.
#[derive(Debug)]
pub enum LinkEvent {
    Connected(String),
    Frame(Vec<u8>),
    Disconnected,
}

/// Human-readable link state for the menu bar and log.
pub type StateSink = Arc<dyn Fn(&str) + Send + Sync>;
pub type EventSink = Arc<dyn Fn(LinkEvent) + Send + Sync>;

const SCAN_WINDOW: Duration = Duration::from_secs(4);
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
/// Time allowed for the user to type the passkey into the macOS prompt.
const PAIRING_WINDOW: Duration = Duration::from_secs(120);
const MAX_BACKOFF: Duration = Duration::from_secs(30);

fn uuid(s: &str) -> Uuid {
    Uuid::parse_str(s).expect("valid UUID constant")
}

async fn adapter(manager: &Manager) -> Option<Adapter> {
    match manager.adapters().await {
        Ok(list) => list.into_iter().next(),
        Err(e) => {
            log::warn!("Bluetooth adapters unavailable: {e}");
            None
        }
    }
}

/// Scan until a Device shows up. Prefers `wanted` (exact name) when given.
async fn find_device(adapter: &Adapter, wanted: Option<&str>) -> Option<(Peripheral, String)> {
    if let Err(e) = adapter.start_scan(ScanFilter::default()).await {
        log::warn!("cannot scan (Bluetooth off or permission denied?): {e}");
        return None;
    }
    sleep(SCAN_WINDOW).await;
    let mut found = None;
    if let Ok(list) = adapter.peripherals().await {
        for p in list {
            let Ok(Some(props)) = p.properties().await else {
                continue;
            };
            let Some(name) = props.local_name else {
                continue;
            };
            if !name.starts_with(NAME_PREFIX) || wanted.is_some_and(|w| w != name) {
                continue;
            }
            let rssi = props.rssi.unwrap_or(i16::MIN);
            if found.as_ref().is_none_or(|(_, _, r)| rssi > *r) {
                found = Some((p, name, rssi));
            }
        }
    }
    let _ = adapter.stop_scan().await;
    found.map(|(p, n, _)| (p, n))
}

fn is_auth_error(msg: &str) -> bool {
    let m = msg.to_lowercase();
    m.contains("auth") || m.contains("encrypt") || m.contains("pair") || m.contains("insufficient")
}

/// Subscribe to TX, waiting for the user to complete pairing if needed.
async fn subscribe_with_pairing(p: &Peripheral, tx: &Characteristic, state: &StateSink) -> bool {
    let deadline = tokio::time::Instant::now() + PAIRING_WINDOW;
    let mut prompted = false;
    loop {
        match timeout(Duration::from_secs(60), p.subscribe(tx)).await {
            Ok(Ok(())) => return true,
            Ok(Err(e)) => {
                let msg = e.to_string();
                if is_auth_error(&msg) && !prompted {
                    prompted = true;
                    state("配对中：请在 Mac 弹窗输入设备屏幕上的 6 位配对码");
                    log::warn!(
                        "pairing required ({msg}); type the passkey shown on the Device into the macOS prompt"
                    );
                } else {
                    log::warn!("subscribe failed: {msg}");
                }
            }
            Err(_) => log::warn!("subscribe timed out (pairing prompt still open?)"),
        }
        if tokio::time::Instant::now() >= deadline || !p.is_connected().await.unwrap_or(false) {
            return false;
        }
        sleep(Duration::from_secs(2)).await;
    }
}

/// Run one connection until it drops. Returns true if the session got going.
async fn run_session(
    adapter: &Adapter,
    p: Peripheral,
    name: &str,
    outgoing: &mut UnboundedReceiver<Vec<u8>>,
    events: &EventSink,
    state: &StateSink,
) -> bool {
    state(&format!("连接中 {name}"));
    match timeout(CONNECT_TIMEOUT, p.connect()).await {
        Ok(Ok(())) => {}
        Ok(Err(e)) => {
            log::warn!("connect {name}: {e}");
            return false;
        }
        Err(_) => {
            log::warn!("connect {name}: timeout");
            let _ = p.disconnect().await;
            return false;
        }
    }
    if let Err(e) = p.discover_services().await {
        log::warn!("discover services: {e}");
        let _ = p.disconnect().await;
        return false;
    }
    let chars = p.characteristics();
    let tx_uuid = uuid(NUS_TX);
    let rx_uuid = uuid(NUS_RX);
    let service = uuid(NUS_SERVICE);
    let tx = chars
        .iter()
        .find(|c| c.uuid == tx_uuid && c.service_uuid == service)
        .cloned();
    let rx = chars
        .iter()
        .find(|c| c.uuid == rx_uuid && c.service_uuid == service)
        .cloned();
    let (Some(tx), Some(rx)) = (tx, rx) else {
        log::warn!("{name} has no Nordic UART Service");
        let _ = p.disconnect().await;
        return false;
    };
    let mut notifications = match p.notifications().await {
        Ok(s) => s,
        Err(e) => {
            log::warn!("notifications: {e}");
            let _ = p.disconnect().await;
            return false;
        }
    };
    if !subscribe_with_pairing(&p, &tx, state).await {
        let _ = p.disconnect().await;
        return false;
    }
    // Frames queued for an earlier link are stale.
    while outgoing.try_recv().is_ok() {}
    log::info!("connected to {name}");
    state(&format!("已连接 {name}"));
    events(LinkEvent::Connected(name.to_owned()));

    let id = p.id();
    let mut central = adapter.events().await.ok();
    let mut check = tokio::time::interval(Duration::from_secs(3));
    loop {
        tokio::select! {
            n = notifications.next() => match n {
                Some(n) if n.uuid == tx_uuid => events(LinkEvent::Frame(n.value)),
                Some(_) => {}
                None => break,
            },
            out = outgoing.recv() => {
                let Some(frame) = out else { break };
                debug_assert!(frame.len() <= MAX_FRAME);
                // PARTIAL is superseded by the next one; everything else must land.
                let kind = if frame.first() == Some(&ty::PARTIAL) { WriteType::WithoutResponse } else { WriteType::WithResponse };
                // A write to a link macOS silently replaced never completes, and
                // this loop would then stop reading the Device's frames too.
                match timeout(WRITE_TIMEOUT, p.write(&rx, &frame, kind)).await {
                    Ok(Ok(())) => {}
                    Ok(Err(e)) => {
                        log::warn!("write failed: {e}");
                        if !p.is_connected().await.unwrap_or(false) {
                            break;
                        }
                    }
                    Err(_) => {
                        log::warn!("write timed out; reconnecting");
                        break;
                    }
                }
            }
            ev = async { match central.as_mut() { Some(s) => s.next().await, None => std::future::pending().await } } => {
                if let Some(CentralEvent::DeviceDisconnected(d)) = ev && d == id {
                    break;
                }
            }
            _ = check.tick() => {
                if !timeout(WRITE_TIMEOUT, p.is_connected()).await.ok().and_then(Result::ok).unwrap_or(false) {
                    break;
                }
            }
        }
    }
    log::info!("disconnected from {name}");
    events(LinkEvent::Disconnected);
    let _ = p.disconnect().await;
    true
}

/// Keep a Device connected forever.
pub async fn run(mut outgoing: UnboundedReceiver<Vec<u8>>, events: EventSink, state: StateSink) {
    let wanted = std::env::var("VIBE_VOICE_DEVICE").ok();
    let mut backoff = Duration::from_secs(1);
    let manager = loop {
        match Manager::new().await {
            Ok(m) => break m,
            Err(e) => {
                log::error!("Bluetooth unavailable: {e}");
                state("蓝牙不可用");
                sleep(MAX_BACKOFF).await;
            }
        }
    };
    let mut last_name: Option<String> = None;
    loop {
        let Some(adapter) = adapter(&manager).await else {
            state("蓝牙不可用");
            sleep(backoff).await;
            backoff = (backoff * 2).min(MAX_BACKOFF);
            continue;
        };
        state("搜索设备…");
        let want = wanted.as_deref().or(last_name.as_deref());
        let mut found = find_device(&adapter, want).await;
        if found.is_none() && wanted.is_none() && last_name.is_some() {
            // The remembered Device is away; accept any Vibe Voice Device.
            found = find_device(&adapter, None).await;
        }
        let Some((p, name)) = found else {
            // Nothing in range yet: keep scanning without growing the backoff.
            sleep(Duration::from_secs(1)).await;
            continue;
        };
        let ok = run_session(&adapter, p, &name, &mut outgoing, &events, &state).await;
        if ok {
            last_name = Some(name);
        }
        if ok {
            backoff = Duration::from_secs(1);
        } else {
            backoff = (backoff * 2).min(MAX_BACKOFF);
        }
        state("未连接，稍后重试");
        sleep(backoff).await;
    }
}
