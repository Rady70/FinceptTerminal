#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/dashboard/widgets/HighlightCharts.h"

#include <QLabel>

namespace fincept::screens::widgets {

/// Market Sentiment — market-wide breadth over every S&P 500 member
/// (services::MarketBreadthFeed, scripts/market_breadth.py): five readings,
/// each ranked against its own past year, averaged into a 0–100 score. The
/// Market Pulse panel shows the same reading.
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

    QWidget* body_ = nullptr;
    QLabel* score_ = nullptr;
    QLabel* rating_ = nullptr;
    QLabel* basis_ = nullptr;
    charts::ScoreGauge* gauge_ = nullptr;
    QLabel* today_ = nullptr;
    QLabel* parts_title_ = nullptr;
    charts::BarList* parts_ = nullptr;
    QLabel* history_title_ = nullptr;
    charts::ScoreLine* history_ = nullptr;
};

} // namespace fincept::screens::widgets
