#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/dashboard/widgets/HighlightCharts.h"

#include <QLabel>

namespace fincept::screens::widgets {

/// Market Sentiment — are investors buying risky stocks or retreating into
/// safe ones? Daily risk appetite over every S&P 500 member
/// (services::MarketBreadthFeed, scripts/market_breadth.py: riskiest vs
/// safest fifth, Kumar & Persaud's rank correlation, cyclical vs defensive
/// sectors), and under it the twice-monthly aggregate short interest
/// (services::ShortInterestFeed, FINRA). It measures something different
/// from CNN's index (options, bonds, volatility) and from Market Pulse
/// (what the market did today).
class MarketSentimentWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit MarketSentimentWidget(QWidget* parent = nullptr);

    void request_bulk_refresh() override;

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void retranslateUi() override;

  private:
    void apply_styles();
    void render();
    void render_short_interest();

    QWidget* body_ = nullptr;
    QLabel* score_ = nullptr;
    QLabel* rating_ = nullptr;
    QLabel* basis_ = nullptr;
    charts::ScoreGauge* gauge_ = nullptr;
    QLabel* spread_ = nullptr;
    QLabel* parts_title_ = nullptr;
    charts::BarList* parts_ = nullptr;
    QLabel* history_title_ = nullptr;
    charts::ScoreLine* history_ = nullptr;
    QLabel* si_title_ = nullptr;
    charts::BarList* si_rows_ = nullptr;
    QLabel* si_note_ = nullptr;
};

} // namespace fincept::screens::widgets
