#include "screens/economics/panels/FedWatchHistoryChart.h"

#include "ui/theme/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <limits>

namespace fincept::screens {
FedWatchHistoryChart::FedWatchHistoryChart(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(270);
    setMouseTracking(true);
}
void FedWatchHistoryChart::set_series(const QVector<fedwatch::Series>& series) {
    series_ = series;
    hover_date_ = {};
    update_accessibility();
    update();
}
void FedWatchHistoryChart::update_accessibility() {
    QStringList coverage;
    for (const auto& source : series_)
        coverage << tr("%1: %2 observations").arg(fedwatch::source_label(source.label)).arg(source.points.size());
    setAccessibleName(probability_scale_ ? tr("Probability evolution graph") : tr("Probability difference graph"));
    setAccessibleDescription(coverage.join("; ") + (hover_date_.isValid() ? "; " + hover_text() : QString{}));
}
void FedWatchHistoryChart::set_hover_date(const QDate& date) {
    hover_date_ = date;
    update_accessibility();
    update();
}
QString FedWatchHistoryChart::hover_text() const {
    if (!hover_date_.isValid())
        return {};
    QStringList values;
    for (const auto& source : series_)
        for (const auto& point : source.points)
            if (point.instant.toUTC().date() == hover_date_)
                values << QString::number(point.value, 'g', 12) + (probability_scale_ ? "%" : " pp") + " @ " +
                              point.instant.toUTC().toString("HH:mm 'UTC'");
    return hover_date_.toString(Qt::ISODate) + " · " + (values.isEmpty() ? tr("No observation") : values.join("; "));
}
void FedWatchHistoryChart::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QFont chart_font = font();
    chart_font.setPixelSize(12);
    painter.setFont(chart_font);
    painter.fillRect(rect(), QColor(BG_SURFACE()));
    painter.setPen(QColor(TEXT_SECONDARY()));
    hit_points_.clear();
    hit_dates_.clear();
    plot_ = {};
    qint64 first = std::numeric_limits<qint64>::max(), last = std::numeric_limits<qint64>::min();
    double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
    for (const auto& s : series_)
        for (const auto& p : s.points) {
            first = qMin(first, p.instant.toMSecsSinceEpoch());
            last = qMax(last, p.instant.toMSecsSinceEpoch());
            low = qMin(low, p.value);
            high = qMax(high, p.value);
        }
    const bool empty = first > last;
    if (empty && first_.isValid() && last_.isValid()) {
        first = first_.toMSecsSinceEpoch();
        last = last_.toMSecsSinceEpoch();
        low = value_bounds_.first;
        high = value_bounds_.second;
    } else if (empty) {
        painter.drawText(rect(), Qt::AlignCenter, tr("No accepted observations in this range"));
        if (hover_date_.isValid())
            painter.drawText(QRectF(12, height() - 24, width() - 24, 20), Qt::AlignLeft, hover_text());
        return;
    }
    if (first_.isValid() && last_.isValid() && first_ <= last_) {
        first = qMin(first, first_.toMSecsSinceEpoch());
        last = qMax(last, last_.toMSecsSinceEpoch());
    }
    if (probability_scale_) {
        low = qMin(low, value_bounds_.first);
        high = qMax(high, value_bounds_.second);
    } else {
        low = qMin(low, 0.0);
        high = qMax(high, 0.0);
    }
    const QRectF plot(65, 50, qMax(1, width() - 90), qMax(1, height() - 110));
    plot_ = plot;
    plotted_first_ = first;
    plotted_last_ = last;
    painter.drawLine(plot.bottomLeft(), plot.bottomRight());
    painter.drawLine(plot.topLeft(), plot.bottomLeft());
    if (!probability_scale_ && high > low) {
        const double zero_y = plot.bottom() - (0.0 - low) / (high - low) * plot.height();
        painter.setPen(QPen(QColor(BORDER_DIM()), 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), zero_y), QPointF(plot.right(), zero_y));
        painter.setPen(QColor(TEXT_SECONDARY()));
    }
    painter.drawText(QRectF(0, plot.top() - 8, 60, 20), Qt::AlignRight, QString::number(high, 'f', 2));
    painter.drawText(QRectF(0, plot.bottom() - 10, 60, 20), Qt::AlignRight, QString::number(low, 'f', 2));
    painter.drawText(QRectF(plot.left(), plot.bottom() + 8, plot.width(), 20), Qt::AlignLeft,
                     QDateTime::fromMSecsSinceEpoch(first, QTimeZone::UTC).toString("yyyy-MM-dd"));
    painter.drawText(QRectF(plot.left(), plot.bottom() + 8, plot.width(), 20), Qt::AlignRight,
                     QDateTime::fromMSecsSinceEpoch(last, QTimeZone::UTC).toString("yyyy-MM-dd"));
    if (empty)
        painter.drawText(plot, Qt::AlignCenter, tr("No accepted observations in this range"));
    const auto& colors = ui::ThemeManager::instance().tokens().chart_colors;
    for (int i = 0; i < series_.size(); ++i) {
        const auto& s = series_[i];
        const QColor color(colors[(s.label == "POLYMARKET_CLOB" ? 1 : i) % colors.size()]);
        painter.setPen(color);
        painter.setBrush(color);
        painter.drawText(QRectF(12, 5 + i * 19, width() - 24, 19), Qt::AlignLeft, fedwatch::source_label(s.label));
        QPointF previous_position;
        const fedwatch::Point* previous = nullptr;
        for (const auto& p : s.points) {
            const double x = first == last ? 0.5 : double(p.instant.toMSecsSinceEpoch() - first) / double(last - first);
            const double y = low == high ? 0.5 : (p.value - low) / (high - low);
            QPointF pos(plot.left() + x * plot.width(), plot.bottom() - y * plot.height());
            if (previous && fedwatch::adjacent_observations(*previous, p)) {
                painter.setPen(QPen(color, 1.25));
                painter.drawLine(previous_position, pos);
            }
            painter.drawEllipse(pos, 2, 2);
            hit_points_.push_back({pos, fedwatch::source_label(s.label) + "\n" + p.detail});
            hit_dates_.push_back(p.instant.toUTC().date());
            previous = &p;
            previous_position = pos;
        }
    }
    if (hover_date_.isValid()) {
        const auto instant = QDateTime(hover_date_, QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        const double ratio = first == last ? 0.5 : double(qBound(first, instant, last) - first) / double(last - first);
        const double x = plot.left() + ratio * plot.width();
        painter.setPen(QPen(QColor(BORDER_BRIGHT()), 1, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(QColor(TEXT_PRIMARY()));
        painter.drawText(QRectF(12, height() - 24, width() - 24, 20), Qt::AlignLeft, hover_text());
    }
}
void FedWatchHistoryChart::mouseMoveEvent(QMouseEvent* event) {
    if (!plot_.contains(event->position())) {
        leaveEvent(nullptr);
        return;
    }
    const double ratio = (event->position().x() - plot_.left()) / plot_.width();
    QDate date = QDateTime::fromMSecsSinceEpoch(plotted_first_ + qint64(ratio * (plotted_last_ - plotted_first_)),
                                                QTimeZone::UTC)
                     .date();
    for (int i = 0; i < hit_points_.size(); ++i)
        if (QLineF(hit_points_[i].first, event->position()).length() < 8) {
            // Use the exact point's date rather than a rounded axis coordinate.
            date = hit_dates_[i];
            break;
        }
    set_hover_date(date);
    emit date_hovered(date);
    for (const auto& hit : hit_points_)
        if (QLineF(hit.first, event->position()).length() < 8) {
            QToolTip::showText(event->globalPosition().toPoint(), hit.second, this);
            return;
        }
    QToolTip::hideText();
}
void FedWatchHistoryChart::leaveEvent(QEvent*) {
    set_hover_date({});
    emit hover_finished();
    QToolTip::hideText();
}
} // namespace fincept::screens
