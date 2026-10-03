#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTimeZone>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace fincept::screens::fedwatch {
inline QString number(const QJsonValue& value, const QString& unit = {}) {
    return value.isDouble() && std::isfinite(value.toDouble()) ? QString::number(value.toDouble(), 'f', 2) + unit
                                                               : QStringLiteral("Unavailable");
}
inline QString outcome_label(int bp, bool open) {
    return QString("%1%2 bp%3")
        .arg(bp > 0 ? "+" : "")
        .arg(bp)
        .arg(open ? (bp < 0 ? " or less" : " or more") : " (exact)");
}
struct Point {
    QDateTime instant;
    double value;
    QString detail;
};
struct Series {
    QString label;
    QVector<Point> points;
};
// Display-only coverage and scale. Never add points or alter backend values.
inline QString history_coverage(const QVector<Point>& points) {
    const auto count = points.size();
    QString text = count == 0   ? QStringLiteral("No observations in this range")
                   : count == 1 ? QStringLiteral("1 observation retained")
                   : count < 7  ? QStringLiteral("Limited history: %1 observations").arg(count)
                                : QStringLiteral("%1 observations retained").arg(count);
    if (count > 1) {
        QSet<QDate> dates;
        for (const auto& point : points)
            dates.insert(point.instant.toUTC().date());
        const auto bounds = std::minmax_element(dates.begin(), dates.end());
        const auto missing = bounds.first->daysTo(*bounds.second) + 1 - dates.size();
        if (missing > 0)
            text += QStringLiteral(" · %1 missing UTC days (line breaks)").arg(missing);
    }
    return text;
}
inline QPair<double, double> shared_probability_bounds(const QVector<Series>& series) {
    double low = 100, high = 0;
    bool observed = false;
    for (const auto& source : series)
        for (const auto& point : source.points) {
            low = std::min(low, point.value);
            high = std::max(high, point.value);
            observed = true;
        }
    if (!observed)
        return {0, 100};
    const double padding = std::max(0.5, (high - low) * 0.15);
    return {std::max(0.0, low - padding), std::min(100.0, high + padding)};
}
inline bool adjacent_observations(const Point& previous, const Point& current) {
    const auto days = previous.instant.toUTC().date().daysTo(current.instant.toUTC().date());
    return days >= 0 && days <= 1;
}
inline QString source_label(QString method) {
    method.replace("LIVE_INVESTING_DERIVED", "Investing-derived");
    method.replace("HISTORICAL_ZQ_RECONSTRUCTED", "ZQ reconstructed");
    method.replace("POLYMARKET_CLOB", "Polymarket");
    return method;
}
inline QVector<Point> points(const QJsonArray& rows, const QString& time_key, const QString& value_key) {
    QVector<Point> result;
    for (const auto& item : rows) {
        const auto row = item.toObject();
        auto time = time_key == "date" ? QDateTime(QDate::fromString(row[time_key].toString(), Qt::ISODate),
                                                   QTime(0, 0), QTimeZone::UTC)
                                       : QDateTime::fromString(row[time_key].toString(), Qt::ISODate);
        const auto value = row[value_key];
        if (time.isValid() && value.isDouble() && std::isfinite(value.toDouble()))
            result.push_back({time.toUTC(), value.toDouble(),
                              row[time_key].toString() + " · " + QString::number(value.toDouble(), 'g', 12) +
                                  (time_key == "date" ? " pp" : "%") + " · " + row["quality_status"].toString()});
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.instant < b.instant; });
    return result;
}
inline QVector<Series> filter_range(QVector<Series> series, int days, QDateTime anchor = {}) {
    if (days <= 0)
        return series;
    QDateTime latest = anchor;
    for (const auto& s : series)
        for (const auto& p : s.points)
            if (!anchor.isValid() && (!latest.isValid() || p.instant > latest))
                latest = p.instant;
    const auto start = latest.addDays(-days);
    for (auto& s : series)
        s.points.erase(
            std::remove_if(s.points.begin(), s.points.end(), [&](const auto& p) { return p.instant < start; }),
            s.points.end());
    return series;
}
} // namespace fincept::screens::fedwatch
