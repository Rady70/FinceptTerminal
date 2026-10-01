#include "screens/economics/panels/FedWatchCurrentChart.h"

#include "screens/economics/panels/FedWatchViewModel.h"
#include "ui/theme/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace fincept::screens {
namespace {
const QStringList fields{"fed_probability_pct", "polymarket_probability_pct"};
const QStringList sources{"Fed-side", "Polymarket"};
bool available(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 && value.toDouble() <= 100;
}
QString exact_value(const QJsonValue& value) {
    return available(value) ? QString::number(value.toDouble(), 'g', 12) + "%" : QStringLiteral("Unavailable");
}
} // namespace
FedWatchCurrentChart::FedWatchCurrentChart(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(300);
    setMouseTracking(true);
    setAccessibleName(tr("Current probabilities by outcome"));
}
void FedWatchCurrentChart::set_rows(const QList<QJsonObject>& rows) {
    rows_ = rows;
    QStringList descriptions;
    for (const auto& row : rows)
        descriptions << fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool()) + ": Fed-side " +
                            exact_value(row[fields[0]]) + "; Polymarket " + exact_value(row[fields[1]]);
    setAccessibleDescription(tr("Probability scale 0–100%. ") + descriptions.join(" | "));
    update();
}
QVector<FedWatchCurrentChart::Bar> FedWatchCurrentChart::bars() const {
    QVector<Bar> result;
    for (const auto& row : rows_)
        for (int source = 0; source < 2; ++source)
            if (available(row[fields[source]]))
                result.push_back(
                    {row["outcome_bp"].toInt(), row["open_ended"].toBool(), source, row[fields[source]].toDouble()});
    return result;
}
QColor FedWatchCurrentChart::series_color(int source) const {
    const auto& colors = ui::ThemeManager::instance().tokens().chart_colors;
    return QColor(source < colors.size() ? colors[source] : ui::colors::TEXT_SECONDARY());
}
void FedWatchCurrentChart::paintEvent(QPaintEvent*) {
    using namespace ui::colors;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(BG_SURFACE()));
    hits_.clear();
    if (rows_.isEmpty()) {
        painter.setPen(QColor(TEXT_SECONDARY()));
        painter.drawText(rect(), Qt::AlignCenter, tr("No current distribution available"));
        return;
    }
    for (int source = 0; source < 2; ++source) {
        painter.fillRect(QRectF(14 + source * 135, 14, 12, 12), series_color(source));
        painter.setPen(QColor(TEXT_PRIMARY()));
        painter.drawText(QRectF(32 + source * 135, 10, 100, 22), sources[source]);
    }
    const QRectF plot(48, 52, qMax(1, width() - 62), qMax(1, height() - 140));
    painter.setPen(QColor(TEXT_SECONDARY()));
    painter.drawText(QRectF(8, 30, 150, 20), tr("Probability %"));
    for (int tick = 0; tick <= 100; tick += 25) {
        const double y = plot.bottom() - plot.height() * tick / 100;
        painter.setPen(QColor(BORDER_DIM()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(QColor(TEXT_SECONDARY()));
        painter.drawText(QRectF(2, y - 10, 40, 20), Qt::AlignRight, QString::number(tick));
    }
    const double group_width = plot.width() / rows_.size();
    const double bar_width = qMin(44.0, group_width * 0.28);
    for (int index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        const double center = plot.left() + (index + 0.5) * group_width;
        const QString label = fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool());
        painter.setPen(QColor(TEXT_PRIMARY()));
        painter.drawText(QRectF(center - group_width / 2, plot.bottom() + 30, group_width, 52),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, label);
        for (int source = 0; source < 2; ++source) {
            const auto value = row[fields[source]];
            const double x = center + (source == 0 ? -bar_width - 2 : 2);
            const QString detail = label + "\n" + sources[source] + ": " + exact_value(value);
            hits_.push_back({QRectF(x, plot.top(), bar_width, plot.height() + 25), detail});
            if (!available(value)) {
                painter.setPen(QColor(TEXT_TERTIARY()));
                painter.drawText(QRectF(x, plot.bottom() + 4, bar_width, 20), Qt::AlignCenter, tr("N/A"));
                continue;
            }
            const double height = plot.height() * value.toDouble() / 100;
            painter.fillRect(QRectF(x, plot.bottom() - height, bar_width, height), series_color(source));
            if (value.toDouble() == 0) {
                painter.setPen(series_color(source));
                painter.drawLine(QPointF(x, plot.bottom()), QPointF(x + bar_width, plot.bottom()));
            }
        }
    }
}
void FedWatchCurrentChart::mouseMoveEvent(QMouseEvent* event) {
    for (const auto& hit : hits_)
        if (hit.first.contains(event->position())) {
            QToolTip::showText(event->globalPosition().toPoint(), hit.second, this);
            return;
        }
    QToolTip::hideText();
}
void FedWatchCurrentChart::leaveEvent(QEvent*) {
    QToolTip::hideText();
}
} // namespace fincept::screens
