//! Minimal menu bar item: the link state, launch at login, and Quit.

use crate::login_macos::{self, LoginState};
use dispatch2::DispatchQueue;
use objc2::rc::Retained;
use objc2::runtime::{AnyObject, NSObject};
use objc2::{AnyThread, MainThreadMarker, MainThreadOnly, define_class, msg_send, sel};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSControlStateValueOff, NSControlStateValueOn,
    NSImage, NSMenu, NSMenuItem, NSSquareStatusItemLength, NSStatusBar, NSStatusItem,
};
use objc2_foundation::{NSData, NSSize, NSString};
use std::cell::RefCell;

struct Ui {
    item: Retained<NSStatusItem>,
    state_row: Retained<NSMenuItem>,
    login_row: Retained<NSMenuItem>,
    login_hint: Retained<NSMenuItem>,
    /// Menu items hold their target weakly: keep it alive here.
    _target: Retained<MenuTarget>,
}

define_class!(
    #[unsafe(super(NSObject))]
    #[thread_kind = MainThreadOnly]
    #[name = "VVMenuTarget"]
    struct MenuTarget;

    impl MenuTarget {
        #[unsafe(method(toggleLaunchAtLogin:))]
        fn toggle_launch_at_login(&self, _sender: Option<&AnyObject>) {
            let state = login_macos::toggle();
            show_login(state);
        }
    }
);

impl MenuTarget {
    fn new(mtm: MainThreadMarker) -> Retained<Self> {
        unsafe { msg_send![Self::alloc(mtm), init] }
    }
}

fn menu_item(mtm: MainThreadMarker, title: &str) -> Retained<NSMenuItem> {
    unsafe {
        NSMenuItem::initWithTitle_action_keyEquivalent(
            NSMenuItem::alloc(mtm),
            &NSString::from_str(title),
            None,
            &NSString::from_str(""),
        )
    }
}

/// Reflect the login item state in the menu (main thread).
fn show_login(state: LoginState) {
    UI.with(|ui| {
        let Some(ui) = ui
            .borrow()
            .as_ref()
            .map(|u| (u.login_row.clone(), u.login_hint.clone()))
        else {
            return;
        };
        let (row, hint) = ui;
        row.setEnabled(state != LoginState::NotInstalled);
        row.setState(
            if matches!(state, LoginState::On | LoginState::NeedsApproval) {
                NSControlStateValueOn
            } else {
                NSControlStateValueOff
            },
        );
        let text = match state {
            LoginState::NotInstalled => Some("请先运行 install.sh 安装"),
            LoginState::NeedsApproval => Some("请在 系统设置 > 通用 > 登录项 中允许"),
            _ => None,
        };
        hint.setHidden(text.is_none());
        if let Some(t) = text {
            hint.setTitle(&NSString::from_str(t));
        }
    });
}

thread_local! {
    static UI: RefCell<Option<Ui>> = const { RefCell::new(None) };
}

/// Create the status item. Must run on the main thread before `run`.
pub fn install(mtm: MainThreadMarker) {
    let app = NSApplication::sharedApplication(mtm);
    app.setActivationPolicy(NSApplicationActivationPolicy::Accessory);
    // Menu bar space is scarce: one square template icon, no text.
    let item = NSStatusBar::systemStatusBar().statusItemWithLength(NSSquareStatusItemLength);
    show_link(&item, mtm, false, "启动中");
    let menu = NSMenu::new(mtm);
    let state_row = unsafe {
        NSMenuItem::initWithTitle_action_keyEquivalent(
            NSMenuItem::alloc(mtm),
            &NSString::from_str("Vibe Voice：启动中"),
            None,
            &NSString::from_str(""),
        )
    };
    state_row.setEnabled(false);
    menu.addItem(&state_row);
    menu.addItem(&NSMenuItem::separatorItem(mtm));
    let target = MenuTarget::new(mtm);
    let login_row = menu_item(mtm, "开机自启动");
    unsafe {
        login_row.setTarget(Some(&target));
        login_row.setAction(Some(sel!(toggleLaunchAtLogin:)));
    }
    menu.addItem(&login_row);
    let login_hint = menu_item(mtm, "");
    login_hint.setEnabled(false);
    login_hint.setHidden(true);
    menu.addItem(&login_hint);
    menu.addItem(&NSMenuItem::separatorItem(mtm));
    let quit = unsafe {
        NSMenuItem::initWithTitle_action_keyEquivalent(
            NSMenuItem::alloc(mtm),
            &NSString::from_str("退出 Vibe Voice"),
            Some(sel!(terminate:)),
            &NSString::from_str("q"),
        )
    };
    menu.addItem(&quit);
    item.setMenu(Some(&menu));
    // Menu enablement is manual so the disabled rows stay disabled.
    menu.setAutoenablesItems(false);
    UI.with(|ui| {
        *ui.borrow_mut() = Some(Ui {
            item,
            state_row,
            login_row,
            login_hint,
            _target: target,
        })
    });
    show_login(login_macos::sync_at_startup());
}

/// Update the shown state from any thread.
pub fn set_state(text: &str) {
    let text = text.to_owned();
    DispatchQueue::main().exec_async(move || {
        let Some(mtm) = MainThreadMarker::new() else {
            return;
        };
        UI.with(|ui| {
            if let Some(ui) = ui.borrow().as_ref() {
                show_link(&ui.item, mtm, text.starts_with("已连接"), &text);
                ui.state_row
                    .setTitle(&NSString::from_str(&format!("Vibe Voice：{text}")));
            }
        });
    });
}

/// Menu bar glyphs (template images, black + alpha, drawn at 2x for an
/// 18 pt square): the Device with voice bars when linked, an empty outline
/// otherwise. Sources: `assets/menubar/*.svg`.
const ICON_LINKED: &[u8] = include_bytes!("../assets/menubar/linked@2x.png");
const ICON_UNLINKED: &[u8] = include_bytes!("../assets/menubar/unlinked@2x.png");

fn show_link(item: &NSStatusItem, mtm: MainThreadMarker, connected: bool, text: &str) {
    let Some(button) = item.button(mtm) else {
        return;
    };
    let label = format!("Vibe Voice：{text}");
    let png = if connected {
        ICON_LINKED
    } else {
        ICON_UNLINKED
    };
    match NSImage::initWithData(NSImage::alloc(), &NSData::with_bytes(png)) {
        Some(image) => {
            image.setSize(NSSize::new(18.0, 18.0));
            image.setTemplate(true);
            image.setAccessibilityDescription(Some(&NSString::from_str(&label)));
            button.setImage(Some(&image));
            button.setTitle(&NSString::from_str(""));
        }
        None => button.setTitle(&NSString::from_str(if connected { "●" } else { "○" })),
    }
    button.setToolTip(Some(&NSString::from_str(&label)));
}

/// Run the AppKit main loop (never returns).
pub fn run(mtm: MainThreadMarker) {
    NSApplication::sharedApplication(mtm).run();
}
