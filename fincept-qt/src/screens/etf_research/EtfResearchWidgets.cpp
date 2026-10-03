#include "screens/etf_research/EtfResearchWidgets.h"

#include "screens/etf_research/EtfResearchFormat.h"

#include <QFontMetricsF>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace fincept::screens::etfr {

namespace {

QFont etfr_font(const QWidget* w, int px, bool bold = false) {
    QFont f = w->font();
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

constexpr int kTileW = 96;
constexpr int kTileH = 46;
constexpr int kGap = 2;
constexpr int kHeader = 16;

} // namespace

// ── Heatmap ──────────────────────────────────────────────────────────────────

HeatmapWidget::HeatmapWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void HeatmapWidget::set_tiles(const QVector<HeatTile>& tiles, double range, const QString& legend) {
    tiles_ = tiles;
    range_ = range > 0 ? range : 1.0;
    legend_ = legend;
    updateGeometry();
    update();
}

void HeatmapWidget::set_selected(const QString& key) {
    selected_ = key;
    update();
}

QVector<HeatmapWidget::Placed> HeatmapWidget::layout_for(int width, int* height,
                                                         QVector<QPair<QString, QRect>>* headers) const {
    QVector<Placed> out;
    const int usable = std::max(kTileW, width - 4);
    const int cols = std::max(1, (usable + kGap) / (kTileW + kGap));
    const int tw = (usable - (cols - 1) * kGap) / cols;
    int y = kHeader + 2; // legend line
    QStringList groups;
    for (const auto& t : tiles_)
        if (!groups.contains(t.group))
            groups.append(t.group);
    for (const QString& g : groups) {
        if (!g.isEmpty()) {
            if (headers)
                headers->append({g, QRect(2, y, usable, kHeader)});
            y += kHeader;
        }
        int c = 0;
        for (int i = 0; i < tiles_.size(); ++i) {
            if (tiles_[i].group != g)
                continue;
            out.append({i, QRect(2 + c * (tw + kGap), y, tw, kTileH)});
            if (++c == cols) {
                c = 0;
                y += kTileH + kGap;
            }
        }
        if (c != 0)
            y += kTileH + kGap;
        y += 3;
    }
    if (height)
        *height = y + 2;
    return out;
}

int HeatmapWidget::heightForWidth(int w) const {
    int h = 0;
    layout_for(w, &h, nullptr);
    return h;
}

QSize HeatmapWidget::sizeHint() const {
    return {6 * (kTileW + kGap), heightForWidth(6 * (kTileW + kGap))};
}

QSize HeatmapWidget::minimumSizeHint() const {
    return {kTileW + 8, kTileH + kHeader * 2};
}

void HeatmapWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), token(&ui::ThemeTokens::bg_base));
    int h = 0;
    QVector<QPair<QString, QRect>> headers;
    const auto placed = layout_for(width(), &h, &headers);
    p.setFont(etfr_font(this, 10));
    p.setPen(token(&ui::ThemeTokens::text_secondary));
    p.drawText(QRect(2, 0, width() - 4, kHeader), Qt::AlignLeft | Qt::AlignVCenter, legend_);
    for (const auto& hd : headers) {
        p.setFont(etfr_font(this, 10, true));
        p.setPen(token(&ui::ThemeTokens::accent));
        p.drawText(hd.second, Qt::AlignLeft | Qt::AlignVCenter, hd.first.toUpper());
    }
    for (const Placed& pl : placed) {
        const HeatTile& t = tiles_[pl.index];
        const QRect r = pl.rect;
        if (t.value) {
            p.fillRect(r, heat_color(*t.value, range_));
        } else {
            // No observation: a hatched neutral tile, never a zero colour.
            p.fillRect(r, token(&ui::ThemeTokens::bg_surface));
            p.save();
            p.setClipRect(r); // the hatch lines start left of the tile: keep them inside it
            p.setPen(QPen(token(&ui::ThemeTokens::border_dim), 1));
            for (int x = r.left() - r.height(); x < r.right(); x += 7)
                p.drawLine(QPoint(x, r.bottom()), QPoint(x + r.height(), r.top()));
            p.restore();
        }
        p.setPen(QPen(t.key == selected_ ? token(&ui::ThemeTokens::accent) : token(&ui::ThemeTokens::border_dim),
                      t.key == selected_ ? 2 : 1));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.setPen(token(&ui::ThemeTokens::text_primary));
        p.setFont(etfr_font(this, 11, true));
        p.drawText(r.adjusted(4, 2, -4, 0), Qt::AlignLeft | Qt::AlignTop, t.label);
        p.setFont(etfr_font(this, 11, true));
        p.drawText(r.adjusted(4, 0, -4, -2), Qt::AlignRight | Qt::AlignBottom, t.value_text);
        p.setFont(etfr_font(this, 9));
        p.setPen(token(&ui::ThemeTokens::text_secondary));
        p.drawText(r.adjusted(4, 15, -4, 0), Qt::AlignLeft | Qt::AlignTop,
                   QFontMetrics(etfr_font(this, 9)).elidedText(t.sub, Qt::ElideRight, r.width() - 8));
        if (!t.tag.isEmpty()) {
            // The tag (class·period) sits on a dark chip so it stays legible on any tile colour.
            const QFont tf = etfr_font(this, 8, true);
            p.setFont(tf);
            const QFontMetrics tm(tf);
            const QRect tb = tm.boundingRect(t.tag).adjusted(-2, 0, 2, 0);
            const QRect chip(r.right() - 3 - tb.width(), r.top() + 2, tb.width(), tm.height());
            QColor bg = token(&ui::ThemeTokens::bg_base);
            bg.setAlpha(200);
            p.fillRect(chip, bg);
            p.setPen(t.tag_color.isValid() ? t.tag_color : token(&ui::ThemeTokens::text_secondary));
            p.drawText(chip, Qt::AlignCenter, t.tag);
        }
        if (t.stale) {
            p.setFont(etfr_font(this, 8, true));
            p.setPen(token(&ui::ThemeTokens::warning));
            p.drawText(r.adjusted(4, 0, 0, -2), Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("STALE"));
        }
    }
}

void HeatmapWidget::mousePressEvent(QMouseEvent* e) {
    int h = 0;
    for (const Placed& pl : layout_for(width(), &h, nullptr))
        if (pl.rect.contains(e->pos())) {
            selected_ = tiles_[pl.index].key;
            update();
            emit tile_activated(selected_);
            return;
        }
    QWidget::mousePressEvent(e);
}

bool HeatmapWidget::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        int h = 0;
        for (const Placed& pl : layout_for(width(), &h, nullptr))
            if (pl.rect.contains(he->pos())) {
                QToolTip::showText(he->globalPos(), tiles_[pl.index].tooltip, this);
                return true;
            }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

// ── RRG map ──────────────────────────────────────────────────────────────────

RrgWidget::RrgWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void RrgWidget::set_series(const QVector<RrgSeries>& series, const QString& title) {
    series_ = series;
    title_ = title;
    double xm = 0.6, ym = 0.6;
    for (const auto& s : series_)
        for (const auto& p : s.points) {
            xm = std::max(xm, std::abs(p.ratio - 100.0));
            ym = std::max(ym, std::abs(p.mom - 100.0));
        }
    xm *= 1.12;
    ym *= 1.12;
    xmin_ = 100 - xm;
    xmax_ = 100 + xm;
    ymin_ = 100 - ym;
    ymax_ = 100 + ym;
    update();
}

void RrgWidget::set_selected(const QString& key) {
    selected_ = key;
    update();
}

QRectF RrgWidget::plot_rect() const {
    return QRectF(42, 22, std::max(10, width() - 54), std::max(10, height() - 46));
}

QPointF RrgWidget::map(double ratio, double mom) const {
    const QRectF r = plot_rect();
    return {r.left() + (ratio - xmin_) / (xmax_ - xmin_) * r.width(),
            r.bottom() - (mom - ymin_) / (ymax_ - ymin_) * r.height()};
}

void RrgWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), token(&ui::ThemeTokens::bg_base));
    const QRectF r = plot_rect();
    const QPointF c = map(100, 100);
    auto quad = [&](const QRectF& q, const QColor& col, const QString& label, Qt::Alignment a) {
        QColor f = col;
        f.setAlphaF(0.07f);
        p.fillRect(q, f);
        p.setPen(col);
        p.setFont(etfr_font(this, 11, true));
        p.drawText(q.adjusted(6, 4, -6, -4), static_cast<int>(a), label);
    };
    quad(QRectF(QPointF(c.x(), r.top()), QPointF(r.right(), c.y())), quadrant_color(QStringLiteral("Leading")),
         tr_("LEADING"), Qt::AlignRight | Qt::AlignTop);
    quad(QRectF(QPointF(r.left(), r.top()), QPointF(c.x(), c.y())), quadrant_color(QStringLiteral("Improving")),
         tr_("IMPROVING"), Qt::AlignLeft | Qt::AlignTop);
    quad(QRectF(QPointF(c.x(), c.y()), QPointF(r.right(), r.bottom())), quadrant_color(QStringLiteral("Weakening")),
         tr_("WEAKENING"), Qt::AlignRight | Qt::AlignBottom);
    quad(QRectF(QPointF(r.left(), c.y()), QPointF(c.x(), r.bottom())), quadrant_color(QStringLiteral("Lagging")),
         tr_("LAGGING"), Qt::AlignLeft | Qt::AlignBottom);
    p.setPen(QPen(token(&ui::ThemeTokens::border_med), 1));
    p.drawRect(r);
    p.drawLine(QPointF(r.left(), c.y()), QPointF(r.right(), c.y()));
    p.drawLine(QPointF(c.x(), r.top()), QPointF(c.x(), r.bottom()));
    p.setFont(etfr_font(this, 9));
    p.setPen(token(&ui::ThemeTokens::text_secondary));
    for (int k = 0; k <= 4; ++k) {
        const double xv = xmin_ + (xmax_ - xmin_) * k / 4.0;
        const double yv = ymin_ + (ymax_ - ymin_) * k / 4.0;
        const QPointF px = map(xv, ymin_);
        const QPointF py = map(xmin_, yv);
        p.drawText(QRectF(px.x() - 22, r.bottom() + 2, 44, 12), Qt::AlignCenter, QString::number(xv, 'f', 1));
        p.drawText(QRectF(0, py.y() - 6, 38, 12), Qt::AlignRight | Qt::AlignVCenter, QString::number(yv, 'f', 1));
    }
    p.drawText(QRectF(r.left(), height() - 12, r.width(), 12), Qt::AlignCenter, tr_("RS-Ratio  →"));
    p.save();
    p.translate(8, r.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-50, -6, 100, 12), Qt::AlignCenter, tr_("RS-Momentum  →"));
    p.restore();
    p.setFont(etfr_font(this, 10, true));
    p.setPen(token(&ui::ThemeTokens::text_primary));
    p.drawText(QRectF(r.left(), 2, r.width(), 18), Qt::AlignLeft | Qt::AlignVCenter, title_);
    // Tails fade from the oldest week (thin, faint) to the latest (full), the
    // last segment ends in an arrowhead, and the hovered or selected series is
    // drawn on top while the others are dimmed.
    const QString focus = !hovered_.isEmpty() ? hovered_ : selected_;
    QVector<const RrgSeries*> order;
    for (const auto& s : series_)
        if (!s.points.isEmpty() && s.key != focus)
            order.append(&s);
    for (const auto& s : series_)
        if (!s.points.isEmpty() && s.key == focus)
            order.append(&s);
    QVector<QRectF> labels;
    const QFontMetricsF fm(etfr_font(this, 10, true));
    for (const RrgSeries* sp : order) {
        const RrgSeries& s = *sp;
        const bool hot = s.key == focus;
        const double dim = focus.isEmpty() || hot ? 1.0 : 0.22;
        const int n = s.points.size();
        for (int i = 1; i < n; ++i) {
            const double age = n > 1 ? static_cast<double>(i) / (n - 1) : 1.0; // 0 oldest .. 1 latest
            QColor col = s.color;
            col.setAlphaF(static_cast<float>(dim * (0.18 + 0.82 * age)));
            p.setPen(QPen(col, (hot ? 1.4 : 0.7) + (hot ? 1.4 : 1.1) * age, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(map(s.points[i - 1].ratio, s.points[i - 1].mom), map(s.points[i].ratio, s.points[i].mom));
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            if (i < n - 1)
                p.drawEllipse(map(s.points[i].ratio, s.points[i].mom), 1.6, 1.6);
        }
        QColor head = s.color;
        head.setAlphaF(static_cast<float>(dim));
        const QPointF last = map(s.points.last().ratio, s.points.last().mom);
        if (n > 1) {
            const QPointF prev = map(s.points[n - 2].ratio, s.points[n - 2].mom);
            const double ang = std::atan2(last.y() - prev.y(), last.x() - prev.x());
            const double len = hot ? 9.0 : 7.0;
            QPolygonF arrow;
            arrow << last << last - QPointF(len * std::cos(ang - 0.45), len * std::sin(ang - 0.45))
                  << last - QPointF(len * std::cos(ang + 0.45), len * std::sin(ang + 0.45));
            p.setPen(Qt::NoPen);
            p.setBrush(head);
            p.drawPolygon(arrow);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(head);
            p.drawEllipse(last, 4.0, 4.0);
        }
        // Label beside the head, nudged down until it no longer overlaps one already placed.
        QRectF box(last + QPointF(7, -fm.height() + 2), QSizeF(fm.horizontalAdvance(s.label) + 2, fm.height()));
        for (int tries = 0; tries < 6; ++tries) {
            bool clash = false;
            for (const QRectF& b : labels)
                clash = clash || b.intersects(box);
            if (!clash)
                break;
            box.translate(0, fm.height() * 0.9);
        }
        labels.append(box);
        p.setFont(etfr_font(this, 10, true));
        p.setPen(head);
        p.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, s.label);
    }
}

void RrgWidget::mouseMoveEvent(QMouseEvent* e) {
    const QString k = hit(e->pos());
    if (k != hovered_) {
        hovered_ = k;
        update();
    }
    QWidget::mouseMoveEvent(e);
}

void RrgWidget::leaveEvent(QEvent* e) {
    if (!hovered_.isEmpty()) {
        hovered_.clear();
        update();
    }
    QWidget::leaveEvent(e);
}

QString RrgWidget::hit(const QPoint& pos) const {
    double best = 14.0 * 14.0;
    QString key;
    for (const auto& s : series_) {
        if (s.points.isEmpty())
            continue;
        const QPointF d = map(s.points.last().ratio, s.points.last().mom) - QPointF(pos);
        const double dist = d.x() * d.x() + d.y() * d.y();
        if (dist < best) {
            best = dist;
            key = s.key;
        }
    }
    return key;
}

void RrgWidget::mousePressEvent(QMouseEvent* e) {
    const QString k = hit(e->pos());
    if (!k.isEmpty()) {
        selected_ = k;
        update();
        emit series_activated(k);
    }
}

bool RrgWidget::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip) {
        auto* he = static_cast<QHelpEvent*>(e);
        const QString k = hit(he->pos());
        for (const auto& s : series_)
            if (s.key == k && !s.points.isEmpty()) {
                const auto& lp = s.points.last();
                QToolTip::showText(he->globalPos(),
                                   tr_("%1  RS-Ratio %2  RS-Mom %3  week of %4  (MODEL, JdK-style approximation)")
                                       .arg(s.label)
                                       .arg(lp.ratio, 0, 'f', 2)
                                       .arg(lp.mom, 0, 'f', 2)
                                       .arg(lp.date.toString(Qt::ISODate)),
                                   this);
                return true;
            }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

// ── Matrix ───────────────────────────────────────────────────────────────────

MatrixWidget::MatrixWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void MatrixWidget::set_matrix(const QStringList& labels, const QVector<QVector<double>>& values, const QString& mode) {
    labels_ = labels;
    values_ = values;
    mode_ = mode;
    update();
}

void MatrixWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), token(&ui::ThemeTokens::bg_base));
    const int n = labels_.size();
    if (n == 0 || values_.size() != n) {
        p.setPen(token(&ui::ThemeTokens::text_tertiary));
        p.drawText(rect(), Qt::AlignCenter, tr_("No correlation matrix: insufficient common history"));
        return;
    }
    const int left = 44, top = 18;
    const double cell = std::max(10.0, std::min((width() - left - 4.0) / n, (height() - top - 4.0) / n));
    p.setFont(etfr_font(this, std::clamp(static_cast<int>(cell / 3.2), 8, 11)));
    for (int i = 0; i < n; ++i) {
        p.setPen(token(&ui::ThemeTokens::text_secondary));
        p.drawText(QRectF(0, top + i * cell, left - 4, cell), Qt::AlignRight | Qt::AlignVCenter, labels_[i]);
        p.drawText(QRectF(left + i * cell, 0, cell, top), Qt::AlignCenter, labels_[i]);
        for (int j = 0; j < n; ++j) {
            const double v = values_[i][j];
            const QRectF r(left + j * cell, top + i * cell, cell - 1, cell - 1);
            QColor col;
            if (mode_ == QLatin1String("geom"))
                col = heat_color(v - 0.5, 0.5); // 0 = co-moving, 1 = orthogonal
            else
                col = heat_color(v, 1.0);
            p.fillRect(r, col);
            if (cell >= 22) {
                p.setPen(token(&ui::ThemeTokens::text_primary));
                p.drawText(r, Qt::AlignCenter, QString::number(v, 'f', 2));
            }
        }
    }
}

bool MatrixWidget::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip && !labels_.isEmpty() && values_.size() == labels_.size()) {
        auto* he = static_cast<QHelpEvent*>(e);
        const int n = labels_.size();
        const int left = 44, top = 18;
        const double cell = std::max(10.0, std::min((width() - left - 4.0) / n, (height() - top - 4.0) / n));
        const int j = static_cast<int>((he->pos().x() - left) / cell);
        const int i = static_cast<int>((he->pos().y() - top) / cell);
        if (i >= 0 && j >= 0 && i < n && j < n) {
            QToolTip::showText(he->globalPos(),
                               QStringLiteral("%1 × %2: %3 (%4)")
                                   .arg(labels_[i], labels_[j])
                                   .arg(values_[i][j], 0, 'f', 3)
                                   .arg(mode_ == QLatin1String("geom") ? tr_("|sin θ| = √(1−ρ²), 1 = orthogonal")
                                                                       : tr_("Pearson ρ of daily total returns")),
                               this);
            return true;
        }
    }
    return QWidget::event(e);
}

// ── PCA vectors ──────────────────────────────────────────────────────────────

PcaWidget::PcaWidget(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PcaWidget::set_vectors(const QStringList& labels, const QVector<QPair<double, double>>& xy, double var1,
                            double var2) {
    labels_ = labels;
    xy_ = xy;
    var1_ = var1;
    var2_ = var2;
    update();
}

void PcaWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), token(&ui::ThemeTokens::bg_base));
    const double rad = std::max(20.0, std::min(width(), height() - 18) / 2.0 - 26);
    const QPointF c(width() / 2.0, (height() + 14) / 2.0);
    p.setPen(QPen(token(&ui::ThemeTokens::border_dim), 1));
    p.drawEllipse(c, rad, rad);
    p.drawLine(QPointF(c.x() - rad, c.y()), QPointF(c.x() + rad, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - rad), QPointF(c.x(), c.y() + rad));
    p.setFont(etfr_font(this, 10));
    p.setPen(token(&ui::ThemeTokens::text_secondary));
    p.drawText(QRectF(4, 0, width() - 8, 14), Qt::AlignLeft,
               tr_("PCA sector vectors  PC1 %1%  PC2 %2%  (close arrows co-move)")
                   .arg(var1_ * 100, 0, 'f', 0)
                   .arg(var2_ * 100, 0, 'f', 0));
    for (int i = 0; i < xy_.size(); ++i) {
        const QPointF tip(c.x() + xy_[i].first * rad, c.y() - xy_[i].second * rad);
        QColor col = QColor::fromHsv((i * 37) % 360, 160, 230);
        p.setPen(QPen(col, 1.6));
        p.drawLine(c, tip);
        p.setBrush(col);
        p.drawEllipse(tip, 2.5, 2.5);
        p.drawText(tip + QPointF(4, -2), labels_.value(i));
    }
}

// ── Line chart ───────────────────────────────────────────────────────────────

LineChartWidget::LineChartWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void LineChartWidget::set_series(const QVector<LineSeries>& series, const QString& title, const QString& units) {
    series_ = series;
    title_ = title;
    units_ = units;
    update();
}

void LineChartWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), token(&ui::ThemeTokens::bg_base));
    p.setFont(etfr_font(this, 10, true));
    p.setPen(token(&ui::ThemeTokens::text_primary));
    p.drawText(QRect(4, 0, width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter, title_);
    double lo = 1e300, hi = -1e300;
    QDate d0, d1;
    for (const auto& s : series_)
        for (int i = 0; i < s.values.size(); ++i) {
            lo = std::min(lo, s.values[i]);
            hi = std::max(hi, s.values[i]);
            if (!d0.isValid() || s.dates[i] < d0)
                d0 = s.dates[i];
            if (!d1.isValid() || s.dates[i] > d1)
                d1 = s.dates[i];
        }
    const QRectF r(48, 20, std::max(10, width() - 56), std::max(10, height() - 38));
    p.setPen(QPen(token(&ui::ThemeTokens::border_dim), 1));
    p.drawRect(r);
    if (!d0.isValid() || d0 == d1) {
        p.setPen(token(&ui::ThemeTokens::text_tertiary));
        p.drawText(r, Qt::AlignCenter, tr_("No stored history for this series"));
        return;
    }
    if (hi - lo < 1e-12) {
        hi += 1;
        lo -= 1;
    }
    const double pad = (hi - lo) * 0.06;
    lo -= pad;
    hi += pad;
    const double span = static_cast<double>(d0.daysTo(d1));
    auto map = [&](const QDate& d, double v) {
        return QPointF(r.left() + d0.daysTo(d) / span * r.width(), r.bottom() - (v - lo) / (hi - lo) * r.height());
    };
    p.setFont(etfr_font(this, 9));
    p.setPen(token(&ui::ThemeTokens::text_secondary));
    for (int k = 0; k <= 3; ++k) {
        const double v = lo + (hi - lo) * k / 3.0;
        p.drawText(QRectF(0, map(d0, v).y() - 6, 44, 12), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(v, 'f', 1));
    }
    p.drawText(QRectF(r.left(), r.bottom() + 2, 80, 12), Qt::AlignLeft, d0.toString(QStringLiteral("yyyy-MM-dd")));
    p.drawText(QRectF(r.right() - 80, r.bottom() + 2, 80, 12), Qt::AlignRight,
               d1.toString(QStringLiteral("yyyy-MM-dd")));
    int legend_x = static_cast<int>(r.right()) - 4;
    for (int si = series_.size() - 1; si >= 0; --si) {
        const auto& s = series_[si];
        QPainterPath path;
        for (int i = 0; i < s.values.size(); ++i) {
            const QPointF pt = map(s.dates[i], s.values[i]);
            if (i == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        QPen pen(s.color, 1.4);
        if (s.dashed)
            pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        const int w = QFontMetrics(etfr_font(this, 9)).horizontalAdvance(s.label) + 8;
        legend_x -= w;
        p.setPen(s.color);
        p.drawText(QRect(legend_x, 2, w, 14), Qt::AlignRight | Qt::AlignVCenter, s.label);
    }
    if (hover_x_ >= r.left() && hover_x_ <= r.right()) {
        const QDate hd = d0.addDays(static_cast<qint64>((hover_x_ - r.left()) / r.width() * span));
        p.setPen(QPen(token(&ui::ThemeTokens::border_bright), 1, Qt::DotLine));
        p.drawLine(QPointF(hover_x_, r.top()), QPointF(hover_x_, r.bottom()));
        QStringList parts{hd.toString(Qt::ISODate)};
        for (const auto& s : series_) {
            for (int i = s.dates.size() - 1; i >= 0; --i)
                if (s.dates[i] <= hd) {
                    parts << QStringLiteral("%1 %2%3").arg(s.label).arg(s.values[i], 0, 'f', 2).arg(units_);
                    break;
                }
        }
        p.setPen(token(&ui::ThemeTokens::text_primary));
        p.drawText(QRectF(r.left() + 4, r.top() + 2, r.width() - 8, 14), Qt::AlignLeft,
                   parts.join(QStringLiteral("  ")));
    }
}

void LineChartWidget::mouseMoveEvent(QMouseEvent* e) {
    hover_x_ = e->pos().x();
    update();
}

void LineChartWidget::leaveEvent(QEvent*) {
    hover_x_ = -1;
    update();
}

// ── Bars ─────────────────────────────────────────────────────────────────────

BarChartWidget::BarChartWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void BarChartWidget::set_bars(const QVector<Bar>& bars, const QString& title) {
    bars_ = bars;
    title_ = title;
    update();
}

void BarChartWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), token(&ui::ThemeTokens::bg_base));
    p.setFont(etfr_font(this, 10, true));
    p.setPen(token(&ui::ThemeTokens::text_primary));
    p.drawText(QRect(4, 0, width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter, title_);
    if (bars_.isEmpty()) {
        p.setPen(token(&ui::ThemeTokens::text_tertiary));
        p.drawText(rect(), Qt::AlignCenter, tr_("No observations"));
        return;
    }
    double mx = 0;
    for (const auto& b : bars_)
        if (b.value)
            mx = std::max(mx, std::abs(*b.value));
    if (mx <= 0)
        mx = 1;
    const QRectF r(4, 30, width() - 8, height() - 46);
    const double bw = r.width() / bars_.size();
    const double zero = r.top() + r.height() / 2;
    p.setPen(QPen(token(&ui::ThemeTokens::border_med), 1));
    p.drawLine(QPointF(r.left(), zero), QPointF(r.right(), zero));
    p.setFont(etfr_font(this, 9));
    for (int i = 0; i < bars_.size(); ++i) {
        const Bar& b = bars_[i];
        const double x = r.left() + i * bw;
        if (b.value) {
            const double h = std::abs(*b.value) / mx * (r.height() / 2 - 12);
            const QColor col = b.color.isValid() ? b.color
                                                 : (*b.value >= 0 ? token(&ui::ThemeTokens::positive)
                                                                  : token(&ui::ThemeTokens::negative));
            const QRectF br =
                *b.value >= 0 ? QRectF(x + bw * 0.15, zero - h, bw * 0.7, h) : QRectF(x + bw * 0.15, zero, bw * 0.7, h);
            p.fillRect(br, col);
            p.setPen(token(&ui::ThemeTokens::text_primary));
            p.drawText(QRectF(x, *b.value >= 0 ? br.top() - 12 : br.bottom(), bw, 12), Qt::AlignCenter, b.value_text);
        } else {
            p.setPen(token(&ui::ThemeTokens::text_tertiary));
            p.drawText(QRectF(x, zero - 12, bw, 12), Qt::AlignCenter, na());
        }
        p.setPen(token(&ui::ThemeTokens::text_secondary));
        p.drawText(QRectF(x, r.bottom() + 2, bw, 12), Qt::AlignCenter, b.label);
    }
}

bool BarChartWidget::event(QEvent* e) {
    if (e->type() == QEvent::ToolTip && !bars_.isEmpty()) {
        auto* he = static_cast<QHelpEvent*>(e);
        const double bw = (width() - 8.0) / bars_.size();
        const int i = static_cast<int>((he->pos().x() - 4) / bw);
        if (i >= 0 && i < bars_.size() && !bars_[i].tooltip.isEmpty()) {
            QToolTip::showText(he->globalPos(), bars_[i].tooltip, this);
            return true;
        }
    }
    return QWidget::event(e);
}

} // namespace fincept::screens::etfr
