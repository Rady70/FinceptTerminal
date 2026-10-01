#include "screens/etf/EtfMonthlyChart.h"

#include "screens/etf/EtfPresentation.h"
#include "ui/theme/ThemeManager.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace fincept::screens {
EtfMonthlyChart::EtfMonthlyChart(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(210);
    setAccessibleName(tr("Monthly SEC regulatory flow bars; missing months remain gaps"));
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this]() { update(); });
}

void EtfMonthlyChart::set_months(const QJsonArray& months) {
    months_ = months;
    QStringList tips;
    for (const auto& value : months_) {
        const auto m = value.toObject();
        tips.append(m.value("month").toString() + ": " + etf_ui::number(m.value("observed_net_flow_usd"), "USD") +
                    " · " + etf_ui::quality(m));
    }
    setToolTip(tips.join(QLatin1Char('\n')));
    setAccessibleDescription(tips.join(QLatin1Char('\n')));
    bool measured = false;
    for (const auto& value : months_)
        measured = measured || value.toObject().value("observed_net_flow_usd").isDouble();
    setMinimumHeight(measured ? 280 : 150);
    setMaximumHeight(measured ? QWIDGETSIZE_MAX : 170);
    update();
}

QJsonArray EtfMonthlyChart::displayed_months() const {
    const int count = std::min({12, static_cast<int>(months_.size()), std::max(1, (width() - 140) / 85)});
    QJsonArray displayed;
    for (int i = static_cast<int>(months_.size()) - count; i < months_.size(); ++i)
        displayed.append(months_[i]);
    return displayed;
}

QRectF EtfMonthlyChart::plot_rect() const {
    return QRectF(120, 95, std::max(1, width() - 130), std::max(0, height() - 180));
}

void EtfMonthlyChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const auto& t = ui::ThemeManager::instance().tokens();
    p.fillRect(rect(), QColor(t.bg_surface));
    p.setPen(QColor(t.text_primary));
    const auto displayed = displayed_months();
    const int count = static_cast<int>(displayed.size());
    p.drawText(QRect(10, 5, width() - 20, 25), Qt::AlignLeft,
               tr("SEC N-PORT · monthly USD · observed subset · latest %1 months").arg(count));
    p.drawText(
        QRect(10, 30, width() - 20, 36), Qt::AlignLeft | Qt::TextWordWrap,
        tr("Above zero: net creations · below zero: net redemptions · hatch: partial · revised inputs labelled"));
    if (count == 0) {
        p.drawText(rect(), Qt::AlignCenter, tr("No regulatory months in the selected classification interval"));
        return;
    }
    double scale = 0.0;
    for (const auto& month : displayed) {
        const auto v = month.toObject().value("observed_net_flow_usd");
        if (v.isDouble() && std::isfinite(v.toDouble()))
            scale = std::max(scale, std::abs(v.toDouble()));
    }
    const auto plot = plot_rect();
    const double step = plot.width() / count;
    const double mid = plot.center().y();
    const double half = plot.height() / 2.0;
    if (scale == 0.0) {
        bool any_value = false;
        for (const auto& value : displayed)
            any_value = any_value || value.toObject().value("observed_net_flow_usd").isDouble();
        if (!any_value)
            p.drawText(QRect(10, 68, width() - 20, 30), Qt::AlignCenter,
                       tr("No measured regulatory flow in this history · missing is not zero"));
    }
    p.setPen(QColor(t.border_med));
    p.drawLine(QPointF(plot.left(), mid), QPointF(plot.right(), mid));
    QFont labels = p.font();
    labels.setPixelSize(10);
    p.setFont(labels);
    p.setPen(QColor(t.text_secondary));
    p.drawText(QRectF(5, mid - 10, 110, 20), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("USD 0.00"));
    if (scale > 0.0) {
        p.drawText(QRectF(5, plot.top() - 10, 110, 20), Qt::AlignRight | Qt::AlignVCenter, etf_ui::compact_usd(scale));
        p.drawText(QRectF(5, plot.bottom() - 10, 110, 20), Qt::AlignRight | Qt::AlignVCenter,
                   etf_ui::compact_usd(-scale));
    }
    for (int j = 0; j < count; ++j) {
        const auto month = displayed[j].toObject();
        const auto v = month.value("observed_net_flow_usd");
        const double x = plot.left() + j * step;
        const bool present = v.isDouble() && std::isfinite(v.toDouble());
        const bool partial = !month.value("complete_net_flow_usd").isDouble();
        if (present) {
            const double h = scale > 0.0 ? v.toDouble() / scale * half : 0.0;
            QBrush brush(QColor(t.accent), partial ? Qt::BDiagPattern : Qt::SolidPattern);
            p.setBrush(brush);
            p.setPen(QColor(t.accent));
            p.drawRect(QRectF(x + step * 0.2, h >= 0.0 ? mid - h : mid, step * 0.6, std::max(1.0, std::abs(h))));
            p.setPen(QColor(t.text_primary));
            p.drawText(QRectF(x, h >= 0.0 ? mid - h - 21 : mid + std::abs(h) + 2, step, 18), Qt::AlignCenter,
                       etf_ui::compact_usd(v.toDouble()));
        }
        p.setPen(QColor(t.text_primary));
        p.drawText(QRectF(x, height() - 46.0, step, 20), Qt::AlignCenter, month.value("month").toString());
        const bool revised = month.value("has_revised_inputs").toBool() ||
                             month.value("quality").toString().compare("REVISED", Qt::CaseInsensitive) == 0;
        const QString state = !present              ? tr("Missing")
                              : v.toDouble() == 0.0 ? (revised ? tr("Zero · rev.") : tr("Zero"))
                              : revised             ? (partial ? tr("Partial · rev.") : tr("Revised"))
                              : partial             ? tr("Partial")
                                                    : tr("Measured");
        p.drawText(QRectF(x, height() - 26.0, step, 20), Qt::AlignCenter, state);
    }
}
} // namespace fincept::screens
