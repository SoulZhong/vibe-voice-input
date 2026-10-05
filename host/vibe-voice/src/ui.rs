//! Minimal menu bar item: the link state, launch at login, and Quit.

use crate::login_macos::{self, LoginState};
use dispatch2::DispatchQueue;
use objc2::rc::Retained;
use objc2::runtime::{AnyObject, NSObject};
use objc2::{MainThreadMarker, MainThreadOnly, define_class, msg_send, sel};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSControlStateValueOff, NSControlStateValueOn,
    NSMenu, NSMenuItem, NSStatusBar, NSStatusItem, NSVariableStatusItemLength,
};
use objc2_foundation::NSString;
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
    let item = NSStatusBar::systemStatusBar().statusItemWithLength(NSVariableStatusItemLength);
    if let Some(button) = item.button(mtm) {
        button.setTitle(&NSString::from_str("VV ○"));
    }
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
                let connected = text.starts_with("已连接");
                if let Some(button) = ui.item.button(mtm) {
                    button.setTitle(&NSString::from_str(if connected {
                        "VV ●"
                    } else {
                        "VV ○"
                    }));
                }
                ui.state_row
                    .setTitle(&NSString::from_str(&format!("Vibe Voice：{text}")));
            }
        });
    });
}

/// Run the AppKit main loop (never returns).
pub fn run(mtm: MainThreadMarker) {
    NSApplication::sharedApplication(mtm).run();
}
