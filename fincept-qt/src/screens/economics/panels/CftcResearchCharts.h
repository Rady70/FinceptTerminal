// src/screens/economics/panels/CftcResearchCharts.h
//
// Painted charts of the CFTC research workspace. They draw only values the
// caller supplies: a missing reading is left empty (a gap, never a zero or an
// interpolated line), and every mark has a hover text with the exact values.
// Colours come from the active theme tokens: amber means "toward long / high
// in its history", cyan "toward short / low in its history"; neither colour
// implies good or bad.
#pragma once

#include "screens/economics/panels/CftcResearchModel.h"

#include <QColor>
#include <QDate>
#include <QVector>
#include <QWidget>

#include <optional>

namespace fincept::screens {

/// Shared base: hover hit areas with tooltips and a pixel-sized font helper.
class CftcChart : public QWidget {
    Q_OBJECT
  public:
    explicit CftcChart(QWidget* parent = nullptr);
    /// Tooltip text associated with a widget-local position (tests, UI automation).
    QString hit_text(const QPointF& position) const;

  protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void add_hit(const QRectF& area, const QString& text) { hits_.push_back({area, text}); }
    void clear_hits() { hits_.clear(); }
    QFont chart_font(int pixel_size, bool bold = false) const;

  private:
    QVector<QPair<QRectF, QString>> hits_;
};

/// Diverging position-in-history colour for a 0-1 percentile: cyan toward the
/// bottom, amber toward the top, the raised surface at the middle.
QColor cftc_percentile_color(double percentile);
/// Diverging colour for a signed change scaled to `extent`.
QColor cftc_change_color(double value, double extent);
QColor cftc_ink_on(const QColor& background);
QColor cftc_group_color(int index);

// ── Overview: positioning board ─────────────────────────────────────────────

/// One market row of the board, or a group heading when `heading` is set.
struct CftcBoardRow {
    bool heading = false;
    QString key;
    QString label; ///< market label, or the asset-class name for a heading
    CftcMarketRow market;
};

class CftcPositioningBoard : public CftcChart {
    Q_OBJECT
  public:
    explicit CftcPositioningBoard(QWidget* parent = nullptr);
    /// `columns` are the family's participant keys in report order; the
    /// principal key is highlighted.
    void set_data(const QVector<CftcBoardRow>& rows, const QStringList& columns, const QString& principal_key,
                  services::CftcFamily family);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int market_count() const;

  signals:
    void marketActivated(const QString& market_key);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    QVector<CftcBoardRow> rows_;
    QStringList columns_;
    QString principal_key_;
    services::CftcFamily family_ = services::CftcFamily::Legacy;
    QVector<QPair<QRectF, QString>> row_targets_;
    int content_height() const;
};

// ── Detail: participant positions now ───────────────────────────────────────

struct CftcParticipantBarRow {
    QString key;
    QString name;
    bool has_long = false;
    double long_pct = 0.0; ///< long as % of open interest
    bool has_short = false;
    double short_pct = 0.0;
    bool has_net = false;
    double net_pct = 0.0;
    double net_contracts = 0.0;
    std::optional<double> percentile;
    std::optional<double> percentile_4r;
    std::optional<double> percentile_13r;
    std::optional<double> net_flow_1w; ///< one-report net flow, % of the previous report's OI
    bool principal = false;
};

class CftcParticipantBars : public CftcChart {
    Q_OBJECT
  public:
    explicit CftcParticipantBars(QWidget* parent = nullptr);
    void set_rows(const QVector<CftcParticipantBarRow>& rows);
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QVector<CftcParticipantBarRow> rows_;
};

// ── Detail: synchronized history ────────────────────────────────────────────

struct CftcHistorySeries {
    QString key;
    QString name;
    QColor color;
    QVector<CftcSeriesPoint> points;
    bool emphasized = false;
};

/// Up to five vertically stacked panes on one report-date axis with a shared
/// crosshair: price (reference, optional), Net %OI by participant, the
/// headline group's 156-report percentile, Open Interest and the 4-largest
/// traders' concentration. Clicking a legend entry hides or shows a series.
class CftcHistoryChart : public CftcChart {
    Q_OBJECT
  public:
    explicit CftcHistoryChart(QWidget* parent = nullptr);
    void set_range(const QDate& from, const QDate& to);
    void set_price(const QVector<CftcSeriesPoint>& points, const QString& label);
    void set_positioning(const QVector<CftcHistorySeries>& series);
    void set_percentile(const QVector<CftcSeriesPoint>& points, const QString& label);
    void set_open_interest(const QVector<CftcSeriesPoint>& points);
    void set_concentration(const QVector<CftcSeriesPoint>& long_side, const QVector<CftcSeriesPoint>& short_side);
    void clear();
    QSize sizeHint() const override;
    QStringList visible_series() const;

  protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    struct Pane {
        QString title;
        QRectF rect;
        double low = 0.0;
        double high = 1.0;
        bool percent_axis = false;
    };
    QDate from_;
    QDate to_;
    QVector<CftcSeriesPoint> price_;
    QString price_label_;
    QVector<CftcHistorySeries> positioning_;
    QVector<bool> hidden_;
    QVector<CftcSeriesPoint> percentile_;
    QString percentile_label_;
    QVector<CftcSeriesPoint> open_interest_;
    QVector<CftcSeriesPoint> concentration_long_;
    QVector<CftcSeriesPoint> concentration_short_;
    std::optional<double> hover_x_;
    QVector<QPair<QRectF, int>> legend_targets_;
    double x_of(const QDate& date, const QRectF& plot) const;
    QDate date_at(double x, const QRectF& plot) const;
};

// ── Detail: week-by-week change strip ───────────────────────────────────────

struct CftcFlowRow {
    QString name;
    QVector<CftcFlowCell> cells;
    bool open_interest = false;
};

class CftcFlowStrip : public CftcChart {
    Q_OBJECT
  public:
    explicit CftcFlowStrip(QWidget* parent = nullptr);
    void set_rows(const QVector<CftcFlowRow>& rows);
    QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QVector<CftcFlowRow> rows_;
};

} // namespace fincept::screens
