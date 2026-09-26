// src/screens/economics/panels/CftcGroupBars.cpp
#include "screens/economics/panels/CftcGroupBars.h"

#include "ui/theme/Theme.h"

#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

namespace {

QColor tone_color(CftcMonitorTone tone) {
    switch (tone) {
        case CftcMonitorTone::Ordinary:
            return QColor(ui::colors::TEXT_TERTIARY());
        case CftcMonitorTone::DataQuality:
            return QColor(ui::colors::NEGATIVE());
        case CftcMonitorTone::Extreme:
            return QColor(ui::colors::AMBER());
        case CftcMonitorTone::ExtremeTransition:
            return QColor(ui::colors::WARNING());
        case CftcMonitorTone::Repositioning:
            return QColor(ui::colors::CYAN());
        case CftcMonitorTone::OpenInterest:
            return QColor(ui::colors::INFO());
        case CftcMonitorTone::Concentration:
            return QColor(ui::colors::TEXT_SECONDARY());
    }
    return QColor(ui::colors::TEXT_TERTIARY());
}

} // namespace

CftcGroupBars::CftcGroupBars(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void CftcGroupBars::set_scale(const Scale& scale) {
    scale_ = scale;
    if (scale_.extent <= 0.0)
        scale_.extent = 1.0;
    update();
}

void CftcGroupBars::set_rows(const QVector<Row>& rows) {
    rows_ = rows;
    hover_row_ = -1;
    updateGeometry();
    update();
}

void CftcGroupBars::refresh_theme() {
    update();
}

QSize CftcGroupBars::sizeHint() const {
    return QSize(560, pad_top_ * 2 + rows_.size() * row_height_ + axis_height_);
}

QSize CftcGroupBars::minimumSizeHint() const {
    return QSize(260, pad_top_ * 2 + rows_.size() * row_height_ + axis_height_);
}

int CftcGroupBars::row_at(const QPoint& pos) const {
    if (rows_.isEmpty() || pos.x() < 0 || pos.x() > width())
        return -1;
    const int row = (pos.y() - pad_top_) / row_height_;
    if (row < 0 || row >= rows_.size())
        return -1;
    return row;
}

void CftcGroupBars::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QColor text_primary(ui::colors::TEXT_PRIMARY());
    const QColor text_secondary(ui::colors::TEXT_SECONDARY());
    const QColor text_tertiary(ui::colors::TEXT_TERTIARY());
    const QColor border(ui::colors::BORDER_DIM());
    const QColor hover(ui::colors::BG_HOVER());

    QFont row_font = font();
    row_font.setPixelSize(ui::fonts::font_px(-3));
    QFont axis_font = font();
    axis_font.setPixelSize(ui::fonts::font_px(-4));

    const int track_left = label_width_;
    const int track_width = std::max(20, width() - label_width_ - value_width_);
    const int track_right = track_left + track_width;
    const int baseline = scale_.percentile ? track_left : track_left + track_width / 2;
    const int rows_bottom = pad_top_ + rows_.size() * row_height_;

    auto value_x = [&](double value) {
        const double ratio = std::clamp(value / scale_.extent, -1.0, 1.0);
        if (scale_.percentile)
            return track_left + static_cast<int>(std::clamp(ratio, 0.0, 1.0) * track_width);
        return baseline + static_cast<int>(ratio * (track_width / 2.0));
    };

    // Shared extent axis: a reference line at the value origin plus min/max
    // labels, so the reader always knows the scale a bar was drawn against.
    painter.setPen(QPen(border, 1));
    painter.drawLine(baseline, pad_top_, baseline, rows_bottom);
    if (scale_.percentile) {
        painter.drawLine(track_left, pad_top_, track_left, rows_bottom);
        painter.drawLine(track_right, pad_top_, track_right, rows_bottom);
    }
    painter.setFont(axis_font);
    painter.setPen(text_tertiary);
    const QString extent_text = QString::number(scale_.extent, 'f', scale_.decimals);
    if (scale_.percentile) {
        painter.drawText(QRect(track_left, rows_bottom, 44, axis_height_), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("0%"));
        painter.drawText(QRect(track_right - 44, rows_bottom, 44, axis_height_), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("100%"));
    } else {
        painter.drawText(QRect(track_left, rows_bottom, 60, axis_height_), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("-") + extent_text);
        painter.drawText(QRect(baseline - 30, rows_bottom, 60, axis_height_), Qt::AlignHCenter | Qt::AlignVCenter,
                         QStringLiteral("0"));
        painter.drawText(QRect(track_right - 60, rows_bottom, 60, axis_height_), Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("+") + extent_text);
    }

    painter.setFont(row_font);
    const QFontMetrics metrics(row_font);
    const int bar_height = 10;

    for (int i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_.at(i);
        const int top = pad_top_ + i * row_height_;
        if (i == hover_row_)
            painter.fillRect(QRect(0, top, width(), row_height_), hover);

        const bool notable = row.tone != CftcMonitorTone::Ordinary;
        int text_left = 4;
        if (notable) {
            const QColor accent = tone_color(row.tone);
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            painter.drawEllipse(QPoint(text_left + 3, top + row_height_ / 2), 3, 3);
            text_left += 12;
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(notable ? text_primary : text_secondary);
        const int label_available = label_width_ - text_left - 8;
        painter.drawText(QRect(text_left, top, std::max(10, label_available), row_height_),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(row.label, Qt::ElideRight, label_available));

        const int center_y = top + row_height_ / 2;
        if (row.has_value) {
            const int x = value_x(row.value);
            const int left = std::min(x, baseline);
            const int width_px = std::max(2, std::abs(x - baseline));
            const QColor fill = notable ? tone_color(row.tone) : QColor(ui::colors::TEXT_TERTIARY());
            QColor bar = fill;
            bar.setAlpha(notable ? 205 : 130);
            painter.setPen(Qt::NoPen);
            painter.setBrush(bar);
            painter.drawRect(QRect(left, center_y - bar_height / 2, width_px, bar_height));
        } else {
            painter.setPen(QPen(text_tertiary, 1, Qt::DashLine));
            painter.drawLine(baseline - 6, center_y, baseline + 6, center_y);
        }

        QString value_text = QStringLiteral("—");
        QColor value_color = text_tertiary;
        if (row.has_value) {
            if (scale_.percentile)
                value_text = QString::number(row.value, 'f', scale_.decimals) + QLatin1Char('%');
            else
                value_text = cftc_monitor_signed_percent(row.value, scale_.decimals) + QLatin1Char('%');
            if (!scale_.percentile && row.value > 0.0)
                value_color = QColor(ui::colors::POSITIVE());
            else if (!scale_.percentile && row.value < 0.0)
                value_color = QColor(ui::colors::NEGATIVE());
            else
                value_color = text_primary;
        }
        painter.setPen(value_color);
        painter.drawText(QRect(track_right + 4, top, value_width_ - 8, row_height_), Qt::AlignRight | Qt::AlignVCenter,
                         value_text);
    }
}

void CftcGroupBars::mouseMoveEvent(QMouseEvent* event) {
    const int row = row_at(event->position().toPoint());
    if (row != hover_row_) {
        hover_row_ = row;
        update();
    }
    if (row >= 0 && !rows_.at(row).tooltip.isEmpty())
        QToolTip::showText(event->globalPosition().toPoint(), rows_.at(row).tooltip, this);
    else
        QToolTip::hideText();
}

void CftcGroupBars::mouseDoubleClickEvent(QMouseEvent* event) {
    const int row = row_at(event->position().toPoint());
    if (row >= 0)
        emit marketActivated(rows_.at(row).market_key);
}

void CftcGroupBars::leaveEvent(QEvent*) {
    if (hover_row_ != -1) {
        hover_row_ = -1;
        update();
    }
    QToolTip::hideText();
}

} // namespace fincept::screens
