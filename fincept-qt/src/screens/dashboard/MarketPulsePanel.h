#pragma once
#include "services/markets/MarketDataService.h"

#include <QHash>
#include <QLabel>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

class QFrame;

namespace fincept::screens::widgets {
class LoadingOverlay;
}

namespace fincept::screens {

/// Right-side market pulse panel — what the market did today: the whole
/// S&P 500's up / down count, basket breadth, top movers, global snapshot,
/// market hours. (Sentiment lives in the Market Sentiment and CNN widgets.)
///
/// Subscribes to `market:quote:<sym>` on the DataHub for the union of
/// breadth + mover + snapshot symbol sets. Each delivery updates the
/// matching row cache and triggers the relevant `rebuild_*_from_cache()`
/// to re-render that section. The hub scheduler owns data-refresh cadence;
/// `hours_timer_` is retained only because it drives wall-clock status
/// labels.
class MarketPulsePanel : public QWidget {
    Q_OBJECT
  public:
    explicit MarketPulsePanel(QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void changeEvent(QEvent* event) override;

  public slots:
    /// Public so the Dashboard REFRESH button can drive it directly. The
    /// internal timer also wires to this slot.
    void refresh_data();

  private slots:
    void refresh_market_hours();

  private:
    // ── Build helpers ──
    QWidget* build_header();
    QWidget* build_section_header(const QString& title, const QString& icon_char, const QString& color);
    QWidget* build_sp500_today_section();
    QWidget* build_breadth_section();
    QWidget* build_gainers_section();
    QWidget* build_losers_section();
    QWidget* build_global_snapshot_section();
    QWidget* build_market_hours_section();

    /// Re-apply all token-based styles so a theme switch updates every child widget.
    void refresh_theme();

    static QString market_status(const QString& region);

    // ── Header ──
    QWidget* header_bar_ = nullptr;
    QLabel* header_icon_ = nullptr;
    QLabel* header_title_ = nullptr;
    QLabel* header_live_dot_ = nullptr;

    // ── Scroll area ──
    QScrollArea* scroll_area_ = nullptr;

    // ── Section headers (5 total) ──
    struct SectionHeader {
        QWidget* container = nullptr;
        QLabel* icon = nullptr;
        QLabel* title = nullptr;
        QString source_key; // English source string for retranslateUi()
    };

    /// Re-apply tr() to every kept-handle label/header.
    void retranslateUi();
    SectionHeader sh_breadth_;
    SectionHeader sh_gainers_;
    SectionHeader sh_losers_;
    SectionHeader sh_snapshot_;
    SectionHeader sh_hours_;

    // ── S&P 500 today (services::MarketBreadthFeed) ──
    QLabel* sp_header_ = nullptr;
    QLabel* sp_up_ = nullptr;
    QLabel* sp_down_ = nullptr;
    QLabel* sp_move_ = nullptr;
    QWidget* sp_up_bar_ = nullptr;
    QWidget* sp_down_bar_ = nullptr;

    // ── Market Breadth ──
    // per-exchange: {name_label, adv_label, slash_label, dec_label, green_bar, red_bar}
    struct BreadthRow {
        QLabel* name = nullptr;
        QLabel* adv = nullptr;
        QLabel* slash = nullptr;
        QLabel* dec = nullptr;
        QWidget* green = nullptr;
        QWidget* red = nullptr;
    };
    BreadthRow nyse_row_;
    BreadthRow nasdaq_row_;
    BreadthRow sp500_row_;

    // ── Top Movers ──
    // Fixed pool of 3 rows per section. Rows are built once and only have
    // their text updated; the old code destroyed and re-created 6 rows (24
    // widgets, 24 inline setStyleSheet calls) on every single quote delivery
    // — ~70 times per refresh cycle.
    struct MoverRow {
        QWidget* container = nullptr;
        QLabel* symbol = nullptr;
        QLabel* arrow = nullptr;
        QLabel* change = nullptr;
        QLabel* volume = nullptr;
    };
    QWidget* gainers_rows_ = nullptr;
    QWidget* losers_rows_ = nullptr;
    QVBoxLayout* gainers_layout_ = nullptr;
    QVBoxLayout* losers_layout_ = nullptr;
    QVector<MoverRow> gainer_rows_;
    QVector<MoverRow> loser_rows_;
    MoverRow make_mover_row(QWidget* parent, QVBoxLayout* into, bool positive_section);
    static void fill_mover_row(const MoverRow& row, const QString& symbol, double change, const QString& volume);
    static void clear_mover_row(const MoverRow& row);
    /// Applies the single hoisted stylesheet for a movers section.
    void style_mover_section(QWidget* container, bool positive_section);
    static constexpr int kMoverRows = 3;

    // ── Global Snapshot ──
    struct StatRow {
        QWidget* container = nullptr;
        QLabel* name_lbl = nullptr;
        QLabel* val = nullptr;
        QLabel* chg = nullptr;
    };
    StatRow vix_row_;
    StatRow us10y_row_;
    StatRow dxy_row_;
    StatRow gold_row_;
    StatRow oil_row_;
    StatRow btc_row_;

    // ── Market Hours ──
    struct HoursRow {
        QWidget* container = nullptr;
        QLabel* name_lbl = nullptr;
        QLabel* dot = nullptr;
        QLabel* status = nullptr;
        QString region;
        QString name_source_key; // English label, fed through tr() on language change
    };
    QVector<HoursRow> hours_rows_;

    void hub_subscribe_all();
    void hub_unsubscribe_all();
    void rebuild_breadth_from_cache();
    /// Today's up / down count of every S&P 500 member (services::MarketBreadthFeed).
    void apply_sp500_today();
    void rebuild_movers_from_cache();
    void rebuild_snapshot_from_cache();

    QHash<QString, services::QuoteData> breadth_cache_;
    QHash<QString, services::QuoteData> movers_cache_;
    QHash<QString, services::QuoteData> snapshot_cache_;
    bool hub_active_ = false;
    QTimer* hours_timer_ = nullptr;

    // ── Render coalescing ──
    // ~70 symbols each arrive as their own hub delivery. Without this every
    // delivery triggered a full re-render of whichever sections it touched.
    QTimer* render_timer_ = nullptr;
    bool breadth_dirty_ = false;
    bool movers_dirty_ = false;
    bool snapshot_dirty_ = false;
    void schedule_render();
    void flush_render();

    // Loading overlay shown over the scroll area while the union of the
    // breadth/movers/snapshot caches fills up. The denominator is the
    // unique-symbol union size (~70), updated every time a cache changes.
    int total_expected_symbols_ = 0;
    widgets::LoadingOverlay* loading_overlay_ = nullptr;
    void update_loading_progress();
};

} // namespace fincept::screens
