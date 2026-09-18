// src/screens/economics/panels/IlostatSeriesData.h
//
// ILOSTAT-specific normalisation and result-shape rules for the shared
// chart-first economics path. The SDMX long-format payload can describe more
// than one country, and its CSV parser emits TIME_PERIOD as a JSON number, so
// the two pure rules are kept header-only (Qt Core) and unit-tested:
//   * a numeric period becomes its exact text form — the shared series contract
//     names periods as text, and the rendered cell and CSV text are unchanged;
//   * a result is one ordinary historical series only when every row names the
//     same REF_AREA and no period repeats in it. Multi-country lists and ALL
//     stay tabular because a single line cannot represent them truthfully.
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <cmath>

namespace fincept::screens {

/// Copy `rows`, converting a numeric TIME_PERIOD to its exact text form.
/// String periods and every other field are preserved unchanged.
inline QJsonArray ilostat_normalize_periods(const QJsonArray& rows) {
    QJsonArray out = rows;
    for (int i = 0; i < out.size(); ++i) {
        const QJsonObject row = out[i].toObject();
        const QJsonValue period = row.value(QStringLiteral("TIME_PERIOD"));
        if (!period.isDouble())
            continue;
        QJsonObject normalized = row;
        const double value = period.toDouble();
        if (value == std::floor(value) && std::abs(value) < 1e15)
            normalized[QStringLiteral("TIME_PERIOD")] = QString::number(static_cast<qint64>(value));
        else
            normalized[QStringLiteral("TIME_PERIOD")] = QString::number(value, 'g', 10);
        out[i] = normalized;
    }
    return out;
}

/// REF_AREA of the one country this result represents, or an empty string when
/// it is not one ordinary series: empty rows, any row without a REF_AREA, more
/// than one distinct REF_AREA, or a repeated TIME_PERIOD (which would draw
/// several observations on the same date).
inline QString ilostat_single_series_area(const QJsonArray& rows) {
    if (rows.isEmpty())
        return {};
    QString area;
    QSet<QString> periods;
    for (const auto& value : rows) {
        const QJsonObject row = value.toObject();
        const QString row_area = row.value(QStringLiteral("REF_AREA")).toString();
        if (row_area.isEmpty())
            return {};
        if (area.isEmpty())
            area = row_area;
        else if (area != row_area)
            return {};
        const QString period = row.value(QStringLiteral("TIME_PERIOD")).toString();
        if (period.isEmpty() || periods.contains(period))
            return {};
        periods.insert(period);
    }
    return area;
}

} // namespace fincept::screens
