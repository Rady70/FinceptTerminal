// src/app/WindowFrame_Auth.cpp
//
// Local-first routing (MarketLab Terminal, FINCEPT_FORK_PLAN.md §5.1).
//
// The upstream file routed between a login/register/pricing auth stack and
// the workspace. In this fork there is no account session: the local
// workspace is always reachable, and only the optional local PIN lock can
// cover it. These handlers keep the same public names so the rest of the
// shell (WindowFrame.cpp wiring) stays unchanged where possible.
//
// Part of the partial-class split of WindowFrame.cpp.

#include "app/WindowFrame.h"
#include "auth/AuthManager.h"
#include "auth/InactivityGuard.h"
#include "auth/PinManager.h"
#include "core/capability/CapabilityManager.h"
#include "core/layout/WorkspaceShell.h"
#include "core/logging/Logger.h"
#include "screens/auth/LockScreen.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/navigation/DockStatusBar.h"
#include "ui/navigation/DockToolBar.h"

#include <QApplication>
#include <QStackedWidget>

#include <DockManager.h>

namespace fincept {

void WindowFrame::on_auth_state_changed() {
    // MarketLab: local workspace access is unconditional. This slot exists
    // because the AuthManager signals are still wired; whatever state the
    // (unused) account session reports, the workspace must stay reachable.
    auto& auth = auth::AuthManager::instance();
    Q_UNUSED(auth);
    LOG_DEBUG("WindowFrame", "on_auth_state_changed: local-first shell — keeping workspace visible");
    set_shell_visible(true);
    if (stack_->currentIndex() != 0)
        stack_->setCurrentIndex(0);
}

void WindowFrame::show_lock_screen() {
    // MarketLab: the local PIN lock no longer requires an account session.
    // Raise the process-wide locked flag. Every WindowFrame listens for
    // terminal_locked_changed and applies the lock UI itself in
    // apply_lock_state() — including this one, idempotently. Stop the
    // inactivity guard so the activity stream on the lock screen does
    // not generate no-op ticks until the PIN is accepted.
    auth::InactivityGuard::instance().set_enabled(false);
    auth::InactivityGuard::instance().set_terminal_locked(true);
}

void WindowFrame::apply_lock_state(bool locked) {
    if (locked) {
        // Idempotent — if already on the lock screen, nothing to do.
        if (stack_->currentIndex() == 1)
            return;
        LOG_INFO("WindowFrame", QString("Locking window %1").arg(window_id_));
        lock_screen_->activate();
        locked_ = true;
        pin_gate_cleared_ = false;
        set_shell_visible(false);
        stack_->setCurrentIndex(1);
        if (chat_bubble_)
            chat_bubble_->setVisible(false);

        // Disable widgets behind the lock screen so keyboard shortcuts,
        // focus traversal, and dock-manager hit-testing cannot mutate state.
        if (dock_manager_ && dock_manager_->parentWidget())
            dock_manager_->parentWidget()->setEnabled(false);
        if (dock_toolbar_)
            dock_toolbar_->setEnabled(false);
        if (dock_status_bar_)
            dock_status_bar_->setEnabled(false);
        return;
    }

    // Unlock — only the window the user typed the PIN into emits the
    // unlocked signal that lands in on_terminal_unlocked(). Secondary
    // windows take this path via terminal_locked_changed(false). For
    // them we just restore the dashboard chrome; PIN gate state is
    // per-window and stays cleared after the originator set it.
    if (stack_->currentIndex() != 1)
        return; // already unlocked
    LOG_INFO("WindowFrame", QString("Unlocking window %1 (sibling)").arg(window_id_));
    locked_ = false;
    pin_gate_cleared_ = true;
    if (dock_manager_ && dock_manager_->parentWidget())
        dock_manager_->parentWidget()->setEnabled(true);
    if (dock_toolbar_)
        dock_toolbar_->setEnabled(true);
    if (dock_status_bar_)
        dock_status_bar_->setEnabled(true);
    set_shell_visible(true);
    stack_->setCurrentIndex(0);
}

void WindowFrame::on_terminal_unlocked() {
    LOG_INFO("WindowFrame", QString("Terminal unlocked via PIN (window %1)").arg(window_id_));
    locked_ = false;
    pin_gate_cleared_ = true;

    // Re-enable this window's widgets. Sibling MainWindows are handled by
    // apply_lock_state(false) once we flip the flag at the bottom of this
    // function — keep the flag-flip last so siblings don't race ahead and
    // restore their own UI before this originator is fully ready.
    if (dock_manager_ && dock_manager_->parentWidget())
        dock_manager_->parentWidget()->setEnabled(true);
    if (dock_toolbar_)
        dock_toolbar_->setEnabled(true);
    if (dock_status_bar_)
        dock_status_bar_->setEnabled(true);

    // Enable/restart inactivity guard. It is disabled in show_lock_screen()
    // and on lock, so it may currently be off even though the filter is
    // already installed on qApp from a previous unlock. Re-read the saved
    // timeout from settings so a value changed in Settings → Security takes
    // effect on the very next lock cycle even if the guard had been stopped.
    auto& guard = auth::InactivityGuard::instance();
    {
        auto r = SettingsRepository::instance().get("security.lock_timeout_minutes");
        if (r.is_ok() && !r.value().isEmpty()) {
            int minutes = r.value().toInt();
            if (minutes > 0)
                guard.set_timeout_minutes(minutes);
        }
    }
    // Honour the user's Settings → Security → "Enable auto-lock" choice.
    // Auto-lock is also pointless without a PIN (there would be nothing to
    // unlock with), so both conditions gate it. Default is ON when the key
    // is absent, which preserves the previous behaviour.
    const bool autolock_wanted = [] {
        auto r = SettingsRepository::instance().get("security.autolock_enabled");
        if (r.is_ok() && !r.value().isEmpty())
            return r.value().compare(QLatin1String("false"), Qt::CaseInsensitive) != 0;
        return true; // unset → previous default
    }();

    if (autolock_wanted && auth::PinManager::instance().has_pin()) {
        if (!guard.is_enabled()) {
            qApp->installEventFilter(&guard);
            guard.set_enabled(true);
        }
        guard.reset_timer();
    } else if (guard.is_enabled()) {
        guard.set_enabled(false);
    }

    // Reset PIN lockout on successful unlock
    auth::PinManager::instance().reset_lockout();

    // MarketLab: no plan gate — the local workspace is always the next stop.
    set_shell_visible(true);
    stack_->setCurrentIndex(0);
    // Restore chat bubble based on setting
    if (chat_bubble_) {
        auto r = SettingsRepository::instance().get("appearance.show_chat_bubble");
        bool show = !r.is_ok() || r.value() != "false";
        chat_bubble_->setVisible(show);
        if (show) {
            chat_bubble_->reposition();
            chat_bubble_->raise();
        }
    }
    // Cold-boot restore via the new system (frame layouts, panels, dock
    // state, monitor variants).
    layout::WorkspaceShell::load_last_or_default();

    // Flip the process-wide locked flag LAST. Sibling MainWindows fan out
    // via terminal_locked_changed → apply_lock_state(false) and restore
    // their own dashboard chrome from this single source of truth.
    auth::InactivityGuard::instance().set_terminal_locked(false);
}

} // namespace fincept
