#pragma once
#include "screens/economics/panels/FedWatchViewModel.h"

#include <QWidget>

namespace fincept::screens {
// Actual observations: lines join only same/adjacent UTC days and stop at missing days.
class FedWatchHistoryChart : public QWidget {
    Q_OBJECT
  public:
    explicit FedWatchHistoryChart(QWidget* parent = nullptr);
    void set_series(const QVector<fedwatch::Series>& series);
    void set_time_bounds(const QDateTime& first, const QDateTime& last) {
        first_ = first;
        last_ = last;
        update();
    }
    QPair<QDateTime, QDateTime> time_bounds() const { return {first_, last_}; }
    void set_probability_scale(bool enabled) {
        probability_scale_ = enabled;
        update();
    }
    const QVector<fedwatch::Series>& series() const { return series_; }
    void set_value_bounds(double low, double high) {
        value_bounds_ = {low, high};
        update();
    }
    QPair<double, double> value_bounds() const { return value_bounds_; }
    void set_hover_date(const QDate& date);
    QDate hover_date() const { return hover_date_; }
    QString hover_text() const;

  signals:
    void date_hovered(const QDate& date);
    void hover_finished();

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

  private:
    QVector<fedwatch::Series> series_;
    QVector<QPair<QPointF, QString>> hit_points_;
    QVector<QDate> hit_dates_;
    bool probability_scale_ = true;
    QDateTime first_, last_;
    QPair<double, double> value_bounds_{0, 100};
    QRectF plot_;
    qint64 plotted_first_ = 0, plotted_last_ = 0;
    QDate hover_date_;
    void update_accessibility();
};
} // namespace fincept::screens
