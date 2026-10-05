//! Minimal menu bar item showing the link state, with a Quit command.

use dispatch2::DispatchQueue;
use objc2::rc::Retained;
use objc2::{MainThreadMarker, MainThreadOnly, sel};
use objc2_app_kit::{
    NSApplication, NSApplicationActivationPolicy, NSMenu, NSMenuItem, NSStatusBar, NSStatusItem,
    NSVariableStatusItemLength,
};
use objc2_foundation::NSString;
use std::cell::RefCell;

struct Ui {
    item: Retained<NSStatusItem>,
    state_row: Retained<NSMenuItem>,
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
    UI.with(|ui| *ui.borrow_mut() = Some(Ui { item, state_row }));
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
