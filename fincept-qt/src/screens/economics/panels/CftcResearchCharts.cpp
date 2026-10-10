#include "screens/economics/panels/CftcResearchCharts.h"

#include "ui/theme/Theme.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

namespace {

QColor cot_with_alpha(QColor color, double a) {
    color.setAlphaF(static_cast<float>(std::clamp(a, 0.0, 1.0)));
    return color;
}

QColor cot_mix(const QColor& from, const QColor& to, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(static_cast<float>(from.redF() + (to.redF() - from.redF()) * t),
                            static_cast<float>(from.greenF() + (to.greenF() - from.greenF()) * t),
                            static_cast<float>(from.blueF() + (to.blueF() - from.blueF()) * t));
}

QString cot_signed_text(double value, int decimals, const QString& suffix = QString()) {
    const double scale = std::pow(10.0, decimals);
    const double rounded = std::round(value * scale) / scale;
    QString text = QString::number(std::fabs(rounded), 'f', decimals) + suffix;
    if (rounded > 0.0)
        return QStringLiteral("+") + text;
    if (rounded < 0.0)
        return QStringLiteral("-") + text;
    return text;
}

QString cot_pct_text(const std::optional<double>& percentile) {
    return percentile ? cftc_percentile_ordinal(*percentile) : QCoreApplication::translate("CftcResearch", "n/a");
}

void cot_draw_tail_zones(QPainter& p, const QRectF& track) {
    using namespace ui::colors;
    const double w = track.width();
    p.fillRect(QRectF(track.left(), track.top(), w * 0.10, track.height()), cot_with_alpha(QColor(CYAN()), 0.16));
    p.fillRect(QRectF(track.left() + w * 0.90, track.top(), w * 0.10, track.height()),
               cot_with_alpha(QColor(AMBER()), 0.16));
    p.setPen(QPen(cot_with_alpha(QColor(TEXT_TERTIARY()), 0.5), 1, Qt::DotLine));
    const double mid = track.left() + w * 0.5;
    p.drawLine(QPointF(mid, track.top()), QPointF(mid, track.bottom()));
}

} // namespace

// ── Colours ─────────────────────────────────────────────────────────────────

QColor cftc_percentile_color(double percentile) {
    using namespace ui::colors;
    const QColor neutral(BG_RAISED());
    const double t = std::clamp(std::fabs(percentile - 0.5) * 2.0, 0.0, 1.0);
    const QColor hue = percentile >= 0.5 ? QColor(AMBER()) : QColor(CYAN());
    // Gentle in the middle, strong in the 10% tails.
    const double strength = t <= 0.8 ? 0.10 + 0.45 * (t / 0.8) : 0.55 + 0.45 * ((t - 0.8) / 0.2);
    return cot_mix(neutral, hue, strength);
}

QColor cftc_change_color(double value, double extent) {
    using namespace ui::colors;
    const QColor neutral(BG_RAISED());
    if (extent <= 0.0)
        return neutral;
    const double t = std::clamp(std::fabs(value) / extent, 0.0, 1.0);
    return cot_mix(neutral, value >= 0.0 ? QColor(AMBER()) : QColor(CYAN()), 0.08 + 0.92 * std::sqrt(t));
}

QColor cftc_ink_on(const QColor& background) {
    const double luminance = 0.2126 * background.redF() + 0.7152 * background.greenF() + 0.0722 * background.blueF();
    return luminance > 0.45 ? QColor(ui::colors::BG_BASE()) : QColor(ui::colors::TEXT_PRIMARY());
}

QColor cftc_group_color(int index) {
    const auto& colors = ui::ThemeManager::instance().tokens().chart_colors;
    return QColor(colors[static_cast<std::size_t>(std::abs(index)) % colors.size()]);
}

// ── Base ────────────────────────────────────────────────────────────────────

CftcChart::CftcChart(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
}

QString CftcChart::hit_text(const QPointF& position) const {
    for (const auto& hit : hits_)
        if (hit.first.contains(position))
            return hit.second;
    return {};
}

void CftcChart::mouseMoveEvent(QMouseEvent* event) {
    const QString text = hit_text(event->position());
    if (text.isEmpty())
        QToolTip::hideText();
    else
        QToolTip::showText(event->globalPosition().toPoint(), text, this);
    QWidget::mouseMoveEvent(event);
}

void CftcChart::leaveEvent(QEvent* event) {
    QToolTip::hideText();
    QWidget::leaveEvent(event);
}

QFont CftcChart::chart_font(int pixel_size, bool bold) const {
    QFont f = font();
    f.setPixelSize(pixel_size);
    f.setBold(bold);
    return f;
}

// ── Positioning board ───────────────────────────────────────────────────────

namespace {
constexpr int kCotBoardHeader = 40;
constexpr int kCotBoardRow = 25;
constexpr int kCotBoardHeading = 26;
constexpr double kCotLabelW = 168;
constexpr double kCotNetW = 66;
constexpr double kCotDeltaW = 62;
constexpr double kCotCellW = 76;
constexpr double kCotOiW = 72;
constexpr double kCotConcW = 64;
constexpr double kCotTagW = 132;
constexpr double kCotTrackMin = 240;
} // namespace

CftcPositioningBoard::CftcPositioningBoard(QWidget* parent) : CftcChart(parent) {
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(tr("Positioning board: every market's position in its 3-year history and every participant "
                         "group's net position"));
}

void CftcPositioningBoard::set_data(const QVector<CftcBoardRow>& rows, const QStringList& columns,
                                    const QString& principal_key, services::CftcFamily family) {
    rows_ = rows;
    columns_ = columns;
    principal_key_ = principal_key;
    family_ = family;
    QStringList description;
    for (const auto& row : rows_) {
        if (row.heading || !row.market.has_reading)
            continue;
        description << QStringLiteral("%1: %2%3")
                           .arg(row.label, cot_signed_text(row.market.net_pct_oi, 1, QStringLiteral("%")),
                                row.market.percentile ? QStringLiteral(", ") + cot_pct_text(row.market.percentile)
                                                      : QString());
    }
    setAccessibleDescription(description.join(QStringLiteral(" | ")));
    setMinimumHeight(content_height());
    setFixedHeight(content_height());
    updateGeometry();
    update();
}

int CftcPositioningBoard::market_count() const {
    int n = 0;
    for (const auto& row : rows_)
        n += row.heading ? 0 : 1;
    return n;
}

int CftcPositioningBoard::content_height() const {
    int h = kCotBoardHeader + 6;
    for (const auto& row : rows_)
        h += row.heading ? kCotBoardHeading : kCotBoardRow;
    return std::max(h, kCotBoardHeader + 60);
}

QSize CftcPositioningBoard::minimumSizeHint() const {
    const double w = kCotLabelW + kCotTrackMin + kCotNetW + kCotDeltaW + columns_.size() * kCotCellW + kCotOiW +
                     kCotConcW + kCotTagW + 24;
    return {static_cast<int>(w), content_height()};
}

QSize CftcPositioningBoard::sizeHint() const {
    return {std::max(1200, minimumSizeHint().width()), content_height()};
}

void CftcPositioningBoard::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    row_targets_.clear();
    if (rows_.isEmpty()) {
        p.setPen(QColor(TEXT_SECONDARY()));
        p.setFont(chart_font(13));
        p.drawText(rect(), Qt::AlignCenter, tr("No stored COT reports yet. Press REFRESH to read the CFTC history."));
        return;
    }
    const double fixed =
        kCotLabelW + kCotNetW + kCotDeltaW + columns_.size() * kCotCellW + kCotOiW + kCotConcW + kCotTagW + 16;
    const double track_w = std::max(kCotTrackMin, width() - fixed);
    const double x_track = kCotLabelW;
    const double x_net = x_track + track_w + 8;
    const double x_delta = x_net + kCotNetW;
    const double x_cells = x_delta + kCotDeltaW;
    const double x_oi = x_cells + columns_.size() * kCotCellW;
    const double x_conc = x_oi + kCotOiW;
    const double x_tags = x_conc + kCotConcW + 8;

    // Header.
    p.setFont(chart_font(10, true));
    p.setPen(QColor(TEXT_SECONDARY()));
    const double head_text_y = 4;
    p.drawText(QRectF(10, head_text_y, kCotLabelW - 10, 18), Qt::AlignLeft | Qt::AlignVCenter, tr("MARKET"));
    p.drawText(QRectF(x_track, head_text_y, track_w, 18), Qt::AlignLeft | Qt::AlignVCenter,
               tr("%1 · NET POSITION IN ITS 3-YEAR HISTORY").arg(cftc_group_name(family_, principal_key_).toUpper()));
    p.drawText(QRectF(x_net, head_text_y, kCotNetW - 6, 18), Qt::AlignRight | Qt::AlignVCenter, tr("NET %OI"));
    p.drawText(QRectF(x_delta, head_text_y, kCotDeltaW - 6, 18), Qt::AlignRight | Qt::AlignVCenter, tr("1W Δ"));
    for (int c = 0; c < columns_.size(); ++c) {
        const QRectF cell(x_cells + c * kCotCellW, head_text_y, kCotCellW - 4, 18);
        const bool principal = columns_[c] == principal_key_;
        p.setPen(QColor(principal ? AMBER() : TEXT_SECONDARY()));
        p.drawText(cell, Qt::AlignCenter, cftc_group_short_name(columns_[c]));
    }
    p.setPen(QColor(TEXT_SECONDARY()));
    p.drawText(QRectF(x_oi, head_text_y, kCotOiW - 6, 18), Qt::AlignRight | Qt::AlignVCenter, tr("OI 4W"));
    p.drawText(QRectF(x_conc, head_text_y, kCotConcW - 6, 18), Qt::AlignRight | Qt::AlignVCenter, tr("TOP-4 L"));
    // Track scale under the header.
    p.setFont(chart_font(9));
    p.setPen(QColor(TEXT_TERTIARY()));
    for (double tick : {0.0, 0.10, 0.50, 0.90, 1.0}) {
        const double x = x_track + 8 + tick * (track_w - 16);
        p.drawText(QRectF(x - 20, 22, 40, 14), Qt::AlignCenter,
                   tick == 0.0   ? tr("lowest")
                   : tick == 1.0 ? tr("highest")
                                 : QString::number(qRound(tick * 100)));
    }
    p.setFont(chart_font(9));
    p.drawText(QRectF(x_cells, 22, columns_.size() * kCotCellW, 14), Qt::AlignCenter,
               tr("net % of open interest · shade = percentile"));
    add_hit(QRectF(x_track, 0, track_w, kCotBoardHeader),
            tr("Each row places the headline group's net position (long minus short, as a share of open interest) "
               "among the previous 156 weekly reports (about 3 years): left = lowest, right = highest. The shaded "
               "ends are the bottom and top 10%. Hollow dots show the same reading 13, 4 and 1 weeks ago; the filled "
               "dot is the latest report."));
    p.setPen(QPen(QColor(BORDER_DIM()), 1));
    p.drawLine(QPointF(0, kCotBoardHeader), QPointF(width(), kCotBoardHeader));

    double y = kCotBoardHeader + 4;
    for (const auto& row : rows_) {
        if (row.heading) {
            p.setFont(chart_font(10, true));
            p.setPen(QColor(AMBER()));
            p.drawText(QRectF(10, y, width() - 20, kCotBoardHeading), Qt::AlignLeft | Qt::AlignBottom,
                       row.label.toUpper());
            p.setPen(QPen(cot_with_alpha(QColor(AMBER()), 0.35), 1));
            p.drawLine(QPointF(10, y + kCotBoardHeading - 1), QPointF(width() - 10, y + kCotBoardHeading - 1));
            y += kCotBoardHeading;
            continue;
        }
        const CftcMarketRow& m = row.market;
        const QRectF row_rect(0, y, width(), kCotBoardRow);
        row_targets_.push_back({row_rect, m.market_key});
        const double cy = y + kCotBoardRow / 2.0;
        // Label and availability.
        p.setFont(chart_font(12, true));
        p.setPen(QColor(TEXT_PRIMARY()));
        p.drawText(QRectF(10, y, kCotLabelW - 14, kCotBoardRow), Qt::AlignLeft | Qt::AlignVCenter, m.label);
        QString availability;
        if (m.availability == CftcRowAvailability::Stored)
            availability = tr("stored");
        else if (m.availability == CftcRowAvailability::Old)
            availability = tr("old");
        if (!availability.isEmpty()) {
            p.setFont(chart_font(9, true));
            p.setPen(QColor(WARNING()));
            p.drawText(QRectF(10, y, kCotLabelW - 14, kCotBoardRow), Qt::AlignRight | Qt::AlignVCenter, availability);
        }
        // Track.
        const QRectF track(x_track + 8, y + 6, track_w - 16, kCotBoardRow - 12);
        p.fillRect(track, QColor(BG_RAISED()));
        cot_draw_tail_zones(p, track);
        QStringList tooltip;
        tooltip << m.label;
        if (m.availability == CftcRowAvailability::Unavailable) {
            p.setFont(chart_font(10));
            p.setPen(QColor(TEXT_SECONDARY()));
            p.drawText(track.adjusted(6, -4, 0, 4), Qt::AlignLeft | Qt::AlignVCenter,
                       tr("no usable report: %1").arg(m.availability_note));
            tooltip << m.availability_note;
        } else if (m.percentile) {
            auto px = [&track](double v) { return track.left() + std::clamp(v, 0.0, 1.0) * track.width(); };
            QVector<QPointF> trail;
            for (const auto& value : {m.percentile_13r, m.percentile_4r, m.percentile_1r}) {
                if (value)
                    trail.push_back(QPointF(px(*value), cy));
            }
            trail.push_back(QPointF(px(*m.percentile), cy));
            if (trail.size() > 1) {
                QPainterPath path(trail.first());
                for (int i = 1; i < trail.size(); ++i)
                    path.lineTo(trail[i]);
                p.setPen(QPen(cot_with_alpha(QColor(TEXT_SECONDARY()), 0.55), 2));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
            const double radii[] = {2.6, 3.0, 3.4};
            int k = 0;
            for (const auto& value : {m.percentile_13r, m.percentile_4r, m.percentile_1r}) {
                if (value) {
                    p.setPen(QPen(QColor(TEXT_SECONDARY()), 1.2));
                    p.setBrush(QColor(BG_SURFACE()));
                    p.drawEllipse(QPointF(px(*value), cy), radii[k], radii[k]);
                }
                ++k;
            }
            const QColor now_color = m.tail == CftcLevelTail::High  ? QColor(AMBER())
                                     : m.tail == CftcLevelTail::Low ? QColor(CYAN())
                                                                    : QColor(TEXT_PRIMARY());
            p.setPen(QPen(QColor(BG_SURFACE()), 1.5));
            p.setBrush(now_color);
            p.drawEllipse(QPointF(px(*m.percentile), cy), 5.5, 5.5);
            tooltip << tr("%1: %2 — %3")
                           .arg(cftc_group_name(family_, m.principal_key), cftc_net_position_text(m.net_pct_oi),
                                cftc_level_rank_text(*m.percentile, 156));
            tooltip << tr("13 weeks ago: %1 · 4 weeks ago: %2 · last week: %3 · now: %4")
                           .arg(cot_pct_text(m.percentile_13r), cot_pct_text(m.percentile_4r),
                                cot_pct_text(m.percentile_1r), cot_pct_text(m.percentile));
            if (m.tail_run >= 2)
                tooltip << tr("%1 consecutive weeks in the %2")
                               .arg(m.tail_run)
                               .arg(m.tail == CftcLevelTail::High ? tr("top 10%") : tr("bottom 10%"));
        } else if (m.has_reading) {
            p.setFont(chart_font(10));
            p.setPen(QColor(TEXT_SECONDARY()));
            p.drawText(track.adjusted(6, -4, 0, 4), Qt::AlignLeft | Qt::AlignVCenter,
                       tr("fewer than 156 earlier reports to compare with"));
        }
        // Net %OI and its weekly change.
        p.setFont(chart_font(12, true));
        p.setPen(QColor(TEXT_PRIMARY()));
        if (m.has_reading)
            p.drawText(QRectF(x_net, y, kCotNetW - 6, kCotBoardRow), Qt::AlignRight | Qt::AlignVCenter,
                       cot_signed_text(m.net_pct_oi, 1, QStringLiteral("%")));
        if (m.net_flow_1w) {
            p.setFont(chart_font(11));
            p.setPen(*m.net_flow_1w >= 0.0 ? QColor(AMBER()) : QColor(CYAN()));
            p.drawText(QRectF(x_delta, y, kCotDeltaW - 6, kCotBoardRow), Qt::AlignRight | Qt::AlignVCenter,
                       cot_signed_text(*m.net_flow_1w, 1));
            tooltip << tr("Net change since the previous report: %1 of that report's open interest")
                           .arg(cot_signed_text(*m.net_flow_1w, 1, QStringLiteral("%")));
        }
        // Participant cells.
        for (int c = 0; c < columns_.size(); ++c) {
            const QRectF cell(x_cells + c * kCotCellW + 2, y + 2, kCotCellW - 4, kCotBoardRow - 4);
            const CftcMatrixCell* data = nullptr;
            for (const auto& candidate : m.cells)
                if (candidate.participant_key == columns_[c])
                    data = &candidate;
            if (!data || !data->has_value)
                continue;
            const QColor fill = data->percentile ? cftc_percentile_color(*data->percentile) : QColor(BG_RAISED());
            p.setPen(Qt::NoPen);
            p.setBrush(fill);
            p.drawRoundedRect(cell, 2, 2);
            if (data->tail != CftcLevelTail::None) {
                p.setPen(QPen(data->tail == CftcLevelTail::High ? QColor(AMBER()) : QColor(CYAN()), 1.2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(cell.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
            }
            p.setFont(chart_font(11, columns_[c] == principal_key_));
            p.setPen(cftc_ink_on(fill));
            p.drawText(cell, Qt::AlignCenter, cot_signed_text(data->net_pct_oi, 1));
            QString cell_tip = QStringLiteral("%1 · %2: %3")
                                   .arg(m.label, cftc_group_name(family_, data->participant_key),
                                        cftc_net_position_text(data->net_pct_oi));
            if (data->percentile)
                cell_tip += QStringLiteral("\n") + cftc_level_rank_text(*data->percentile, 156) + QStringLiteral(" (") +
                            cftc_percentile_ordinal(*data->percentile) + QStringLiteral(")");
            add_hit(cell, cell_tip);
        }
        // Open interest and concentration.
        p.setFont(chart_font(11));
        if (m.oi_change_4r) {
            const bool notable = m.oi_rank_4r && *m.oi_rank_4r >= 0.75;
            p.setPen(QColor(notable ? TEXT_PRIMARY() : TEXT_SECONDARY()));
            p.setFont(chart_font(11, notable));
            p.drawText(QRectF(x_oi, y, kCotOiW - 6, kCotBoardRow), Qt::AlignRight | Qt::AlignVCenter,
                       cot_signed_text(*m.oi_change_4r, 1, QStringLiteral("%")));
            QString oi_tip =
                tr("Open interest %1 contracts; %2 over 4 weeks")
                    .arg(cftc_contracts(m.open_interest), cot_signed_text(*m.oi_change_4r, 1, QStringLiteral("%")));
            if (m.oi_rank_4r)
                oi_tip += QStringLiteral(" (") + cftc_move_size_text(*m.oi_rank_4r, 4) + QStringLiteral(")");
            add_hit(QRectF(x_oi, y, kCotOiW, kCotBoardRow), oi_tip);
        }
        if (m.concentration_long) {
            const bool high = m.concentration_long_percentile && *m.concentration_long_percentile >= 0.90;
            p.setFont(chart_font(11, high));
            p.setPen(QColor(high ? AMBER() : TEXT_SECONDARY()));
            p.drawText(QRectF(x_conc, y, kCotConcW - 6, kCotBoardRow), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(*m.concentration_long, 'f', 1) + QLatin1Char('%'));
            QString conc_tip = tr("The 4 largest traders on the long side account for %1 of open interest")
                                   .arg(QString::number(*m.concentration_long, 'f', 1) + QLatin1Char('%'));
            if (m.concentration_long_percentile)
                conc_tip += QStringLiteral("\n") +
                            cftc_rank_position(*m.concentration_long_percentile, tr("earlier reports for this market"));
            add_hit(QRectF(x_conc, y, kCotConcW, kCotBoardRow), conc_tip);
        }
        // Tags.
        double tx = x_tags;
        p.setFont(chart_font(9, true));
        for (const QString& tag : m.tags) {
            const double tw = QFontMetricsF(p.font()).horizontalAdvance(tag) + 10;
            if (tx + tw > width() - 4)
                break;
            const QRectF chip(tx, y + 5, tw, kCotBoardRow - 10);
            const QColor tone = tag == tr("TURNING")      ? QColor(INFO())
                                : tag == tr("LARGE MOVE") ? QColor(TEXT_PRIMARY())
                                                          : QColor(AMBER());
            p.setPen(QPen(cot_with_alpha(tone, 0.8), 1));
            p.setBrush(cot_with_alpha(tone, 0.12));
            p.drawRoundedRect(chip, 3, 3);
            p.setPen(tone);
            p.drawText(chip, Qt::AlignCenter, tag);
            tx += tw + 4;
        }
        if (!m.availability_note.isEmpty() && m.availability != CftcRowAvailability::Unavailable)
            tooltip << m.availability_note;
        if (m.report_date.isValid())
            tooltip << tr("Latest report: %1").arg(m.report_date.toString(Qt::ISODate));
        tooltip << tr("Click to open the market");
        add_hit(QRectF(0, y, x_net + kCotNetW + kCotDeltaW, kCotBoardRow), tooltip.join(QStringLiteral("\n")));
        // Row separator.
        p.setPen(QPen(cot_with_alpha(QColor(BORDER_DIM()), 0.6), 1));
        p.drawLine(QPointF(10, y + kCotBoardRow - 0.5), QPointF(width() - 10, y + kCotBoardRow - 0.5));
        y += kCotBoardRow;
    }
}

void CftcPositioningBoard::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        for (const auto& target : row_targets_) {
            if (target.first.contains(event->position())) {
                emit marketActivated(target.second);
                return;
            }
        }
    }
    CftcChart::mousePressEvent(event);
}

// ── Participant bars ────────────────────────────────────────────────────────

namespace {
constexpr int kCotBarsHeader = 24;
constexpr int kCotBarsRow = 34;
} // namespace

CftcParticipantBars::CftcParticipantBars(QWidget* parent) : CftcChart(parent) {
    setAccessibleName(tr("Participant positions in the latest report"));
}

void CftcParticipantBars::set_rows(const QVector<CftcParticipantBarRow>& rows) {
    rows_ = rows;
    QStringList description;
    for (const auto& row : rows_)
        if (row.has_net)
            description << QStringLiteral("%1 %2").arg(row.name, cot_signed_text(row.net_pct, 1, QStringLiteral("%")));
    setAccessibleDescription(description.join(QStringLiteral(" | ")));
    setFixedHeight(kCotBarsHeader + std::max<int>(1, rows_.size()) * kCotBarsRow + 8);
    update();
}

QSize CftcParticipantBars::sizeHint() const {
    return {640, kCotBarsHeader + std::max<int>(1, rows_.size()) * kCotBarsRow + 8};
}

void CftcParticipantBars::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    if (rows_.isEmpty())
        return;
    const double name_w = 150;
    const double net_w = 76;
    const double pct_w = std::clamp(width() * 0.24, 120.0, 220.0);
    const double delta_w = 58;
    const double bar_left = name_w;
    const double bar_w = std::max(140.0, width() - name_w - net_w - pct_w - delta_w - 16);
    const double center = bar_left + bar_w / 2.0;
    double scale = 10.0;
    for (const auto& row : rows_) {
        if (row.has_long)
            scale = std::max(scale, row.long_pct);
        if (row.has_short)
            scale = std::max(scale, row.short_pct);
    }
    scale = std::ceil(scale / 10.0) * 10.0;
    const double half = bar_w / 2.0 - 34;
    auto bx = [&](double value) { return center + value / scale * half; };

    p.setFont(chart_font(10, true));
    p.setPen(QColor(TEXT_SECONDARY()));
    // The caption under the chart says the bars are shares of open interest;
    // the header only names the two sides so it fits the narrowest half.
    p.drawText(QRectF(bar_left, 2, bar_w / 2 - 6, 18), Qt::AlignRight | Qt::AlignVCenter, tr("← SHORT"));
    p.drawText(QRectF(center + 6, 2, bar_w / 2 - 6, 18), Qt::AlignLeft | Qt::AlignVCenter, tr("LONG →"));
    const double x_net = bar_left + bar_w + 4;
    const double x_pct = x_net + net_w + 6;
    const double x_delta = x_pct + pct_w + 4;
    p.drawText(QRectF(x_net, 2, net_w - 4, 18), Qt::AlignRight | Qt::AlignVCenter, tr("NET"));
    p.drawText(QRectF(x_pct, 2, pct_w, 18), Qt::AlignCenter, tr("3-YEAR PERCENTILE"));
    p.drawText(QRectF(x_delta, 2, delta_w, 18), Qt::AlignRight | Qt::AlignVCenter, tr("1W Δ"));

    double y = kCotBarsHeader;
    for (const auto& row : rows_) {
        const double cy = y + kCotBarsRow / 2.0;
        p.setFont(chart_font(12, row.principal));
        p.setPen(QColor(row.principal ? TEXT_PRIMARY() : TEXT_SECONDARY()));
        p.drawText(QRectF(8, y, name_w - 12, kCotBarsRow), Qt::AlignLeft | Qt::AlignVCenter, row.name);
        // Butterfly bars.
        p.setPen(Qt::NoPen);
        if (row.has_short) {
            const QRectF bar(bx(-row.short_pct), cy - 7, center - bx(-row.short_pct), 14);
            p.setBrush(cot_with_alpha(QColor(CYAN()), row.principal ? 0.85 : 0.55));
            p.drawRect(bar);
            p.setFont(chart_font(10));
            p.setPen(QColor(TEXT_SECONDARY()));
            p.drawText(QRectF(bar.left() - 46, y, 44, kCotBarsRow), Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(row.short_pct, 'f', 1) + QLatin1Char('%'));
            p.setPen(Qt::NoPen);
        }
        if (row.has_long) {
            const QRectF bar(center, cy - 7, bx(row.long_pct) - center, 14);
            p.setBrush(cot_with_alpha(QColor(AMBER()), row.principal ? 0.85 : 0.55));
            p.drawRect(bar);
            p.setFont(chart_font(10));
            p.setPen(QColor(TEXT_SECONDARY()));
            p.drawText(QRectF(bar.right() + 3, y, 44, kCotBarsRow), Qt::AlignLeft | Qt::AlignVCenter,
                       QString::number(row.long_pct, 'f', 1) + QLatin1Char('%'));
            p.setPen(Qt::NoPen);
        }
        if (row.has_net) {
            const double nx = bx(row.net_pct);
            QPainterPath diamond;
            diamond.moveTo(nx, cy - 6);
            diamond.lineTo(nx + 6, cy);
            diamond.lineTo(nx, cy + 6);
            diamond.lineTo(nx - 6, cy);
            diamond.closeSubpath();
            p.setPen(QPen(QColor(BG_SURFACE()), 1.5));
            p.setBrush(QColor(TEXT_PRIMARY()));
            p.drawPath(diamond);
        }
        p.setPen(QPen(QColor(BORDER_MED()), 1));
        p.drawLine(QPointF(center, y + 3), QPointF(center, y + kCotBarsRow - 3));
        // Net.
        if (row.has_net) {
            p.setFont(chart_font(12, true));
            p.setPen(QColor(TEXT_PRIMARY()));
            p.drawText(QRectF(x_net, y, net_w - 4, kCotBarsRow), Qt::AlignRight | Qt::AlignVCenter,
                       cot_signed_text(row.net_pct, 1, QStringLiteral("%")));
        }
        // Percentile mini track.
        const QRectF track(x_pct + 4, cy - 5, pct_w - 8, 10);
        p.fillRect(track, QColor(BG_RAISED()));
        cot_draw_tail_zones(p, track);
        auto tx = [&track](double v) { return track.left() + std::clamp(v, 0.0, 1.0) * track.width(); };
        for (const auto& value : {row.percentile_13r, row.percentile_4r}) {
            if (!value)
                continue;
            p.setPen(QPen(QColor(TEXT_SECONDARY()), 1.2));
            p.setBrush(QColor(BG_SURFACE()));
            p.drawEllipse(QPointF(tx(*value), cy), 3, 3);
        }
        if (row.percentile) {
            const QColor dot = *row.percentile >= 0.90   ? QColor(AMBER())
                               : *row.percentile <= 0.10 ? QColor(CYAN())
                                                         : QColor(TEXT_PRIMARY());
            p.setPen(QPen(QColor(BG_SURFACE()), 1.2));
            p.setBrush(dot);
            p.drawEllipse(QPointF(tx(*row.percentile), cy), 5, 5);
        }
        // Weekly change.
        if (row.net_flow_1w) {
            p.setFont(chart_font(11));
            p.setPen(*row.net_flow_1w >= 0.0 ? QColor(AMBER()) : QColor(CYAN()));
            p.drawText(QRectF(x_delta, y, delta_w, kCotBarsRow), Qt::AlignRight | Qt::AlignVCenter,
                       cot_signed_text(*row.net_flow_1w, 1));
        }
        QStringList tip;
        tip << row.name;
        if (row.has_long)
            tip << tr("Long: %1 of open interest").arg(QString::number(row.long_pct, 'f', 1) + QLatin1Char('%'));
        if (row.has_short)
            tip << tr("Short: %1 of open interest").arg(QString::number(row.short_pct, 'f', 1) + QLatin1Char('%'));
        if (row.has_net)
            tip << tr("Net: %1 contracts (%2 of open interest)")
                       .arg(cot_signed_text(row.net_contracts, 0),
                            cot_signed_text(row.net_pct, 1, QStringLiteral("%")));
        if (row.percentile)
            tip << tr("Percentile vs the previous 156 weekly reports: %1 (4 weeks ago %2, 13 weeks ago %3)")
                       .arg(cot_pct_text(row.percentile), cot_pct_text(row.percentile_4r),
                            cot_pct_text(row.percentile_13r));
        if (row.net_flow_1w)
            tip << tr("Net change since the previous report: %1 of that report's open interest")
                       .arg(cot_signed_text(*row.net_flow_1w, 1, QStringLiteral("%")));
        add_hit(QRectF(0, y, width(), kCotBarsRow), tip.join(QStringLiteral("\n")));
        y += kCotBarsRow;
    }
}

// ── History chart ───────────────────────────────────────────────────────────

namespace {
constexpr double kCotHistLeft = 64;
constexpr double kCotHistRight = 14;
constexpr double kCotHistLegend = 26;
constexpr double kCotHistBottom = 22;
constexpr double kCotPaneTitle = 18;
constexpr double kCotPaneGap = 8;

bool cot_connectable(const CftcSeriesPoint& a, const CftcSeriesPoint& b) {
    return a.has_value && b.has_value && a.date.daysTo(b.date) <= 21;
}

QString cot_axis_number(double value, bool percent) {
    const double magnitude = std::fabs(value);
    QString text;
    if (magnitude < 1e-9) // the zero line reads "0", whatever the neighbouring ticks' precision
        text = percent ? QStringLiteral("0%") : QStringLiteral("0");
    else if (percent)
        text = QString::number(value, 'f', magnitude < 10 ? 1 : 0) + QLatin1Char('%');
    else if (magnitude >= 1e6)
        text = QString::number(value / 1e6, 'f', 1) + QLatin1Char('M');
    else if (magnitude >= 1e3)
        text = QString::number(value / 1e3, 'f', 0) + QLatin1Char('k');
    else
        text = QString::number(value, 'f', magnitude < 10 ? 2 : 0);
    return text;
}

double cot_nice_step(double span, int target) {
    const double raw = span / std::max(1, target);
    const double power = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-12))));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (raw <= m * power)
            return m * power;
    return 10.0 * power;
}

const CftcSeriesPoint* cot_point_at(const QVector<CftcSeriesPoint>& points, const QDate& date) {
    const CftcSeriesPoint* best = nullptr;
    for (const auto& point : points) {
        if (point.date > date)
            break;
        best = &point;
    }
    if (best && best->date.daysTo(date) > 10)
        return nullptr;
    return best;
}
} // namespace

CftcHistoryChart::CftcHistoryChart(QWidget* parent) : CftcChart(parent) {
    setMinimumHeight(420);
    setAccessibleName(tr("Positioning history"));
}

QSize CftcHistoryChart::sizeHint() const {
    return {1000, price_.isEmpty() ? 470 : 590};
}

void CftcHistoryChart::set_range(const QDate& from, const QDate& to) {
    from_ = from;
    to_ = to;
    update();
}

void CftcHistoryChart::set_price(const QVector<CftcSeriesPoint>& points, const QString& label) {
    price_ = points;
    price_label_ = label;
    setMinimumHeight(price_.isEmpty() ? 420 : 540);
    updateGeometry();
    update();
}

void CftcHistoryChart::set_positioning(const QVector<CftcHistorySeries>& series) {
    QVector<bool> hidden(series.size(), false);
    for (int i = 0; i < series.size(); ++i)
        for (int j = 0; j < positioning_.size() && j < hidden_.size(); ++j)
            if (positioning_[j].key == series[i].key)
                hidden[i] = hidden_[j];
    positioning_ = series;
    hidden_ = hidden;
    update();
}

void CftcHistoryChart::set_percentile(const QVector<CftcSeriesPoint>& points, const QString& label) {
    percentile_ = points;
    percentile_label_ = label;
    update();
}

void CftcHistoryChart::set_open_interest(const QVector<CftcSeriesPoint>& points) {
    open_interest_ = points;
    update();
}

void CftcHistoryChart::set_concentration(const QVector<CftcSeriesPoint>& long_side,
                                         const QVector<CftcSeriesPoint>& short_side) {
    concentration_long_ = long_side;
    concentration_short_ = short_side;
    update();
}

void CftcHistoryChart::clear() {
    price_.clear();
    positioning_.clear();
    hidden_.clear();
    percentile_.clear();
    open_interest_.clear();
    concentration_long_.clear();
    concentration_short_.clear();
    hover_x_.reset();
    update();
}

QStringList CftcHistoryChart::visible_series() const {
    QStringList out;
    for (int i = 0; i < positioning_.size(); ++i)
        if (i >= hidden_.size() || !hidden_[i])
            out << positioning_[i].key;
    return out;
}

double CftcHistoryChart::x_of(const QDate& date, const QRectF& plot) const {
    const double span = std::max<qint64>(1, from_.daysTo(to_));
    return plot.left() + plot.width() * static_cast<double>(from_.daysTo(date)) / span;
}

QDate CftcHistoryChart::date_at(double x, const QRectF& plot) const {
    const double span = std::max<qint64>(1, from_.daysTo(to_));
    return from_.addDays(static_cast<qint64>(std::round((x - plot.left()) / plot.width() * span)));
}

void CftcHistoryChart::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    legend_targets_.clear();
    if (!from_.isValid() || !to_.isValid() || positioning_.isEmpty()) {
        p.setPen(QColor(TEXT_SECONDARY()));
        p.setFont(chart_font(13));
        p.drawText(rect(), Qt::AlignCenter, tr("No history to draw."));
        return;
    }
    // Legend.
    double lx = kCotHistLeft;
    p.setFont(chart_font(11, true));
    for (int i = 0; i < positioning_.size(); ++i) {
        const bool hidden = i < hidden_.size() && hidden_[i];
        const QString text = positioning_[i].name;
        const double tw = QFontMetricsF(p.font()).horizontalAdvance(text) + 26;
        const QRectF chip(lx, 4, tw, kCotHistLegend - 8);
        p.setPen(QPen(cot_with_alpha(positioning_[i].color, hidden ? 0.3 : 0.9), 1));
        p.setBrush(cot_with_alpha(positioning_[i].color, hidden ? 0.0 : 0.12));
        p.drawRoundedRect(chip, 3, 3);
        p.setBrush(cot_with_alpha(positioning_[i].color, hidden ? 0.25 : 1.0));
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(chip.left() + 7, chip.center().y() - 1.5, 10, 3));
        p.setPen(QColor(hidden ? TEXT_TERTIARY() : TEXT_PRIMARY()));
        p.drawText(chip.adjusted(20, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
        legend_targets_.push_back({chip, i});
        add_hit(chip, tr("Click to show or hide %1").arg(text));
        lx += tw + 6;
    }

    struct PaneSpec {
        int kind; // 0 price, 1 positioning, 2 percentile, 3 open interest, 4 concentration
        double weight;
    };
    QVector<PaneSpec> specs;
    auto has_values = [this](const QVector<CftcSeriesPoint>& points) {
        for (const auto& point : points)
            if (point.has_value && point.date >= from_ && point.date <= to_)
                return true;
        return false;
    };
    if (has_values(price_))
        specs.push_back({0, 1.0});
    specs.push_back({1, 1.5});
    if (has_values(percentile_))
        specs.push_back({2, 0.62});
    if (has_values(open_interest_))
        specs.push_back({3, 0.62});
    if (has_values(concentration_long_) || has_values(concentration_short_))
        specs.push_back({4, 0.55});
    double total_weight = 0;
    for (const auto& spec : specs)
        total_weight += spec.weight;
    const double available = height() - kCotHistLegend - kCotHistBottom - specs.size() * (kCotPaneTitle + kCotPaneGap);
    double y = kCotHistLegend + 4;
    const double plot_left = kCotHistLeft;
    const double plot_right = width() - kCotHistRight;
    const QRectF full_plot(plot_left, 0, plot_right - plot_left, height());
    const std::optional<QDate> hover_date =
        hover_x_ ? std::optional<QDate>(date_at(*hover_x_, full_plot)) : std::nullopt;

    auto draw_line = [&](const QVector<CftcSeriesPoint>& points, const QRectF& plot, double low, double high,
                         const QColor& color, double width_px) {
        p.setPen(QPen(color, width_px, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath path;
        bool open = false;
        const CftcSeriesPoint* previous = nullptr;
        for (const auto& point : points) {
            if (point.date < from_ || point.date > to_) {
                previous = nullptr;
                open = false;
                continue;
            }
            if (!point.has_value) {
                open = false;
                previous = &point;
                continue;
            }
            const QPointF xy(x_of(point.date, plot),
                             plot.bottom() - (point.value - low) / (high - low) * plot.height());
            if (open && previous && cot_connectable(*previous, point))
                path.lineTo(xy);
            else
                path.moveTo(xy);
            open = true;
            previous = &point;
        }
        p.drawPath(path);
    };
    auto range_of = [this](std::initializer_list<const QVector<CftcSeriesPoint>*> sets, double& low, double& high) {
        bool any = false;
        for (const auto* points : sets)
            for (const auto& point : *points) {
                if (!point.has_value || point.date < from_ || point.date > to_)
                    continue;
                if (!any) {
                    low = high = point.value;
                    any = true;
                }
                low = std::min(low, point.value);
                high = std::max(high, point.value);
            }
        if (!any) {
            low = 0;
            high = 1;
        }
        if (high - low < 1e-9) {
            low -= 1;
            high += 1;
        }
        const double pad = (high - low) * 0.08;
        low -= pad;
        high += pad;
    };
    auto draw_axis = [&](const QRectF& plot, double low, double high, bool percent, bool zero_line) {
        const double step = cot_nice_step(high - low, 4);
        p.setFont(chart_font(10));
        for (double v = std::ceil(low / step) * step; v <= high + 1e-9; v += step) {
            const double yy = plot.bottom() - (v - low) / (high - low) * plot.height();
            p.setPen(QPen(cot_with_alpha(QColor(BORDER_DIM()), 0.8), 1));
            p.drawLine(QPointF(plot.left(), yy), QPointF(plot.right(), yy));
            p.setPen(QColor(TEXT_TERTIARY()));
            p.drawText(QRectF(0, yy - 8, kCotHistLeft - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                       cot_axis_number(v, percent));
        }
        if (zero_line && low < 0 && high > 0) {
            const double yy = plot.bottom() - (0 - low) / (high - low) * plot.height();
            p.setPen(QPen(QColor(TEXT_SECONDARY()), 1));
            p.drawLine(QPointF(plot.left(), yy), QPointF(plot.right(), yy));
        }
    };
    auto pane_title = [&](double top, const QString& title, const QString& readout) {
        p.setFont(chart_font(10, true));
        p.setPen(QColor(TEXT_SECONDARY()));
        p.drawText(QRectF(plot_left, top, plot_right - plot_left, kCotPaneTitle), Qt::AlignLeft | Qt::AlignVCenter,
                   title);
        if (!readout.isEmpty()) {
            p.setFont(chart_font(10));
            p.setPen(QColor(TEXT_PRIMARY()));
            p.drawText(QRectF(plot_left, top, plot_right - plot_left, kCotPaneTitle), Qt::AlignRight | Qt::AlignVCenter,
                       readout);
        }
    };

    QRectF last_plot;
    for (const auto& spec : specs) {
        const double h = available * spec.weight / total_weight;
        const double title_top = y;
        const QRectF plot(plot_left, y + kCotPaneTitle, plot_right - plot_left, h);
        p.fillRect(plot, QColor(BG_BASE()));
        QString readout;
        switch (spec.kind) {
            case 0: {
                double low;
                double high;
                range_of({&price_}, low, high);
                draw_axis(plot, low, high, false, false);
                draw_line(price_, plot, low, high, QColor(TEXT_PRIMARY()), 1.4);
                if (hover_date)
                    if (const auto* pt = cot_point_at(price_, *hover_date); pt && pt->has_value)
                        readout = QString::number(pt->value, 'f', 2);
                pane_title(title_top, tr("PRICE · %1").arg(price_label_), readout);
                break;
            }
            case 1: {
                QVector<const QVector<CftcSeriesPoint>*> visible;
                for (int i = 0; i < positioning_.size(); ++i)
                    if (i >= hidden_.size() || !hidden_[i])
                        visible.push_back(&positioning_[i].points);
                double low = 0;
                double high = 0;
                bool any = false;
                for (const auto* points : visible)
                    for (const auto& point : *points) {
                        if (!point.has_value || point.date < from_ || point.date > to_)
                            continue;
                        low = any ? std::min(low, point.value) : point.value;
                        high = any ? std::max(high, point.value) : point.value;
                        any = true;
                    }
                low = std::min(low, 0.0);
                high = std::max(high, 0.0);
                if (high - low < 1)
                    high = low + 1;
                const double pad = (high - low) * 0.06;
                low -= pad;
                high += pad;
                draw_axis(plot, low, high, true, true);
                QStringList values;
                for (int i = 0; i < positioning_.size(); ++i) {
                    if (i < hidden_.size() && hidden_[i])
                        continue;
                    draw_line(positioning_[i].points, plot, low, high, positioning_[i].color,
                              positioning_[i].emphasized ? 2.2 : 1.3);
                    if (hover_date)
                        if (const auto* pt = cot_point_at(positioning_[i].points, *hover_date); pt && pt->has_value)
                            values << QStringLiteral("%1 %2").arg(positioning_[i].name,
                                                                  cot_signed_text(pt->value, 1, QStringLiteral("%")));
                }
                readout = values.join(QStringLiteral("   "));
                pane_title(title_top, tr("NET POSITION · % OF OPEN INTEREST"), readout);
                break;
            }
            case 2: {
                p.fillRect(
                    QRectF(plot.left(), plot.bottom() - 0.10 * plot.height(), plot.width(), 0.10 * plot.height()),
                    cot_with_alpha(QColor(CYAN()), 0.16));
                p.fillRect(QRectF(plot.left(), plot.top(), plot.width(), 0.10 * plot.height()),
                           cot_with_alpha(QColor(AMBER()), 0.16));
                p.setFont(chart_font(10));
                for (double v : {0.10, 0.50, 0.90}) {
                    const double yy = plot.bottom() - v * plot.height();
                    p.setPen(QPen(cot_with_alpha(QColor(BORDER_DIM()), 0.8), 1, Qt::DotLine));
                    p.drawLine(QPointF(plot.left(), yy), QPointF(plot.right(), yy));
                    p.setPen(QColor(TEXT_TERTIARY()));
                    p.drawText(QRectF(0, yy - 8, kCotHistLeft - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                               QString::number(qRound(v * 100)));
                }
                draw_line(percentile_, plot, 0.0, 1.0, QColor(TEXT_PRIMARY()), 1.4);
                if (hover_date)
                    if (const auto* pt = cot_point_at(percentile_, *hover_date); pt && pt->has_value)
                        readout = cftc_percentile_ordinal(pt->value);
                pane_title(title_top, tr("%1 · PERCENTILE VS PREVIOUS 156 REPORTS").arg(percentile_label_.toUpper()),
                           readout);
                break;
            }
            case 3: {
                double low;
                double high;
                range_of({&open_interest_}, low, high);
                low = std::max(0.0, low);
                draw_axis(plot, low, high, false, false);
                QPainterPath area;
                bool open = false;
                double last_x = plot.left();
                for (const auto& point : open_interest_) {
                    if (!point.has_value || point.date < from_ || point.date > to_)
                        continue;
                    const QPointF xy(x_of(point.date, plot),
                                     plot.bottom() - (point.value - low) / (high - low) * plot.height());
                    if (!open) {
                        area.moveTo(xy.x(), plot.bottom());
                        open = true;
                    }
                    area.lineTo(xy);
                    last_x = xy.x();
                }
                if (open) {
                    area.lineTo(last_x, plot.bottom());
                    area.closeSubpath();
                    p.setPen(Qt::NoPen);
                    p.setBrush(cot_with_alpha(QColor(INFO()), 0.22));
                    p.drawPath(area);
                }
                draw_line(open_interest_, plot, low, high, cot_with_alpha(QColor(INFO()), 0.9), 1.2);
                if (hover_date)
                    if (const auto* pt = cot_point_at(open_interest_, *hover_date); pt && pt->has_value)
                        readout = tr("%1 contracts").arg(cftc_contracts(pt->value));
                pane_title(title_top, tr("OPEN INTEREST"), readout);
                break;
            }
            case 4: {
                double low;
                double high;
                range_of({&concentration_long_, &concentration_short_}, low, high);
                low = std::max(0.0, low);
                draw_axis(plot, low, high, true, false);
                draw_line(concentration_long_, plot, low, high, QColor(AMBER()), 1.3);
                draw_line(concentration_short_, plot, low, high, QColor(CYAN()), 1.3);
                if (hover_date) {
                    QStringList values;
                    if (const auto* pt = cot_point_at(concentration_long_, *hover_date); pt && pt->has_value)
                        values << tr("long %1").arg(QString::number(pt->value, 'f', 1) + QLatin1Char('%'));
                    if (const auto* pt = cot_point_at(concentration_short_, *hover_date); pt && pt->has_value)
                        values << tr("short %1").arg(QString::number(pt->value, 'f', 1) + QLatin1Char('%'));
                    readout = values.join(QStringLiteral("   "));
                }
                pane_title(title_top, tr("4 LARGEST TRADERS · SHARE OF OPEN INTEREST (long amber, short cyan)"),
                           readout);
                break;
            }
            default:
                break;
        }
        last_plot = plot;
        y += kCotPaneTitle + h + kCotPaneGap;
    }

    // Time axis.
    const qint64 span = from_.daysTo(to_);
    p.setFont(chart_font(10));
    p.setPen(QColor(TEXT_TERTIARY()));
    QDate tick(from_.year(), from_.month(), 1);
    int month_step = span <= 450 ? 1 : span <= 1300 ? 3 : span <= 4000 ? 12 : span <= 9000 ? 24 : 60;
    if (month_step >= 12)
        tick = QDate(from_.year(), 1, 1);
    while (tick <= to_) {
        if (tick >= from_) {
            const double x = x_of(tick, last_plot);
            const QString text =
                month_step >= 12 ? QString::number(tick.year()) : tick.toString(QStringLiteral("MMM yy"));
            p.drawText(QRectF(x - 30, height() - kCotHistBottom + 2, 60, kCotHistBottom - 4), Qt::AlignCenter, text);
            p.setPen(QPen(cot_with_alpha(QColor(BORDER_DIM()), 0.5), 1));
            p.drawLine(QPointF(x, kCotHistLegend + 4), QPointF(x, height() - kCotHistBottom));
            p.setPen(QColor(TEXT_TERTIARY()));
        }
        tick = tick.addMonths(month_step);
    }
    // Crosshair.
    if (hover_x_ && *hover_x_ >= plot_left && *hover_x_ <= plot_right) {
        p.setPen(QPen(cot_with_alpha(QColor(TEXT_PRIMARY()), 0.55), 1, Qt::DashLine));
        p.drawLine(QPointF(*hover_x_, kCotHistLegend + 4), QPointF(*hover_x_, height() - kCotHistBottom));
        if (hover_date) {
            p.setFont(chart_font(10, true));
            const QString text = hover_date->toString(Qt::ISODate);
            const double tw = QFontMetricsF(p.font()).horizontalAdvance(text) + 10;
            const QRectF box(std::clamp(*hover_x_ - tw / 2, plot_left, plot_right - tw), height() - kCotHistBottom + 1,
                             tw, kCotHistBottom - 3);
            p.fillRect(box, QColor(BG_RAISED()));
            p.setPen(QColor(TEXT_PRIMARY()));
            p.drawText(box, Qt::AlignCenter, text);
        }
    }
}

void CftcHistoryChart::mouseMoveEvent(QMouseEvent* event) {
    const double x = event->position().x();
    if (event->position().y() > kCotHistLegend && x >= kCotHistLeft && x <= width() - kCotHistRight) {
        hover_x_ = x;
        QToolTip::hideText();
    } else {
        hover_x_.reset();
        CftcChart::mouseMoveEvent(event);
    }
    update();
}

void CftcHistoryChart::mousePressEvent(QMouseEvent* event) {
    for (const auto& target : legend_targets_) {
        if (target.first.contains(event->position())) {
            if (target.second < hidden_.size()) {
                hidden_[target.second] = !hidden_[target.second];
                update();
            }
            return;
        }
    }
    CftcChart::mousePressEvent(event);
}

void CftcHistoryChart::leaveEvent(QEvent* event) {
    hover_x_.reset();
    update();
    CftcChart::leaveEvent(event);
}

// ── Flow strip ──────────────────────────────────────────────────────────────

namespace {
constexpr int kCotFlowRow = 24;
constexpr int kCotFlowHeader = 6;
constexpr int kCotFlowAxis = 20;
constexpr double kCotFlowName = 150;
constexpr double kCotFlowLatest = 78;
} // namespace

CftcFlowStrip::CftcFlowStrip(QWidget* parent) : CftcChart(parent) {
    setAccessibleName(tr("Week-by-week change of each group's net position"));
}

void CftcFlowStrip::set_rows(const QVector<CftcFlowRow>& rows) {
    rows_ = rows;
    setFixedHeight(kCotFlowHeader + std::max<int>(1, rows_.size()) * kCotFlowRow + kCotFlowAxis);
    update();
}

QSize CftcFlowStrip::sizeHint() const {
    return {900, kCotFlowHeader + std::max<int>(1, rows_.size()) * kCotFlowRow + kCotFlowAxis};
}

void CftcFlowStrip::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter p(this);
    p.fillRect(rect(), QColor(BG_SURFACE()));
    clear_hits();
    if (rows_.isEmpty())
        return;
    const double plot_w = std::max(100.0, width() - kCotFlowName - kCotFlowLatest - 8);
    int columns = 0;
    for (const auto& row : rows_)
        columns = std::max<int>(columns, row.cells.size());
    const int shown = std::min(columns, std::max(8, static_cast<int>(plot_w / 7.0)));
    const double cell_w = plot_w / std::max(1, shown);
    double y = kCotFlowHeader;
    QVector<QDate> dates;
    for (const auto& row : rows_) {
        const int start = std::max(0, static_cast<int>(row.cells.size()) - shown);
        double extent = 0.0;
        for (int i = start; i < row.cells.size(); ++i)
            if (row.cells[i].has_value)
                extent = std::max(extent, std::fabs(row.cells[i].value));
        extent = std::max(extent, row.open_interest ? 1.0 : 0.25);
        p.setFont(chart_font(11, false));
        p.setPen(QColor(row.open_interest ? TEXT_SECONDARY() : TEXT_PRIMARY()));
        p.drawText(QRectF(8, y, kCotFlowName - 12, kCotFlowRow), Qt::AlignLeft | Qt::AlignVCenter, row.name);
        for (int i = start; i < row.cells.size(); ++i) {
            const CftcFlowCell& cell = row.cells[i];
            const QRectF r(kCotFlowName + (i - start) * cell_w, y + 2, std::max(1.0, cell_w - 1), kCotFlowRow - 4);
            if (dates.size() < shown && &row == &rows_.first())
                dates.push_back(cell.date);
            if (!cell.has_value) {
                p.fillRect(r, QColor(BG_BASE()));
                add_hit(r, tr("%1 · %2: no one-week change (missing reading or not a weekly step)")
                               .arg(row.name, cell.date.toString(Qt::ISODate)));
                continue;
            }
            p.fillRect(r, cftc_change_color(cell.value, extent));
            add_hit(r, row.open_interest ? tr("%1 · week to %2: open interest %3")
                                               .arg(row.name, cell.date.toString(Qt::ISODate),
                                                    cot_signed_text(cell.value, 2, QStringLiteral("%")))
                                         : tr("%1 · week to %2: net change %3 of open interest")
                                               .arg(row.name, cell.date.toString(Qt::ISODate),
                                                    cot_signed_text(cell.value, 2, QStringLiteral("%"))));
        }
        if (!row.cells.isEmpty() && row.cells.last().has_value) {
            p.setFont(chart_font(11, true));
            p.setPen(row.cells.last().value >= 0.0 ? QColor(AMBER()) : QColor(CYAN()));
            p.drawText(QRectF(kCotFlowName + plot_w + 4, y, kCotFlowLatest - 4, kCotFlowRow),
                       Qt::AlignRight | Qt::AlignVCenter,
                       cot_signed_text(row.cells.last().value, 1, QStringLiteral("%")));
        }
        y += kCotFlowRow;
    }
    // Month labels.
    p.setFont(chart_font(10));
    p.setPen(QColor(TEXT_TERTIARY()));
    int last_month = -1;
    double last_x = -100;
    for (int i = 0; i < dates.size(); ++i) {
        if (!dates[i].isValid() || dates[i].month() == last_month)
            continue;
        last_month = dates[i].month();
        const double x = kCotFlowName + i * cell_w;
        if (x - last_x < 44)
            continue;
        p.drawText(QRectF(x, y + 2, 60, kCotFlowAxis - 4), Qt::AlignLeft | Qt::AlignVCenter,
                   dates[i].toString(QStringLiteral("MMM yy")));
        last_x = x;
    }
}

} // namespace fincept::screens
