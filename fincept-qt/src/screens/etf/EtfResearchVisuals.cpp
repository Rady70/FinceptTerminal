#include "screens/etf/EtfResearchVisuals.h"

#include "screens/etf/EtfPresentation.h"
#include "ui/theme/ThemeManager.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include <algorithm>
#include <cmath>

namespace fincept::screens {
namespace {
QString amount(const QJsonValue& value) {
    if (!value.isDouble())
        return QStringLiteral("Flow unavailable");
    const double n = value.toDouble();
    if (std::abs(n) >= 1e9)
        return QStringLiteral("USD %1 bn").arg(n / 1e9, 0, 'f', 2);
    if (std::abs(n) >= 1e6)
        return QStringLiteral("USD %1 m").arg(n / 1e6, 0, 'f', 2);
    return etf_ui::number(value, "USD");
}
void text(QPainter& painter, const QRect& rect, const QString& value, int size, const QColor& color) {
    QFont font = painter.font();
    font.setPixelSize(size);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, value);
}
} // namespace
EtfGroupBoard::EtfGroupBoard(QWidget* parent) : QWidget(parent) {
    setObjectName("etfGroupBoard");
    setAccessibleName(tr("ETF hierarchy and selected-month regulatory flow"));
    setFocusPolicy(Qt::StrongFocus);
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this] { update(); });
}
void EtfGroupBoard::set_groups(const QJsonArray& groups, const QString& month) {
    groups_ = groups;
    month_ = month;
    focus_index_ = 0;
    QStringList description;
    for (const auto& g : groups_) {
        const auto group = g.toObject();
        const auto m = month_for(group);
        description.append(etf_ui::label(group.value("group_id").toString()) + " · " + month_ + " · " +
                           etf_ui::number(m.value("observed_net_flow_usd"), "USD") + " · " + etf_ui::quality(m) +
                           " · " + etf_ui::coverage(m) + " · " + etf_ui::rotation_availability(group));
    }
    setAccessibleDescription(description.join('\n'));
    setToolTip(description.join('\n'));
    fit_height();
    update();
}
void EtfGroupBoard::set_selected(const QString& group) {
    selected_ = group;
    update();
}
QJsonObject EtfGroupBoard::month_for(const QJsonObject& group) const {
    for (const auto& m : group.value("regulatory_months").toArray())
        if (m.toObject().value("month").toString() == month_)
            return m.toObject();
    return {};
}
QRect EtfGroupBoard::card_rect(int index) const {
    const int columns = std::min(std::max(1, static_cast<int>(groups_.size())), width() >= 960   ? 3
                                                                                : width() >= 560 ? 2
                                                                                                 : 1);
    const int step = width() / columns;
    return QRect((index % columns) * step + 5, (index / columns) * 148 + 5, step - 10, 138);
}
void EtfGroupBoard::fit_height() {
    const int columns = std::min(std::max(1, static_cast<int>(groups_.size())), width() >= 960   ? 3
                                                                                : width() >= 560 ? 2
                                                                                                 : 1);
    setFixedHeight(std::max(1, (static_cast<int>(groups_.size()) + columns - 1) / columns) * 148);
}
void EtfGroupBoard::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    fit_height();
}
void EtfGroupBoard::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& t = ui::ThemeManager::instance().tokens();
    p.fillRect(rect(), QColor(t.bg_base));
    double scale = 0;
    for (const auto& g : groups_)
        scale = std::max(scale, std::abs(month_for(g.toObject()).value("observed_net_flow_usd").toDouble()));
    for (int i = 0; i < groups_.size(); ++i) {
        const auto group = groups_[i].toObject();
        const auto m = month_for(group);
        const auto r = card_rect(i);
        p.setPen(QPen(QColor(selected_ == group.value("group_id").toString() ? t.accent : t.border_med), 1));
        p.setBrush(QColor(t.bg_surface));
        p.drawRoundedRect(r, 5, 5);
        const int x = r.x() + 12, w = r.width() - 24;
        text(p, QRect(x, r.y() + 7, w, 22), etf_ui::label(group.value("group_id").toString()), 17,
             QColor(t.text_primary));
        text(p, QRect(x, r.y() + 30, w, 29), amount(m.value("observed_net_flow_usd")), 22, QColor(t.text_primary));
        const QString state = m.isEmpty() ? tr("Historical classification unavailable") : etf_ui::quality(m);
        text(p, QRect(x, r.y() + 61, w, 20),
             month_ + " · " + state +
                 (m.value("complete_net_flow_usd").isDouble() ? tr(" · complete") : tr(" · observed subset")),
             12, QColor(t.text_secondary));
        const auto value = m.value("observed_net_flow_usd");
        const int axis = x + w / 2;
        p.setPen(QColor(t.border_med));
        p.drawLine(x, r.y() + 88, x + w, r.y() + 88);
        if (value.isDouble()) {
            const double len = scale > 0 ? value.toDouble() / scale * (w / 2.0 - 3) : 0;
            p.setPen(QColor(t.accent));
            p.setBrush(QBrush(QColor(t.accent),
                              m.value("complete_net_flow_usd").isDouble() ? Qt::SolidPattern : Qt::BDiagPattern));
            p.drawRect(QRectF(len < 0 ? axis + len : axis, r.y() + 82, std::max(1.0, std::abs(len)), 12));
        }
        text(p, QRect(x, r.y() + 100, w, 33), rotation_summary(group), 12, QColor(t.text_primary));
    }
    if (groups_.isEmpty())
        text(p, rect().adjusted(15, 10, -15, -10),
             tr("Choose an ETF research view. Missing observations remain unavailable."), 16, QColor(t.text_secondary));
}
void EtfGroupBoard::mousePressEvent(QMouseEvent* e) {
    if (!isEnabled())
        return;
    for (int i = 0; i < groups_.size(); ++i)
        if (card_rect(i).contains(e->position().toPoint())) {
            focus_index_ = i;
            emit group_activated(groups_[i].toObject().value("group_id").toString());
            break;
        }
}
void EtfGroupBoard::keyPressEvent(QKeyEvent* e) {
    if (groups_.isEmpty())
        return;
    if (e->key() == Qt::Key_Right || e->key() == Qt::Key_Down)
        focus_index_ = std::min(focus_index_ + 1, static_cast<int>(groups_.size()) - 1);
    else if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Up)
        focus_index_ = std::max(0, focus_index_ - 1);
    else if (e->key() != Qt::Key_Return && e->key() != Qt::Key_Enter) {
        QWidget::keyPressEvent(e);
        return;
    }
    emit group_activated(groups_[focus_index_].toObject().value("group_id").toString());
}

QString EtfGroupBoard::rotation_summary(const QJsonObject& group) const {
    return etf_ui::rotation_availability(group);
}

EtfSessionChart::EtfSessionChart(QWidget* parent) : QWidget(parent) {
    setObjectName("etfSessionChart");
    setAccessibleName(tr("Individual market-rotation session histories, independent axes, no composite score"));
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this] { update(); });
    set_sessions({});
}
void EtfSessionChart::set_sessions(const QJsonArray& sessions) {
    sessions_ = sessions;
    panels_.clear();
    const QStringList fields{"price_return_21", "trend_efficiency_21", "return_acceleration_21", "volume_ratio_5_63"};
    const QStringList units{"price_return_ratio", "efficiency_ratio_minus1_1", "price_return_difference",
                            "self_relative_volume_ratio"};
    const QStringList references{tr("0%: flat price return"), tr("0: neutral efficiency (-1 to +1)"),
                                 tr("0 pp: unchanged return"), tr("1.00x: recent volume equals baseline")};
    QStringList descriptions;
    for (int panel = 0; panel < fields.size(); ++panel) {
        double observed_min = 0, observed_max = 0;
        int count = 0;
        QJsonObject latest;
        for (const auto& entry : sessions_) {
            const auto value = entry.toObject().value("values").toObject().value(fields[panel]).toObject();
            latest = value;
            const auto number = value.value("value");
            if (!number.isDouble() || !std::isfinite(number.toDouble()))
                continue;
            const double n = number.toDouble();
            if (count == 0)
                observed_min = observed_max = n;
            else {
                observed_min = std::min(observed_min, n);
                observed_max = std::max(observed_max, n);
            }
            ++count;
        }
        const double reference = panel == 3 ? 1.0 : 0.0;
        double plot_min = std::min(observed_min, reference), plot_max = std::max(observed_max, reference);
        if (panel == 1) {
            // The qualified efficiency scale is fixed; observed data stays separate.
            plot_min = -1.0;
            plot_max = 1.0;
        } else if (plot_min == plot_max) {
            const double padding = std::max(0.01, std::abs(plot_min) * 0.05);
            plot_min -= padding;
            plot_max += padding;
        }
        const QString range =
            count > 0 ? tr("Observed: %1 to %2")
                            .arg(etf_ui::number(observed_min, units[panel]), etf_ui::number(observed_max, units[panel]))
                      : tr("Observed range unavailable");
        const QString value = etf_ui::number(latest.value("value"), units[panel]);
        panels_.append(QJsonObject{{"field", fields[panel]},
                                   {"units", units[panel]},
                                   {"observed_min", count > 0 ? QJsonValue(observed_min) : QJsonValue()},
                                   {"observed_max", count > 0 ? QJsonValue(observed_max) : QJsonValue()},
                                   {"plot_min", plot_min},
                                   {"plot_max", plot_max},
                                   {"reference", reference},
                                   {"reference_label", references[panel]},
                                   {"value_label", value},
                                   {"range_label", range},
                                   {"observations", count}});
        descriptions.append(etf_ui::component_label(fields[panel]) + ": " + value + "; " + range + "; " +
                            references[panel]);
    }
    // Summary only: full input/availability history belongs to lazy exact detail.
    setToolTip(descriptions.join('\n'));
    setAccessibleDescription(descriptions.join('\n'));
    fit_height();
    update();
}
QJsonObject EtfSessionChart::panel_state(int index) const {
    return index >= 0 && index < panels_.size() ? panels_[index] : QJsonObject{};
}
QRect EtfSessionChart::panel_plot_rect(int index) const {
    const int columns = width() < 700 ? 1 : 2, rows = 4 / columns;
    return QRect((index % columns) * width() / columns + 8, (index / columns) * height() / rows + 5,
                 width() / columns - 16, height() / rows - 10)
        .adjusted(12, 82, -12, -48);
}
void EtfSessionChart::fit_height() {
    bool measured = false;
    for (const auto& panel : panels_)
        measured = measured || panel.value("observations").toInt() > 0;
    setMinimumHeight(measured ? (width() < 700 ? 840 : 420) : (width() < 700 ? 400 : 200));
    setMaximumHeight(measured ? QWIDGETSIZE_MAX : minimumHeight());
}
void EtfSessionChart::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    fit_height();
}
void EtfSessionChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& t = ui::ThemeManager::instance().tokens();
    p.fillRect(rect(), QColor(t.bg_surface));
    const QStringList titles{tr("21-session price return"), tr("21-session trend efficiency (-1 to +1)"),
                             tr("21-session return acceleration (percentage points)"),
                             tr("Volume ratio: 5 / 63 sessions")};
    const int columns = width() < 700 ? 1 : 2, rows = 4 / columns;
    for (int panel = 0; panel < panels_.size(); ++panel) {
        const auto state = panel_state(panel);
        const QRect box((panel % columns) * width() / columns + 8, (panel / columns) * height() / rows + 5,
                        width() / columns - 16, height() / rows - 10);
        p.setPen(QColor(t.border_med));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box, 4, 4);
        text(p, box.adjusted(10, 4, -10, -box.height() + 28), titles[panel], 13, QColor(t.text_secondary));
        text(p, QRect(box.x() + 10, box.y() + 30, box.width() - 20, 25), state.value("value_label").toString(), 18,
             QColor(t.text_primary));
        text(p, QRect(box.x() + 10, box.y() + 55, box.width() - 20, 19), state.value("range_label").toString(), 11,
             QColor(t.text_secondary));
        if (state.value("observations").toInt() == 0)
            continue;
        const QRect plot = panel_plot_rect(panel);
        const double low = state.value("plot_min").toDouble(), high = state.value("plot_max").toDouble();
        const auto y_for = [&plot, low, high](double n) {
            return plot.bottom() - (n - low) / (high - low) * plot.height();
        };
        p.setPen(QPen(QColor(t.text_secondary), 1, Qt::DashLine));
        p.drawLine(QPointF(plot.left(), y_for(state.value("reference").toDouble())),
                   QPointF(plot.right(), y_for(state.value("reference").toDouble())));
        QPainterPath path;
        bool connected = false;
        for (int i = 0; i < sessions_.size(); ++i) {
            const auto value = sessions_[i]
                                   .toObject()
                                   .value("values")
                                   .toObject()
                                   .value(state.value("field").toString())
                                   .toObject()
                                   .value("value");
            if (!value.isDouble() || !std::isfinite(value.toDouble())) {
                connected = false;
                continue;
            }
            const QPointF point(plot.x() + static_cast<double>(i) * plot.width() /
                                               std::max(1, static_cast<int>(sessions_.size()) - 1),
                                y_for(value.toDouble()));
            if (connected)
                path.lineTo(point);
            else
                path.moveTo(point);
            connected = true;
            p.setPen(QPen(QColor(t.accent), 1.5));
            p.drawEllipse(point, 1.5, 1.5);
        }
        p.setPen(QPen(QColor(t.accent), 1.5));
        p.drawPath(path);
        text(p, QRect(plot.x(), box.bottom() - 44, plot.width(), 19), state.value("reference_label").toString(), 10,
             QColor(t.text_secondary));
        text(p, QRect(plot.x(), box.bottom() - 24, plot.width(), 19),
             sessions_.first().toObject().value("session").toString() + " to " +
                 sessions_.last().toObject().value("session").toString() + tr(" : exchange sessions"),
             11, QColor(t.text_secondary));
    }
}
} // namespace fincept::screens
