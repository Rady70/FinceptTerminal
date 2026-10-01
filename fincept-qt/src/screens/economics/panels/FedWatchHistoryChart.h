#pragma once
#include "screens/economics/panels/FedWatchViewModel.h"

#include <QWidget>

namespace fincept::screens {
// Actual observations: lines join only same/adjacent UTC days and stop at missing days.
class FedWatchHistoryChart : public QWidget {
  public:
    explicit FedWatchHistoryChart(QWidget* parent = nullptr);
    void set_series(const QVector<fedwatch::Series>& series);
    void set_probability_scale(bool enabled) {
        probability_scale_ = enabled;
        update();
    }
    const QVector<fedwatch::Series>& series() const { return series_; }

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

  private:
    QVector<fedwatch::Series> series_;
    QVector<QPair<QPointF, QString>> hit_points_;
    bool probability_scale_ = true;
};
} // namespace fincept::screens
