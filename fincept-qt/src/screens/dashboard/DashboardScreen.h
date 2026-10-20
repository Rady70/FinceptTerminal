#pragma once
#include "screens/dashboard/DashboardStatusBar.h"
#include "screens/dashboard/DashboardToolBar.h"
#include "screens/dashboard/MarketPulsePanel.h"
#include "screens/dashboard/TickerBar.h"
#include "screens/dashboard/canvas/DashboardCanvas.h"
#include "services/markets/MarketDataService.h"

namespace fincept::ui {
class NotifToast;
} // namespace fincept::ui

#include <QHash>
#include <QHideEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

namespace fincept::screens {

/// Main dashboard screen — toolbar, ticker, draggable widget grid, market pulse, status bar.
///
/// The ticker driver subscribes to `market:quote:<sym>` on the DataHub for
/// every user-configured ticker symbol. A cached per-symbol QuoteData is
/// pushed to the TickerBar on each delivery via `rebuild_ticker_from_cache()`.
/// `symbols_changed` wipes old subscriptions and re-subscribes to the new
/// set. Cadence is owned by the hub scheduler, not a local refresh timer.
class DashboardScreen : public QWidget {
    Q_OBJECT
  public:
    explicit DashboardScreen(QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

  private:
    void refresh_theme();
    void build_default_layout(DashboardCanvas* canvas);
    void save_layout();
    void restore_layout();

    // ── Pages ────────────────────────────────────────────────────────────────
    // The dashboard holds several pages, each its own canvas with its own saved
    // layout. The first page keeps the original settings key, so an existing
    // layout opens unchanged as page 1.
    struct Page {
        QString id;
        QString name;
        QScrollArea* scroll = nullptr;
        DashboardCanvas* canvas = nullptr;
        bool restored = false; ///< its stored layout has been read (safe to save)
    };
    void build_page_bar();
    int add_page(const QString& id, const QString& name);
    void switch_to_page(int index);
    void new_page();
    void rename_page(int index);
    void delete_page(int index);
    void show_page_menu(int index, const QPoint& global_pos);
    void show_tile_menu(DashboardCanvas* canvas, const QString& instance_id, const QPoint& global_pos);
    void move_widget_to_page(DashboardCanvas* from, const QString& instance_id, int target);
    void restore_page_layout(int index, bool default_if_empty);
    void sync_page_tabs();
    int page_index(const DashboardCanvas* canvas) const;
    static QString layout_key(const QString& page_id);

    /// Lock or unlock the layout (every page): tiles cannot be moved, resized,
    /// closed or added, and pages cannot be added, renamed, deleted or
    /// reordered. Persisted under `dashboard_layout_locked`.
    void set_layout_locked(bool locked, bool persist);
    void refresh_ticker();
    void on_refresh_clicked();

    void hub_resubscribe_ticker();
    void hub_unsubscribe_ticker();
    void rebuild_ticker_from_cache();

    DashboardToolBar* toolbar_ = nullptr;
    TickerBar* ticker_bar_ = nullptr;
    QScrollArea* scroll_area_ = nullptr; ///< the current page's scroll area
    DashboardCanvas* canvas_ = nullptr;  ///< the current page's canvas
    QWidget* page_bar_ = nullptr;
    QTabBar* page_tabs_ = nullptr;
    QStackedWidget* pages_stack_ = nullptr;
    QVector<Page> pages_;
    int current_page_ = -1;
    QToolButton* add_page_btn_ = nullptr;
    bool layout_locked_ = false;
    QSplitter* content_split_ = nullptr;
    MarketPulsePanel* market_pulse_ = nullptr;
    DashboardStatusBar* status_bar_ = nullptr;
    fincept::ui::NotifToast* notif_toast_ = nullptr;
    QTimer* save_timer_ = nullptr;
    bool pulse_visible_ = true;
    bool layout_restored_ = false;
    bool split_sized_ = false;
    /// COMPACT toggle state. Was a function-local `static bool` in the lambda,
    /// i.e. shared by every DashboardScreen instance and out of sync with the
    /// canvas after a screen rebuild.
    bool compact_rows_ = false;

    QHash<QString, services::QuoteData> ticker_cache_;
    QStringList ticker_subscribed_;
    bool hub_active_ = false;
};

} // namespace fincept::screens
