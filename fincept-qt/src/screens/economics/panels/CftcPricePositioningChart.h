// src/screens/economics/panels/CftcPricePositioningChart.h
//
// Batch 4B: the large synchronized Price + Positioning chart. Two stacked,
// semantically separate panes share one report-date time axis and one crosshair:
//
//   upper pane: the price path aligned to official CFTC report dates
//   lower pane: the report-family-appropriate primary participant's reported
//               net position (contracts)
//
// Price and positioning are different quantities and never share one axis. The
// widget renders the pure contract in CftcSyncChartData.h; it computes no
// analytical state and adds no signal, marker or forecast. Gaps are derived
// from the shared weekly gap-splitting rule, hover reports exact values for the
// snapped report date across both panes, and the current 1/4/13-report
// interpretation context is shown as a caption below the panes.
#pragma once

#include "screens/economics/panels/CftcSyncChartData.h"

#include <QDate>
#include <QPoint>
#include <QWidget>

class QLabel;

namespace fincept::screens {

class CftcPricePositioningChart : public QWidget {
    Q_OBJECT
  public:
    explicit CftcPricePositioningChart(QWidget* parent = nullptr);

    /// Replace the chart data. The contract already contains the aligned,
    /// gap-preserving pane points and the truthful source/proxy labels.
    void set_data(const CftcSyncChartData& data);
    void clear();

    void refresh_theme();

  protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    class Canvas;

    void rebuild();
    void retranslateUi();
    void handle_hover(const QDate& date, const QPoint& global_pos);
    void hide_hover();
    void align_plot_areas();

    Canvas* price_canvas_ = nullptr;
    Canvas* position_canvas_ = nullptr;
    QLabel* price_header_ = nullptr;
    QLabel* position_header_ = nullptr;
    QLabel* price_unavailable_lbl_ = nullptr;
    QLabel* position_unavailable_lbl_ = nullptr;
    QLabel* context_lbl_ = nullptr;
    QLabel* empty_lbl_ = nullptr;
    QLabel* tooltip_ = nullptr;
    CftcSyncChartData data_;
    QDate hover_date_;
};

} // namespace fincept::screens
