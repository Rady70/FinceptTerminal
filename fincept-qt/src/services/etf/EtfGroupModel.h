// Batch D pure group calculations. The caller resolves taxonomy and identity;
// this layer refuses to invent either. All rows, including missing and
// excluded subjects, survive into the result for reconstruction.
#pragma once
#include "services/etf/EtfRegulatoryFlowAnalytics.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <optional>

namespace fincept::services::etf {

struct GroupFlowMember {
    QString subject_type;              ///< reporting_entity | listed_instrument
    QString stable_key;                ///< cik10/series or IBKR conId, never ticker
    QString reporting_key;             ///< exact cik10/series when an eligible N-PORT reporting identity is known
    QString identity_basis;            ///< curated_reporting_identity | declared_link | none
    QString exclusion_reason;          ///< empty when potentially eligible
    QJsonObject taxonomy;              ///< exact classification used for this member and effective interval
    QJsonObject identity_link;         ///< declared SEC/listed link provenance, when present
    RegulatoryFlowAnalytics analytics; ///< Batch C result of reporting_key, if available
};

struct GroupFlowMemberResult {
    GroupFlowMember member;
    QString status; ///< observed | missing_flow | excluded | duplicate_reporting_identity
    QString reason;
    std::optional<RegulatoryMonthResult> month;
    NetAssetsDenominator denominator;
};

struct GroupFlowMonth {
    QDate month;
    DerivedTimeFrame frame;
    QString group_level;
    QString group_id;
    QString taxonomy_version;
    QVector<GroupFlowMemberResult> members;
    std::optional<double> observed_net_flow_usd;
    std::optional<double> complete_net_flow_usd;
    std::optional<double> regulatory_assets_coverage;
    QString coverage_reason;
    QString quality; ///< CONFIRMED | REVISED | PARTIAL | MISSING
    QDateTime available_from;
    PointInTimeStatus point_in_time_status = PointInTimeStatus::NotPointInTime;
    QDateTime coverage_available_from;
    PointInTimeStatus coverage_point_in_time_status = PointInTimeStatus::Observed;
    int classified_subjects = 0;
    int unique_reporting_identities = 0;
    int observed_reporting_identities = 0;
    int unresolved_subjects = 0;
    int excluded_subjects = 0;
    bool revised = false;
};

inline NetAssetsDenominator group_prior_regulatory_assets(const RegulatoryFlowAnalytics& analytics,
                                                          const QDate& month) {
    // The same Batch C 0..2 month rule, applied also when this month has no
    // flow row. Its available net-assets vintages already obey as_of/known_at.
    NetAssetsDenominator out;
    for (int lag = 0; lag <= RegulatoryFlowParameters::kDenominatorMaxLagMonths; ++lag) {
        const QDate report_date = month.addMonths(-lag).addDays(-1);
        for (const RegulatoryNetAssetsInput& n : analytics.net_assets) {
            if (n.report_date != report_date)
                continue;
            out.found = true;
            out.report_date = report_date;
            out.lag_months = lag;
            out.input = n.input;
            return out;
        }
    }
    return out;
}

inline const RegulatoryMonthResult* group_find_month(const RegulatoryFlowAnalytics& a, const QDate& month) {
    for (const RegulatoryMonthResult& m : a.months) {
        if (m.inputs.month == month)
            return &m;
    }
    return nullptr;
}

inline GroupFlowMonth aggregate_group_regulatory_month(const QString& level, const QString& group_id,
                                                       const QString& taxonomy_version, const QDate& month,
                                                       const DerivedTimeFrame& frame,
                                                       const QVector<GroupFlowMember>& members) {
    GroupFlowMonth out;
    out.month = month;
    out.frame = frame;
    out.group_level = level;
    out.group_id = group_id;
    out.taxonomy_version = taxonomy_version;

    QVector<GroupFlowMember> ordered = members;
    std::sort(ordered.begin(), ordered.end(), [](const GroupFlowMember& a, const GroupFlowMember& b) {
        // The exact SEC identity is preferred when both an SEC classification
        // and a declared listed link describe it. The latter stays visible.
        const auto key_a =
            a.reporting_key + QLatin1Char('/') +
            (a.subject_type == QStringLiteral("reporting_entity") ? QStringLiteral("0") : QStringLiteral("1")) +
            a.stable_key;
        const auto key_b =
            b.reporting_key + QLatin1Char('/') +
            (b.subject_type == QStringLiteral("reporting_entity") ? QStringLiteral("0") : QStringLiteral("1")) +
            b.stable_key;
        return key_a < key_b;
    });

    QSet<QString> used_reporting;
    double observed_sum = 0.0;
    double assets_all = 0.0;
    double assets_observed = 0.0;
    bool every_denominator = true;
    QDate common_report_date;
    bool mixed_report_dates = false;
    bool every_flow = true;
    bool finite_assets = true;
    PointInTimeStatus weakest = PointInTimeStatus::Observed;

    for (const GroupFlowMember& member : ordered) {
        GroupFlowMemberResult r;
        r.member = member;
        ++out.classified_subjects;
        if (!member.exclusion_reason.isEmpty()) {
            const bool policy_exclusion =
                member.exclusion_reason == QLatin1String("leveraged_inverse_excluded_default");
            r.status = policy_exclusion ? QStringLiteral("excluded") : QStringLiteral("unresolved");
            r.reason = member.exclusion_reason;
            if (policy_exclusion) {
                ++out.excluded_subjects;
            } else {
                ++out.unresolved_subjects;
                every_flow = false;
                every_denominator = false;
            }
            out.members.append(r);
            continue;
        }
        if (member.reporting_key.isEmpty()) {
            r.status = QStringLiteral("missing_flow");
            r.reason = QStringLiteral("identity_not_established");
            ++out.unresolved_subjects;
            every_flow = false;
            every_denominator = false;
            out.members.append(r);
            continue;
        }
        if (used_reporting.contains(member.reporting_key)) {
            r.status = QStringLiteral("duplicate_reporting_identity");
            r.reason = QStringLiteral("counted_once");
            out.members.append(r);
            continue;
        }
        used_reporting.insert(member.reporting_key);
        ++out.unique_reporting_identities;
        if (const RegulatoryMonthResult* found = group_find_month(member.analytics, month))
            r.month = *found;
        r.denominator = group_prior_regulatory_assets(member.analytics, month);
        if (!r.denominator.found || !r.denominator.input.usable_number() || !(r.denominator.input.value.value > 0.0) ||
            r.denominator.input.units != QStringLiteral("USD") ||
            r.denominator.input.basis != QStringLiteral("regulatory_quarter_end_net_assets")) {
            every_denominator = false;
        } else {
            assets_all += r.denominator.input.value.value;
            finite_assets = finite_assets && std::isfinite(assets_all);
            if (!out.coverage_available_from.isValid() ||
                r.denominator.input.available_from > out.coverage_available_from)
                out.coverage_available_from = r.denominator.input.available_from;
            out.coverage_point_in_time_status =
                weaker_point_in_time(out.coverage_point_in_time_status, r.denominator.input.point_in_time_status);
            if (!common_report_date.isValid())
                common_report_date = r.denominator.report_date;
            else if (r.denominator.report_date != common_report_date)
                mixed_report_dates = true;
        }
        if (!r.month || !r.month->net_flow.usable() || !r.month->inputs.available) {
            r.status = QStringLiteral("missing_flow");
            r.reason = r.month ? QLatin1String(derived_reason_id(r.month->net_flow.reason))
                               : QStringLiteral("month_not_available");
            every_flow = false;
            out.members.append(r);
            continue;
        }
        // RegulatoryFlowAnalytics computes the number from one selected SEC
        // filing. Assert its units here instead of treating an arbitrary
        // same-shaped derived value as dollars of ETF flow.
        if (r.month->inputs.sales.units != QStringLiteral("USD") ||
            r.month->inputs.redemption.units != QStringLiteral("USD") ||
            r.month->inputs.reinvestment.units != QStringLiteral("USD")) {
            r.status = QStringLiteral("missing_flow");
            r.reason = QStringLiteral("input_units_not_usd");
            every_flow = false;
            out.members.append(r);
            continue;
        }
        r.status = QStringLiteral("observed");
        if (!std::isfinite(observed_sum + *r.month->net_flow.value)) {
            r.status = QStringLiteral("missing_flow");
            r.reason = QStringLiteral("group_sum_overflow");
            every_flow = false;
            out.members.append(r);
            continue;
        }
        ++out.observed_reporting_identities;
        observed_sum += *r.month->net_flow.value;
        if (r.denominator.found && r.denominator.input.usable_number() && r.denominator.input.value.value > 0.0 &&
            r.denominator.input.units == QStringLiteral("USD") &&
            r.denominator.input.basis == QStringLiteral("regulatory_quarter_end_net_assets"))
            assets_observed += r.denominator.input.value.value;
        finite_assets = finite_assets && std::isfinite(assets_observed);
        if (!out.coverage_available_from.isValid() || r.month->net_flow.available_from > out.coverage_available_from)
            out.coverage_available_from = r.month->net_flow.available_from;
        out.coverage_point_in_time_status =
            weaker_point_in_time(out.coverage_point_in_time_status, r.month->net_flow.point_in_time_status);
        if (!out.available_from.isValid() || r.month->net_flow.available_from > out.available_from)
            out.available_from = r.month->net_flow.available_from;
        weakest = weaker_point_in_time(weakest, r.month->net_flow.point_in_time_status);
        out.revised = out.revised || r.month->net_flow.state == QualityState::Revised;
        out.members.append(r);
    }
    if (out.observed_reporting_identities > 0) {
        out.observed_net_flow_usd = observed_sum;
        out.point_in_time_status = weakest;
    }
    if (out.unique_reporting_identities > 0 && every_flow && out.unresolved_subjects == 0)
        out.complete_net_flow_usd = observed_sum;
    if (out.classified_subjects == 0)
        out.coverage_reason = QStringLiteral("no_classified_members");
    else if (out.unresolved_subjects > 0)
        out.coverage_reason = QStringLiteral("unresolved_constituent");
    else if (!every_denominator || out.unique_reporting_identities == 0)
        out.coverage_reason = QStringLiteral("regulatory_assets_unavailable");
    else if (mixed_report_dates)
        out.coverage_reason = QStringLiteral("mixed_regulatory_report_dates");
    else if (!finite_assets || !(assets_all > 0.0))
        out.coverage_reason = QStringLiteral("invalid_regulatory_assets");
    else {
        out.regulatory_assets_coverage = assets_observed / assets_all;
        out.coverage_reason = QStringLiteral("regulatory_prior_net_assets_estimate");
    }
    out.quality = out.observed_reporting_identities == 0 ? QStringLiteral("MISSING")
                  : out.complete_net_flow_usd ? (out.revised ? QStringLiteral("REVISED") : QStringLiteral("CONFIRMED"))
                                              : QStringLiteral("PARTIAL");
    return out;
}

inline QJsonObject group_flow_month_json(const GroupFlowMonth& m) {
    QJsonArray member_rows;
    for (const GroupFlowMemberResult& r : m.members) {
        QJsonObject item{
            {QStringLiteral("subject_type"), r.member.subject_type},
            {QStringLiteral("stable_key"), r.member.stable_key},
            {QStringLiteral("reporting_key"),
             r.member.reporting_key.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(r.member.reporting_key)},
            {QStringLiteral("identity_basis"), r.member.identity_basis},
            {QStringLiteral("taxonomy"), r.member.taxonomy},
            {QStringLiteral("identity_link"),
             r.member.identity_link.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(r.member.identity_link)},
            {QStringLiteral("status"), r.status},
            {QStringLiteral("reason"), r.reason}};
        if (r.month) {
            item.insert(QStringLiteral("selected_accession"), r.month->inputs.selected_accession);
            item.insert(QStringLiteral("accepted_at"), derived_time_text(r.month->inputs.accepted_at));
            item.insert(QStringLiteral("sales"), regulatory_selected_input_json(r.month->inputs.sales));
            item.insert(QStringLiteral("redemption"), regulatory_selected_input_json(r.month->inputs.redemption));
            item.insert(QStringLiteral("reinvestment"), regulatory_selected_input_json(r.month->inputs.reinvestment));
            item.insert(QStringLiteral("net_flow"),
                        derived_value_json(r.month->net_flow,
                                           QLatin1String(measurement_kind_id(MeasurementKind::RegulatoryReportedFlow)),
                                           QStringLiteral("USD")));
            QJsonArray filings;
            for (const RegulatoryMonthVintage& v : r.month->inputs.vintages)
                filings.append(QJsonObject{{QStringLiteral("accession"), v.accession},
                                           {QStringLiteral("form"), v.form},
                                           {QStringLiteral("amends_accession"), v.amends_accession},
                                           {QStringLiteral("accepted_at"), derived_time_text(v.accepted_at)},
                                           {QStringLiteral("available_from"), derived_time_text(v.available_from)}});
            item.insert(QStringLiteral("filing_vintages"), filings);
        }
        item.insert(QStringLiteral("prior_regulatory_assets"), net_assets_denominator_json(r.denominator));
        member_rows.append(item);
    }
    return QJsonObject{
        {QStringLiteral("method"), QLatin1String("etf_group_analytics_v2")},
        {QStringLiteral("taxonomy_version"), m.taxonomy_version},
        {QStringLiteral("group_level"), m.group_level},
        {QStringLiteral("group_id"), m.group_id},
        {QStringLiteral("frequency"), QStringLiteral("calendar_month")},
        {QStringLiteral("measurement_kind"),
         QLatin1String(measurement_kind_id(MeasurementKind::RegulatoryReportedFlow))},
        {QStringLiteral("calculation_version"), QLatin1String(kRegulatoryFlowAnalyticsVersion)},
        {QStringLiteral("month"), m.month.toString(QStringLiteral("yyyy-MM"))},
        {QStringLiteral("period_start"), m.month.toString(Qt::ISODate)},
        {QStringLiteral("period_end"), m.month.addMonths(1).addDays(-1).toString(Qt::ISODate)},
        {QStringLiteral("as_of"), derived_time_text(m.frame.as_of)},
        {QStringLiteral("known_at"), derived_time_text(m.frame.known_at)},
        {QStringLiteral("observed_net_flow_usd"),
         m.observed_net_flow_usd ? QJsonValue(*m.observed_net_flow_usd) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("complete_net_flow_usd"),
         m.complete_net_flow_usd ? QJsonValue(*m.complete_net_flow_usd) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("quality"), m.quality},
        {QStringLiteral("has_revised_inputs"), m.revised},
        {QStringLiteral("available_from"),
         m.available_from.isValid() ? QJsonValue(derived_time_text(m.available_from)) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("point_in_time_status"), QLatin1String(point_in_time_status_id(m.point_in_time_status))},
        {QStringLiteral("coverage"),
         QJsonObject{{QStringLiteral("classified_subject_rows"), m.classified_subjects},
                     {QStringLiteral("unique_reporting_identities"), m.unique_reporting_identities},
                     {QStringLiteral("observed_reporting_identities"), m.observed_reporting_identities},
                     {QStringLiteral("unresolved_subjects"), m.unresolved_subjects},
                     {QStringLiteral("excluded_subjects"), m.excluded_subjects},
                     {QStringLiteral("basis"), m.coverage_reason},
                     {QStringLiteral("regulatory_assets_estimate"), m.regulatory_assets_coverage
                                                                        ? QJsonValue(*m.regulatory_assets_coverage)
                                                                        : QJsonValue(QJsonValue::Null)},
                     {QStringLiteral("available_from"), m.regulatory_assets_coverage
                                                            ? QJsonValue(derived_time_text(m.coverage_available_from))
                                                            : QJsonValue(QJsonValue::Null)},
                     {QStringLiteral("point_in_time_status"),
                      m.regulatory_assets_coverage
                          ? QJsonValue(QLatin1String(point_in_time_status_id(m.coverage_point_in_time_status)))
                          : QJsonValue(QJsonValue::Null)}}},
        {QStringLiteral("constituents"), member_rows}};
}

} // namespace fincept::services::etf
