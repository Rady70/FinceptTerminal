#include "screens/dashboard/DashboardScreen.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/dashboard/canvas/AddWidgetDialog.h"
#include "screens/dashboard/canvas/DashboardPages.h"
#include "screens/dashboard/canvas/DashboardTemplates.h"
#include "screens/dashboard/canvas/TemplatePicker.h"
#include "screens/dashboard/canvas/WidgetRegistry.h"
#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/markets/MarketDataService.h"
#include "services/notifications/NotificationService.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/NotifToast.h"

#include <QDataStream>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

namespace {

void apply_solid_background(QWidget* widget, const QColor& color) {
    if (!widget)
        return;

    widget->setAttribute(Qt::WA_StyledBackground, true);
    widget->setAutoFillBackground(true);

    QPalette pal = widget->palette();
    pal.setColor(QPalette::Window, color);
    pal.setColor(QPalette::Base, color);
    pal.setColor(QPalette::Button, color);
    widget->setPalette(pal);
}

} // namespace

namespace fincept::screens {

DashboardScreen::DashboardScreen(QWidget* parent) : QWidget(parent) {
    apply_solid_background(this, QColor(ui::colors::BG_BASE()));
    refresh_theme();

    auto* vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    // ── Top Toolbar ──
    toolbar_ = new DashboardToolBar;
    vl->addWidget(toolbar_);

    // ── Scrolling Ticker Bar ──
    ticker_bar_ = new TickerBar;
    vl->addWidget(ticker_bar_);
    // When user saves a new symbol list, re-fetch quotes immediately.
    connect(ticker_bar_, &TickerBar::symbols_changed, this, &DashboardScreen::refresh_ticker);

    // ── Page bar ──
    build_page_bar();
    vl->addWidget(page_bar_);

    // ── Main Content: pages (each a canvas in a scroll area) + Market Pulse ──
    content_split_ = new QSplitter(Qt::Horizontal);
    content_split_->setHandleWidth(1);
    content_split_->setStyleSheet(QString("QSplitter::handle{background:%1;}QSplitter::handle:hover{background:%2;}")
                                      .arg(ui::colors::BORDER_DIM(), ui::colors::BORDER_MED()));

    // Pages are created by restore_layout() from the saved page list.
    pages_stack_ = new QStackedWidget;
    pages_stack_->setMinimumWidth(0);

    market_pulse_ = new MarketPulsePanel;

    content_split_->addWidget(pages_stack_);
    content_split_->addWidget(market_pulse_);
    content_split_->setStretchFactor(0, 3);
    content_split_->setStretchFactor(1, 1);
    content_split_->setCollapsible(0, false);
    content_split_->setCollapsible(1, false);
    market_pulse_->setMinimumWidth(180);

    vl->addWidget(content_split_, 1);

    // ── Bottom Status Bar ──
    status_bar_ = new DashboardStatusBar;
    vl->addWidget(status_bar_);

    // ── In-app notification toast (overlay, not in layout) ────────────────────
    notif_toast_ = new fincept::ui::NotifToast(this);
    connect(&fincept::notifications::NotificationService::instance(),
            &fincept::notifications::NotificationService::notification_received, notif_toast_,
            &fincept::ui::NotifToast::show_notification);

    // ── Debounced save timer (P3: not started in constructor) ──
    save_timer_ = new QTimer(this);
    save_timer_->setSingleShot(true);
    save_timer_->setInterval(800);
    connect(save_timer_, &QTimer::timeout, this, &DashboardScreen::save_layout);

    // ── Canvas signals → toolbar/statusbar are wired per page (add_page) ──
    // The toolbar's LIVE badge was hardcoded — set_connected() had no caller
    // anywhere in the codebase. Drive it from the status bar's API health probe.
    connect(status_bar_, &DashboardStatusBar::connectivity_changed, toolbar_, &DashboardToolBar::set_connected);

    // ── Toolbar buttons ──
    // Kept as named lambdas so the keyboard shortcuts below drive exactly the
    // same code path as the toolbar buttons.
    auto toggle_pulse = [this]() {
        pulse_visible_ = !pulse_visible_;
        market_pulse_->setVisible(pulse_visible_);
    };
    auto open_add_widget = [this]() {
        if (!canvas_)
            return;
        auto* dlg = new AddWidgetDialog(this);
        connect(dlg, &AddWidgetDialog::widget_selected, canvas_, &DashboardCanvas::add_widget);
        dlg->exec();
        dlg->deleteLater();
    };

    connect(toolbar_, &DashboardToolBar::toggle_pulse_clicked, this, toggle_pulse);

    connect(toolbar_, &DashboardToolBar::add_widget_clicked, this, open_add_widget);

    connect(toolbar_, &DashboardToolBar::reset_layout_clicked, this, [this]() {
        auto* dlg = new TemplatePicker(this);
        connect(dlg, &TemplatePicker::template_selected, this, [this](const QString& tid) {
            if (!canvas_ || current_page_ < 0)
                return;
            // The template replaces the CURRENT page only; its saved state is
            // cleared so the fresh template is used.
            const QString key = layout_key(pages_[current_page_].id);
            (void)QtConcurrent::run([key]() { fincept::SettingsRepository::instance().remove(key); });
            canvas_->apply_template(tid);
        });
        dlg->exec();
        dlg->deleteLater();
    });

    connect(toolbar_, &DashboardToolBar::save_layout_clicked, this, &DashboardScreen::save_layout);

    connect(toolbar_, &DashboardToolBar::refresh_clicked, this, &DashboardScreen::on_refresh_clicked);

    connect(toolbar_, &DashboardToolBar::toggle_lock_clicked, this,
            [this]() { set_layout_locked(!layout_locked_, /*persist=*/true); });

    connect(toolbar_, &DashboardToolBar::toggle_compact_clicked, this, [this]() {
        compact_rows_ = !compact_rows_;
        for (const auto& page : pages_)
            page.canvas->set_row_height(compact_rows_ ? 40 : 60);
    });

    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this,
            [this](const ui::ThemeTokens&) { refresh_theme(); });

    // ── Keyboard shortcuts ────────────────────────────────────────────────────
    // The dashboard had zero keyboard affordances; every action needed a mouse
    // trip to the toolbar. WindowShortcut scope so they fire while any child
    // of the dashboard has focus, but not while another screen is active.
    auto add_shortcut = [this](QKeySequence seq, auto&& handler) {
        auto* sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);
        connect(sc, &QShortcut::activated, this, handler);
    };
    add_shortcut(QKeySequence(Qt::Key_F5), [this]() { on_refresh_clicked(); });
    add_shortcut(QKeySequence(QKeySequence::Save), [this]() { save_layout(); });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_N), [this, open_add_widget]() {
        if (!layout_locked_)
            open_add_widget();
    });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L),
                 [this]() { set_layout_locked(!layout_locked_, /*persist=*/true); });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_P), toggle_pulse);
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_PageDown), [this]() {
        if (pages_.size() > 1)
            switch_to_page((current_page_ + 1) % pages_.size());
    });
    add_shortcut(QKeySequence(Qt::CTRL | Qt::Key_PageUp), [this]() {
        if (pages_.size() > 1)
            switch_to_page((current_page_ + pages_.size() - 1) % pages_.size());
    });
}

// ── Theme ─────────────────────────────────────────────────────────────────────

void DashboardScreen::refresh_theme() {
    const QColor bg(ui::colors::BG_BASE());
    apply_solid_background(this, bg);

    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    if (content_split_) {
        apply_solid_background(content_split_, bg);
        content_split_->setStyleSheet(
            QString("QSplitter{background:%1;}QSplitter::handle{background:%2;}QSplitter::handle:hover{background:%3;}")
                .arg(ui::colors::BG_BASE(), ui::colors::BORDER_DIM(), ui::colors::BORDER_MED()));
    }
    for (const auto& page : pages_) {
        QScrollArea* area = page.scroll;
        apply_solid_background(area, bg);
        area->setStyleSheet(QString("QScrollArea{border:none;background:%1;}"
                                    "QScrollBar:vertical{width:6px;background:transparent;}"
                                    "QScrollBar::handle:vertical{background:%2;border-radius:3px;min-height:20px;}"
                                    "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}")
                                .arg(ui::colors::BG_BASE(), ui::colors::BORDER_MED()));
        // Ensure viewport doesn't paint its own opaque background over the canvas
        apply_solid_background(area->viewport(), bg);
        area->viewport()->setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    }
    if (page_bar_) {
        page_bar_->setStyleSheet(
            QString("#dashPageBar{background:%1;border-bottom:1px solid %2;}"
                    "#dashPageBar QTabBar::tab{background:transparent;color:%3;padding:3px 14px;border:none;"
                    "font-size:10px;font-weight:bold;letter-spacing:0.5px;}"
                    "#dashPageBar QTabBar::tab:selected{color:%4;border-bottom:2px solid %4;}"
                    "#dashPageBar QTabBar::tab:hover{color:%5;}"
                    "#dashPageBar QToolButton{background:transparent;color:%3;border:1px solid %2;border-radius:2px;"
                    "padding:1px 8px;font-size:10px;font-weight:bold;}"
                    "#dashPageBar QToolButton:hover{color:%5;border-color:%4;}")
                .arg(ui::colors::BG_RAISED(), ui::colors::BORDER_DIM(), ui::colors::TEXT_TERTIARY(),
                     ui::colors::AMBER(), ui::colors::TEXT_PRIMARY()));
    }
}

// ── Show/Hide ─────────────────────────────────────────────────────────────────

void DashboardScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh_theme();

    if (ticker_bar_) {
        // Honour the appearance settings that were previously saved but never
        // read: "Show Ticker Bar" toggles visibility; "Enable Animations" toggles
        // the scroll (off → the bar shows its symbols but doesn't animate).
        auto& repo = SettingsRepository::instance();
        const auto tr = repo.get("appearance.show_ticker_bar");
        const bool show_ticker = !tr.is_ok() ? true : tr.value() != "false";
        const auto ar = repo.get("appearance.animations");
        const bool animations = !ar.is_ok() ? true : ar.value() != "false";
        ticker_bar_->setVisible(show_ticker);
        if (show_ticker && animations)
            ticker_bar_->resume();
        else
            ticker_bar_->pause();
    }

    // Subscribe to current ticker symbols — hub schedules refreshes per TopicPolicy.
    hub_resubscribe_ticker();

    // Set splitter sizes on first show using actual pixel width.
    // Must be done here (not in constructor) because the widget has no size yet
    // during construction. MarketPulsePanel's large sizeHint otherwise causes it
    // to claim ~70% of the splitter before the user sees it.
    if (!split_sized_) {
        split_sized_ = true;
        const int total = content_split_->width();
        if (total > 400) {
            const int pulse_w = qBound(180, total / 4, 320);
            content_split_->setSizes({total - pulse_w, pulse_w});
        }
    }

    if (!layout_restored_) {
        layout_restored_ = true;
        restore_layout();
    }
}

void DashboardScreen::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (ticker_bar_)
        ticker_bar_->pause();
    hub_unsubscribe_ticker();
}

// ── Ticker refresh ────────────────────────────────────────────────────────────

void DashboardScreen::refresh_ticker() {
    if (!ticker_bar_)
        return;
    // Hub path: user edited symbols → drop old subs, attach to new set,
    // kick the hub so consumers see data immediately.
    //
    // Note: this used to early-return on an empty symbol list, which left the
    // *previous* symbols subscribed and still scrolling after the user cleared
    // the bar. hub_resubscribe_ticker() handles the empty case correctly
    // (unsubscribe, then no-op).
    hub_resubscribe_ticker();
}

void DashboardScreen::on_refresh_clicked() {
    // Toolbar REFRESH button — force-refresh every live data source on the
    // dashboard. The hub's per-producer rate limit is still honoured, so
    // rage-clicking can't hammer upstream APIs.
    if (ticker_bar_ && !ticker_subscribed_.isEmpty()) {
        QStringList topics;
        topics.reserve(ticker_subscribed_.size());
        for (const QString& sym : ticker_subscribed_)
            topics.append(QStringLiteral("market:quote:") + sym);
        datahub::DataHub::instance().request(topics, /*force=*/true);
    }
    if (market_pulse_)
        market_pulse_->refresh_data();
    // Fan refresh out to every visible BaseWidget tile on the canvas.
    // Hidden tiles (collapsed, off-screen, on a non-visible workspace) are
    // skipped — refreshing them burns the producer's rate limit on data
    // the user can't see, and the visibility-driven subscribe/unsubscribe
    // (P3) means hidden widgets aren't subscribed anyway.
    if (canvas_) {
        const auto widgets = canvas_->findChildren<widgets::BaseWidget*>();
        for (auto* w : widgets) {
            if (w && w->isVisible())
                w->request_bulk_refresh();
        }
    }
}

void DashboardScreen::rebuild_ticker_from_cache() {
    if (!ticker_bar_)
        return;
    QVector<TickerBar::Entry> entries;
    entries.reserve(ticker_subscribed_.size());
    for (const auto& sym : ticker_subscribed_) {
        if (!ticker_cache_.contains(sym))
            continue;
        const auto& q = ticker_cache_.value(sym);
        entries.append({q.symbol, q.price, q.change, q.change_pct, q.has_price, q.has_change, q.has_change_pct});
    }
    if (!entries.isEmpty())
        ticker_bar_->set_data(entries);
}

void DashboardScreen::hub_resubscribe_ticker() {
    if (!ticker_bar_)
        return;

    auto& hub = datahub::DataHub::instance();
    // Drop any prior subscriptions owned by this screen, since the symbol
    // set may have changed (user edited ticker symbols).
    hub.unsubscribe(this);
    hub_active_ = false;
    ticker_cache_.clear();

    ticker_subscribed_ = ticker_bar_->symbols();
    if (ticker_subscribed_.isEmpty()) {
        ticker_bar_->set_data({}); // clear stale entries so nothing misleading scrolls
        return;
    }

    QStringList topics;
    topics.reserve(ticker_subscribed_.size());
    for (const QString& sym : ticker_subscribed_) {
        const QString topic = QStringLiteral("market:quote:") + sym;
        topics.append(topic);
        hub.subscribe(this, topic, [this, sym](const QVariant& v) {
            if (!v.canConvert<services::QuoteData>())
                return;
            ticker_cache_.insert(sym, v.value<services::QuoteData>());
            rebuild_ticker_from_cache();
        });
    }
    // force=true: ticker bar re-subscribe happens on user edits and tab shows —
    // bypass min_interval so the ticker doesn't sit blank. Subscribe's built-in
    // cold-start fetch (task 4) already handles the cold case; force is for
    // the "symbols changed, existing cache is for old symbols" case.
    hub.request(topics, /*force=*/true);
    hub_active_ = true;
}

void DashboardScreen::hub_unsubscribe_ticker() {
    if (!hub_active_)
        return;
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

// ── Event filter: sync canvas width to scroll viewport ────────────────────────

bool DashboardScreen::eventFilter(QObject* obj, QEvent* event) {
    for (const auto& page : pages_) {
        if (obj != page.scroll->viewport() || event->type() != QEvent::Resize)
            continue;
        DashboardCanvas* canvas = page.canvas;
        int vp_w = page.scroll->viewport()->width();
        // Only update if width actually changed — avoids triggering
        // resizeEvent → rebuild_grid_cache → reflow_tiles on every scroll tick.
        // Use setMaximumWidth + resize instead of setFixedWidth so the canvas
        // never imposes a minimum-width constraint that fights ADS splitters
        // when two screens are placed side-by-side.
        if (vp_w > 0 && vp_w != canvas->width()) {
            canvas->setMinimumWidth(0);
            canvas->setMaximumWidth(QWIDGETSIZE_MAX);
            canvas->resize(vp_w, canvas->height());
        }
        break;
    }
    return QWidget::eventFilter(obj, event);
}

// ── Save / Restore layout via SettingsRepository ──────────────────────────────

namespace {
/// Dynamic-property guard: set when restore_layout() could not *read* the
/// stored page list or a page's stored blob (as opposed to reading a genuinely
/// absent/corrupt one). While it is set, save_layout() refuses to write, because
/// what is on screen is not everything the user arranged — persisting it would
/// replace their real dashboard (or drop pages from the stored list).
constexpr const char* kDashLayoutLoadFailedProp = "fincept_dash_layout_load_failed";
} // namespace

QString DashboardScreen::layout_key(const QString& page_id) {
    return pages::layout_key(page_id);
}

void DashboardScreen::save_layout() {
    save_timer_->stop();
    if (pages_.isEmpty())
        return; // not restored yet: nothing on screen may replace the stored pages

    if (property(kDashLayoutLoadFailedProp).toBool()) {
        LOG_WARN("Dashboard", "Skipping layout save — the stored layout could not be read this session, so the "
                              "on-screen default must not overwrite it");
        return;
    }

    // Every page whose stored layout has been read is written back; a page
    // still being restored is skipped so its stored layout is not replaced by
    // an empty canvas.
    QVector<QPair<QString, QString>> writes;
    QVector<pages::PageRef> list;
    for (const auto& page : pages_) {
        list.append({page.id, page.name});
        if (page.restored)
            writes.append({layout_key(page.id), pages::encode_layout(page.canvas->canonical_layout())});
    }
    const QString current =
        current_page_ >= 0 && current_page_ < pages_.size() ? pages_[current_page_].id : QString(pages::kFirstPageId);
    const QString pages_encoded = pages::pages_json(list, current);
    (void)QtConcurrent::run([writes, pages_encoded]() {
        auto& repo = fincept::SettingsRepository::instance();
        for (const auto& w : writes)
            repo.set(w.first, w.second, "dashboard");
        repo.set(pages::kPagesKey, pages_encoded, "dashboard");
    });
}

void DashboardScreen::restore_layout() {
    QPointer<DashboardScreen> self = this;
    (void)QtConcurrent::run([self]() {
        auto result = fincept::SettingsRepository::instance().get(pages::kPagesKey);
        const auto lock = fincept::SettingsRepository::instance().get("dashboard_layout_locked");
        const bool locked = lock.is_ok() && lock.value() == QLatin1String("true");
        QMetaObject::invokeMethod(
            self,
            [self, result, locked]() {
                if (!self)
                    return;
                self->set_layout_locked(locked, /*persist=*/false);
                if (result.is_err()) {
                    // The page list could not be READ (as opposed to never
                    // saved): show the first page, and save nothing this
                    // session — writing the list now would drop every other page.
                    LOG_ERROR("Dashboard",
                              QString("settings read failed for '%1' — showing the first page only, "
                                      "read-only: %2")
                                  .arg(QString::fromLatin1(pages::kPagesKey), QString::fromStdString(result.error())));
                    self->setProperty(kDashLayoutLoadFailedProp, true);
                }
                // Absent (first run, or an older build's dashboard) means a
                // single first page; parse_pages always puts it first.
                QString current_id;
                const QVector<pages::PageRef> list =
                    pages::parse_pages(result.is_ok() ? result.value() : QString(), tr("Main"), &current_id);
                int current = 0;
                for (const auto& p : list) {
                    const int index = self->add_page(p.id, p.name.isEmpty() ? tr("Page") : p.name);
                    if (p.id == current_id)
                        current = index;
                    // Only the first page falls back to the default template;
                    // an added page starts empty.
                    self->restore_page_layout(index, p.id == QLatin1String(pages::kFirstPageId));
                }
                self->switch_to_page(current);
            },
            Qt::QueuedConnection);
    });
}

void DashboardScreen::restore_page_layout(int index, bool default_if_empty) {
    QPointer<DashboardScreen> self = this;
    const QString page_id = pages_[index].id;
    const QString key = layout_key(page_id);
    (void)QtConcurrent::run([self, key, page_id, default_if_empty]() {
        auto result = fincept::SettingsRepository::instance().get(key);
        QMetaObject::invokeMethod(
            self,
            [self, result, page_id, default_if_empty]() {
                if (!self)
                    return;
                const auto it = std::find_if(self->pages_.begin(), self->pages_.end(),
                                             [&page_id](const Page& p) { return p.id == page_id; });
                if (it == self->pages_.end())
                    return; // deleted meanwhile
                Page& page = *it;

                if (result.is_err()) {
                    LOG_ERROR("Dashboard",
                              QString("settings read failed for '%1' — rendering the default layout read-only, "
                                      "leaving the stored layout untouched: %2")
                                  .arg(layout_key(page_id), QString::fromStdString(result.error())));
                    // Degrade read-only: the user still gets a usable dashboard,
                    // but the layout_changed → save_timer_ → set() chain that
                    // apply_template() is about to trigger is disarmed.
                    self->setProperty(kDashLayoutLoadFailedProp, true);
                    if (default_if_empty)
                        self->build_default_layout(page.canvas);

                    // Say so. Suppressing the write stops the saved layout being
                    // destroyed, but on its own it just swaps one silent failure
                    // for another: the user sees a default dashboard, rearranges
                    // it, and none of it persists — with only a LOG_WARN to
                    // explain why. Reuse the toast this screen already owns.
                    if (self->notif_toast_) {
                        fincept::notifications::NotificationRecord rec;
                        rec.request.title = tr("Dashboard layout unavailable");
                        rec.request.message = tr("Your saved layout could not be read, so a default is shown. "
                                                 "Changes will not be saved this session — restart to retry.");
                        rec.request.level = fincept::notifications::NotifLevel::Warning;
                        self->notif_toast_->show_notification(rec);
                    }
                    return;
                }

                // The read succeeded, so anything below this point is a genuine
                // "unset or corrupt" case where writing the default back is the
                // correct behaviour.
                page.restored = true;
                GridLayout layout;
                if (pages::decode_layout(result.value(), layout)) {
                    if (self->compact_rows_)
                        layout.row_h = 40;
                    page.canvas->load_layout(layout);
                } else if (default_if_empty) {
                    self->build_default_layout(page.canvas);
                }
            },
            Qt::QueuedConnection);
    });
}

void DashboardScreen::build_default_layout(DashboardCanvas* canvas) {
    if (canvas)
        canvas->apply_template("portfolio_manager");
}

// ── Pages ─────────────────────────────────────────────────────────────────────

void DashboardScreen::build_page_bar() {
    page_bar_ = new QWidget(this);
    page_bar_->setObjectName("dashPageBar");
    page_bar_->setAttribute(Qt::WA_StyledBackground, true);
    page_bar_->setFixedHeight(26);
    auto* hl = new QHBoxLayout(page_bar_);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(6);

    page_tabs_ = new QTabBar(page_bar_);
    page_tabs_->setObjectName("dashPageTabs");
    page_tabs_->setMovable(true);
    page_tabs_->setExpanding(false);
    page_tabs_->setDrawBase(false);
    page_tabs_->setContextMenuPolicy(Qt::CustomContextMenu);
    page_tabs_->setAccessibleName(tr("Dashboard pages"));
    page_tabs_->setToolTip(tr("Dashboard pages — right-click a page to rename or delete it; drag to reorder"));
    hl->addWidget(page_tabs_);

    add_page_btn_ = new QToolButton(page_bar_);
    auto* add_btn = add_page_btn_;
    add_btn->setText(tr("+ PAGE"));
    add_btn->setToolTip(tr("Add a dashboard page"));
    add_btn->setAccessibleName(tr("Add a dashboard page"));
    add_btn->setCursor(Qt::PointingHandCursor);
    hl->addWidget(add_btn);
    hl->addStretch(1);

    auto* hint = new QLabel(tr("Right-click a widget title to move it to another page"), page_bar_);
    hint->setStyleSheet(QString("color:%1;font-size:9px;background:transparent;").arg(ui::colors::TEXT_TERTIARY()));
    hl->addWidget(hint);

    connect(add_btn, &QToolButton::clicked, this, &DashboardScreen::new_page);
    connect(page_tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && index != current_page_)
            switch_to_page(index);
    });
    connect(page_tabs_, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || to < 0 || from >= pages_.size() || to >= pages_.size())
            return;
        pages_.move(from, to);
        current_page_ = page_tabs_->currentIndex();
        save_timer_->start();
    });
    connect(page_tabs_, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        if (index >= 0 && !layout_locked_)
            rename_page(index);
    });
    connect(page_tabs_, &QTabBar::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int index = page_tabs_->tabAt(pos);
        if (index >= 0)
            show_page_menu(index, page_tabs_->mapToGlobal(pos));
    });
}

int DashboardScreen::add_page(const QString& id, const QString& name) {
    Page page;
    page.id = id;
    page.name = name;
    page.canvas = new DashboardCanvas;
    if (compact_rows_)
        page.canvas->set_row_height(40);

    // Scroll area wraps canvas — provides vertical scroll when content exceeds viewport
    page.scroll = new QScrollArea;
    page.scroll->setWidgetResizable(false); // canvas controls its own size
    page.scroll->setMinimumWidth(0);        // allow ADS to shrink this panel freely
    page.scroll->setWidget(page.canvas);
    // Sync canvas width to scroll viewport via event filter
    page.scroll->viewport()->installEventFilter(this);
    pages_stack_->addWidget(page.scroll);

    DashboardCanvas* canvas = page.canvas;
    connect(canvas, &DashboardCanvas::widget_count_changed, this, [this, canvas](int count) {
        if (canvas == canvas_) {
            toolbar_->set_widget_count(count);
            status_bar_->set_widget_count(count);
        }
    });
    connect(canvas, &DashboardCanvas::layout_changed, this, [this](const GridLayout&) { save_timer_->start(); });
    connect(canvas, &DashboardCanvas::tile_menu_requested, this,
            [this, canvas](const QString& instance_id, const QPoint& global_pos) {
                show_tile_menu(canvas, instance_id, global_pos);
            });

    page.canvas->set_locked(layout_locked_);
    pages_.append(page);
    {
        const QSignalBlocker block(page_tabs_);
        page_tabs_->addTab(QString(name.toUpper()).replace(QLatin1Char('&'), QStringLiteral("&&")));
    }
    refresh_theme();
    return static_cast<int>(pages_.size()) - 1;
}

void DashboardScreen::sync_page_tabs() {
    const QSignalBlocker block(page_tabs_);
    for (int i = 0; i < pages_.size() && i < page_tabs_->count(); ++i)
        page_tabs_->setTabText(i, QString(pages_[i].name.toUpper()).replace(QLatin1Char('&'), QStringLiteral("&&")));
    if (current_page_ >= 0)
        page_tabs_->setCurrentIndex(current_page_);
}

void DashboardScreen::switch_to_page(int index) {
    if (index < 0 || index >= pages_.size())
        return;
    current_page_ = index;
    scroll_area_ = pages_[index].scroll;
    canvas_ = pages_[index].canvas;
    // Hidden pages hide their widgets, which unsubscribes them from the data
    // hub (BaseWidget show/hide), so only the visible page fetches data.
    pages_stack_->setCurrentWidget(scroll_area_);
    {
        const QSignalBlocker block(page_tabs_);
        page_tabs_->setCurrentIndex(index);
    }
    // The stack does not resize a page that was hidden; size its canvas now.
    const int vp_w = scroll_area_->viewport()->width();
    if (vp_w > 0 && vp_w != canvas_->width())
        canvas_->resize(vp_w, canvas_->height());
    toolbar_->set_widget_count(canvas_->tile_count());
    status_bar_->set_widget_count(canvas_->tile_count());
    save_timer_->start();
}

void DashboardScreen::new_page() {
    if (layout_locked_)
        return;
    bool ok = false;
    const QString suggested = tr("Page %1").arg(pages_.size() + 1);
    const QString name =
        QInputDialog::getText(this, tr("New dashboard page"), tr("Page name:"), QLineEdit::Normal, suggested, &ok)
            .trimmed();
    if (!ok)
        return;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    const int index = add_page(id, name.isEmpty() ? suggested : name);
    pages_[index].restored = true; // new and empty: nothing stored to protect
    switch_to_page(index);
    save_layout();
}

void DashboardScreen::rename_page(int index) {
    if (layout_locked_)
        return;
    if (index < 0 || index >= pages_.size())
        return;
    bool ok = false;
    const QString name =
        QInputDialog::getText(this, tr("Rename page"), tr("Page name:"), QLineEdit::Normal, pages_[index].name, &ok)
            .trimmed();
    if (!ok || name.isEmpty())
        return;
    pages_[index].name = name;
    sync_page_tabs();
    save_layout();
}

void DashboardScreen::delete_page(int index) {
    if (layout_locked_)
        return;
    if (index < 0 || index >= pages_.size() || pages_.size() <= 1)
        return;
    const Page page = pages_[index];
    if (property(kDashLayoutLoadFailedProp).toBool()) {
        QMessageBox::information(this, tr("Delete page"),
                                 tr("The saved dashboard could not be read this session, so pages cannot be changed. "
                                    "Restart to retry."));
        return;
    }
    if (page.id == QLatin1String(pages::kFirstPageId)) {
        QMessageBox::information(this, tr("Delete page"), tr("The first page cannot be deleted."));
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Delete page"),
        tr("Delete the page \"%1\" and its %2 widget(s)?").arg(page.name).arg(page.canvas->tile_count()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    const int shown = current_page_;
    pages_.removeAt(index);
    {
        const QSignalBlocker block(page_tabs_);
        page_tabs_->removeTab(index);
    }
    pages_stack_->removeWidget(page.scroll);
    page.scroll->deleteLater();
    const QString key = layout_key(page.id);
    (void)QtConcurrent::run([key]() { fincept::SettingsRepository::instance().remove(key); });
    // The page being viewed stays in view unless it is the one deleted.
    switch_to_page(pages::current_after_delete(index, shown, static_cast<int>(pages_.size())));
    save_layout();
}

void DashboardScreen::show_page_menu(int index, const QPoint& global_pos) {
    QMenu menu(this);
    auto* ren = menu.addAction(tr("Rename…"), this, [this, index]() { rename_page(index); });
    auto* del = menu.addAction(tr("Delete page…"), this, [this, index]() { delete_page(index); });
    ren->setEnabled(!layout_locked_);
    del->setEnabled(!layout_locked_ && pages_.size() > 1 && pages_[index].id != QLatin1String(pages::kFirstPageId));
    if (layout_locked_) {
        menu.addSeparator();
        menu.addAction(tr("Unlock layout"), this, [this]() { set_layout_locked(false, /*persist=*/true); });
    }
    menu.exec(global_pos);
}

int DashboardScreen::page_index(const DashboardCanvas* canvas) const {
    for (int i = 0; i < pages_.size(); ++i)
        if (pages_[i].canvas == canvas)
            return i;
    return -1;
}

void DashboardScreen::show_tile_menu(DashboardCanvas* canvas, const QString& instance_id, const QPoint& global_pos) {
    QMenu menu(this);
    if (layout_locked_) {
        auto* note = menu.addAction(tr("The layout is locked"));
        note->setEnabled(false);
        menu.addAction(tr("Unlock layout"), this, [this]() { set_layout_locked(false, /*persist=*/true); });
        menu.exec(global_pos);
        return;
    }
    auto* move = menu.addMenu(tr("Move to page"));
    const int from = page_index(canvas);
    for (int i = 0; i < pages_.size(); ++i) {
        if (i == from)
            continue;
        move->addAction(pages_[i].name, this,
                        [this, canvas, instance_id, i]() { move_widget_to_page(canvas, instance_id, i); });
    }
    move->addSeparator();
    move->addAction(tr("New page…"), this, [this, canvas, instance_id]() {
        const int before = static_cast<int>(pages_.size());
        new_page();
        if (pages_.size() > before)
            move_widget_to_page(canvas, instance_id, static_cast<int>(pages_.size()) - 1);
    });
    menu.exec(global_pos);
}

void DashboardScreen::set_layout_locked(bool locked, bool persist) {
    layout_locked_ = locked;
    for (const auto& page : pages_)
        page.canvas->set_locked(locked);
    if (page_tabs_)
        page_tabs_->setMovable(!locked);
    if (add_page_btn_)
        add_page_btn_->setVisible(!locked);
    toolbar_->set_locked(locked);
    if (persist) {
        const QString value = locked ? QStringLiteral("true") : QStringLiteral("false");
        (void)QtConcurrent::run(
            [value]() { fincept::SettingsRepository::instance().set("dashboard_layout_locked", value, "dashboard"); });
    }
}

void DashboardScreen::move_widget_to_page(DashboardCanvas* from, const QString& instance_id, int target) {
    if (layout_locked_ || !from || target < 0 || target >= pages_.size() || pages_[target].canvas == from)
        return;
    if (!pages_[target].restored) {
        // Its stored layout is still being read: the read would replace the
        // canvas and the moved tile with it.
        QMessageBox::information(
            this, tr("Move to page"),
            tr("The page \"%1\" is still loading. Try again in a moment.").arg(pages_[target].name));
        return;
    }
    QString type_id;
    QJsonObject config;
    GridCell cell;
    if (!from->item_info(instance_id, &type_id, &config, &cell))
        return;
    // The widget is re-created on the target page with the same settings and size.
    pages_[target].canvas->add_widget_with_config(type_id, config, cell.w, cell.h);
    from->remove_widget(instance_id);
    switch_to_page(target);
    save_layout();
}

} // namespace fincept::screens
