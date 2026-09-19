// src/screens/economics/panels/CftcPositioningChart.h
//
// The R3 workspace's principal positioning chart: several participant series
// (net / long / short, chosen by the panel) on one time axis, plus optional
// open interest on a secondary axis. Gaps come from the shared gap-splitting
// rules, so a missing report breaks the line instead of being interpolated.
//
// The widget is presentation only: the panel owns the range window, which
// series are visible and what each series measures, and hands the chart final
// point sets. Hover inspection reports the exact observation at the snapped
// report date for every series that carries one there.
#pragma once

#include "ui/charts/TimeSeriesData.h"

#include <QColor>
#include <QVector>
#include <QWidget>

class QLabel;

namespace fincept::screens {

struct CftcChartSeries {
    QString key;   // participant key
    QString label; // translated display label
    QColor color;
    QVector<ui::TimeSeriesPoint> points; // ascending by date; real observations only
};

class CftcPositioningChart : public QWidget {
    Q_OBJECT
  public:
    explicit CftcPositioningChart(QWidget* parent = nullptr);

    /// Replace the chart. `open_interest` is drawn only while
    /// set_open_interest_visible(true) is in effect.
    void set_series(const QVector<CftcChartSeries>& series, const QVector<ui::TimeSeriesPoint>& open_interest);

    void set_open_interest_visible(bool visible);
    void clear();

    void refresh_theme();

  protected:
    void changeEvent(QEvent* event) override;

  private:
    class Canvas;

    void rebuild();
    void retranslateUi();

    Canvas* canvas_ = nullptr;
    QLabel* empty_lbl_ = nullptr;
    QVector<CftcChartSeries> series_;
    QVector<ui::TimeSeriesPoint> open_interest_;
    bool show_open_interest_ = false;
};

} // namespace fincept::screens
