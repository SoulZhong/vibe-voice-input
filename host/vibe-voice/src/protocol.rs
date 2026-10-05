//! Vibe Voice BLE protocol v1 codec (see `docs/vibe-voice/protocol.md`).
//!
//! Pure data handling: no I/O. Every encoded frame is at most [`MAX_FRAME`]
//! bytes; text that does not fit is sent as its tail, cut on a UTF-8 boundary.

use std::fmt;

/// Protocol version carried in HELLO / HELLO_ACK.
pub const PROTOCOL_VERSION: u8 = 1;
/// Maximum size of one frame (one GATT write or notification).
pub const MAX_FRAME: usize = 180;
/// Samples carried by one AUDIO frame (20 ms at 16 kHz).
pub const SAMPLES_PER_FRAME: usize = 320;
/// ADPCM payload bytes in one AUDIO frame.
pub const ADPCM_BYTES: usize = SAMPLES_PER_FRAME / 2;

pub const NUS_SERVICE: &str = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
pub const NUS_RX: &str = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
pub const NUS_TX: &str = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
/// Advertised name prefix of a Device.
pub const NAME_PREFIX: &str = "VibeVoice-";

pub mod ty {
    pub const HELLO: u8 = 0x01;
    pub const DICT_START: u8 = 0x10;
    pub const AUDIO: u8 = 0x11;
    pub const DICT_STOP: u8 = 0x12;
    pub const DICT_CANCEL: u8 = 0x13;
    pub const SUBMIT: u8 = 0x20;
    pub const UNDO: u8 = 0x21;
    pub const TARGETS_REQ: u8 = 0x30;
    pub const TARGET_SELECT: u8 = 0x31;

    pub const HELLO_ACK: u8 = 0x81;
    pub const STATUS: u8 = 0x82;
    pub const PARTIAL: u8 = 0x90;
    pub const RESULT: u8 = 0x91;
    pub const ACTION_RESULT: u8 = 0xA0;
    pub const TARGET_ITEM: u8 = 0xB0;
    pub const TARGET_END: u8 = 0xB1;
    pub const TARGET_STATE: u8 = 0xB2;
}

/// RESULT / ACTION_RESULT / TARGET_STATE status.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Status {
    Ok = 0,
    Empty = 1,
    Cancelled = 2,
    TargetUnavailable = 3,
    RecognizerError = 4,
    Permission = 5,
    NothingToUndo = 6,
}

/// STATUS frame codes (Companion-level problems).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum StatusCode {
    Clear = 0,
    SpeechPermission = 1,
    AccessibilityPermission = 2,
    OrcaUnavailable = 3,
    RecognizerUnavailable = 4,
}

/// TARGET_STATE kind.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum TargetKind {
    FollowFocus = 0,
    App = 1,
    OrcaSession = 2,
}

/// Which list a TARGETS_REQ / TARGET_ITEM refers to.
pub const LIST_ROOT: u8 = 0;
pub const LIST_ORCA: u8 = 1;

/// TARGET_ITEM flags.
pub const FLAG_CURRENT: u8 = 0x01;
pub const FLAG_SUBLIST: u8 = 0x02;
pub const FLAG_NOT_RUNNING: u8 = 0x04;

/// ACTION_RESULT action codes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Action {
    Submit = 0x20,
    Undo = 0x21,
}

/// One AUDIO frame.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AudioFrame {
    pub dict: u8,
    pub seq: u16,
    pub pred: i16,
    pub index: u8,
    pub adpcm: Vec<u8>,
}

/// A frame sent by the Device.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DeviceFrame {
    Hello { ver: u8, fw: String },
    DictStart { dict: u8 },
    Audio(AudioFrame),
    DictStop { dict: u8 },
    DictCancel { dict: u8 },
    Submit,
    Undo,
    TargetsReq { list: u8 },
    TargetSelect { list: u8, index: u8 },
}

/// A frame sent by the Companion.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CompanionFrame {
    HelloAck {
        ver: u8,
    },
    Status {
        code: StatusCode,
        text: String,
    },
    Partial {
        dict: u8,
        text: String,
    },
    Result {
        dict: u8,
        status: Status,
        text: String,
    },
    ActionResult {
        action: Action,
        status: Status,
    },
    TargetItem {
        list: u8,
        index: u8,
        count: u8,
        flags: u8,
        label: String,
    },
    TargetEnd {
        list: u8,
        count: u8,
    },
    TargetState {
        status: Status,
        kind: TargetKind,
        label: String,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DecodeError {
    Empty,
    TooLong(usize),
    UnknownType(u8),
    Truncated { ty: u8, len: usize },
    BadUtf8 { ty: u8 },
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            DecodeError::Empty => write!(f, "empty frame"),
            DecodeError::TooLong(n) => write!(f, "frame of {n} bytes exceeds {MAX_FRAME}"),
            DecodeError::UnknownType(t) => write!(f, "unknown frame type 0x{t:02x}"),
            DecodeError::Truncated { ty, len } => {
                write!(f, "frame 0x{ty:02x} truncated ({len} bytes)")
            }
            DecodeError::BadUtf8 { ty } => write!(f, "frame 0x{ty:02x} has invalid UTF-8"),
        }
    }
}

impl std::error::Error for DecodeError {}

/// Return the longest tail of `text` that fits in `max` bytes and starts on a
/// UTF-8 character boundary.
pub fn utf8_tail(text: &str, max: usize) -> &str {
    if text.len() <= max {
        return text;
    }
    let mut start = text.len() - max;
    while !text.is_char_boundary(start) {
        start += 1;
    }
    &text[start..]
}

/// Return the longest head of `text` that fits in `max` bytes, cut on a UTF-8
/// character boundary.
pub fn utf8_head(text: &str, max: usize) -> &str {
    if text.len() <= max {
        return text;
    }
    let mut end = max;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

fn need(bytes: &[u8], len: usize) -> Result<(), DecodeError> {
    if bytes.len() < len {
        Err(DecodeError::Truncated {
            ty: bytes[0],
            len: bytes.len(),
        })
    } else {
        Ok(())
    }
}

impl DeviceFrame {
    pub fn decode(bytes: &[u8]) -> Result<Self, DecodeError> {
        if bytes.is_empty() {
            return Err(DecodeError::Empty);
        }
        if bytes.len() > MAX_FRAME {
            return Err(DecodeError::TooLong(bytes.len()));
        }
        let t = bytes[0];
        Ok(match t {
            ty::HELLO => {
                need(bytes, 2)?;
                let fw = std::str::from_utf8(&bytes[2..])
                    .map_err(|_| DecodeError::BadUtf8 { ty: t })?
                    .to_owned();
                DeviceFrame::Hello { ver: bytes[1], fw }
            }
            ty::DICT_START => {
                need(bytes, 2)?;
                DeviceFrame::DictStart { dict: bytes[1] }
            }
            ty::AUDIO => {
                need(bytes, 7 + ADPCM_BYTES)?;
                DeviceFrame::Audio(AudioFrame {
                    dict: bytes[1],
                    seq: u16::from_le_bytes([bytes[2], bytes[3]]),
                    pred: i16::from_le_bytes([bytes[4], bytes[5]]),
                    index: bytes[6],
                    adpcm: bytes[7..7 + ADPCM_BYTES].to_vec(),
                })
            }
            ty::DICT_STOP => {
                need(bytes, 2)?;
                DeviceFrame::DictStop { dict: bytes[1] }
            }
            ty::DICT_CANCEL => {
                need(bytes, 2)?;
                DeviceFrame::DictCancel { dict: bytes[1] }
            }
            ty::SUBMIT => DeviceFrame::Submit,
            ty::UNDO => DeviceFrame::Undo,
            ty::TARGETS_REQ => {
                need(bytes, 2)?;
                DeviceFrame::TargetsReq { list: bytes[1] }
            }
            ty::TARGET_SELECT => {
                need(bytes, 3)?;
                DeviceFrame::TargetSelect {
                    list: bytes[1],
                    index: bytes[2],
                }
            }
            other => return Err(DecodeError::UnknownType(other)),
        })
    }

    /// Encode (used by tests and the simulator to play the Device role).
    pub fn encode(&self) -> Vec<u8> {
        match self {
            DeviceFrame::Hello { ver, fw } => {
                let mut v = vec![ty::HELLO, *ver];
                v.extend_from_slice(utf8_head(fw, MAX_FRAME - 2).as_bytes());
                v
            }
            DeviceFrame::DictStart { dict } => vec![ty::DICT_START, *dict],
            DeviceFrame::Audio(a) => {
                let mut v = Vec::with_capacity(7 + ADPCM_BYTES);
                v.push(ty::AUDIO);
                v.push(a.dict);
                v.extend_from_slice(&a.seq.to_le_bytes());
                v.extend_from_slice(&a.pred.to_le_bytes());
                v.push(a.index);
                v.extend_from_slice(&a.adpcm);
                v
            }
            DeviceFrame::DictStop { dict } => vec![ty::DICT_STOP, *dict],
            DeviceFrame::DictCancel { dict } => vec![ty::DICT_CANCEL, *dict],
            DeviceFrame::Submit => vec![ty::SUBMIT],
            DeviceFrame::Undo => vec![ty::UNDO],
            DeviceFrame::TargetsReq { list } => vec![ty::TARGETS_REQ, *list],
            DeviceFrame::TargetSelect { list, index } => vec![ty::TARGET_SELECT, *list, *index],
        }
    }
}

fn with_text(mut head: Vec<u8>, text: &str) -> Vec<u8> {
    let room = MAX_FRAME - head.len();
    head.extend_from_slice(utf8_tail(text, room).as_bytes());
    head
}

fn status_from(b: u8) -> Option<Status> {
    Some(match b {
        0 => Status::Ok,
        1 => Status::Empty,
        2 => Status::Cancelled,
        3 => Status::TargetUnavailable,
        4 => Status::RecognizerError,
        5 => Status::Permission,
        6 => Status::NothingToUndo,
        _ => return None,
    })
}

impl CompanionFrame {
    /// Encode to at most [`MAX_FRAME`] bytes. Text fields are cut to their tail.
    pub fn encode(&self) -> Vec<u8> {
        match self {
            CompanionFrame::HelloAck { ver } => vec![ty::HELLO_ACK, *ver],
            CompanionFrame::Status { code, text } => with_text(vec![ty::STATUS, *code as u8], text),
            CompanionFrame::Partial { dict, text } => with_text(vec![ty::PARTIAL, *dict], text),
            CompanionFrame::Result { dict, status, text } => {
                with_text(vec![ty::RESULT, *dict, *status as u8], text)
            }
            CompanionFrame::ActionResult { action, status } => {
                vec![ty::ACTION_RESULT, *action as u8, *status as u8]
            }
            CompanionFrame::TargetItem {
                list,
                index,
                count,
                flags,
                label,
            } => with_text(vec![ty::TARGET_ITEM, *list, *index, *count, *flags], label),
            CompanionFrame::TargetEnd { list, count } => vec![ty::TARGET_END, *list, *count],
            CompanionFrame::TargetState {
                status,
                kind,
                label,
            } => with_text(vec![ty::TARGET_STATE, *status as u8, *kind as u8], label),
        }
    }

    /// Decode (used by tests and the simulator to play the Device role).
    pub fn decode(bytes: &[u8]) -> Result<Self, DecodeError> {
        if bytes.is_empty() {
            return Err(DecodeError::Empty);
        }
        if bytes.len() > MAX_FRAME {
            return Err(DecodeError::TooLong(bytes.len()));
        }
        let t = bytes[0];
        let text = |from: usize| -> Result<String, DecodeError> {
            std::str::from_utf8(&bytes[from..])
                .map(str::to_owned)
                .map_err(|_| DecodeError::BadUtf8 { ty: t })
        };
        let st = |b: u8| {
            status_from(b).ok_or(DecodeError::Truncated {
                ty: t,
                len: bytes.len(),
            })
        };
        Ok(match t {
            ty::HELLO_ACK => {
                need(bytes, 2)?;
                CompanionFrame::HelloAck { ver: bytes[1] }
            }
            ty::STATUS => {
                need(bytes, 2)?;
                let code = match bytes[1] {
                    0 => StatusCode::Clear,
                    1 => StatusCode::SpeechPermission,
                    2 => StatusCode::AccessibilityPermission,
                    3 => StatusCode::OrcaUnavailable,
                    4 => StatusCode::RecognizerUnavailable,
                    _ => {
                        return Err(DecodeError::Truncated {
                            ty: t,
                            len: bytes.len(),
                        });
                    }
                };
                CompanionFrame::Status {
                    code,
                    text: text(2)?,
                }
            }
            ty::PARTIAL => {
                need(bytes, 2)?;
                CompanionFrame::Partial {
                    dict: bytes[1],
                    text: text(2)?,
                }
            }
            ty::RESULT => {
                need(bytes, 3)?;
                CompanionFrame::Result {
                    dict: bytes[1],
                    status: st(bytes[2])?,
                    text: text(3)?,
                }
            }
            ty::ACTION_RESULT => {
                need(bytes, 3)?;
                let action = match bytes[1] {
                    0x20 => Action::Submit,
                    0x21 => Action::Undo,
                    _ => {
                        return Err(DecodeError::Truncated {
                            ty: t,
                            len: bytes.len(),
                        });
                    }
                };
                CompanionFrame::ActionResult {
                    action,
                    status: st(bytes[2])?,
                }
            }
            ty::TARGET_ITEM => {
                need(bytes, 5)?;
                CompanionFrame::TargetItem {
                    list: bytes[1],
                    index: bytes[2],
                    count: bytes[3],
                    flags: bytes[4],
                    label: text(5)?,
                }
            }
            ty::TARGET_END => {
                need(bytes, 3)?;
                CompanionFrame::TargetEnd {
                    list: bytes[1],
                    count: bytes[2],
                }
            }
            ty::TARGET_STATE => {
                need(bytes, 3)?;
                let kind = match bytes[2] {
                    0 => TargetKind::FollowFocus,
                    1 => TargetKind::App,
                    2 => TargetKind::OrcaSession,
                    _ => {
                        return Err(DecodeError::Truncated {
                            ty: t,
                            len: bytes.len(),
                        });
                    }
                };
                CompanionFrame::TargetState {
                    status: st(bytes[1])?,
                    kind,
                    label: text(3)?,
                }
            }
            other => return Err(DecodeError::UnknownType(other)),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tail_respects_utf8_boundaries() {
        let s = "把这个函数改成异步"; // 9 chars * 3 bytes
        assert_eq!(utf8_tail(s, 100), s);
        assert_eq!(utf8_tail(s, 6), "异步");
        assert_eq!(utf8_tail(s, 7), "异步");
        assert_eq!(utf8_tail(s, 8), "异步");
        assert_eq!(utf8_tail(s, 9), "成异步");
        assert_eq!(utf8_tail(s, 2), "");
        assert_eq!(utf8_tail("abc", 2), "bc");
        assert_eq!(utf8_head(s, 7), "把这");
    }

    #[test]
    fn long_partial_is_cut_to_tail_within_limit() {
        let text: String = "测试".repeat(100) + "结尾";
        let bytes = CompanionFrame::Partial {
            dict: 7,
            text: text.clone(),
        }
        .encode();
        assert!(bytes.len() <= MAX_FRAME);
        assert_eq!(bytes[0], ty::PARTIAL);
        assert_eq!(bytes[1], 7);
        let body = std::str::from_utf8(&bytes[2..]).unwrap();
        assert!(text.ends_with(body));
        assert!(body.ends_with("结尾"));
        // 178 bytes of room; 3-byte chars → 59 chars = 177 bytes.
        assert_eq!(body.len(), 177);
    }

    #[test]
    fn mixed_width_tail_never_splits() {
        for n in 0..10 {
            let text = format!("{}{}", "a".repeat(n), "é中🎉".repeat(40));
            for f in [
                CompanionFrame::Result {
                    dict: 1,
                    status: Status::Ok,
                    text: text.clone(),
                },
                CompanionFrame::Status {
                    code: StatusCode::Clear,
                    text: text.clone(),
                },
                CompanionFrame::TargetItem {
                    list: 0,
                    index: 0,
                    count: 1,
                    flags: 0,
                    label: text.clone(),
                },
                CompanionFrame::TargetState {
                    status: Status::Ok,
                    kind: TargetKind::App,
                    label: text.clone(),
                },
            ] {
                let b = f.encode();
                assert!(b.len() <= MAX_FRAME);
                let back = CompanionFrame::decode(&b).expect("valid utf8 after cut");
                let _ = back;
            }
        }
    }

    #[test]
    fn companion_frames_byte_layout() {
        assert_eq!(CompanionFrame::HelloAck { ver: 1 }.encode(), vec![0x81, 1]);
        assert_eq!(
            CompanionFrame::Status {
                code: StatusCode::AccessibilityPermission,
                text: "AX".into()
            }
            .encode(),
            vec![0x82, 2, b'A', b'X']
        );
        assert_eq!(
            CompanionFrame::Result {
                dict: 3,
                status: Status::Cancelled,
                text: String::new()
            }
            .encode(),
            vec![0x91, 3, 2]
        );
        assert_eq!(
            CompanionFrame::ActionResult {
                action: Action::Undo,
                status: Status::NothingToUndo
            }
            .encode(),
            vec![0xA0, 0x21, 6]
        );
        assert_eq!(
            CompanionFrame::TargetItem {
                list: 1,
                index: 2,
                count: 5,
                flags: FLAG_CURRENT | FLAG_NOT_RUNNING,
                label: "x".into()
            }
            .encode(),
            vec![0xB0, 1, 2, 5, 5, b'x']
        );
        assert_eq!(
            CompanionFrame::TargetEnd { list: 0, count: 8 }.encode(),
            vec![0xB1, 0, 8]
        );
        assert_eq!(
            CompanionFrame::TargetState {
                status: Status::TargetUnavailable,
                kind: TargetKind::OrcaSession,
                label: "o".into()
            }
            .encode(),
            vec![0xB2, 3, 2, b'o']
        );
    }

    #[test]
    fn companion_roundtrip() {
        let frames = vec![
            CompanionFrame::HelloAck { ver: 1 },
            CompanionFrame::Partial {
                dict: 255,
                text: "你好".into(),
            },
            CompanionFrame::Result {
                dict: 0,
                status: Status::Ok,
                text: "完成".into(),
            },
            CompanionFrame::ActionResult {
                action: Action::Submit,
                status: Status::Ok,
            },
            CompanionFrame::TargetEnd { list: 1, count: 0 },
        ];
        for f in frames {
            assert_eq!(CompanionFrame::decode(&f.encode()).unwrap(), f);
        }
    }

    #[test]
    fn device_frames_decode() {
        assert_eq!(
            DeviceFrame::decode(&[0x01, 1, b'f', b'w']).unwrap(),
            DeviceFrame::Hello {
                ver: 1,
                fw: "fw".into()
            }
        );
        assert_eq!(
            DeviceFrame::decode(&[0x10, 9]).unwrap(),
            DeviceFrame::DictStart { dict: 9 }
        );
        assert_eq!(
            DeviceFrame::decode(&[0x12, 9]).unwrap(),
            DeviceFrame::DictStop { dict: 9 }
        );
        assert_eq!(
            DeviceFrame::decode(&[0x13, 9]).unwrap(),
            DeviceFrame::DictCancel { dict: 9 }
        );
        assert_eq!(DeviceFrame::decode(&[0x20]).unwrap(), DeviceFrame::Submit);
        assert_eq!(DeviceFrame::decode(&[0x21]).unwrap(), DeviceFrame::Undo);
        assert_eq!(
            DeviceFrame::decode(&[0x30, 1]).unwrap(),
            DeviceFrame::TargetsReq { list: 1 }
        );
        assert_eq!(
            DeviceFrame::decode(&[0x31, 0, 4]).unwrap(),
            DeviceFrame::TargetSelect { list: 0, index: 4 }
        );
        assert_eq!(DeviceFrame::decode(&[]), Err(DecodeError::Empty));
        assert_eq!(
            DeviceFrame::decode(&[0x55]),
            Err(DecodeError::UnknownType(0x55))
        );
        assert!(matches!(
            DeviceFrame::decode(&[0x31, 0]),
            Err(DecodeError::Truncated { .. })
        ));
        assert!(matches!(
            DeviceFrame::decode(&[0x01, 1, 0xff]),
            Err(DecodeError::BadUtf8 { .. })
        ));
        assert!(matches!(
            DeviceFrame::decode(&[0x20; 181]),
            Err(DecodeError::TooLong(181))
        ));
    }

    #[test]
    fn audio_frame_layout() {
        let mut raw = vec![0x11, 5, 0x34, 0x12, 0xfe, 0xff, 42];
        raw.extend((0..160).map(|i| i as u8));
        assert_eq!(raw.len(), 167);
        match DeviceFrame::decode(&raw).unwrap() {
            DeviceFrame::Audio(a) => {
                assert_eq!(a.dict, 5);
                assert_eq!(a.seq, 0x1234);
                assert_eq!(a.pred, -2);
                assert_eq!(a.index, 42);
                assert_eq!(a.adpcm.len(), 160);
                assert_eq!(a.adpcm[159], 159);
                assert_eq!(DeviceFrame::Audio(a).encode(), raw);
            }
            other => panic!("unexpected {other:?}"),
        }
        assert!(matches!(
            DeviceFrame::decode(&raw[..100]),
            Err(DecodeError::Truncated { .. })
        ));
    }
}
