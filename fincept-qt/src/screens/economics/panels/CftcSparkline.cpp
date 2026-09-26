// src/screens/economics/panels/CftcSparkline.cpp
#include "screens/economics/panels/CftcSparkline.h"

#include "services/economics/CftcMetricModel.h"
#include "ui/theme/Theme.h"

#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

CftcSparkline::CftcSparkline(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void CftcSparkline::set_points(const QVector<CftcSparkPoint>& points) {
    points_ = points;
    update();
}

void CftcSparkline::refresh_theme() {
    update();
}

QSize CftcSparkline::sizeHint() const {
    return QSize(190, 40);
}

QSize CftcSparkline::minimumSizeHint() const {
    return QSize(120, 32);
}

void CftcSparkline::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    double min_value = 0.0;
    double max_value = 0.0;
    bool have_range = false;
    for (const CftcSparkPoint& point : points_) {
        if (!point.has_value)
            continue;
        if (!have_range) {
            min_value = point.value;
            max_value = point.value;
            have_range = true;
        } else {
            min_value = std::min(min_value, point.value);
            max_value = std::max(max_value, point.value);
        }
    }
    if (!have_range)
        return;

    // Always include zero so a sign change stays visible; pad a flat series so
    // the line does not collapse onto the widget edge.
    min_value = std::min(min_value, 0.0);
    max_value = std::max(max_value, 0.0);
    if (std::abs(max_value - min_value) < 1e-9) {
        min_value -= 1.0;
        max_value += 1.0;
    }

    const int left = 3;
    const int right = width() - 4;
    const int top = 3;
    const int bottom = height() - 4;
    const int plot_width = std::max(10, right - left);
    const int plot_height = std::max(8, bottom - top);

    auto y_for = [&](double value) {
        const double ratio = (value - min_value) / (max_value - min_value);
        return bottom - static_cast<int>(ratio * plot_height);
    };
    auto x_for = [&](int index) {
        if (points_.size() <= 1)
            return left;
        return left + static_cast<int>(static_cast<double>(index) / (points_.size() - 1) * plot_width);
    };

    // Zero reference, then the trajectory with explicit breaks: a missing value
    // or a gap longer than a normal weekly step ends the segment instead of
    // being bridged.
    const int zero_y = y_for(0.0);
    painter.setPen(QPen(QColor(ui::colors::BORDER_MED()), 1));
    painter.drawLine(left, zero_y, right, zero_y);

    const QColor line_color(ui::colors::AMBER());
    painter.setPen(QPen(line_color, 1.6));
    QPolygonF segment;
    QDate previous_date;
    for (int i = 0; i < points_.size(); ++i) {
        const CftcSparkPoint& point = points_.at(i);
        if (!point.has_value) {
            if (segment.size() > 1)
                painter.drawPolyline(segment);
            segment.clear();
            previous_date = QDate();
            continue;
        }
        if (previous_date.isValid() && previous_date.daysTo(point.date) > services::kCftcWeeklyGapDays) {
            if (segment.size() > 1)
                painter.drawPolyline(segment);
            segment.clear();
        }
        segment << QPointF(x_for(i), y_for(point.value));
        previous_date = point.date;
    }
    if (segment.size() > 1)
        painter.drawPolyline(segment);

    // Latest point marker.
    for (int i = points_.size() - 1; i >= 0; --i) {
        if (!points_.at(i).has_value)
            continue;
        painter.setPen(Qt::NoPen);
        painter.setBrush(line_color);
        painter.drawEllipse(QPointF(x_for(i), y_for(points_.at(i).value)), 2.6, 2.6);
        break;
    }
}

} // namespace fincept::screens
