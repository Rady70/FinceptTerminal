// Presentation only: read finalized Batch C/D fields, never compute research.
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QString>

#include <cmath>

namespace fincept::screens::etf_ui {

inline QString label(QString id) {
    if (id.isEmpty())
        return QStringLiteral("Unavailable");
    if (id == QLatin1String("sp500"))
        return QStringLiteral("S&P 500");
    if (id == id.toUpper())
        id = id.toLower();
    id.replace(QLatin1Char('_'), QLatin1Char(' '));
    id[0] = id[0].toUpper();
    return id;
}

inline QString number(const QJsonValue& value, const QString& units) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()))
        return QStringLiteral("Unavailable");
    const double n = value.toDouble();
    if (units == QLatin1String("USD"))
        return QStringLiteral("USD %1").arg(QLocale(QLocale::English, QLocale::UnitedStates).toString(n, 'f', 2));
    if (units == QLatin1String("price_return_ratio") || units == QLatin1String("fraction_of_regulatory_net_assets") ||
        units == QLatin1String("percentile_rank_0_1") || units == QLatin1String("relative_price_return_ratio"))
        return QStringLiteral("%1%").arg(QLocale::c().toString(n * 100.0, 'f', 2));
    if (units == QLatin1String("price_return_difference"))
        return QStringLiteral("%1 pp").arg(QLocale::c().toString(n * 100.0, 'f', 2));
    if (units == QLatin1String("efficiency_ratio_minus1_1"))
        return QLocale::c().toString(n, 'f', 2);
    if (units == QLatin1String("self_relative_volume_ratio"))
        return QStringLiteral("%1x").arg(QLocale::c().toString(n, 'f', 2));
    return QStringLiteral("%1 (%2)").arg(QLocale::c().toString(n, 'g', 8), label(units));
}

inline QString compact_usd(double value) {
    if (std::abs(value) >= 1e9)
        return QStringLiteral("USD %1 bn").arg(value / 1e9, 0, 'f', 2);
    if (std::abs(value) >= 1e6)
        return QStringLiteral("USD %1 m").arg(value / 1e6, 0, 'f', 2);
    if (std::abs(value) >= 1e3)
        return QStringLiteral("USD %1 k").arg(value / 1e3, 0, 'f', 2);
    return QStringLiteral("USD %1").arg(value, 0, 'f', 2);
}

inline QString quality(const QJsonObject& month) {
    QString text = label(month.value(QStringLiteral("quality")).toString().toLower());
    if (month.value(QStringLiteral("has_revised_inputs")).toBool())
        text += QStringLiteral(" · revised inputs");
    return text;
}

inline QString coverage(const QJsonObject& month) {
    const QJsonObject c = month.value(QStringLiteral("coverage")).toObject();
    if (c.isEmpty())
        return QStringLiteral("Coverage unavailable");
    return QStringLiteral("%1 / %2 SEC identities measured · %3 unresolved · %4 policy excluded")
        .arg(c.value(QStringLiteral("observed_reporting_identities")).toInt())
        .arg(c.value(QStringLiteral("unique_reporting_identities")).toInt())
        .arg(c.value(QStringLiteral("unresolved_subjects")).toInt())
        .arg(c.value(QStringLiteral("excluded_subjects")).toInt());
}

inline QString assets_coverage(const QJsonObject& month) {
    const QJsonObject c = month.value(QStringLiteral("coverage")).toObject();
    const auto value = c.value(QStringLiteral("regulatory_assets_estimate"));
    return value.isDouble()
               ? QStringLiteral("%1 · prior regulatory net assets estimate").arg(number(value, "percentile_rank_0_1"))
               : QStringLiteral("Unavailable · %1").arg(label(c.value(QStringLiteral("basis")).toString()));
}

inline QString component(const QJsonObject& value) {
    return number(value.value(QStringLiteral("value")), value.value(QStringLiteral("units")).toString());
}

inline QString subject_key(const QJsonObject& row) {
    return row.value(QStringLiteral("subject_type")).toString() + QLatin1Char(':') +
           row.value(QStringLiteral("stable_key")).toString();
}

inline QString component_label(const QString& id) {
    QString text = label(id);
    if (id.startsWith(QLatin1String("price_return_")) || id.startsWith(QLatin1String("trend_efficiency_")) ||
        id.startsWith(QLatin1String("return_acceleration_")))
        text += QStringLiteral(" sessions");
    else if (id.startsWith(QLatin1String("volume_ratio_")))
        text += QStringLiteral(" sessions (self-relative)");
    return text;
}

inline QString reason(const QString& id) {
    if (id.isEmpty())
        return QStringLiteral("No exclusion reported");
    if (id == QLatin1String("identity_not_established"))
        return QStringLiteral("No proven link to a SEC reporting identity");
    if (id == QLatin1String("ambiguous_reporting_identity_links"))
        return QStringLiteral("Conflicting listed claims to one SEC identity; attribution refused");
    if (id == QLatin1String("classification_history_unverified") || id == QLatin1String("classification_month_partial"))
        return QStringLiteral("Taxonomy does not establish the requested historical exposure");
    if (id == QLatin1String("leveraged_inverse_excluded_default"))
        return QStringLiteral("Leveraged/inverse product excluded by the selected policy");
    if (id == QLatin1String("price_return_not_cross_asset_comparable_d5"))
        return QStringLiteral(
            "Individual price-return components; cross-asset total-return comparability unresolved (D5)");
    if (id == QLatin1String("counted_once"))
        return QStringLiteral("SEC reporting identity counted once; overlapping products are not independent signals");
    return label(id);
}

inline QJsonObject last_month(const QJsonObject& group) {
    const auto months = group.value(QStringLiteral("regulatory_months")).toArray();
    return months.isEmpty() ? QJsonObject{} : months.last().toObject();
}

// Counts describe emitted backend states, never an aggregate/rank of returns.
inline QString rotation_availability(const QJsonObject& group) {
    int components = 0, stale = 0, missing = 0, excluded = 0;
    for (const auto& value : group.value(QStringLiteral("rotation_constituents")).toArray()) {
        const QString state = value.toObject().value(QStringLiteral("status")).toString();
        if (state == QLatin1String("component_only"))
            ++components;
        else if (state == QLatin1String("stale"))
            ++stale;
        else if (state == QLatin1String("excluded"))
            ++excluded;
        else
            ++missing;
    }
    return QStringLiteral("Rotation: %1 snapshots · %2 stale · %3 missing · %4 excluded")
        .arg(components)
        .arg(stale)
        .arg(missing)
        .arg(excluded);
}

} // namespace fincept::screens::etf_ui
