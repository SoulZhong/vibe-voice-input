//! Launch at login through `SMAppService.mainApp` (macOS 13+).

use crate::login_item::{Settings, running_installed, settings_path};
use objc2_service_management::{SMAppService, SMAppServiceStatus};

/// What the menu shows for the login item.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoginState {
    /// Not an installed copy: the item is disabled.
    NotInstalled,
    On,
    Off,
    /// Registered, but the user must allow it in System Settings.
    NeedsApproval,
}

fn status() -> SMAppServiceStatus {
    unsafe { SMAppService::mainAppService().status() }
}

fn status_name(s: SMAppServiceStatus) -> &'static str {
    match s {
        SMAppServiceStatus::NotRegistered => "not registered",
        SMAppServiceStatus::Enabled => "enabled",
        SMAppServiceStatus::RequiresApproval => "requires approval",
        SMAppServiceStatus::NotFound => "not found",
        _ => "unknown",
    }
}

fn register() {
    match unsafe { SMAppService::mainAppService().registerAndReturnError() } {
        Ok(()) => log::info!("login item registered"),
        Err(e) => log::warn!("login item register failed: {e:?}"),
    }
}

fn unregister() {
    match unsafe { SMAppService::mainAppService().unregisterAndReturnError() } {
        Ok(()) => log::info!("login item unregistered"),
        Err(e) => log::warn!("login item unregister failed: {e:?}"),
    }
}

fn current() -> LoginState {
    if !running_installed() {
        return LoginState::NotInstalled;
    }
    match status() {
        SMAppServiceStatus::Enabled => LoginState::On,
        SMAppServiceStatus::RequiresApproval => LoginState::NeedsApproval,
        _ => LoginState::Off,
    }
}

/// At startup: register an installed copy unless the user turned it off.
/// Development builds never touch the login items.
pub fn sync_at_startup() -> LoginState {
    let settings = Settings::load(&settings_path());
    if !running_installed() {
        log::info!("launch at login: not an installed copy; login items left alone");
        return LoginState::NotInstalled;
    }
    let s = status();
    log::info!(
        "launch at login: wanted={}, status={}",
        settings.launch_at_login,
        status_name(s)
    );
    if settings.launch_at_login && s != SMAppServiceStatus::Enabled {
        register();
    }
    let state = current();
    if state == LoginState::NeedsApproval {
        log::warn!("launch at login needs approval: System Settings > General > Login Items");
    }
    state
}

/// The menu toggle: flip, persist, apply.
pub fn toggle() -> LoginState {
    let path = settings_path();
    let mut settings = Settings::load(&path);
    if !running_installed() {
        return LoginState::NotInstalled;
    }
    settings.launch_at_login = !settings.launch_at_login;
    settings.save(&path);
    if settings.launch_at_login {
        register();
    } else {
        unregister();
    }
    let state = current();
    if state == LoginState::NeedsApproval {
        unsafe { SMAppService::openSystemSettingsLoginItems() };
    }
    state
}
