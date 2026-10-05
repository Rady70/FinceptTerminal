#include "screens/economics/panels/FedWatchCharts.h"

#include "ui/theme/Theme.h"

#include <QFontMetricsF>
#include <QHash>
#include <QKeyEvent>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSet>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace fincept::screens {
using namespace fedwatch;

namespace {
constexpr double kStep = 0.25;

double round_rate(double value) {
    return std::round(value * 100.0) / 100.0;
}
QColor alpha(QColor color, double a) {
    color.setAlphaF(float(std::clamp(a, 0.0, 1.0)));
    return color;
}
void diamond(QPainter& p, const QPointF& c, double r, const QColor& fill, const QColor& outline) {
    QPainterPath path;
    path.moveTo(c.x(), c.y() - r);
    path.lineTo(c.x() + r, c.y());
    path.lineTo(c.x(), c.y() + r);
    path.lineTo(c.x() - r, c.y());
    path.closeSubpath();
    p.setPen(QPen(outline, 1.5));
    p.setBrush(fill);
    p.drawPath(path);
}
// Matrix columns: every 25 bp band between the lowest and highest band shown.
QVector<QPair<double, double>> band_columns(const QVector<const Meeting*>& meetings, Value target_low,
                                            Value target_high) {
    double low = 1e9, high = -1e9;
    for (const auto* m : meetings)
        for (const auto& b : m->fed.bands) {
            low = std::min(low, b.low);
            high = std::max(high, b.high);
        }
    if (target_low && target_high) {
        low = std::min(low, *target_low);
        high = std::max(high, *target_high);
    }
    QVector<QPair<double, double>> out;
    if (low > high)
        return out;
    for (double x = round_rate(low); x < high - 1e-9 && out.size() < 60; x = round_rate(x + kStep))
        out.push_back({x, round_rate(x + kStep)});
    return out;
}
const Band* find_band(const QVector<Band>& bands, double low) {
    for (const auto& b : bands)
        if (std::abs(b.low - low) < 1e-6)
            return &b;
    return nullptr;
}
QString source_state(const FedState& fed) {
    if (fed.state == QLatin1String("CURRENT"))
        return QObject::tr("current");
    if (fed.state == QLatin1String("STALE"))
        return QObject::tr("stale · %1").arg(age_text(fed.age_days));
    if (fed.state == QLatin1String("HISTORICAL"))
        return QObject::tr("historical");
    return QObject::tr("no data");
}
} // namespace

// ── Base ─────────────────────────────────────────────────────────────────────
FedWatchChart::FedWatchChart(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}
QString FedWatchChart::hit_text(const QPointF& position) const {
    for (const auto& hit : hits_)
        if (hit.first.contains(position))
            return hit.second;
    return {};
}
void FedWatchChart::mouseMoveEvent(QMouseEvent* event) {
    const QString text = hit_text(event->position());
    if (text.isEmpty())
        QToolTip::hideText();
    else
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    QWidget::mouseMoveEvent(event);
}
void FedWatchChart::leaveEvent(QEvent* event) {
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}
QFont FedWatchChart::chart_font(int pixel_size, bool bold) const {
    QFont f = font();
    f.setPixelSize(pixel_size);
    f.setBold(bold);
    return f;
}

// ── Matrix ───────────────────────────────────────────────────────────────────
FedWatchMatrix::FedWatchMatrix(QWidget* parent) : FedWatchChart(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(tr("Target-range probability by FOMC meeting"));
}
int FedWatchMatrix::header_height() const {
    return 46;
}
int FedWatchMatrix::row_height() const {
    return 30;
}
QSize FedWatchMatrix::sizeHint() const {
    return {1000, header_height() + qMax(1, int(rows_.size())) * row_height() + 4};
}
void FedWatchMatrix::set_workspace(const Workspace& workspace, const QString& selected) {
    workspace_ = workspace;
    selected_ = selected;
    const auto upcoming = workspace_.upcoming();
    columns_ = band_columns(upcoming, workspace_.target_lower, workspace_.target_upper);
    rows_.clear();
    QStringList description;
    for (const auto* m : upcoming) {
        rows_ << m->id;
        QStringList cells;
        for (const auto& b : m->fed.bands)
            cells << band_label(b.low, b.high) + " " + pct(b.pct);
        description << meeting_label(m->date) + ": " +
                           (cells.isEmpty() ? tr("no Fed-side distribution") : cells.join(", ")) + "; " +
                           tr("expected %1; %2").arg(rate(m->fed.expected), source_state(m->fed));
    }
    setAccessibleDescription(description.join(" | "));
    setMinimumWidth(170 + 5 * 78 + int(columns_.size()) * 50);
    setFixedHeight(header_height() + qMax(1, int(rows_.size())) * row_height() + 4);
    updateGeometry();
    update();
}
QRectF FedWatchMatrix::row_rect(const QString& meeting) const {
    const int index = rows_.indexOf(meeting);
    return index >= 0 && index < row_rects_.size() ? row_rects_[index] : QRectF{};
}
void FedWatchMatrix::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    row_rects_.clear();
    const auto upcoming = workspace_.upcoming();
    if (upcoming.isEmpty() || columns_.isEmpty()) {
        p.setPen(QColor(TEXT_SECONDARY()));
        p.setFont(chart_font(13));
        p.drawText(rect(), Qt::AlignCenter,
                   tr("No stored meeting expectations yet. Press REFRESH to collect current observations."));
        return;
    }
    const double label_w = 170;
    const QStringList meta{tr("Expected"), tr("vs now"), tr("1D"), tr("1W"), tr("Data")};
    const QVector<double> meta_w{84, 76, 66, 66, 118};
    double meta_total = 0;
    for (double w : meta_w)
        meta_total += w;
    const double band_w = std::max(50.0, (width() - label_w - meta_total - 8) / double(columns_.size()));
    const double top = header_height();
    const double rh = row_height();
    const auto target_low = workspace_.target_lower;
    const auto mid = workspace_.target_mid();

    // Header.
    p.setFont(chart_font(11, true));
    p.setPen(QColor(TEXT_SECONDARY()));
    p.drawText(QRectF(8, 4, label_w - 8, top - 8), Qt::AlignLeft | Qt::AlignBottom, tr("FOMC MEETING"));
    for (int c = 0; c < columns_.size(); ++c) {
        const QRectF cell(label_w + c * band_w, 2, band_w, top - 4);
        const bool current = target_low && std::abs(*target_low - columns_[c].first) < 1e-6;
        if (current) {
            p.fillRect(QRectF(cell.left() + 1, cell.bottom() - 3, cell.width() - 2, 3), QColor(AMBER()));
            p.setPen(QColor(AMBER()));
            p.setFont(chart_font(9, true));
            p.drawText(QRectF(cell.left(), 0, cell.width(), 14), Qt::AlignHCenter | Qt::AlignTop, tr("CURRENT"));
        }
        p.setPen(QColor(current ? TEXT_PRIMARY() : TEXT_SECONDARY()));
        p.setFont(chart_font(11, current));
        const QString low = QString::number(columns_[c].first, 'f', 2);
        const QString high = QString::number(columns_[c].second, 'f', 2);
        const QString dash = QString(QChar(0x2013));
        const QString one_line = low + dash + high;
        if (QFontMetricsF(p.font()).horizontalAdvance(one_line) + 8 <= band_w)
            p.drawText(cell.adjusted(0, 0, 0, -6), Qt::AlignHCenter | Qt::AlignBottom, one_line);
        else // Narrow columns: lower bound over upper bound.
            p.drawText(cell.adjusted(0, 0, 0, -4), Qt::AlignHCenter | Qt::AlignBottom, low + dash + QChar('\n') + high);
    }
    double x = label_w + columns_.size() * band_w + 8;
    p.setFont(chart_font(11, true));
    p.setPen(QColor(TEXT_SECONDARY()));
    for (int i = 0; i < meta.size(); ++i) {
        p.drawText(QRectF(x, 4, meta_w[i] - 6, top - 10), Qt::AlignRight | Qt::AlignBottom, meta[i]);
        x += meta_w[i];
    }
    p.setPen(QColor(BORDER_MED()));
    p.drawLine(QPointF(0, top - 1), QPointF(width(), top - 1));

    for (int r = 0; r < upcoming.size(); ++r) {
        const Meeting& m = *upcoming[r];
        const QRectF row(0, top + r * rh, width(), rh);
        row_rects_.push_back(row);
        const bool selected = m.id == selected_;
        if (selected) {
            p.fillRect(row, QColor(BG_HOVER()));
            p.fillRect(QRectF(0, row.top(), 3, rh), QColor(AMBER()));
        }
        p.setFont(chart_font(12, selected));
        p.setPen(QColor(TEXT_PRIMARY()));
        p.drawText(QRectF(10, row.top(), label_w - 60, rh), Qt::AlignVCenter | Qt::AlignLeft, meeting_label(m.date));
        p.setFont(chart_font(10));
        p.setPen(QColor(TEXT_SECONDARY()));
        p.drawText(QRectF(10, row.top(), label_w - 16, rh), Qt::AlignVCenter | Qt::AlignRight,
                   tr("%1 d").arg(m.days_until));
        const Band* likely = nullptr;
        for (const auto& b : m.fed.bands)
            if (!likely || b.pct > likely->pct)
                likely = &b;
        for (int c = 0; c < columns_.size(); ++c) {
            const QRectF cell(label_w + c * band_w + 1, row.top() + 1, band_w - 2, rh - 2);
            const Band* band = find_band(m.fed.bands, columns_[c].first);
            if (!band)
                continue;
            const QColor fill = heat(band->pct);
            p.fillRect(cell, fill);
            if (band == likely) {
                p.setPen(QPen(QColor(TEXT_PRIMARY()), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRect(cell.adjusted(0.75, 0.75, -0.75, -0.75));
            }
            p.setPen(ink_on(fill));
            p.setFont(chart_font(11, band == likely));
            p.drawText(cell, Qt::AlignCenter, QString::number(band->pct, 'f', 1));
            QString tip = tr("%1 FOMC · target %2\nFed-side probability %3")
                              .arg(meeting_label(m.date), band_label(band->low, band->high), pct(band->pct, 2));
            if (band->raw)
                tip += tr(" (displayed %1)").arg(pct(band->raw));
            if (band->previous_day || band->previous_week)
                tip += tr("\nInvesting previous day %1 · previous week %2")
                           .arg(pct(band->previous_day), pct(band->previous_week));
            tip += tr("\n%1 · %2")
                       .arg(source_state(m.fed),
                            utc(m.fed.source_updated_at.isValid() ? m.fed.source_updated_at : m.fed.observed_at));
            add_hit(cell, tip);
        }
        x = label_w + columns_.size() * band_w + 8;
        p.setFont(chart_font(12));
        const auto change = [](Value a, Value b) -> Value { return a && b ? Value(*a - *b) : std::nullopt; };
        const QStringList values{rate(m.fed.expected), bp_change(change(m.fed.expected, mid)),
                                 bp_change(change(m.fed.expected, m.fed.expected_previous_day)),
                                 bp_change(change(m.fed.expected, m.fed.expected_previous_week)), source_state(m.fed)};
        for (int i = 0; i < values.size(); ++i) {
            const bool state_col = i == values.size() - 1;
            p.setPen(QColor(state_col && m.fed.state != QLatin1String("CURRENT") ? WARNING()
                            : state_col                                          ? TEXT_SECONDARY()
                                                                                 : TEXT_PRIMARY()));
            p.setFont(chart_font(state_col ? 10 : 12, i == 0));
            p.drawText(QRectF(x, row.top(), meta_w[i] - 6, rh), Qt::AlignVCenter | Qt::AlignRight, values[i]);
            x += meta_w[i];
        }
        add_hit(QRectF(label_w + columns_.size() * band_w, row.top(), meta_total + 8, rh),
                tr("%1 FOMC\nExpected policy rate after the meeting: %2 (target-range midpoints weighted by "
                   "Fed-side probability)\nvs current target midpoint: %3 · vs Investing previous day: %4 · vs "
                   "previous week: %5")
                    .arg(meeting_label(m.date), rate(m.fed.expected, 3), values[1], values[2], values[3]));
        p.setPen(QColor(BORDER_DIM()));
        p.drawLine(QPointF(0, row.bottom()), QPointF(width(), row.bottom()));
    }
}
void FedWatchMatrix::mousePressEvent(QMouseEvent* event) {
    for (int i = 0; i < row_rects_.size(); ++i)
        if (row_rects_[i].contains(event->position())) {
            selected_ = rows_.value(i);
            update();
            emit meeting_selected(selected_);
            return;
        }
    FedWatchChart::mousePressEvent(event);
}
void FedWatchMatrix::keyPressEvent(QKeyEvent* event) {
    if (event->key() != Qt::Key_Up && event->key() != Qt::Key_Down) {
        FedWatchChart::keyPressEvent(event);
        return;
    }
    int index = rows_.indexOf(selected_);
    index = std::clamp(index + (event->key() == Qt::Key_Down ? 1 : -1), 0, int(rows_.size()) - 1);
    if (index >= 0 && index < rows_.size()) {
        selected_ = rows_[index];
        update();
        emit meeting_selected(selected_);
    }
}

// ── Path chart ───────────────────────────────────────────────────────────────
FedWatchPathChart::FedWatchPathChart(QWidget* parent) : FedWatchChart(parent) {
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(260);
    setAccessibleName(tr("Market-implied policy rate path"));
}
void FedWatchPathChart::set_workspace(const Workspace& workspace, const QString& selected) {
    workspace_ = workspace;
    selected_ = selected;
    QStringList description;
    for (const auto* m : workspace_.upcoming())
        description << tr("%1: expected %2, one week earlier %3")
                           .arg(meeting_label(m->date), rate(m->fed.expected), rate(m->fed.expected_previous_week));
    setAccessibleDescription(description.join(" | "));
    update();
}
void FedWatchPathChart::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    columns_.clear();
    QVector<const Meeting*> meetings;
    for (const auto* m : workspace_.upcoming())
        if (!m->fed.bands.isEmpty())
            meetings.push_back(m);
    if (meetings.isEmpty()) {
        p.setPen(QColor(TEXT_SECONDARY()));
        p.drawText(rect(), Qt::AlignCenter, tr("No stored Fed-side distributions to plot"));
        return;
    }
    double low = 1e9, high = -1e9;
    for (const auto* m : meetings)
        for (const auto& b : m->fed.bands) {
            low = std::min(low, b.low);
            high = std::max(high, b.high);
        }
    if (workspace_.target_lower && workspace_.target_upper) {
        low = std::min(low, *workspace_.target_lower);
        high = std::max(high, *workspace_.target_upper);
    }
    low -= 0.0625;
    high += 0.0625;
    const QRectF plot(70, 34, std::max(10.0, width() - 86.0), std::max(10.0, height() - 34.0 - 44.0));
    const auto y_of = [&](double r) { return plot.bottom() - (r - low) / (high - low) * plot.height(); };

    // Legend.
    p.setFont(chart_font(11));
    double lx = plot.left();
    const auto legend = [&](const QString& text, auto draw_mark) {
        draw_mark(QPointF(lx + 7, 15));
        p.setPen(QColor(TEXT_SECONDARY()));
        const double w = QFontMetricsF(p.font()).horizontalAdvance(text);
        p.drawText(QRectF(lx + 18, 5, w + 4, 20), Qt::AlignVCenter | Qt::AlignLeft, text);
        lx += w + 40;
    };
    const QColor fed = source_color(0);
    legend(tr("Probability of target range (bubble area)"), [&](QPointF c) {
        p.setPen(QPen(fed, 1));
        p.setBrush(alpha(fed, 0.6));
        p.drawEllipse(c, 6, 6);
    });
    legend(tr("Expected rate now"), [&](QPointF c) { diamond(p, c, 6, QColor(TEXT_PRIMARY()), QColor(BG_BASE())); });
    legend(tr("One week earlier (Investing previous week)"),
           [&](QPointF c) { diamond(p, c, 6, Qt::transparent, QColor(TEXT_SECONDARY())); });
    if (workspace_.target_lower)
        legend(tr("Current target range"), [&](QPointF c) {
            p.setPen(Qt::NoPen);
            p.setBrush(alpha(QColor(TEXT_SECONDARY()), 0.18));
            p.drawRect(QRectF(c.x() - 7, c.y() - 5, 14, 10));
        });

    // Rate grid and current target band.
    p.setFont(chart_font(10));
    const double band_px = plot.height() / ((high - low) / kStep);
    const int label_every = band_px < 16 ? 2 : 1;
    int index = 0;
    for (double r = std::ceil(low / kStep) * kStep; r <= high + 1e-9; r += kStep, ++index) {
        const double y = y_of(r);
        p.setPen(QPen(QColor(BORDER_DIM()), 1));
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        if (index % label_every == 0) {
            p.setPen(QColor(TEXT_SECONDARY()));
            p.drawText(QRectF(0, y - 9, plot.left() - 8, 18), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(r, 'f', 2) + "%");
        }
    }
    if (workspace_.target_lower && workspace_.target_upper) {
        const QRectF band(plot.left(), y_of(*workspace_.target_upper), plot.width(),
                          y_of(*workspace_.target_lower) - y_of(*workspace_.target_upper));
        p.fillRect(band, alpha(QColor(TEXT_SECONDARY()), 0.14));
        add_hit(QRectF(0, band.top(), plot.left(), band.height()),
                tr("Current target range %1 (FRED, latest observation %2)")
                    .arg(band_label(*workspace_.target_lower, *workspace_.target_upper),
                         workspace_.target_date.toString(Qt::ISODate)));
    }

    const double cw = plot.width() / meetings.size();
    // Bubbles stay inside their own 25 bp row; markers sit to their right.
    const double max_r = std::max(3.0, std::min(cw * 0.30, band_px * 0.5));
    for (int i = 0; i < meetings.size(); ++i) {
        const Meeting& m = *meetings[i];
        const QRectF column(plot.left() + i * cw, plot.top(), cw, plot.height() + 40);
        columns_.push_back({column, m.id});
        const double cx = column.center().x() - std::min(12.0, cw * 0.12);
        const double marker_x = cx + max_r + 10;
        if (m.id == selected_)
            p.fillRect(QRectF(column.left() + 2, plot.top(), cw - 4, plot.height()), alpha(QColor(BG_HOVER()), 0.9));
        for (const auto& b : m.fed.bands) {
            if (b.pct <= 0)
                continue;
            const double radius = std::max(1.5, max_r * std::sqrt(b.pct / 100.0));
            const QPointF c(cx, y_of((b.low + b.high) / 2));
            p.setPen(QPen(fed, 1));
            p.setBrush(alpha(fed, 0.25 + 0.6 * b.pct / 100.0));
            p.drawEllipse(c, radius, radius);
            if (radius >= 11 && b.pct >= 15) {
                const QColor fill = alpha(fed, 0.25 + 0.6 * b.pct / 100.0);
                p.setPen(ink_on(mix(QColor(BG_SURFACE()), fill, fill.alphaF())));
                p.setFont(chart_font(10, true));
                p.drawText(QRectF(c.x() - radius, c.y() - 8, 2 * radius, 16), Qt::AlignCenter,
                           QString::number(b.pct, 'f', 0) + "%");
            }
            add_hit(QRectF(c.x() - std::max(radius, 6.0), c.y() - std::max(radius, 6.0), 2 * std::max(radius, 6.0),
                           2 * std::max(radius, 6.0)),
                    tr("%1 FOMC · %2: %3").arg(meeting_label(m.date), band_label(b.low, b.high), pct(b.pct, 2)));
        }
        if (m.fed.expected_previous_week)
            diamond(p, QPointF(marker_x + 12, y_of(*m.fed.expected_previous_week)), 5.5, Qt::transparent,
                    QColor(TEXT_SECONDARY()));
        if (m.fed.expected) {
            const QPointF c(marker_x, y_of(*m.fed.expected));
            diamond(p, c, 6, QColor(TEXT_PRIMARY()), QColor(BG_BASE()));
            add_hit(QRectF(c.x() - 9, c.y() - 9, 18, 18),
                    tr("%1 FOMC\nExpected rate %2 · one week earlier %3")
                        .arg(meeting_label(m.date), rate(m.fed.expected, 3), rate(m.fed.expected_previous_week, 3)));
        }
        p.setPen(QColor(m.id == selected_ ? TEXT_PRIMARY() : TEXT_SECONDARY()));
        p.setFont(chart_font(11, m.id == selected_));
        p.drawText(QRectF(column.left(), plot.bottom() + 6, cw, 18), Qt::AlignHCenter | Qt::AlignTop,
                   m.date.toString(QStringLiteral("MMM d")));
        p.setFont(chart_font(10));
        p.setPen(QColor(TEXT_TERTIARY()));
        p.drawText(QRectF(column.left(), plot.bottom() + 22, cw, 16), Qt::AlignHCenter | Qt::AlignTop,
                   m.date.toString(QStringLiteral("yyyy")) +
                       (m.fed.state == QLatin1String("CURRENT") ? QString{} : tr(" · stale")));
    }
}
void FedWatchPathChart::mousePressEvent(QMouseEvent* event) {
    for (const auto& column : columns_)
        if (column.first.contains(event->position())) {
            selected_ = column.second;
            update();
            emit meeting_selected(selected_);
            return;
        }
    FedWatchChart::mousePressEvent(event);
}

// ── Target-range bars ────────────────────────────────────────────────────────
FedWatchBandBars::FedWatchBandBars(QWidget* parent) : FedWatchChart(parent) {
    setAccessibleName(tr("Target-range probabilities for the selected meeting"));
}
void FedWatchBandBars::set_meeting(const Meeting& meeting, Value target_lower) {
    meeting_ = meeting;
    target_lower_ = target_lower;
    QStringList description;
    for (const auto& b : meeting_.fed.bands)
        description << tr("%1 %2 (previous day %3, previous week %4)")
                           .arg(band_label(b.low, b.high), pct(b.pct), pct(b.previous_day), pct(b.previous_week));
    setAccessibleDescription(description.isEmpty() ? tr("No Fed-side distribution") : description.join("; "));
    setFixedHeight(40 + std::max(1, int(meeting_.fed.bands.size())) * 28 + 6);
    update();
}
void FedWatchBandBars::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    const auto& bands = meeting_.fed.bands;
    if (bands.isEmpty()) {
        p.setPen(QColor(TEXT_SECONDARY()));
        p.drawText(rect(), Qt::AlignCenter, tr("No Fed-side target-range distribution stored for this meeting"));
        return;
    }
    const QColor fed = source_color(0);
    // Legend.
    p.setFont(chart_font(11));
    p.fillRect(QRectF(10, 10, 14, 10), fed);
    p.setPen(QColor(TEXT_SECONDARY()));
    p.drawText(QRectF(30, 5, 120, 20), Qt::AlignVCenter, tr("Now"));
    p.setPen(QPen(QColor(TEXT_PRIMARY()), 2));
    p.drawLine(QPointF(90, 8), QPointF(90, 22));
    p.setPen(QColor(TEXT_SECONDARY()));
    p.drawText(QRectF(98, 5, 160, 20), Qt::AlignVCenter, tr("Previous day"));
    QPolygonF tri;
    tri << QPointF(222, 21) << QPointF(228, 11) << QPointF(234, 21);
    p.setPen(QPen(QColor(TEXT_SECONDARY()), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(tri);
    p.drawText(QRectF(240, 5, std::max(60.0, width() - 250.0), 20), Qt::AlignVCenter,
               tr("Previous week (as displayed by Investing.com)"));

    const double label_w = 150, value_w = 64, delta_w = 190;
    const QRectF track(label_w, 34, std::max(40.0, width() - label_w - value_w - delta_w - 16), bands.size() * 28.0);
    const auto x_of = [&](double v) { return track.left() + track.width() * std::clamp(v, 0.0, 100.0) / 100.0; };
    p.setFont(chart_font(10));
    for (int t = 0; t <= 100; t += 25) {
        p.setPen(QPen(QColor(BORDER_DIM()), 1));
        p.drawLine(QPointF(x_of(t), track.top()), QPointF(x_of(t), track.bottom()));
    }
    int row = 0;
    for (int i = bands.size() - 1; i >= 0; --i, ++row) {
        const Band& b = bands[i];
        const double y = track.top() + row * 28;
        const bool current = target_lower_ && std::abs(*target_lower_ - b.low) < 1e-6;
        p.setFont(chart_font(12, current));
        p.setPen(QColor(current ? AMBER() : TEXT_PRIMARY()));
        p.drawText(QRectF(8, y, label_w - 14, 28), Qt::AlignVCenter | Qt::AlignLeft,
                   band_label(b.low, b.high) + (current ? tr(" now") : QString{}));
        const double w = x_of(b.pct) - track.left();
        p.setPen(Qt::NoPen);
        p.setBrush(fed);
        p.drawRoundedRect(QRectF(track.left(), y + 7, std::max(w, 1.5), 14), 2, 2);
        if (b.previous_day) {
            p.setPen(QPen(QColor(TEXT_PRIMARY()), 2));
            p.drawLine(QPointF(x_of(*b.previous_day), y + 4), QPointF(x_of(*b.previous_day), y + 24));
        }
        if (b.previous_week) {
            QPolygonF mark;
            const double px = x_of(*b.previous_week);
            mark << QPointF(px - 5, y + 27) << QPointF(px, y + 19) << QPointF(px + 5, y + 27);
            p.setPen(QPen(QColor(TEXT_SECONDARY()), 1.5));
            p.setBrush(QColor(BG_SURFACE()));
            p.drawPolygon(mark);
        }
        p.setPen(QColor(TEXT_PRIMARY()));
        p.setFont(chart_font(12, true));
        p.drawText(QRectF(track.right() + 8, y, value_w, 28), Qt::AlignVCenter | Qt::AlignRight, pct(b.pct));
        QStringList deltas;
        if (b.raw && b.previous_day)
            deltas << tr("1D %1").arg(pp(*b.raw - *b.previous_day));
        if (b.raw && b.previous_week)
            deltas << tr("1W %1").arg(pp(*b.raw - *b.previous_week));
        p.setFont(chart_font(11));
        p.setPen(QColor(TEXT_SECONDARY()));
        p.drawText(QRectF(track.right() + value_w + 16, y, delta_w, 28), Qt::AlignVCenter | Qt::AlignLeft,
                   deltas.join("  "));
        add_hit(QRectF(0, y, width(), 28),
                tr("%1 at the %2 meeting\nNow %3 (displayed %4) · previous day %5 · previous week %6")
                    .arg(band_label(b.low, b.high), meeting_label(meeting_.date), pct(b.pct, 2), pct(b.raw),
                         pct(b.previous_day), pct(b.previous_week)));
    }
}

// ── Outcome bars ─────────────────────────────────────────────────────────────
FedWatchOutcomeBars::FedWatchOutcomeBars(QWidget* parent) : FedWatchChart(parent) {
    setAccessibleName(tr("Decision probabilities, Fed-side and Polymarket"));
}
void FedWatchOutcomeBars::set_meeting(const Meeting& meeting) {
    rows_.clear();
    QMap<QString, Row> rows;
    auto entry = [&](int bp, bool open) -> Row& {
        auto& row = rows[outcome_key(bp, open)];
        row.bp = bp;
        row.open = open;
        return row;
    };
    for (const auto& o : meeting.fed.local)
        entry(o.bp, o.open).fed = o.pct;
    for (const auto& o : meeting.poly.outcomes)
        entry(o.bp, o.open).poly = o.pct;
    for (const auto& c : meeting.comparison) {
        auto& row = entry(c.bp, c.open);
        if (c.fed)
            row.fed = c.fed;
        row.diff = c.diff;
    }
    for (auto row : rows) {
        // A stored local distribution is complete: an outcome it does not carry
        // is exactly 0% (tails sum their side), as in the comparison contract.
        if (!row.fed && !meeting.fed.local.isEmpty()) {
            double sum = 0;
            for (const auto& o : meeting.fed.local)
                if (row.open ? (row.bp < 0 ? o.bp <= row.bp : o.bp >= row.bp) : o.bp == row.bp)
                    sum += o.pct;
            row.fed = sum;
        }
        rows_.push_back(row);
    }
    std::sort(rows_.begin(), rows_.end(),
              [](const Row& a, const Row& b) { return a.bp != b.bp ? a.bp < b.bp : a.open < b.open; });
    fed_state_ = meeting.fed.local.isEmpty() ? tr("unavailable") : state_label(meeting.fed.state);
    poly_state_ = meeting.poly.outcomes.isEmpty()
                      ? (meeting.poly.mapping_status == QLatin1String("VALIDATED") ? tr("no prices stored")
                                                                                   : tr("no validated market"))
                      : state_label(meeting.poly.state);
    QStringList description;
    for (const auto& r : rows_)
        description << tr("%1: Fed-side %2, Polymarket %3, difference %4")
                           .arg(outcome_label(r.bp, r.open), pct(r.fed), pct(r.poly), pp(r.diff));
    setAccessibleDescription(description.join("; "));
    setFixedHeight(42 + std::max(1, int(rows_.size())) * 38 + 6);
    update();
}
void FedWatchOutcomeBars::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    const QColor fed = source_color(0), poly = source_color(1);
    p.setFont(chart_font(11));
    p.fillRect(QRectF(10, 10, 14, 10), fed);
    p.setPen(QColor(TEXT_SECONDARY()));
    const QString fed_text = tr("Fed-side local step (%1)").arg(fed_state_);
    p.drawText(QRectF(30, 5, 300, 20), Qt::AlignVCenter, fed_text);
    const double px = 40 + QFontMetricsF(p.font()).horizontalAdvance(fed_text);
    p.fillRect(QRectF(px, 10, 14, 10), poly);
    p.drawText(QRectF(px + 20, 5, 300, 20), Qt::AlignVCenter, tr("Polymarket (%1)").arg(poly_state_));
    if (rows_.isEmpty()) {
        p.drawText(rect().adjusted(0, 30, 0, 0), Qt::AlignCenter, tr("No decision probabilities stored"));
        return;
    }
    const double label_w = 110, value_w = 62, gap_w = 96;
    const QRectF track(label_w, 38, std::max(40.0, width() - label_w - value_w - gap_w - 16), rows_.size() * 38.0);
    const auto x_of = [&](double v) { return track.left() + track.width() * std::clamp(v, 0.0, 100.0) / 100.0; };
    for (int t = 0; t <= 100; t += 25) {
        p.setPen(QPen(QColor(BORDER_DIM()), 1));
        p.drawLine(QPointF(x_of(t), track.top()), QPointF(x_of(t), track.bottom()));
    }
    p.setFont(chart_font(10, true));
    p.setPen(QColor(TEXT_SECONDARY()));
    p.drawText(QRectF(track.right() + value_w + 12, 22, gap_w, 16), Qt::AlignRight, tr("POLY − FED"));
    for (int i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        const double y = track.top() + i * 38;
        p.setFont(chart_font(12, r.bp == 0 && !r.open));
        p.setPen(QColor(TEXT_PRIMARY()));
        p.drawText(QRectF(8, y, label_w - 12, 38), Qt::AlignVCenter | Qt::AlignLeft, outcome_label(r.bp, r.open));
        const auto bar = [&](Value v, double top, const QColor& color) {
            if (!v)
                return;
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawRoundedRect(QRectF(track.left(), top, std::max(x_of(*v) - track.left(), 1.5), 11), 2, 2);
            p.setPen(QColor(TEXT_PRIMARY()));
            p.setFont(chart_font(11));
            p.drawText(QRectF(track.right() + 6, top - 3, value_w, 17), Qt::AlignVCenter | Qt::AlignRight, pct(*v));
        };
        bar(r.fed, y + 6, fed);
        bar(r.poly, y + 20, poly);
        p.setPen(QColor(TEXT_PRIMARY()));
        p.setFont(chart_font(12, true));
        p.drawText(QRectF(track.right() + value_w + 12, y, gap_w, 38), Qt::AlignVCenter | Qt::AlignRight, pp(r.diff));
        add_hit(QRectF(0, y, width(), 38),
                tr("%1\nFed-side %2 · Polymarket %3 · Polymarket − Fed-side %4\nA gap can reflect market disagreement "
                   "and the binary Fed-side model shape; it is not a trading signal.")
                    .arg(outcome_label(r.bp, r.open), pct(r.fed, 2), pct(r.poly, 2), pp(r.diff, 2)));
    }
}

// ── Daily heat strip ─────────────────────────────────────────────────────────
FedWatchHeatStrip::FedWatchHeatStrip(int source, QWidget* parent) : FedWatchChart(parent), source_(source) {
    setAccessibleName(source == 0 ? tr("Fed-side probability of each target range by day")
                                  : tr("Polymarket probability of each decision by day"));
}
QRectF FedWatchHeatStrip::plot_rect() const {
    return {kLabel, 8, std::max(10.0, width() - kLabel - kLatest - 8), std::max(1, int(order_.size())) * kRow};
}
void FedWatchHeatStrip::set_data(const QVector<Day>& days, const QVector<QPair<QString, QString>>& order,
                                 const QDate& first, const QDate& last, const QString& empty_text) {
    days_ = days;
    order_ = order;
    first_ = first;
    last_ = last;
    empty_text_ = empty_text;
    QString description =
        days_.isEmpty() ? empty_text_
                        : tr("%1 observed days between %2 and %3")
                              .arg(days_.size())
                              .arg(days_.first().date.toString(Qt::ISODate), days_.last().date.toString(Qt::ISODate));
    if (!days_.isEmpty()) {
        QStringList latest;
        for (const auto& s : days_.last().segments)
            latest << s.label + " " + pct(s.value);
        description += tr("; latest day: ") + latest.join(", ");
    }
    setAccessibleDescription(description);
    // Rows + date axis + legend; an empty strip keeps one row for its message.
    setFixedHeight(int(8 + std::max(1, int(order_.size())) * kRow + 26 + 30));
    update();
}
QPointF FedWatchHeatStrip::cell_center(const QDate& date, const QString& key) const {
    const QRectF plot = plot_rect();
    const int slot_count = int(std::max<qint64>(1, first_.daysTo(last_) + 1));
    const double slot = plot.width() / slot_count;
    int row = -1;
    for (int i = 0; i < order_.size(); ++i)
        if (order_[i].first == key)
            row = int(order_.size()) - 1 - i;
    if (row < 0 || !date.isValid())
        return {};
    return {plot.left() + (first_.daysTo(date) + 0.5) * slot, plot.top() + (row + 0.5) * kRow};
}
void FedWatchHeatStrip::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    const QRectF plot = plot_rect();
    if (!first_.isValid() || !last_.isValid() || days_.isEmpty() || order_.isEmpty()) {
        p.setPen(QColor(TEXT_SECONDARY()));
        p.setFont(chart_font(12));
        p.drawText(QRectF(0, 0, width(), height() - 30), Qt::AlignCenter | Qt::TextWordWrap, empty_text_);
        return;
    }
    const int rows = order_.size();
    const auto row_top = [&](int index_in_order) { return plot.top() + (rows - 1 - index_in_order) * kRow; };
    // Row labels and the empty track every day sits on.
    for (int i = 0; i < rows; ++i) {
        const double y = row_top(i);
        p.fillRect(QRectF(plot.left(), y + 1, plot.width(), kRow - 2), QColor(BG_BASE()));
        p.setPen(QColor(TEXT_PRIMARY()));
        p.setFont(chart_font(12, order_[i].second == QLatin1String("Hold")));
        p.drawText(QRectF(8, y, kLabel - 16, kRow), Qt::AlignVCenter | Qt::AlignLeft, order_[i].second);
    }
    const int slot_count = int(std::max<qint64>(1, first_.daysTo(last_) + 1));
    const double slot = plot.width() / slot_count;
    QHash<QString, int> rank;
    for (int i = 0; i < order_.size(); ++i)
        rank[order_[i].first] = i;
    p.setFont(chart_font(10, true));
    for (const auto& day : days_) {
        const auto index = first_.daysTo(day.date);
        if (index < 0 || index >= slot_count)
            continue;
        // Whole-pixel cells with a one-pixel gap between days when space allows.
        const double x = std::floor(plot.left() + index * slot);
        const double right = std::floor(plot.left() + (index + 1) * slot);
        const double w = std::max(1.0, right - x - (slot >= 3 ? 1.0 : 0.0));
        QStringList tip{day.date.toString(QStringLiteral("ddd yyyy-MM-dd"))};
        QVector<Segment> ordered = day.segments;
        std::sort(ordered.begin(), ordered.end(),
                  [&](const Segment& a, const Segment& b) { return rank.value(a.key) > rank.value(b.key); });
        for (const auto& s : ordered) {
            if (!rank.contains(s.key))
                continue;
            const QRectF cell(x, row_top(rank.value(s.key)) + 1, w, kRow - 2);
            // An observed 0% stays visibly distinct from a day with no observation.
            const QColor fill = s.value > 0 ? heat(s.value, source_) : QColor(BORDER_MED());
            p.fillRect(cell, fill);
            if (w >= 38) {
                p.setPen(ink_on(fill));
                p.drawText(cell, Qt::AlignCenter, QString::number(s.value, 'f', 1));
            }
            tip << s.label + ": " + pct(s.value, 2);
        }
        if (!day.note.isEmpty())
            tip << day.note;
        add_hit(QRectF(x, plot.top(), std::max(slot, 3.0), plot.height()), tip.join("\n"));
    }
    // Latest observed value per row, beside the strip.
    p.setFont(chart_font(12, true));
    for (const auto& s : days_.last().segments)
        if (rank.contains(s.key)) {
            p.setPen(QColor(TEXT_PRIMARY()));
            p.drawText(QRectF(plot.right() + 8, row_top(rank.value(s.key)), kLatest - 8, kRow),
                       Qt::AlignVCenter | Qt::AlignRight, pct(s.value));
        }
    p.setFont(chart_font(10));
    p.setPen(QColor(TEXT_SECONDARY()));
    p.drawText(QRectF(plot.right() + 8, plot.top() - 8, kLatest - 8, 10), Qt::AlignRight | Qt::AlignBottom,
               days_.last().date.toString(QStringLiteral("MMM d")));
    // Date axis: ends plus month starts that fit.
    const double axis_y = plot.bottom() + 4;
    double last_right = -1e9;
    const auto label = [&](const QDate& d, const QString& text, Qt::Alignment align) {
        const double cx = plot.left() + (first_.daysTo(d) + 0.5) * slot;
        QRectF r(cx - 35, axis_y, 70, 16);
        if (align & Qt::AlignLeft)
            r.moveLeft(plot.left());
        if (align & Qt::AlignRight)
            r.moveRight(plot.right());
        if (r.left() < last_right + 6)
            return;
        p.drawText(r, align | Qt::AlignTop, text);
        last_right = r.right();
    };
    label(first_, first_.toString(QStringLiteral("MMM d")), Qt::AlignLeft);
    for (QDate d(first_.year(), first_.month(), 1); d <= last_; d = d.addMonths(1))
        if (d > first_.addDays(6) && d < last_.addDays(-6))
            label(d, d.toString(QStringLiteral("MMM")), Qt::AlignHCenter);
    label(last_, last_.toString(QStringLiteral("MMM d")), Qt::AlignRight);
    // Legend: brightness scale and the meaning of an empty cell.
    const double ly = axis_y + 22;
    p.drawText(QRectF(plot.left(), ly, 120, 16), Qt::AlignVCenter | Qt::AlignLeft, tr("Probability"));
    for (int step = 0; step <= 4; ++step) {
        const QRectF swatch(plot.left() + 90 + step * 26, ly + 2, 24, 12);
        p.fillRect(swatch, heat(step * 25.0, source_));
    }
    p.drawText(QRectF(plot.left() + 90 + 5 * 26 + 4, ly, 300, 16), Qt::AlignVCenter | Qt::AlignLeft,
               tr("0% → 100%    dark track = no observation that day"));
}

} // namespace fincept::screens
