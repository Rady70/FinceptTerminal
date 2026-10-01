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
        int ready = 0, stale = 0, missing = 0, excluded = 0;
        for (const auto& entry : group.value("rotation_constituents").toArray()) {
            const auto status = entry.toObject().value("status").toString();
            if (status == "component_only")
                ++ready;
            else if (status == "stale")
                ++stale;
            else if (status == "excluded")
                ++excluded;
            else
                ++missing;
        }
        text(p, QRect(x, r.y() + 100, w, 33),
             excluded > 0 && ready + stale + missing == 0
                 ? tr("Rotation: excluded by policy")
                 : tr("Rotation: %1 snapshots · %2 stale · %3 missing").arg(ready).arg(stale).arg(missing),
             12, QColor(t.text_primary));
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

EtfSessionChart::EtfSessionChart(QWidget* parent) : QWidget(parent) {
    setObjectName("etfSessionChart");
    setMinimumHeight(370);
    setAccessibleName(tr("Individual market-rotation session histories, no composite score"));
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this] { update(); });
}
void EtfSessionChart::set_sessions(const QJsonArray& sessions) {
    sessions_ = sessions;
    QStringList tips;
    for (const auto& entry : sessions_) {
        const auto session = entry.toObject();
        for (const auto& field :
             {"price_return_21", "trend_efficiency_21", "return_acceleration_21", "volume_ratio_5_63"}) {
            const auto value = session.value("values").toObject().value(field).toObject();
            tips.append(session.value("session").toString() + " · " + etf_ui::component_label(field) + " · " +
                        etf_ui::component(value) + " · " + etf_ui::label(value.value("state").toString()));
        }
    }
    setToolTip(tips.join('\n'));
    setAccessibleDescription(tips.join('\n'));
    update();
}
void EtfSessionChart::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    setMinimumHeight(width() < 700 ? 700 : 370);
}
void EtfSessionChart::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto& t = ui::ThemeManager::instance().tokens();
    p.fillRect(rect(), QColor(t.bg_surface));
    const QStringList fields{"price_return_21", "trend_efficiency_21", "return_acceleration_21", "volume_ratio_5_63"};
    const QStringList titles{tr("21-session price return"), tr("21-session trend efficiency"),
                             tr("21-session return acceleration"), tr("Volume ratio · 5 / 63 sessions")};
    const int columns = width() < 700 ? 1 : 2, rows = 4 / columns;
    for (int panel = 0; panel < 4; ++panel) {
        const QRect box((panel % columns) * width() / columns + 8, (panel / columns) * height() / rows + 5,
                        width() / columns - 16, height() / rows - 10);
        p.setPen(QColor(t.border_med));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box, 4, 4);
        text(p, box.adjusted(10, 4, -10, -box.height() + 28), titles[panel], 14, QColor(t.text_secondary));
        double low = 0, high = 0;
        bool have = false;
        QJsonObject latest;
        for (const auto& entry : sessions_) {
            const auto v = entry.toObject().value("values").toObject().value(fields[panel]).toObject();
            latest = v;
            if (!v.value("value").isDouble())
                continue;
            const double n = v.value("value").toDouble();
            if (!have)
                low = high = n;
            else {
                low = std::min(low, n);
                high = std::max(high, n);
            }
            have = true;
        }
        text(p, QRect(box.x() + 10, box.y() + 31, box.width() - 20, 26), etf_ui::component(latest), 18,
             QColor(t.text_primary));
        const QRect plot = box.adjusted(12, 66, -12, -30);
        if (!have || sessions_.size() < 2) {
            text(p, plot, tr("Component history unavailable"), 14, QColor(t.text_secondary));
            continue;
        }
        if (low == high) {
            low -= 0.5;
            high += 0.5;
        }
        QPainterPath path;
        bool connected = false;
        for (int i = 0; i < sessions_.size(); ++i) {
            const auto v = sessions_[i].toObject().value("values").toObject().value(fields[panel]).toObject();
            if (!v.value("value").isDouble()) {
                connected = false;
                continue;
            }
            const QPointF point(plot.x() + static_cast<double>(i) * plot.width() / (sessions_.size() - 1),
                                plot.bottom() - (v.value("value").toDouble() - low) / (high - low) * plot.height());
            if (connected)
                path.lineTo(point);
            else
                path.moveTo(point);
            connected = true;
        }
        text(p, QRect(plot.x(), plot.y() - 15, plot.width(), 14),
             tr("Range: %1 to %2 · independent component axis")
                 .arg(QString::number(low, 'g', 4), QString::number(high, 'g', 4)),
             10, QColor(t.text_secondary));
        p.setPen(QPen(QColor(t.accent), 1.5));
        p.drawPath(path);
        text(p, QRect(plot.x(), box.bottom() - 26, plot.width(), 21),
             sessions_.first().toObject().value("session").toString() + " → " +
                 sessions_.last().toObject().value("session").toString() + tr(" · exchange sessions"),
             11, QColor(t.text_secondary));
    }
}
} // namespace fincept::screens
