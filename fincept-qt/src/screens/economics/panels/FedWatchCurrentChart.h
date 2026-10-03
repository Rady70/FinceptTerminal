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
    void set_selected_outcome(int bp, bool open);
    static bool has_current_value(const QJsonObject& row);
    QRectF source_plot_bounds(int source) const;
  signals:
    void outcome_selected(int bp, bool open);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;

  private:
    QList<QJsonObject> rows_;
    QVector<QPair<QRectF, QString>> hits_;
    QVector<QRectF> categories_;
    int selected_bp_ = 0;
    bool selected_open_ = false;
    void select_row(int index);
    QVector<int> active_sources() const;
};
} // namespace fincept::screens
