// src/screens/etf_research/EtfResearchWidgets.h
//
// Compact custom-painted views for the ETF Flow & Sector Rotation workspace:
// a grouped heatmap, an RRG rotation map with trails, a correlation/wedge
// matrix, a PCA sector-vector map, a line chart and a signed bar chart. They
// paint from plain data handed to them; none reads the store or the network.
#pragma once
#include "services/etf/research/EtfResearchSnapshot.h"

#include <QColor>
#include <QDate>
#include <QString>
#include <QVector>
#include <QWidget>

#include <optional>

namespace fincept::screens::etfr {

struct HeatTile {
    QString key;   ///< selection key (symbol)
    QString label; ///< bold top line
    QString sub;   ///< secondary line (name / group)
    std::optional<double> value;
    QString value_text; ///< already formatted with sign/unit ("—" when missing)
    QString group;
    QString tag; ///< evidence / state tag shown in the corner (e.g. PRXY, EST)
    QColor tag_color;
    QString tooltip;
    bool stale = false;
};

class HeatmapWidget : public QWidget {
    Q_OBJECT
  public:
    explicit HeatmapWidget(QWidget* parent = nullptr);
    void set_tiles(const QVector<HeatTile>& tiles, double range, const QString& legend);
    void set_selected(const QString& key);
    int tile_count() const { return tiles_.size(); }
    const QVector<HeatTile>& tiles() const { return tiles_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override;

  signals:
    void tile_activated(const QString& key);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    bool event(QEvent* e) override;

  private:
    struct Placed {
        int index = 0;
        QRect rect;
    };
    QVector<Placed> layout_for(int width, int* height, QVector<QPair<QString, QRect>>* headers) const;
    QVector<HeatTile> tiles_;
    double range_ = 1.0;
    QString legend_;
    QString selected_;
};

struct RrgSeries {
    QString key;
    QString label;
    QVector<services::etf::research::RrgPoint> points;
    QColor color;
};

class RrgWidget : public QWidget {
    Q_OBJECT
  public:
    explicit RrgWidget(QWidget* parent = nullptr);
    void set_series(const QVector<RrgSeries>& series, const QString& title);
    void set_selected(const QString& key);
    int series_count() const { return series_.size(); }
    QSize minimumSizeHint() const override { return {260, 220}; }
    QSize sizeHint() const override { return {560, 440}; }

  signals:
    void series_activated(const QString& key);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    bool event(QEvent* e) override;

  private:
    QRectF plot_rect() const;
    QPointF map(double ratio, double mom) const;
    QString hit(const QPoint& p) const;
    QVector<RrgSeries> series_;
    QString title_;
    QString selected_;
    QString hovered_;
    double xmin_ = 98, xmax_ = 102, ymin_ = 98, ymax_ = 102;
};

class MatrixWidget : public QWidget {
    Q_OBJECT
  public:
    explicit MatrixWidget(QWidget* parent = nullptr);
    /// mode "corr": −1..+1 diverging; "geom": 0..1 (|sin θ|).
    void set_matrix(const QStringList& labels, const QVector<QVector<double>>& values, const QString& mode);
    QSize minimumSizeHint() const override { return {240, 220}; }
    QSize sizeHint() const override { return {460, 420}; }

  protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* e) override;

  private:
    QStringList labels_;
    QVector<QVector<double>> values_;
    QString mode_;
};

class PcaWidget : public QWidget {
    Q_OBJECT
  public:
    explicit PcaWidget(QWidget* parent = nullptr);
    void set_vectors(const QStringList& labels, const QVector<QPair<double, double>>& xy, double var1, double var2);
    QSize minimumSizeHint() const override { return {220, 200}; }
    QSize sizeHint() const override { return {380, 340}; }

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QStringList labels_;
    QVector<QPair<double, double>> xy_;
    double var1_ = 0, var2_ = 0;
};

struct LineSeries {
    QString label;
    QVector<QDate> dates;
    QVector<double> values;
    QColor color;
    bool dashed = false;
};

class LineChartWidget : public QWidget {
    Q_OBJECT
  public:
    explicit LineChartWidget(QWidget* parent = nullptr);
    void set_series(const QVector<LineSeries>& series, const QString& title, const QString& units);
    QSize minimumSizeHint() const override { return {240, 140}; }
    QSize sizeHint() const override { return {460, 220}; }

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;

  private:
    QVector<LineSeries> series_;
    QString title_, units_;
    int hover_x_ = -1;
};

struct Bar {
    QString label;
    std::optional<double> value; ///< nullopt draws a "no observation" marker, never a zero bar
    QString value_text;
    QColor color; ///< invalid: sign colour
    QString tooltip;
};

class BarChartWidget : public QWidget {
    Q_OBJECT
  public:
    explicit BarChartWidget(QWidget* parent = nullptr);
    void set_bars(const QVector<Bar>& bars, const QString& title);
    QSize minimumSizeHint() const override { return {220, 120}; }
    QSize sizeHint() const override { return {420, 180}; }

  protected:
    void paintEvent(QPaintEvent*) override;
    bool event(QEvent* e) override;

  private:
    QVector<Bar> bars_;
    QString title_;
};

} // namespace fincept::screens::etfr
