#pragma once
// HighlightCharts — small painted charts for the dashboard's summary widgets
// (FedWatch, COT, ETF flows, Risk Metrics). Colours are read from the live
// theme at paint time, so a theme switch needs only a repaint. No signals,
// no moc: header-only QWidget subclasses.
#include "screens/dashboard/widgets/MarketGauges.h"
#include "ui/theme/Theme.h"

#include <QDate>
#include <QEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QVector>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::screens::widgets::charts {

inline QFont chart_font(int px, bool bold = false) {
    QFont f(QString::fromLatin1(ui::fonts::DATA_FAMILY()));
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

/// Shows the tooltip of the row under the pointer (rows of a fixed height).
template <typename Rows>
bool chart_row_tooltip(QWidget* w, QEvent* e, const Rows& rows, int top, int row_h) {
    if (e->type() != QEvent::ToolTip)
        return false;
    auto* he = static_cast<QHelpEvent*>(e);
    const int i = row_h > 0 ? (he->pos().y() - top) / row_h : -1;
    if (i >= 0 && i < rows.size() && !rows[i].tooltip.isEmpty() && he->pos().y() >= top)
        QToolTip::showText(he->globalPos(), rows[i].tooltip, w);
    else
        QToolTip::hideText();
    return true;
}

// ── Horizontal bar list ──────────────────────────────────────────────────────

struct BarRow {
    QString label;
    QString value_text; ///< bold, coloured by sign
    QString note;       ///< dim, right of the value (e.g. "1.7× typical")
    double value = 0.0;
    QString tooltip;
    const ui::ColorToken* color = nullptr; ///< one-sided bars: this row's colour (default: the list's)
};

/// One labelled horizontal bar per row. Diverging: a centre zero line,
/// positive values grow right (green), negative left (red). One-sided: bars
/// grow from the left in `one_sided_color`. Rows grow taller (up to 26 px)
/// when the tile has room, so the chart fills its space.
class BarList : public QWidget {
  public:
    explicit BarList(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }

    void set_rows(QVector<BarRow> rows, bool diverging) {
        rows_ = std::move(rows);
        diverging_ = diverging;
        updateGeometry();
        update();
    }
    void set_empty_text(const QString& text) {
        empty_ = text;
        update();
    }
    void set_one_sided_color(const ui::ColorToken& token) { one_sided_ = &token; }
    /// A fixed full-scale value (e.g. 100 for scores); 0 = the largest |value|.
    void set_fixed_scale(double scale) { fixed_scale_ = scale; }

    QSize sizeHint() const override { return {240, std::max<int>(1, int(rows_.size())) * 20}; }
    QSize minimumSizeHint() const override { return {120, std::max<int>(1, int(rows_.size())) * kMinRow}; }

  protected:
    bool event(QEvent* e) override {
        if (chart_row_tooltip(this, e, rows_, 0, row_height()))
            return true;
        return QWidget::event(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor text = QColor(ui::colors::TEXT_PRIMARY());
        const QColor dim = QColor(ui::colors::TEXT_TERTIARY());
        if (rows_.isEmpty()) {
            p.setFont(chart_font(10));
            p.setPen(dim);
            p.drawText(rect().adjusted(0, 0, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, empty_);
            return;
        }
        const QFont label_font = chart_font(10);
        const QFont value_font = chart_font(10, true);
        const QFont note_font = chart_font(9);
        const QFontMetrics lm(label_font), vm(value_font), nm(note_font);

        int value_w = 0, note_w = 0, label_w = 0;
        double max_abs = 0;
        for (const auto& r : rows_) {
            value_w = std::max(value_w, vm.horizontalAdvance(r.value_text));
            note_w = std::max(note_w, nm.horizontalAdvance(r.note));
            label_w = std::max(label_w, lm.horizontalAdvance(r.label));
            max_abs = std::max(max_abs, std::abs(r.value));
        }
        label_w = std::min(label_w + 8, int(width() * 0.42));
        const int right_w = value_w + (note_w > 0 ? note_w + 8 : 0) + 8;
        const int bar_x = label_w;
        const int bar_w = std::max(20, width() - label_w - right_w);
        const int row_h = row_height();
        const int bar_h = std::clamp(row_h / 2, 5, 10);
        const QColor pos = QColor(ui::colors::POSITIVE()), neg = QColor(ui::colors::NEGATIVE());
        const QColor track = QColor(ui::colors::BORDER_DIM());
        const double scale = fixed_scale_ > 0 ? fixed_scale_ : (max_abs > 0 ? max_abs : 1.0);

        if (diverging_) {
            const int zx = bar_x + bar_w / 2;
            p.setPen(QPen(track, 1));
            p.drawLine(zx, 0, zx, row_h * int(rows_.size()));
        }
        for (int i = 0; i < rows_.size(); ++i) {
            const auto& r = rows_[i];
            const int y = i * row_h;
            const int cy = y + row_h / 2;
            p.setFont(label_font);
            p.setPen(text);
            p.drawText(QRect(0, y, label_w - 6, row_h), Qt::AlignLeft | Qt::AlignVCenter,
                       lm.elidedText(r.label, Qt::ElideRight, label_w - 6));

            const QColor c = diverging_ ? (r.value >= 0 ? pos : neg)
                             : r.color  ? QColor((*r.color)())
                                        : (one_sided_ ? QColor((*one_sided_)()) : QColor(ui::colors::INFO()));
            QRectF bar;
            if (diverging_) {
                const double half = bar_w / 2.0 - 2;
                const double len = std::max(1.5, std::abs(r.value) / scale * half);
                const double zx = bar_x + bar_w / 2.0;
                bar = r.value >= 0 ? QRectF(zx, cy - bar_h / 2.0, len, bar_h)
                                   : QRectF(zx - len, cy - bar_h / 2.0, len, bar_h);
            } else {
                p.setPen(Qt::NoPen);
                p.setBrush(track);
                p.drawRoundedRect(QRectF(bar_x, cy - bar_h / 2.0, bar_w, bar_h), 1.5, 1.5);
                bar = QRectF(bar_x, cy - bar_h / 2.0, std::max(1.5, r.value / scale * bar_w), bar_h);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(bar, 1.5, 1.5);

            const int vx = bar_x + bar_w + 8;
            p.setFont(value_font);
            p.setPen(diverging_ ? c : text);
            p.drawText(QRect(vx, y, value_w, row_h), Qt::AlignRight | Qt::AlignVCenter, r.value_text);
            if (!r.note.isEmpty()) {
                p.setFont(note_font);
                p.setPen(dim);
                p.drawText(QRect(vx + value_w + 8, y, note_w, row_h), Qt::AlignLeft | Qt::AlignVCenter, r.note);
            }
        }
    }

  private:
    static constexpr int kMinRow = 16;
    static constexpr int kMaxRow = 26;
    int row_height() const {
        if (rows_.isEmpty())
            return kMinRow;
        return std::clamp(height() / int(rows_.size()), kMinRow, kMaxRow);
    }

    QVector<BarRow> rows_;
    bool diverging_ = true;
    QString empty_;
    const ui::ColorToken* one_sided_ = nullptr;
    double fixed_scale_ = 0.0;
};

// ── Paired probability bars (FedWatch outcomes) ─────────────────────────────

struct PairRow {
    QString label;
    std::optional<double> a; ///< percent 0..100
    std::optional<double> b;
    QString tooltip;
};

/// One row per outcome with two probability bars: series A (solid, INFO)
/// over series B (thinner, PURPLE-ish secondary). A missing side shows "—".
class PairBarList : public QWidget {
  public:
    PairBarList(QString a_name, QString b_name, QWidget* parent = nullptr)
        : QWidget(parent), a_name_(std::move(a_name)), b_name_(std::move(b_name)) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMouseTracking(true);
    }
    void set_names(const QString& a, const QString& b) {
        a_name_ = a;
        b_name_ = b;
        update();
    }
    void set_rows(QVector<PairRow> rows) {
        rows_ = std::move(rows);
        updateGeometry();
        update();
    }
    void set_empty_text(const QString& t) {
        empty_ = t;
        update();
    }
    QSize sizeHint() const override { return {240, kLegend + std::max<int>(1, int(rows_.size())) * kRow}; }
    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    bool event(QEvent* e) override {
        if (chart_row_tooltip(this, e, rows_, kLegend, kRow))
            return true;
        return QWidget::event(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor text = QColor(ui::colors::TEXT_PRIMARY());
        const QColor dim = QColor(ui::colors::TEXT_TERTIARY());
        const QColor ca = QColor(ui::colors::INFO());
        const QColor cb = QColor(ui::colors::WARNING());
        const QColor track = QColor(ui::colors::BORDER_DIM());
        const QFont small = chart_font(9), label_font = chart_font(10), value_font = chart_font(10, true);

        // Legend
        p.setFont(small);
        int x = 0;
        for (const auto& [name, c] : {std::pair{a_name_, ca}, std::pair{b_name_, cb}}) {
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRect(QRectF(x, 5, 8, 6));
            p.setPen(dim);
            p.drawText(QPoint(x + 12, 11), name);
            x += 12 + QFontMetrics(small).horizontalAdvance(name) + 16;
        }
        if (rows_.isEmpty()) {
            p.setFont(label_font);
            p.setPen(dim);
            p.drawText(QRect(0, kLegend, width(), kRow), Qt::AlignLeft | Qt::AlignVCenter, empty_);
            return;
        }
        const QFontMetrics lm(label_font), vm(value_font);
        int label_w = 0;
        for (const auto& r : rows_)
            label_w = std::max(label_w, lm.horizontalAdvance(r.label));
        label_w = std::min(label_w + 10, int(width() * 0.38));
        const int value_w = vm.horizontalAdvance(QStringLiteral("100%")) + 6;
        const int bar_x = label_w, bar_w = std::max(20, width() - label_w - value_w - 6);

        for (int i = 0; i < rows_.size(); ++i) {
            const auto& r = rows_[i];
            const int y = kLegend + i * kRow;
            p.setFont(label_font);
            p.setPen(text);
            p.drawText(QRect(0, y, label_w - 6, kRow), Qt::AlignLeft | Qt::AlignVCenter,
                       lm.elidedText(r.label, Qt::ElideRight, label_w - 6));
            auto draw = [&](const std::optional<double>& v, const QColor& c, int top, int h, bool bold) {
                p.setPen(Qt::NoPen);
                p.setBrush(track);
                p.drawRoundedRect(QRectF(bar_x, top, bar_w, h), 1.5, 1.5);
                if (v) {
                    p.setBrush(c);
                    p.drawRoundedRect(QRectF(bar_x, top, std::max(1.5, std::clamp(*v, 0.0, 100.0) / 100.0 * bar_w), h),
                                      1.5, 1.5);
                }
                p.setFont(bold ? value_font : small);
                p.setPen(v ? c : dim);
                p.drawText(QRect(bar_x + bar_w + 6, top - 4, value_w, h + 8), Qt::AlignRight | Qt::AlignVCenter,
                           v ? QStringLiteral("%1%").arg(*v, 0, 'f', 0) : QStringLiteral("—"));
            };
            draw(r.a, ca, y + 5, 8, true);
            draw(r.b, cb, y + 17, 5, false);
        }
    }

  private:
    static constexpr int kLegend = 18;
    static constexpr int kRow = 28;
    QString a_name_, b_name_, empty_;
    QVector<PairRow> rows_;
};

// ── Expected-rate path (FedWatch) ───────────────────────────────────────────

struct PathPoint {
    QString x_label;            ///< meeting date
    std::optional<double> rate; ///< expected (probability-weighted) rate after the meeting, percent
    QString note;               ///< most likely move, e.g. "Hold 52%"
    QString tooltip;
    QString note2;                ///< the next most likely move, e.g. "Hike 48%"
    std::optional<double> likely; ///< rate if the most likely move happens at every meeting
};

/// Expected policy rate after each upcoming meeting, against the current
/// target band (shaded). Fills the space it is given.
class RatePathChart : public QWidget {
  public:
    explicit RatePathChart(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }
    void set_legend(const QString& expected, const QString& likely) {
        legend_a_ = expected;
        legend_b_ = likely;
        update();
    }
    void set_data(QVector<PathPoint> points, std::optional<double> band_low, std::optional<double> band_high) {
        points_ = std::move(points);
        low_ = band_low;
        high_ = band_high;
        update();
    }
    QSize sizeHint() const override { return {300, 150}; }
    QSize minimumSizeHint() const override { return {160, 110}; }

  protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip && !points_.isEmpty()) {
            auto* he = static_cast<QHelpEvent*>(e);
            const int n = visible();
            const double step = plot_w() / n;
            const int i = int((he->pos().x() - kLeft) / step);
            if (i >= 0 && i < n && !points_[i].tooltip.isEmpty())
                QToolTip::showText(he->globalPos(), points_[i].tooltip, this);
            else
                QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor text = QColor(ui::colors::TEXT_PRIMARY());
        const QColor dim = QColor(ui::colors::TEXT_TERTIARY());
        const QColor line = QColor(ui::colors::INFO());
        const QColor grid = QColor(ui::colors::BORDER_DIM());
        if (points_.isEmpty())
            return;

        double lo = 1e9, hi = -1e9;
        for (int i = 0; i < visible(); ++i)
            for (const auto& r : {points_[i].rate, points_[i].likely})
                if (r) {
                    lo = std::min(lo, *r);
                    hi = std::max(hi, *r);
                }
        if (low_ && high_) {
            lo = std::min(lo, *low_);
            hi = std::max(hi, *high_);
        }
        if (lo > hi)
            return;
        const double pad = std::max(0.08, (hi - lo) * 0.18);
        lo -= pad;
        hi += pad;

        const int top = 26, bottom = height() - kBottom;
        const int n = visible();
        const double step = plot_w() / n;
        auto y_of = [&](double r) { return bottom - (r - lo) / (hi - lo) * (bottom - top); };
        auto x_of = [&](int i) { return kLeft + step * (i + 0.5); };

        // Current target band.
        if (low_ && high_) {
            QColor band = line;
            band.setAlpha(38);
            p.setPen(Qt::NoPen);
            p.setBrush(band);
            p.drawRect(QRectF(kLeft, y_of(*high_), plot_w(), y_of(*low_) - y_of(*high_)));
            p.setFont(chart_font(9));
            p.setPen(dim);
            p.drawText(QRectF(0, y_of(*high_) - 6, kLeft - 4, 12), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1").arg(*high_, 0, 'f', 2));
            p.drawText(QRectF(0, y_of(*low_) - 6, kLeft - 4, 12), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1").arg(*low_, 0, 'f', 2));
        }
        p.setPen(QPen(grid, 1));
        p.drawLine(QPointF(kLeft, bottom), QPointF(kLeft + plot_w(), bottom));

        // Path line.
        QPainterPath path;
        bool started = false;
        for (int i = 0; i < n; ++i) {
            if (!points_[i].rate)
                continue;
            const QPointF pt(x_of(i), y_of(*points_[i].rate));
            if (!started) {
                path.moveTo(pt);
                started = true;
            } else {
                path.lineTo(pt);
            }
        }
        p.setPen(QPen(line, 2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        // Most likely path: the rate moves only by each meeting's most likely
        // outcome, so it steps (and stays flat through holds).
        const QColor likely_c(ui::colors::AMBER());
        QPainterPath steps;
        bool s_started = false;
        double prev_y = 0;
        for (int i = 0; i < n; ++i) {
            if (!points_[i].likely)
                continue;
            const double y = y_of(*points_[i].likely);
            const double x0 = x_of(i) - step / 2, x1 = x_of(i) + step / 2;
            if (!s_started) {
                steps.moveTo(x0, y);
                s_started = true;
            } else {
                steps.lineTo(x0, prev_y);
                steps.lineTo(x0, y);
            }
            steps.lineTo(x1, y);
            prev_y = y;
        }
        p.setPen(QPen(likely_c, 1.5, Qt::DashLine));
        p.drawPath(steps);

        const QFont value_font = chart_font(10, true), small = chart_font(9);
        // Legend.
        p.setFont(small);
        double lx = kLeft;
        auto legend = [&](const QColor& c, Qt::PenStyle style, const QString& t) {
            p.setPen(QPen(c, 2, style));
            p.drawLine(QPointF(lx, 8), QPointF(lx + 16, 8));
            p.setPen(dim);
            p.drawText(QPointF(lx + 20, 12), t);
            lx += 20 + QFontMetrics(small).horizontalAdvance(t) + 16;
        };
        legend(line, Qt::SolidLine, legend_a_);
        if (s_started)
            legend(likely_c, Qt::DashLine, legend_b_);
        for (int i = 0; i < n; ++i) {
            const auto& pt = points_[i];
            const double x = x_of(i);
            if (pt.rate) {
                const double y = y_of(*pt.rate);
                p.setPen(Qt::NoPen);
                p.setBrush(line);
                p.drawEllipse(QPointF(x, y), 3.5, 3.5);
                p.setFont(value_font);
                p.setPen(text);
                p.drawText(QRectF(x - step / 2, y - 18, step, 14), Qt::AlignCenter,
                           QStringLiteral("%1%").arg(*pt.rate, 0, 'f', 2));
            }
            p.setFont(small);
            p.setPen(text);
            p.drawText(QRectF(x - step / 2, bottom + 3, step, 13), Qt::AlignCenter, pt.x_label);
            p.setPen(dim);
            p.drawText(QRectF(x - step / 2, bottom + 16, step, 13), Qt::AlignCenter,
                       QFontMetrics(small).elidedText(pt.note, Qt::ElideRight, int(step) - 2));
            p.drawText(QRectF(x - step / 2, bottom + 29, step, 13), Qt::AlignCenter,
                       QFontMetrics(small).elidedText(pt.note2, Qt::ElideRight, int(step) - 2));
        }
    }

  private:
    static constexpr int kLeft = 34;
    static constexpr int kBottom = 45;
    double plot_w() const { return std::max(40, width() - kLeft - 4); }
    /// Meetings that fit side by side (at least 3, about 64 px each).
    int visible() const { return std::min(int(points_.size()), std::max(3, int(plot_w() / 64))); }

    QVector<PathPoint> points_;
    std::optional<double> low_, high_;
    QString legend_a_ = QStringLiteral("Expected (average)");
    QString legend_b_ = QStringLiteral("Most likely path");
};

// ── Split bar (COT lean) ─────────────────────────────────────────────────────

struct SplitPart {
    QString label;
    int count = 0;
    const ui::ColorToken* color = nullptr;
    QString tooltip;
};

/// One bar split into proportional parts, each labelled with its count.
class SplitBar : public QWidget {
  public:
    explicit SplitBar(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(22);
        setMouseTracking(true);
    }
    void set_parts(QVector<SplitPart> parts) {
        parts_ = std::move(parts);
        update();
    }

  protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            const auto spans = layout_spans();
            for (int i = 0; i < spans.size(); ++i)
                if (he->pos().x() >= spans[i].first && he->pos().x() < spans[i].second &&
                    !parts_[i].tooltip.isEmpty()) {
                    QToolTip::showText(he->globalPos(), parts_[i].tooltip, this);
                    return true;
                }
            QToolTip::hideText();
            return true;
        }
        return QWidget::event(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const auto spans = layout_spans();
        const QFont f = chart_font(9, true);
        p.setFont(f);
        for (int i = 0; i < spans.size(); ++i) {
            const auto& part = parts_[i];
            const QRectF r(spans[i].first, 0, spans[i].second - spans[i].first - 2, height());
            if (r.width() <= 0)
                continue;
            QColor c = part.color ? QColor((*part.color)()) : QColor(ui::colors::TEXT_TERTIARY());
            QColor fill = c;
            fill.setAlpha(60);
            p.setPen(QPen(c, 1));
            p.setBrush(fill);
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
            const QString t = QStringLiteral("%1 %2").arg(part.label).arg(part.count);
            const QString shown =
                QFontMetrics(f).horizontalAdvance(t) + 6 <= r.width() ? t : QString::number(part.count);
            p.setPen(QColor(ui::colors::TEXT_PRIMARY()));
            p.drawText(r, Qt::AlignCenter, shown);
        }
    }

  private:
    QVector<QPair<int, int>> layout_spans() const {
        QVector<QPair<int, int>> out;
        int total = 0;
        for (const auto& part : parts_)
            total += part.count;
        if (total <= 0)
            return out;
        // Every non-empty part keeps room for its count.
        const int min_w = 28;
        int nonzero = 0;
        for (const auto& part : parts_)
            nonzero += part.count > 0 ? 1 : 0;
        const int free_w = std::max(0, width() - nonzero * min_w);
        int x = 0;
        for (const auto& part : parts_) {
            const int w = part.count > 0 ? min_w + int(std::round(double(part.count) / total * free_w)) : 0;
            out.append({x, x + w});
            x += w;
        }
        return out;
    }

    QVector<SplitPart> parts_;
};

// ── VIX gauge ────────────────────────────────────────────────────────────────

/// The VIX on a 0–50 scale whose colour bands are the regime boundaries
/// (15, 20, 30, 40), with a marker at the reading.
class VixGauge : public QWidget {
  public:
    explicit VixGauge(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(28);
    }
    void set_value(std::optional<double> vix) {
        vix_ = vix;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double w = width() - 2;
        const double bar_y = 2, bar_h = 6;
        const ui::ColorToken* tokens[] = {&ui::colors::POSITIVE, &ui::colors::POSITIVE_DIM, &ui::colors::WARNING,
                                          &ui::colors::AMBER, &ui::colors::NEGATIVE};
        double from = 0;
        for (int i = 0; i < 5; ++i) {
            const double to = i < 4 ? gauges::kVixBounds[i] : gauges::kVixGaugeMax;
            const double x0 = 1 + from / gauges::kVixGaugeMax * w, x1 = 1 + to / gauges::kVixGaugeMax * w;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor((*tokens[i])()));
            p.drawRect(QRectF(x0, bar_y, x1 - x0 - 1, bar_h));
            from = to;
        }
        p.setFont(chart_font(8));
        p.setPen(QColor(ui::colors::TEXT_TERTIARY()));
        for (double b : gauges::kVixBounds) {
            const double x = 1 + b / gauges::kVixGaugeMax * w;
            p.drawText(QRectF(x - 12, bar_y + bar_h + 10, 24, 10), Qt::AlignCenter, QString::number(int(b)));
        }
        if (vix_) {
            const double x = 1 + gauges::vix_gauge_fraction(*vix_) * w;
            QPainterPath tri;
            tri.moveTo(x, bar_y + bar_h + 1);
            tri.lineTo(x - 5, bar_y + bar_h + 8);
            tri.lineTo(x + 5, bar_y + bar_h + 8);
            tri.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(ui::colors::TEXT_PRIMARY()));
            p.drawPath(tri);
            p.setPen(QPen(QColor(ui::colors::TEXT_PRIMARY()), 2));
            p.drawLine(QPointF(x, bar_y - 2), QPointF(x, bar_y + bar_h + 1));
        }
    }

  private:
    std::optional<double> vix_;
};

// ── Fear & greed scores (0–100) ─────────────────────────────────────────────

/// CNN's bands, also used for the market-breadth score: below 25 extreme
/// fear, 25–45 fear, 45–55 neutral, 55–75 greed, above 75 extreme greed.
inline constexpr double kScoreBounds[] = {25.0, 45.0, 55.0, 75.0};

inline const ui::ColorToken* score_color(double score) {
    if (score < kScoreBounds[0])
        return &ui::colors::NEGATIVE;
    if (score < kScoreBounds[1])
        return &ui::colors::AMBER;
    if (score <= kScoreBounds[2])
        return &ui::colors::WARNING;
    if (score <= kScoreBounds[3])
        return &ui::colors::POSITIVE_DIM;
    return &ui::colors::POSITIVE;
}

/// A 0–100 bar in the five score bands with a marker at the score and,
/// optionally, a hollow marker at an earlier reading.
class ScoreGauge : public QWidget {
  public:
    explicit ScoreGauge(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(30);
    }
    void set_score(std::optional<double> score, std::optional<double> earlier = std::nullopt) {
        score_ = score;
        earlier_ = earlier;
        update();
    }
    /// The words under the two ends of the bar (default FEAR / GREED).
    void set_end_labels(const QString& low, const QString& high) {
        low_label_ = low;
        high_label_ = high;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double w = width() - 2, bar_y = 4, bar_h = 7;
        const double bounds[] = {0, kScoreBounds[0], kScoreBounds[1], kScoreBounds[2], kScoreBounds[3], 100};
        for (int i = 0; i < 5; ++i) {
            const double x0 = 1 + bounds[i] / 100 * w, x1 = 1 + bounds[i + 1] / 100 * w;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor((*score_color((bounds[i] + bounds[i + 1]) / 2))()));
            p.drawRect(QRectF(x0, bar_y, x1 - x0 - 1, bar_h));
        }
        p.setFont(chart_font(8));
        p.setPen(QColor(ui::colors::TEXT_TERTIARY()));
        p.drawText(QRectF(1, bar_y + bar_h + 9, w / 2, 10), Qt::AlignLeft | Qt::AlignVCenter, low_label_);
        p.drawText(QRectF(w / 2, bar_y + bar_h + 9, w / 2, 10), Qt::AlignRight | Qt::AlignVCenter, high_label_);
        auto marker = [&](double v, bool solid) {
            const double x = 1 + std::clamp(v, 0.0, 100.0) / 100 * w;
            QPainterPath tri;
            tri.moveTo(x, bar_y + bar_h + 1);
            tri.lineTo(x - 5, bar_y + bar_h + 8);
            tri.lineTo(x + 5, bar_y + bar_h + 8);
            tri.closeSubpath();
            const QColor c(ui::colors::TEXT_PRIMARY());
            p.setPen(solid ? Qt::NoPen : QPen(c, 1));
            p.setBrush(solid ? QBrush(c) : Qt::NoBrush);
            p.drawPath(tri);
            if (solid) {
                p.setPen(QPen(c, 2));
                p.drawLine(QPointF(x, bar_y - 3), QPointF(x, bar_y + bar_h + 1));
            }
        };
        if (earlier_)
            marker(*earlier_, false);
        if (score_)
            marker(*score_, true);
    }

  private:
    std::optional<double> score_, earlier_;
    QString low_label_ = QStringLiteral("FEAR");
    QString high_label_ = QStringLiteral("GREED");
};

struct ScorePoint {
    QDate date;
    double score = 0.0;
};

/// A score history on a 0–100 axis with the fear (25) and greed (75) lines.
class ScoreLine : public QWidget {
  public:
    explicit ScoreLine(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }
    void set_points(QVector<ScorePoint> points) {
        points_ = std::move(points);
        update();
    }
    QSize sizeHint() const override { return {260, 90}; }
    QSize minimumSizeHint() const override { return {120, 56}; }

  protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip && points_.size() > 1) {
            auto* he = static_cast<QHelpEvent*>(e);
            const double plot_w = std::max(1, width() - kLeft - 2);
            const int i = std::clamp(int(std::round((he->pos().x() - kLeft) / plot_w * (points_.size() - 1))), 0,
                                     int(points_.size()) - 1);
            QToolTip::showText(
                he->globalPos(),
                QStringLiteral("%1: %2").arg(points_[i].date.toString(Qt::ISODate)).arg(points_[i].score, 0, 'f', 1),
                this);
            return true;
        }
        return QWidget::event(e);
    }

    void paintEvent(QPaintEvent*) override {
        if (points_.size() < 2)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double top = 4, bottom = height() - 14, plot_w = std::max(1, width() - kLeft - 2);
        auto y_of = [&](double s) { return bottom - std::clamp(s, 0.0, 100.0) / 100.0 * (bottom - top); };
        auto x_of = [&](int i) { return kLeft + plot_w * i / double(points_.size() - 1); };
        const QColor dim(ui::colors::TEXT_TERTIARY()), grid(ui::colors::BORDER_DIM());
        p.setFont(chart_font(8));
        for (double level : {25.0, 50.0, 75.0}) {
            p.setPen(QPen(grid, 1, level == 50.0 ? Qt::SolidLine : Qt::DashLine));
            p.drawLine(QPointF(kLeft, y_of(level)), QPointF(kLeft + plot_w, y_of(level)));
            p.setPen(dim);
            p.drawText(QRectF(0, y_of(level) - 6, kLeft - 4, 12), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(int(level)));
        }
        QPainterPath path;
        for (int i = 0; i < points_.size(); ++i) {
            const QPointF pt(x_of(i), y_of(points_[i].score));
            if (i == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        p.setPen(QPen(QColor(ui::colors::INFO()), 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        const auto& last = points_.last();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor((*score_color(last.score))()));
        p.drawEllipse(QPointF(x_of(int(points_.size()) - 1), y_of(last.score)), 3, 3);
        p.setPen(dim);
        p.drawText(QRectF(kLeft, bottom + 2, 90, 12), Qt::AlignLeft | Qt::AlignVCenter,
                   points_.first().date.toString(QStringLiteral("d MMM yy")));
        p.drawText(QRectF(kLeft + plot_w - 90, bottom + 2, 90, 12), Qt::AlignRight | Qt::AlignVCenter,
                   last.date.toString(QStringLiteral("d MMM yy")));
    }

  private:
    static constexpr int kLeft = 18;
    QVector<ScorePoint> points_;
};

struct ValuePoint {
    QDate date;
    double value = 0.0;
};

/// A dated value series (with an optional second line, e.g. a moving
/// average) on an automatic axis, with a legend and the latest values.
class SeriesLine : public QWidget {
  public:
    explicit SeriesLine(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
    }
    void set_series(const QString& name, QVector<ValuePoint> points, const QString& second_name = {},
                    QVector<ValuePoint> second = {}) {
        name_ = name;
        a_ = std::move(points);
        second_name_ = second_name;
        b_ = std::move(second);
        update();
    }
    QSize sizeHint() const override { return {260, 110}; }
    QSize minimumSizeHint() const override { return {120, 64}; }

  protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip && a_.size() > 1) {
            auto* he = static_cast<QHelpEvent*>(e);
            const double plot_w = std::max(1, width() - kLeft - 2);
            const int i =
                std::clamp(int(std::round((he->pos().x() - kLeft) / plot_w * (a_.size() - 1))), 0, int(a_.size()) - 1);
            QString t = QStringLiteral("%1\n%2: %3").arg(a_[i].date.toString(Qt::ISODate), name_, fmt(a_[i].value));
            for (const auto& q : b_)
                if (q.date == a_[i].date)
                    t += QStringLiteral("\n%1: %2").arg(second_name_, fmt(q.value));
            QToolTip::showText(he->globalPos(), t, this);
            return true;
        }
        return QWidget::event(e);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor dim(ui::colors::TEXT_TERTIARY()), grid(ui::colors::BORDER_DIM());
        const QColor ca(ui::colors::INFO()), cb(ui::colors::AMBER());
        const QFont small = chart_font(9);
        p.setFont(small);
        if (a_.size() < 2) {
            p.setPen(dim);
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("—"));
            return;
        }
        // Legend with the latest values.
        int x = 0;
        auto legend = [&](const QString& name, const QVector<ValuePoint>& s, const QColor& c) {
            if (s.isEmpty())
                return;
            const QString t = QStringLiteral("%1 %2").arg(name, fmt(s.last().value));
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRect(QRectF(x, 5, 10, 3));
            p.setPen(dim);
            p.drawText(QPointF(x + 14, 10), t);
            x += 14 + QFontMetrics(small).horizontalAdvance(t) + 16;
        };
        legend(name_, a_, ca);
        legend(second_name_, b_, cb);

        // Axis over both series, on the dates of the first.
        const QDate d0 = a_.first().date, d1 = a_.last().date;
        double lo = 1e300, hi = -1e300;
        for (const auto* s : {&a_, &b_})
            for (const auto& q : *s)
                if (q.date >= d0 && q.date <= d1) {
                    lo = std::min(lo, q.value);
                    hi = std::max(hi, q.value);
                }
        if (hi <= lo)
            hi = lo + 1;
        const double pad = (hi - lo) * 0.08;
        lo -= pad;
        hi += pad;
        const double top = 18, bottom = height() - 14, plot_w = std::max(1, width() - kLeft - 2);
        const double span_days = std::max<qint64>(1, d0.daysTo(d1));
        auto x_of = [&](const QDate& d) { return kLeft + plot_w * d0.daysTo(d) / span_days; };
        auto y_of = [&](double v) { return bottom - (v - lo) / (hi - lo) * (bottom - top); };

        for (int i = 0; i <= 2; ++i) {
            const double v = lo + (hi - lo) * i / 2.0;
            p.setPen(QPen(grid, 1));
            p.drawLine(QPointF(kLeft, y_of(v)), QPointF(kLeft + plot_w, y_of(v)));
            p.setPen(dim);
            p.drawText(QRectF(0, y_of(v) - 6, kLeft - 4, 12), Qt::AlignRight | Qt::AlignVCenter, fmt(v));
        }
        if (lo < 0 && hi > 0) {
            p.setPen(QPen(dim, 1, Qt::DashLine));
            p.drawLine(QPointF(kLeft, y_of(0)), QPointF(kLeft + plot_w, y_of(0)));
        }
        auto draw = [&](const QVector<ValuePoint>& s, const QColor& c, double w) {
            QPainterPath path;
            bool started = false;
            for (const auto& q : s) {
                if (q.date < d0 || q.date > d1)
                    continue;
                const QPointF pt(x_of(q.date), y_of(q.value));
                if (!started) {
                    path.moveTo(pt);
                    started = true;
                } else {
                    path.lineTo(pt);
                }
            }
            p.setPen(QPen(c, w));
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        };
        draw(b_, cb, 1.2);
        draw(a_, ca, 1.6);
        p.setPen(dim);
        p.drawText(QRectF(kLeft, bottom + 2, 90, 12), Qt::AlignLeft | Qt::AlignVCenter,
                   d0.toString(QStringLiteral("d MMM yy")));
        p.drawText(QRectF(kLeft + plot_w - 90, bottom + 2, 90, 12), Qt::AlignRight | Qt::AlignVCenter,
                   d1.toString(QStringLiteral("d MMM yy")));
    }

  private:
    static constexpr int kLeft = 44;
    /// Digits by size: 7712.6, 17.31, 0.836.
    static QString fmt(double v) {
        const double a = std::abs(v);
        const int dp = a >= 1000 ? 0 : a >= 100 ? 1 : a >= 1 ? 2 : 3;
        return QString::number(v, 'f', dp);
    }

    QString name_, second_name_;
    QVector<ValuePoint> a_, b_;
};

} // namespace fincept::screens::widgets::charts
