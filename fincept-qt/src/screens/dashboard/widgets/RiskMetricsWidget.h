#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/dashboard/widgets/HighlightCharts.h"
#include "screens/dashboard/widgets/QuoteSubscriptions.h"
#include "services/markets/MarketDataService.h"

#include <QHash>
#include <QLabel>

namespace fincept::screens::widgets {

/// Risk Metrics Widget — the VIX on its regime gauge, today's move and day
/// range of a fixed list of high-beta stocks, and today's relative moves
/// (SPY minus QQQ / IWM / TLT, percentage points). All data from yfinance.
///
/// Subscribes to `market:quote:<sym>` on the DataHub for the fixed
/// risk-symbol set; on every delivery `populate()` is re-invoked with the
/// aggregated cache.
class RiskMetricsWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit RiskMetricsWidget(QWidget* parent = nullptr);

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void retranslateUi() override;

  private:
    void apply_styles();
    void refresh_data();
    void populate(const QVector<services::QuoteData>& quotes);

    void hub_subscribe_all();
    void hub_unsubscribe_all();
    /// Rebuild a QVector<QuoteData> from `feed_` and hand to `populate()`.
    void rebuild_from_cache();

    // VIX section
    QWidget* vix_card_ = nullptr;
    QLabel* vix_header_lbl_ = nullptr;
    QLabel* vix_value_ = nullptr;
    QLabel* vix_regime_ = nullptr;
    charts::VixGauge* vix_gauge_ = nullptr; ///< 0–50 scale, bands at the regime bounds

    // Separators and section headers
    QFrame* sep1_ = nullptr;
    QFrame* sep2_ = nullptr;
    QLabel* stocks_hdr_ = nullptr;
    QLabel* chg_hdr_ = nullptr;
    QLabel* range_hdr_ = nullptr;
    QLabel* corr_hdr_ = nullptr;

    // Volatility cluster labels [6 stocks]
    struct StockRisk {
        QLabel* symbol = nullptr;
        QLabel* chg_pct = nullptr;
        QLabel* hi_lo = nullptr;
    };
    QVector<StockRisk> stock_rows_;

    // Spread row labels (the left-hand descriptors)
    QVector<QLabel*> spread_labels_;
    // Correlation proxies
    QLabel* spy_qqq_spread_ = nullptr;
    QLabel* spy_iwm_spread_ = nullptr;
    QLabel* equity_bond_lbl_ = nullptr; // SPY vs TLT proxy

    QuoteSubscriptions feed_; ///< per-symbol quote + last failure
    bool hub_active_ = false;
};

} // namespace fincept::screens::widgets
