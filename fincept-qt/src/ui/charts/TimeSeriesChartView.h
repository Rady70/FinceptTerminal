// src/ui/charts/TimeSeriesChartView.h
//
// Reusable chart-first presentation for an ordinary historical numeric series.
// Knows nothing about any provider: callers hand it a TimeSeries (already
// normalized) and get a themed line chart with hover inspection and practical
// history ranges. Raw tables/CSV remain owned by the economics result layer.
#pragma once

#include "ui/charts/TimeAxisNavigator.h"
#include "ui/charts/TimeSeriesData.h"

#include <QColor>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;

namespace fincept::ui {

class TimeSeriesCrosshairView;

class TimeSeriesChartView : public QWidget {
    Q_OBJECT
  public:
    explicit TimeSeriesChartView(QWidget* parent = nullptr);

    /// Replace the displayed series. Empty series show an explicit empty state.
    void set_series(const TimeSeries& series);
    void clear_series();

    /// Provider accent used for the line/markers and range-tab selection.
    void set_accent_color(const QColor& color);

    /// Re-read the active theme tokens (connected by the owning panel).
    void refresh_theme();

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void build_ui();
    void rebuild_chart(bool reset_zoom);
    void apply_range(TimeRange range);
    void update_range_buttons();
    void retranslateUi();

    QLabel* range_lbl_ = nullptr;
    QVector<QPushButton*> range_btns_;
    QVector<TimeRange> range_values_;
    TimeSeriesCrosshairView* chart_view_ = nullptr;
    QLabel* empty_lbl_ = nullptr;

    TimeSeries series_;
    TimeRange range_ = TimeRange::Max;
    QColor accent_;
    TimeAxisNavigator navigator_;
};

} // namespace fincept::ui
