#include "services/etf/EtfGroupAnalytics.h"

#include "services/etf/EtfGroupModel.h"
#include "services/etf/EtfTaxonomy.h"
#include "storage/repositories/EtfDataRepository.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace fincept::services::etf {
namespace {

QString reporting_key(const QString& cik, const QString& series) {
    return cik + QLatin1Char('/') + series;
}

QString group_of(const TaxonomyEntry& e, const QString& level) {
    if (level == QLatin1String("complex"))
        return e.complex_id;
    if (level == QLatin1String("category"))
        return e.category;
    return e.asset_class;
}

bool overlaps(const TaxonomyEntry& e, const QDate& first, const QDate& last) {
    return e.effective_from <= last && (!e.effective_to.isValid() || e.effective_to >= first);
}

bool spans(const TaxonomyEntry& e, const QDate& first, const QDate& last) {
    return e.effective_from <= first && (!e.effective_to.isValid() || e.effective_to >= last);
}

bool rotation_window_outside_classification(const RotationSessionResult& values, const TaxonomyEntry& e) {
    auto outside = [&](const DerivedValue& v) {
        return v.usable() && v.window_first.isValid() &&
               (v.window_first < e.effective_from || (e.effective_to.isValid() && v.window_last > e.effective_to));
    };
    for (const DerivedValue& v : values.price_return)
        if (outside(v))
            return true;
    for (const DerivedValue& v : values.trend_efficiency)
        if (outside(v))
            return true;
    for (const DerivedValue& v : values.volume_ratio)
        if (outside(v))
            return true;
    for (const DerivedValue& v : values.relative_price_return)
        if (outside(v))
            return true;
    return outside(values.return_acceleration);
}

QDate rotation_input_start(const RotationMeasures& measures) {
    QDate first = measures.snapshot.latest_session;
    const QJsonObject values = rotation_values_json(measures.snapshot.values);
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const QDate start =
            QDate::fromString(it.value().toObject().value(QStringLiteral("window_first")).toString(), Qt::ISODate);
        if (start.isValid() && (!first.isValid() || start < first))
            first = start;
    }
    return first;
}

Result<RegulatoryFlowAnalytics> regulatory_for(EtfDataRepository& repo, qint64 entity_id,
                                               const DerivedTimeFrame& frame) {
    using R = Result<RegulatoryFlowAnalytics>;
    auto obs = repo.subject_observations(SubjectType::ReportingEntity, entity_id);
    if (obs.is_err())
        return R::err(obs.error());
    auto filing_rows = repo.sec_filing_lineage(entity_id);
    if (filing_rows.is_err())
        return R::err(filing_rows.error());
    SecFilingLineageMap lineage;
    for (const etf_store::SecFilingLineageRow& f : filing_rows.value())
        lineage.insert(f.accession, SecFilingLineage{f.accession, f.form, f.amends_accession});
    return R::ok(compute_regulatory_flow_analytics(obs.value(), frame, lineage));
}

QJsonObject taxonomy_entry_json(const TaxonomyEntry& e) {
    return QJsonObject{{QStringLiteral("subject_type"), e.subject_type == TaxonomySubjectType::Listed
                                                            ? QStringLiteral("listed_instrument")
                                                            : QStringLiteral("reporting_entity")},
                       {QStringLiteral("stable_key"), e.subject_type == TaxonomySubjectType::Listed
                                                          ? QString::number(e.con_id)
                                                          : reporting_key(e.cik10, e.series_id)},
                       {QStringLiteral("asset_class"), e.asset_class},
                       {QStringLiteral("category"), e.category},
                       {QStringLiteral("complex_id"), e.complex_id},
                       {QStringLiteral("exposure_mechanism"), e.exposure_mechanism},
                       {QStringLiteral("fund_structure"), e.fund_structure},
                       {QStringLiteral("leveraged"), e.leveraged},
                       {QStringLiteral("leverage_multiple"), e.leverage_multiple},
                       {QStringLiteral("inverse"), e.inverse},
                       {QStringLiteral("option_overlay"), e.option_overlay},
                       {QStringLiteral("currency_hedge"), e.currency_hedge},
                       {QStringLiteral("region"), e.region},
                       {QStringLiteral("exposure_reference"), e.reference},
                       {QStringLiteral("evidence_url"), e.evidence_url},
                       {QStringLiteral("effective_from"), e.effective_from.toString(Qt::ISODate)},
                       {QStringLiteral("effective_to"), e.effective_to.isValid()
                                                            ? QJsonValue(e.effective_to.toString(Qt::ISODate))
                                                            : QJsonValue(QJsonValue::Null)}};
}

} // namespace

QString group_request_problem(const GroupRunRequest& r) {
    if (!r.frame.valid())
        return QStringLiteral("group research requires as_of and known_at");
    if (r.group_level != QLatin1String("complex") && r.group_level != QLatin1String("category") &&
        r.group_level != QLatin1String("asset_class") && r.group_level != QLatin1String("cross_asset"))
        return QStringLiteral("group level must be complex, category, asset_class, or cross_asset");
    if (r.group_level == QLatin1String("cross_asset") && !r.group_id.isEmpty())
        return QStringLiteral("cross_asset enumerates asset classes and takes no group id");
    if (!r.output_from.isValid() || !r.output_to.isValid() || r.output_from > r.output_to)
        return QStringLiteral("group research requires an ordered from/to date range");
    if (r.output_from.day() != 1 || r.output_to.day() != r.output_to.daysInMonth())
        return QStringLiteral("regulatory group range must start and end on calendar-month boundaries");
    if (r.output_to > r.frame.as_of.date())
        return QStringLiteral("group range ends after as_of");
    if (r.output_from.year() < 2019 || r.output_to.year() > 2100)
        return QStringLiteral("group date range is outside supported research bounds");
    if ((r.output_to.year() - r.output_from.year()) * 12 + r.output_to.month() - r.output_from.month() > 119)
        return QStringLiteral("group query is limited to 120 calendar months");
    return {};
}

Result<QJsonObject> run_group_research(const GroupRunRequest& request) {
    using R = Result<QJsonObject>;
    if (const QString problem = group_request_problem(request); !problem.isEmpty())
        return R::err(problem.toStdString());
    QFile resource(QStringLiteral(":/etf/taxonomy_v1.json"));
    if (!resource.open(QIODevice::ReadOnly))
        return R::err("the bundled ETF taxonomy could not be opened");
    QString taxonomy_error;
    const QByteArray taxonomy_bytes = resource.readAll();
    const QString taxonomy_sha256 =
        QString::fromLatin1(QCryptographicHash::hash(taxonomy_bytes, QCryptographicHash::Sha256).toHex());
    const auto loaded = TaxonomySnapshot::load(taxonomy_bytes, &taxonomy_error);
    if (!loaded)
        return R::err((QStringLiteral("invalid ETF taxonomy: ") + taxonomy_error).toStdString());
    const TaxonomySnapshot& taxonomy = *loaded;
    if (!request.expected_taxonomy_version.isEmpty() && request.expected_taxonomy_version != taxonomy.version())
        return R::err("taxonomy_version_mismatch");

    auto& repo = EtfDataRepository::instance();
    auto entity_read = repo.reporting_entities(request.frame.known_at);
    if (entity_read.is_err())
        return R::err(entity_read.error());
    auto instrument_read = repo.listed_instruments(request.frame.known_at);
    if (instrument_read.is_err())
        return R::err(instrument_read.error());
    QHash<QString, etf_store::ReportingEntityRow> entities;
    QHash<qint64, etf_store::ReportingEntityRow> entities_by_id;
    for (const auto& e : entity_read.value()) {
        entities.insert(reporting_key(e.cik, e.series_id), e);
        entities_by_id.insert(e.entity_id, e);
    }
    QHash<qint64, etf_store::ListedInstrumentRow> instruments;
    for (const auto& i : instrument_read.value())
        instruments.insert(i.con_id, i);

    QHash<qint64, RegulatoryFlowAnalytics> regulatory_cache;
    auto analytics_for = [&](qint64 entity_id) -> Result<RegulatoryFlowAnalytics> {
        if (regulatory_cache.contains(entity_id))
            return Result<RegulatoryFlowAnalytics>::ok(regulatory_cache.value(entity_id));
        auto a = regulatory_for(repo, entity_id, request.frame);
        if (a.is_ok())
            regulatory_cache.insert(entity_id, a.value());
        return a;
    };

    const QString effective_level =
        request.group_level == QLatin1String("cross_asset") ? QStringLiteral("asset_class") : request.group_level;
    const QDate first_month(request.output_from.year(), request.output_from.month(), 1);
    const QDate last_month(request.output_to.year(), request.output_to.month(), 1);
    QSet<QString> group_set;
    for (const TaxonomyEntry& e : taxonomy.entries()) {
        const QString id = group_of(e, effective_level);
        if (id.isEmpty() || (!request.group_id.isEmpty() && id != request.group_id))
            continue;
        // Keep the group and its known identities visible before a supported
        // classification interval; historical exposure is then unavailable.
        group_set.insert(id);
    }
    if (group_set.isEmpty())
        return R::err("the requested group has no taxonomy members in the date range");
    QStringList group_ids = group_set.values();
    std::sort(group_ids.begin(), group_ids.end());

    QJsonArray groups;
    for (const QString& group_id : group_ids) {
        QJsonArray months;
        for (QDate month = first_month; month <= last_month; month = month.addMonths(1)) {
            const QDate month_end = month.addMonths(1).addDays(-1);
            if (month > request.frame.as_of.date())
                break;
            QVector<GroupFlowMember> members;
            QHash<QString, TaxonomyEntry> selected_entries;
            for (const TaxonomyEntry& e : taxonomy.entries()) {
                if (group_of(e, effective_level) != group_id)
                    continue;
                const QString key = e.subject_type == TaxonomySubjectType::Listed
                                        ? QStringLiteral("L/") + QString::number(e.con_id)
                                        : QStringLiteral("R/") + reporting_key(e.cik10, e.series_id);
                const auto current = selected_entries.constFind(key);
                if (current == selected_entries.cend() ||
                    (overlaps(e, month, month_end) && !overlaps(current.value(), month, month_end)) ||
                    (overlaps(e, month, month_end) == overlaps(current.value(), month, month_end) &&
                     e.effective_from > current->effective_from))
                    selected_entries.insert(key, e);
            }
            QStringList stable_keys = selected_entries.keys();
            std::sort(stable_keys.begin(), stable_keys.end());
            for (const QString& stable_key : stable_keys) {
                const TaxonomyEntry& e = selected_entries[stable_key];
                const TaxonomyLookup active = e.subject_type == TaxonomySubjectType::Listed
                                                  ? taxonomy.lookup_listed(e.con_id, month)
                                                  : taxonomy.lookup_reporting(e.cik10, e.series_id, month);
                // A historical category assignment must not keep the same
                // identity in its old category once a different assignment
                // is established for the whole requested month.
                if (active.entry && spans(*active.entry, month, month_end) &&
                    group_of(*active.entry, effective_level) != group_id)
                    continue;
                GroupFlowMember m;
                m.subject_type = e.subject_type == TaxonomySubjectType::Reporting ? QStringLiteral("reporting_entity")
                                                                                  : QStringLiteral("listed_instrument");
                m.stable_key = e.subject_type == TaxonomySubjectType::Reporting ? reporting_key(e.cik10, e.series_id)
                                                                                : QString::number(e.con_id);
                m.identity_basis = e.subject_type == TaxonomySubjectType::Reporting
                                       ? QStringLiteral("curated_reporting_identity")
                                       : QStringLiteral("none");
                m.taxonomy = taxonomy_entry_json(e);
                if (!spans(e, month, month_end))
                    m.exclusion_reason = overlaps(e, month, month_end)
                                             ? QStringLiteral("classification_month_partial")
                                             : QStringLiteral("classification_history_unverified");
                else if ((e.leveraged || e.inverse) && !request.include_leveraged)
                    m.exclusion_reason = QStringLiteral("leveraged_inverse_excluded_default");
                if (e.subject_type == TaxonomySubjectType::Reporting) {
                    m.reporting_key = m.stable_key;
                    const auto found = entities.constFind(m.stable_key);
                    if (found != entities.cend() && m.exclusion_reason.isEmpty()) {
                        auto a = analytics_for(found->entity_id);
                        if (a.is_err())
                            return R::err(a.error());
                        m.analytics = a.value();
                    }
                } else {
                    const auto found = instruments.constFind(e.con_id);
                    if (found == instruments.cend()) {
                        if (m.exclusion_reason.isEmpty())
                            m.exclusion_reason = QStringLiteral("instrument_not_recorded_by_known_at");
                    } else {
                        auto links = repo.nport_links_known_at(found->instrument_id, request.frame.known_at);
                        if (links.is_err())
                            return R::err(links.error());
                        if (links.value().size() > 1) {
                            // v052 has no link revocation. No newest-link shortcut.
                            if (m.exclusion_reason.isEmpty())
                                m.exclusion_reason = QStringLiteral("ambiguous_identity_links");
                        } else if (links.value().size() == 1) {
                            const auto& link = links.value().first();
                            if (link.relationship == LinkRelationship::ClassOfMultiClassSeries) {
                                if (m.exclusion_reason.isEmpty())
                                    m.exclusion_reason = QStringLiteral("multi_class_series_not_etf_flow");
                            } else {
                                const auto target = entities_by_id.constFind(link.entity_id);
                                if (target == entities_by_id.cend()) {
                                    if (m.exclusion_reason.isEmpty())
                                        m.exclusion_reason = QStringLiteral("linked_entity_not_recorded_by_known_at");
                                } else {
                                    const TaxonomyLookup reporting_class =
                                        taxonomy.lookup_reporting(target->cik, target->series_id, month);
                                    if (reporting_class.status == TaxonomyStatus::Classified &&
                                        group_of(*reporting_class.entry, effective_level) != group_id) {
                                        if (m.exclusion_reason.isEmpty())
                                            m.exclusion_reason = QStringLiteral("taxonomy_identity_conflict");
                                    } else {
                                        m.reporting_key = reporting_key(target->cik, target->series_id);
                                        m.identity_basis = QStringLiteral("declared_link");
                                        m.identity_link = QJsonObject{
                                            {QStringLiteral("link_id"), link.link_id},
                                            {QStringLiteral("entity_id"), link.entity_id},
                                            {QStringLiteral("class_id"), link.class_id},
                                            {QStringLiteral("relationship"),
                                             QLatin1String(link_relationship_id(link.relationship))},
                                            {QStringLiteral("basis"), link.basis},
                                            {QStringLiteral("declared_at"), derived_time_text(link.declared_at)}};
                                        if (m.exclusion_reason.isEmpty()) {
                                            auto a = analytics_for(target->entity_id);
                                            if (a.is_err())
                                                return R::err(a.error());
                                            m.analytics = a.value();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                members.append(m);
            }
            if (members.isEmpty())
                continue;
            months.append(group_flow_month_json(aggregate_group_regulatory_month(
                effective_level, group_id, taxonomy.version(), month, request.frame, members)));
        }

        // A rotation group is a set of individual secondary-market component
        // snapshots. No mean, rank or cross-asset return comparison is made:
        // Batch C's close is split-adjusted price, not total return (D5).
        QJsonArray rotation;
        auto session_rows =
            repo.market_sessions(QLatin1String(kUsEquityCalendarId), QLatin1String(kUsEquityCalendarVersion));
        if (session_rows.is_err())
            return R::err(session_rows.error());
        const RotationSessionRecord calendar = RotationSessionRecord::from_rows(session_rows.value());
        for (const TaxonomyEntry& e :
             taxonomy.members(TaxonomySubjectType::Listed, effective_level, group_id, request.frame.as_of.date())) {
            QJsonObject row = taxonomy_entry_json(e);
            const auto found = instruments.constFind(e.con_id);
            if ((e.leveraged || e.inverse) && !request.include_leveraged) {
                row.insert(QStringLiteral("status"), QStringLiteral("excluded"));
                row.insert(QStringLiteral("reason"), QStringLiteral("leveraged_inverse_excluded_default"));
            } else if (found == instruments.cend()) {
                row.insert(QStringLiteral("status"), QStringLiteral("missing"));
                row.insert(QStringLiteral("reason"), QStringLiteral("instrument_not_recorded_by_known_at"));
            } else {
                auto obs = repo.subject_observations(SubjectType::ListedInstrument, found->instrument_id);
                if (obs.is_err())
                    return R::err(obs.error());
                const RotationMeasures measures = compute_rotation_measures(obs.value(), request.frame, calendar);
                const QJsonObject latest =
                    rotation_measures_json(measures, measures.snapshot.latest_session, measures.snapshot.latest_session)
                        .value(QStringLiteral("latest"))
                        .toObject();
                row.insert(QStringLiteral("status"), measures.snapshot.has_data
                                                         ? (measures.snapshot.freshness == RotationFreshness::Stale
                                                                ? QStringLiteral("stale")
                                                                : QStringLiteral("component_only"))
                                                         : QStringLiteral("missing"));
                row.insert(QStringLiteral("reason"), measures.snapshot.has_data
                                                         ? QStringLiteral("price_return_not_cross_asset_comparable_d5")
                                                         : QStringLiteral("no_available_rotation_bar"));
                row.insert(QStringLiteral("latest"), latest);
                // Preserve every session actually needed by the emitted
                // components. Index-count truncation can lose an input when
                // persisted calendars contain extra/missing session rows.
                QJsonArray source_sessions;
                const QDate input_start = rotation_input_start(measures);
                for (const RotationSessionInputs& in : measures.inputs) {
                    if (!input_start.isValid() || in.session < input_start ||
                        in.session > measures.snapshot.latest_session)
                        continue;
                    source_sessions.append(QJsonObject{
                        {QStringLiteral("session"), in.session.toString(Qt::ISODate)},
                        {QStringLiteral("session_type"), QLatin1String(session_day_type_id(in.session_type))},
                        {QStringLiteral("recorded"), in.recorded},
                        {QStringLiteral("inputs"),
                         QJsonObject{{QLatin1String(kBarClose), rotation_bar_input_json(in.close)},
                                     {QLatin1String(kBarVolume), rotation_bar_input_json(in.volume)}}}});
                }
                row.insert(QStringLiteral("source_sessions"), source_sessions);
                row.insert(QStringLiteral("return_basis"), QLatin1String(kRotationReturnBasis));
                row.insert(QStringLiteral("total_return"), false);
                row.insert(QStringLiteral("volume_basis"), QLatin1String(kRotationVolumeBasis));
                row.insert(QStringLiteral("calendar"), rotation_calendar_json(measures));
                if (measures.snapshot.has_data && rotation_window_outside_classification(measures.snapshot.values, e))
                    row.insert(QStringLiteral("classification_lookback"),
                               QStringLiteral("history_unverified_for_one_or_more_components"));
                else
                    row.insert(QStringLiteral("classification_lookback"),
                               QStringLiteral("within_classification_interval"));
            }
            row.insert(QStringLiteral("calculation_version"), QLatin1String(kRotationProxyMeasuresVersion));
            row.insert(QStringLiteral("calculation_parameters"), rotation_parameters_text());
            row.insert(QStringLiteral("reference"), QJsonValue(QJsonValue::Null));
            row.insert(QStringLiteral("reference_status"), QStringLiteral("not_declared"));
            row.insert(QStringLiteral("measurement_kind"),
                       QLatin1String(measurement_kind_id(MeasurementKind::RotationProxy)));
            row.insert(QStringLiteral("comparability"),
                       row.value(QStringLiteral("classification_lookback")).toString() ==
                               QLatin1String("history_unverified_for_one_or_more_components")
                           ? QStringLiteral("classification_history_unverified")
                           : QStringLiteral("individual_component_only_d5"));
            rotation.append(row);
        }
        groups.append(QJsonObject{{QStringLiteral("level"), effective_level},
                                  {QStringLiteral("group_id"), group_id},
                                  {QStringLiteral("regulatory_months"), months},
                                  {QStringLiteral("rotation_constituents"), rotation},
                                  {QStringLiteral("rotation_aggregate"), QJsonValue(QJsonValue::Null)}});
    }

    QJsonArray unclassified;
    QJsonArray history_unverified;
    for (const auto& e : entity_read.value()) {
        const auto status = taxonomy.lookup_reporting(e.cik, e.series_id, request.frame.as_of.date()).status;
        const QJsonObject identity{{QStringLiteral("subject_type"), QStringLiteral("reporting_entity")},
                                   {QStringLiteral("stable_key"), reporting_key(e.cik, e.series_id)}};
        if (status == TaxonomyStatus::UnknownIdentity)
            unclassified.append(identity);
        else if (status == TaxonomyStatus::HistoryUnverified)
            history_unverified.append(identity);
    }
    for (const auto& i : instrument_read.value()) {
        const auto status = taxonomy.lookup_listed(i.con_id, request.frame.as_of.date()).status;
        const QJsonObject identity{{QStringLiteral("subject_type"), QStringLiteral("listed_instrument")},
                                   {QStringLiteral("stable_key"), QString::number(i.con_id)}};
        if (status == TaxonomyStatus::UnknownIdentity)
            unclassified.append(identity);
        else if (status == TaxonomyStatus::HistoryUnverified)
            history_unverified.append(identity);
    }
    return R::ok(QJsonObject{
        {QStringLiteral("method"), QLatin1String(kGroupAnalyticsVersion)},
        {QStringLiteral("taxonomy_version"), taxonomy.version()},
        {QStringLiteral("taxonomy_sha256"), taxonomy_sha256},
        {QStringLiteral("universe_scope"),
         QStringLiteral("curated_research_snapshot_not_complete_historical_universe")},
        {QStringLiteral("as_of"), derived_time_text(request.frame.as_of)},
        {QStringLiteral("known_at"), derived_time_text(request.frame.known_at)},
        {QStringLiteral("requested_level"), request.group_level},
        {QStringLiteral("requested_group"), request.group_id},
        {QStringLiteral("include_leveraged"), request.include_leveraged},
        {QStringLiteral("output_from"), request.output_from.toString(Qt::ISODate)},
        {QStringLiteral("output_to"), request.output_to.toString(Qt::ISODate)},
        {QStringLiteral("flow_source"), QStringLiteral("sec_nport_monthly")},
        {QStringLiteral("calculated_daily_flow"),
         QJsonObject{{QStringLiteral("value"), QJsonValue(QJsonValue::Null)},
                     {QStringLiteral("state"), QStringLiteral("ROUTE_DISABLED")},
                     {QStringLiteral("reason"), QStringLiteral("no_qualified_daily_creation_redemption_route")}}},
        {QStringLiteral("daily_flow_coverage"),
         QJsonObject{{QStringLiteral("value"), QJsonValue(QJsonValue::Null)},
                     {QStringLiteral("basis"), QStringLiteral("prior_daily_aum_unavailable")}}},
        {QStringLiteral("overlap_policy"),
         QStringLiteral("reporting_identity_counted_once_per_group; groups_at_different_levels_must_not_be_added; "
                        "overlapping_etfs_are_not_independent_signals")},
        {QStringLiteral("flow_interpretation"),
         QStringLiteral("ETF_vehicle_creation_redemption_activity_not_cash_entering_referenced_physical_assets")},
        {QStringLiteral("rotation_basis"), QLatin1String(kRotationReturnBasis)},
        {QStringLiteral("cross_asset_return_comparability"), QStringLiteral("not_established_d5")},
        {QStringLiteral("groups"), groups},
        {QStringLiteral("unclassified_tracked_at_as_of"), unclassified},
        {QStringLiteral("classification_history_unverified_at_as_of"), history_unverified}});
}

} // namespace fincept::services::etf
