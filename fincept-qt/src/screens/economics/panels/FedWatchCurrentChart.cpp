#include "screens/economics/panels/FedWatchCurrentChart.h"

#include "screens/economics/panels/FedWatchViewModel.h"
#include "ui/theme/Theme.h"

#include <QKeyEvent>
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
    setMinimumHeight(160);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
    setAccessibleName(tr("Current probabilities by outcome"));
}
void FedWatchCurrentChart::set_rows(const QList<QJsonObject>& rows) {
    rows_.clear();
    for (const auto& row : rows)
        if (available(row[fields[0]]) || available(row[fields[1]]))
            rows_.push_back(row);
    setFixedHeight(72 + rows_.size() * 42);
    QStringList descriptions;
    for (const auto& row : rows_)
        descriptions << fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool()) + ": Fed-side " +
                            exact_value(row[fields[0]]) + "; Polymarket " + exact_value(row[fields[1]]);
    setAccessibleDescription(
        tr("Probability scale 0–100%. Select a category to view history; use Up/Down and Enter. ") +
        descriptions.join(" | "));
    categories_.clear();
    hits_.clear();
    update();
}
void FedWatchCurrentChart::set_selected_outcome(int bp, bool open) {
    selected_bp_ = bp;
    selected_open_ = open;
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
    categories_.clear();
    if (rows_.isEmpty()) {
        painter.setPen(QColor(TEXT_SECONDARY()));
        painter.drawText(rect(), Qt::AlignCenter, tr("No current distribution available"));
        return;
    }
    const double label_width = qMin(150.0, width() * 0.25);
    const double column_width = (width() - label_width - 16) / 2;
    const double plot_width = qMax(1.0, column_width - 64);
    for (int source = 0; source < 2; ++source) {
        const double left = label_width + source * column_width;
        painter.fillRect(QRectF(left, 14, 12, 12), series_color(source));
        painter.setPen(QColor(TEXT_PRIMARY()));
        painter.drawText(QRectF(left + 18, 10, column_width - 18, 22), sources[source]);
        for (int tick = 0; tick <= 100; tick += 25) {
            const double x = left + plot_width * tick / 100;
            painter.setPen(QColor(BORDER_DIM()));
            painter.drawLine(QPointF(x, 42), QPointF(x, height() - 28));
            painter.setPen(QColor(TEXT_SECONDARY()));
            painter.drawText(QRectF(x - 18, height() - 24, 36, 20), Qt::AlignCenter, QString::number(tick) + "%");
        }
    }
    for (int index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_[index];
        const double y = 42 + index * 42;
        const QRectF category(2, y, width() - 4, 40);
        categories_.push_back(category);
        if (selected_bp_ == row["outcome_bp"].toInt() && selected_open_ == row["open_ended"].toBool()) {
            painter.setPen(QColor(BORDER_BRIGHT()));
            painter.drawRoundedRect(category, 3, 3);
        }
        const QString label = fedwatch::outcome_label(row["outcome_bp"].toInt(), row["open_ended"].toBool());
        painter.setPen(QColor(TEXT_PRIMARY()));
        painter.drawText(QRectF(8, y, label_width - 16, 40), Qt::AlignVCenter | Qt::TextWordWrap, label);
        for (int source = 0; source < 2; ++source) {
            const auto value = row[fields[source]];
            if (!available(value))
                continue;
            const double x = label_width + source * column_width;
            const QString detail = label + "\n" + sources[source] + ": " + exact_value(value);
            hits_.push_back({QRectF(x, y, column_width, 40), detail});
            const double length = plot_width * value.toDouble() / 100;
            painter.fillRect(QRectF(x, y + 12, length, 16), series_color(source));
            if (value.toDouble() == 0) {
                painter.setPen(series_color(source));
                painter.drawLine(QPointF(x, y + 12), QPointF(x, y + 28));
            }
            painter.setPen(QColor(TEXT_PRIMARY()));
            painter.drawText(QRectF(x + length + 4, y, column_width - length - 4, 40), Qt::AlignVCenter,
                             QString::number(value.toDouble(), 'f', 1) + "%");
        }
    }
}
void FedWatchCurrentChart::select_row(int index) {
    if (index < 0 || index >= rows_.size())
        return;
    set_selected_outcome(rows_[index]["outcome_bp"].toInt(), rows_[index]["open_ended"].toBool());
    emit outcome_selected(selected_bp_, selected_open_);
}
void FedWatchCurrentChart::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        for (int index = 0; index < categories_.size(); ++index)
            if (categories_[index].contains(event->position())) {
                select_row(index);
                return;
            }
    QWidget::mousePressEvent(event);
}
void FedWatchCurrentChart::keyPressEvent(QKeyEvent* event) {
    int index = 0;
    for (int i = 0; i < rows_.size(); ++i)
        if (rows_[i]["outcome_bp"].toInt() == selected_bp_ && rows_[i]["open_ended"].toBool() == selected_open_)
            index = i;
    if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up)
        select_row(qBound(0, index + (event->key() == Qt::Key_Down ? 1 : -1), qMax(0, rows_.size() - 1)));
    else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Space)
        select_row(index);
    else
        QWidget::keyPressEvent(event);
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
