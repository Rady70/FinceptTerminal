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
    QStringList coverage;
    for (const auto& source : series_)
        coverage << tr("%1: %2 observations").arg(fedwatch::source_label(source.label)).arg(source.points.size());
    setAccessibleName(probability_scale_ ? tr("Probability evolution graph") : tr("Probability difference graph"));
    setAccessibleDescription(coverage.join("; "));
    update();
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
    qint64 first = std::numeric_limits<qint64>::max(), last = std::numeric_limits<qint64>::min();
    double low = std::numeric_limits<double>::max(), high = std::numeric_limits<double>::lowest();
    for (const auto& s : series_)
        for (const auto& p : s.points) {
            first = qMin(first, p.instant.toMSecsSinceEpoch());
            last = qMax(last, p.instant.toMSecsSinceEpoch());
            low = qMin(low, p.value);
            high = qMax(high, p.value);
        }
    if (first > last) {
        painter.drawText(rect(), Qt::AlignCenter, tr("No accepted observations in this range"));
        return;
    }
    if (probability_scale_) {
        low = 0;
        high = 100;
    } else {
        low = qMin(low, 0.0);
        high = qMax(high, 0.0);
    }
    const QRectF plot(65, 50, qMax(1, width() - 90), qMax(1, height() - 92));
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
    const QVector<QColor> colors{QColor("#3B82F6"), QColor("#F59E0B")};
    for (int i = 0; i < series_.size(); ++i) {
        const auto& s = series_[i];
        painter.setPen(colors[i % colors.size()]);
        painter.setBrush(colors[i % colors.size()]);
        painter.drawText(QRectF(12, 5 + i * 19, width() - 24, 19), Qt::AlignLeft, fedwatch::source_label(s.label));
        QPointF previous_position;
        const fedwatch::Point* previous = nullptr;
        for (const auto& p : s.points) {
            const double x = first == last ? 0.5 : double(p.instant.toMSecsSinceEpoch() - first) / double(last - first);
            const double y = low == high ? 0.5 : (p.value - low) / (high - low);
            QPointF pos(plot.left() + x * plot.width(), plot.bottom() - y * plot.height());
            if (previous && fedwatch::adjacent_observations(*previous, p)) {
                painter.setPen(QPen(colors[i % colors.size()], 2));
                painter.drawLine(previous_position, pos);
            }
            painter.drawEllipse(pos, 3, 3);
            hit_points_.push_back({pos, fedwatch::source_label(s.label) + "\n" + p.detail});
            previous = &p;
            previous_position = pos;
        }
    }
}
void FedWatchHistoryChart::mouseMoveEvent(QMouseEvent* event) {
    for (const auto& hit : hit_points_)
        if (QLineF(hit.first, event->position()).length() < 8) {
            QToolTip::showText(event->globalPosition().toPoint(), hit.second, this);
            return;
        }
    QToolTip::hideText();
}
} // namespace fincept::screens
