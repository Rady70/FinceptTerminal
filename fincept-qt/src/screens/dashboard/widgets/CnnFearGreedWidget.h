#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/dashboard/widgets/HighlightCharts.h"

#include <QComboBox>
#include <QJsonObject>
#include <QLabel>

namespace fincept::screens::widgets {

/// CNN Fear & Greed Index — CNN's own value, earlier readings, its seven
/// indicators (score bars, and each indicator's own chart as on CNN's page)
/// and the past year, read from CNN's page feed by scripts/cnn_fear_greed.py.
/// Nothing is recomputed here.
class CnnFearGreedWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit CnnFearGreedWidget(QWidget* parent = nullptr);

    void request_bulk_refresh() override;

  protected:
    void showEvent(QShowEvent* e) override;
    void on_theme_changed() override;
    void retranslateUi() override;

  private:
    void on_feed_changed();
    void render();
    void render_chart();
    void apply_styles();

    QWidget* body_ = nullptr;
    QLabel* score_ = nullptr;
    QLabel* rating_ = nullptr;
    QLabel* updated_ = nullptr;
    charts::ScoreGauge* gauge_ = nullptr;
    QLabel* previous_ = nullptr;
    QLabel* parts_title_ = nullptr;
    charts::BarList* parts_ = nullptr;
    QLabel* history_title_ = nullptr;
    QComboBox* chart_pick_ = nullptr; ///< the index, or one of the seven indicators
    charts::ScoreLine* history_ = nullptr;
    charts::SeriesLine* series_ = nullptr;
    QLabel* series_note_ = nullptr;

    QJsonObject data_; ///< the shared feed's last good read
    QString error_;    ///< the shared feed's last failure
};

/// One of CNN's seven indicators on its own: its score and rating, what it
/// measures, and its chart (CNN's own series, with the moving average CNN
/// draws for momentum and volatility). Same shared read as the index widget.
class CnnIndicatorWidget : public BaseWidget {
    Q_OBJECT
  public:
    CnnIndicatorWidget(const QString& key, const QString& label, QWidget* parent = nullptr);

    void request_bulk_refresh() override;

  protected:
    void showEvent(QShowEvent* e) override;
    void on_theme_changed() override;

  private:
    void render();
    void apply_styles();

    QString key_;
    QString label_;
    QWidget* body_ = nullptr;
    QLabel* score_ = nullptr;
    QLabel* rating_ = nullptr;
    QLabel* detail_ = nullptr;
    QLabel* now_ = nullptr;
    charts::SeriesLine* chart_ = nullptr;
};

} // namespace fincept::screens::widgets
