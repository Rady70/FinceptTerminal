// src/screens/economics/panels/CftcResearchCharts.h
//
// Painted charts of the CFTC research workspace. They draw only values the
// caller supplies: a missing reading is left empty (a gap, never a zero or an
// interpolated line), and every mark has a hover text with the exact values.
// Colours come from the active theme tokens: amber means "toward long / high
// in its history", cyan "toward short / low in its history". The standard
// labels (CftcResearchText.h) are drawn as tags: BULLISH green, BEARISH red,
// NEUTRAL grey, CROWDED in the warning colour, flips and turns in the info
// colour, flows by the direction they move the net position.
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

// ── Standard-label tags ─────────────────────────────────────────────────────

QColor cftc_bias_color(CftcBias bias);
QColor cftc_flow_color(CftcFlowType type);
QColor cftc_band_color(CftcLevelBand band);
/// Colour of a row tag (CROWDED …, FLIPPED …, TURNING, LARGE MOVE).
QColor cftc_tag_color(const QString& tag);
/// Draws a tag whose left edge is at `left`, vertically centred in [top, top+height];
/// returns its width. `filled` tags use a solid background, others an outline.
double cftc_draw_tag(QPainter& painter, double left, double top, double height, const QString& text, const QColor& tone,
                     bool filled, const QFont& font);

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

// ── Overview: positioning map ───────────────────────────────────────────────

/// Every market as a tile in one of five columns by the headline group's
/// 52-week level (VERY LOW … VERY HIGH), most extreme first within a column.
class CftcPositioningMap : public CftcChart {
    Q_OBJECT
  public:
    explicit CftcPositioningMap(QWidget* parent = nullptr);
    void set_rows(const QVector<CftcMarketRow>& rows, const QString& group_name);
    QSize sizeHint() const override;
    int tile_count() const;

  signals:
    void marketActivated(const QString& market_key);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    QVector<CftcMarketRow> columns_[5];
    QString group_name_;
    QVector<QPair<QRectF, QString>> targets_;
    int content_height() const;
};

// ── Detail: readings grid ───────────────────────────────────────────────────

/// One cell of the readings grid: an optional tag, a value and its tooltip.
struct CftcReadingCell {
    QString tag;
    QColor tag_color;
    bool tag_filled = false;
    QString value;
    QString size; ///< "LARGE" / "NOTABLE" / empty
    QString tooltip;
};

struct CftcReadingsRow {
    QString name;
    bool principal = false;
    bool open_interest = false;
    CftcReadingCell position;
    CftcReadingCell level;
    CftcReadingCell horizons[3]; ///< 1, 4 and 13 weeks
    QStringList tags;
    QString tags_tooltip;
};

/// The market page's comparison grid: one row per participant group plus Open
/// Interest; columns: position, 52-week level, and the 1 / 4 / 13-week change
/// with its flow type and size.
class CftcReadingsGrid : public CftcChart {
    Q_OBJECT
  public:
    explicit CftcReadingsGrid(QWidget* parent = nullptr);
    void set_rows(const QVector<CftcReadingsRow>& rows);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QVector<CftcReadingsRow> rows_;
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
/// headline group's 52-report percentile, Open Interest and the 4-largest
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
