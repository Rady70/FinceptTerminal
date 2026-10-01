#pragma once

#include <QColor>
#include <QJsonObject>
#include <QWidget>

namespace fincept::screens {
// Discrete current backend outcomes; absence is never represented by a zero bar.
class FedWatchCurrentChart : public QWidget {
    Q_OBJECT
  public:
    struct Bar {
        int outcome_bp;
        bool open_ended;
        int source;
        double value;
    };
    explicit FedWatchCurrentChart(QWidget* parent = nullptr);
    void set_rows(const QList<QJsonObject>& rows);
    const QList<QJsonObject>& rows() const { return rows_; }
    QVector<Bar> bars() const;
    QColor series_color(int source) const;
    QPair<double, double> value_bounds() const { return {0, 100}; }

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

  private:
    QList<QJsonObject> rows_;
    QVector<QPair<QRectF, QString>> hits_;
};
} // namespace fincept::screens
