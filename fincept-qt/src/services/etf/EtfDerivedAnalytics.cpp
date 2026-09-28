#include "services/etf/EtfDerivedAnalytics.h"

#include "services/etf/EtfRegulatoryFlowAnalytics.h"
#include "services/etf/EtfRotationMeasures.h"
#include "services/etf/EtfRoutePolicy.h"
#include "storage/repositories/EtfDataRepository.h"

#include <QJsonArray>

namespace fincept::services::etf {

namespace {

/// Identity only: what cannot change once stored, each part dated by its own
/// first sighting.
QJsonObject etf_derived_entity_json(const etf_store::ReportingEntityRow& e) {
    return QJsonObject{{QStringLiteral("entity_id"), e.entity_id},
                       {QStringLiteral("cik"), e.cik},
                       {QStringLiteral("series_id"), e.series_id},
                       {QStringLiteral("reporting_level"), e.reporting_level},
                       {QStringLiteral("first_recorded_at"), derived_time_text(e.first_seen_at)}};
}

QJsonObject etf_derived_instrument_json(const etf_store::ListedInstrumentRow& row) {
    QJsonArray tickers;
    for (const etf_store::InstrumentSymbolRow& s : row.symbols) {
        tickers.append(QJsonObject{{QStringLiteral("symbol"), s.symbol},
                                   {QStringLiteral("first_recorded_at"), derived_time_text(s.first_seen_at)}});
    }
    return QJsonObject{{QStringLiteral("instrument_id"), row.instrument_id},
                       {QStringLiteral("ibkr_con_id"), row.con_id},
                       {QStringLiteral("first_recorded_at"), derived_time_text(row.first_seen_at)},
                       {QStringLiteral("tickers"), tickers}};
}

/// Which creation/redemption flow routes exist for the listed ETF (Batch B
/// route policy), from the identity link declared by the knowledge cutoff:
/// with none, regulatory monthly flow is identity_not_established; quarterly
/// is not_qualified (D7); calculated daily is route_disabled (D1-d).
QJsonArray etf_derived_flow_routes_json(std::optional<LinkRelationship> link) {
    QJsonArray routes;
    for (const FlowRouteStatus& s : flow_route_availability(link)) {
        const auto state = flow_route_quality_state(s.availability);
        routes.append(QJsonObject{
            {QStringLiteral("route"), QLatin1String(flow_route_id(s.route))},
            {QStringLiteral("availability"), QLatin1String(flow_route_availability_id(s.availability))},
            {QStringLiteral("state"), state ? QJsonValue(QLatin1String(quality_state_id(*state))) : QJsonValue()},
            {QStringLiteral("reason"), s.reason}});
    }
    return routes;
}

QJsonValue etf_derived_optional_id(const std::optional<qint64>& id) {
    return id ? QJsonValue(*id) : QJsonValue(QJsonValue::Null);
}

} // namespace

QJsonObject derived_methods_json() {
    const QString reg_kind = QLatin1String(measurement_kind_id(MeasurementKind::RegulatoryReportedFlow));
    const QString rot_kind = QLatin1String(measurement_kind_id(MeasurementKind::RotationProxy));
    QJsonObject regulatory{
        {QStringLiteral("version"), QLatin1String(kRegulatoryFlowAnalyticsVersion)},
        {QStringLiteral("measurement_kind"), reg_kind},
        {QStringLiteral("subject_type"), QLatin1String(subject_type_id(SubjectType::ReportingEntity))},
        {QStringLiteral("source_type"), QLatin1String(source_type_id(SourceType::SecNport))},
        {QStringLiteral("frequency"), QStringLiteral("calendar_month")},
        {QStringLiteral("parameters"), regulatory_flow_parameters_text()},
        {QStringLiteral("usable_states"), QJsonArray{QLatin1String(quality_state_id(QualityState::Confirmed)),
                                                     QLatin1String(quality_state_id(QualityState::Revised))}}};
    QJsonObject rotation{
        {QStringLiteral("version"), QLatin1String(kRotationProxyMeasuresVersion)},
        {QStringLiteral("measurement_kind"), rot_kind},
        {QStringLiteral("subject_type"), QLatin1String(subject_type_id(SubjectType::ListedInstrument))},
        {QStringLiteral("source_type"), QLatin1String(source_type_id(SourceType::IbkrTwsReadonly))},
        {QStringLiteral("frequency"), QStringLiteral("exchange_session")},
        {QStringLiteral("calendar_id"), QLatin1String(kUsEquityCalendarId)},
        {QStringLiteral("calendar_version"), QLatin1String(kUsEquityCalendarVersion)},
        {QStringLiteral("session_source"), QLatin1String(kRotationSessionSource)},
        {QStringLiteral("return_basis"), QLatin1String(kRotationReturnBasis)},
        {QStringLiteral("total_return"), false},
        // D5: no total-return basis is qualified, so price-return measures of
        // exposures with different distributions are not comparable.
        {QStringLiteral("cross_asset_return_comparability"), QStringLiteral("not_established_d5")},
        {QStringLiteral("volume_basis"), QLatin1String(kRotationVolumeBasis)},
        {QStringLiteral("freshness_rule"), QLatin1String(kRotationFreshnessRule)},
        {QStringLiteral("parameters"), rotation_parameters_text()},
        {QStringLiteral("usable_states"), QJsonArray{QLatin1String(quality_state_id(QualityState::Proxy)),
                                                     QLatin1String(quality_state_id(QualityState::Revised))}}};
    QJsonObject calculated{
        {QStringLiteral("measurement_kind"),
         QLatin1String(measurement_kind_id(MeasurementKind::CalculatedCreationRedemptionFlow))},
        {QStringLiteral("state"), QLatin1String(quality_state_id(QualityState::RouteDisabled))},
        {QStringLiteral("reason"), QLatin1String(derived_reason_id(DerivedReason::D1dNoPermissionBasis))},
        {QStringLiteral("implemented"), false},
        {QStringLiteral("route_policy"), QLatin1String(kEtfRoutePolicyVersion)}};
    return QJsonObject{{QStringLiteral("regulatory_flow_analytics"), regulatory},
                       {QStringLiteral("rotation_proxy_measures"), rotation},
                       {QStringLiteral("calculated_creation_redemption_flow"), calculated},
                       {QStringLiteral("document_scope"),
                        QStringLiteral("subjects, tickers and identity links recorded by known_at; vintages "
                                       "recorded by known_at and available at as_of; names, exchange and currency "
                                       "are left out (they are rewritten in place, so no earlier value can be "
                                       "reproduced)")}};
}

QString derived_request_problem(const DerivedRunRequest& request) {
    if (!request.frame.valid())
        return QStringLiteral("a derived calculation needs its as_of and known_at times");
    if (!request.regulatory && !request.rotation)
        return QStringLiteral("a derived calculation needs at least one family, regulatory or rotation");
    if (request.entity_id && !request.regulatory)
        return QStringLiteral("a reporting-entity filter applies to the regulatory family, which is not run");
    if (request.instrument_id && !request.rotation)
        return QStringLiteral("a listed-instrument filter applies to the rotation family, which is not run");
    if (request.reference_instrument_id && !request.rotation)
        return QStringLiteral("a reference applies to rotation measures, and the rotation family is not run");
    if (request.output_from.isValid() && request.output_to.isValid() && request.output_from > request.output_to)
        return QStringLiteral("the output range starts after it ends");
    return {};
}

Result<QJsonObject> run_derived_calculations(const DerivedRunRequest& request) {
    using R = Result<QJsonObject>;
    if (const QString problem = derived_request_problem(request); !problem.isEmpty())
        return R::err(problem.toStdString());
    auto& repo = EtfDataRepository::instance();
    const QDateTime& known_at = request.frame.known_at;

    QJsonArray regulatory;
    if (request.regulatory) {
        auto entities = repo.reporting_entities(known_at);
        if (entities.is_err())
            return R::err(entities.error());
        bool requested_found = false;
        for (const etf_store::ReportingEntityRow& e : entities.value()) {
            if (request.entity_id && *request.entity_id != e.entity_id)
                continue;
            requested_found = true;
            auto obs = repo.subject_observations(SubjectType::ReportingEntity, e.entity_id);
            if (obs.is_err())
                return R::err(obs.error());
            auto lineage_rows = repo.sec_filing_lineage(e.entity_id);
            if (lineage_rows.is_err())
                return R::err(lineage_rows.error());
            SecFilingLineageMap lineage;
            for (const etf_store::SecFilingLineageRow& f : lineage_rows.value())
                lineage.insert(f.accession, SecFilingLineage{f.accession, f.form, f.amends_accession});
            const RegulatoryFlowAnalytics a = compute_regulatory_flow_analytics(obs.value(), request.frame, lineage);
            regulatory.append(QJsonObject{{QStringLiteral("entity"), etf_derived_entity_json(e)},
                                          {QStringLiteral("analytics"),
                                           regulatory_flow_analytics_json(a, request.output_from, request.output_to)}});
        }
        if (request.entity_id && !requested_found)
            return R::err("the reporting entity was not recorded by the knowledge cutoff");
    }

    QJsonArray rotation;
    QJsonValue reference_json = QJsonValue(QJsonValue::Null);
    if (request.rotation) {
        auto instruments = repo.listed_instruments(known_at);
        if (instruments.is_err())
            return R::err(instruments.error());
        auto session_rows =
            repo.market_sessions(QLatin1String(kUsEquityCalendarId), QLatin1String(kUsEquityCalendarVersion));
        if (session_rows.is_err())
            return R::err(session_rows.error());
        const RotationSessionRecord sessions = RotationSessionRecord::from_rows(session_rows.value());
        QVector<StoredObservation> reference_obs;
        if (request.reference_instrument_id) {
            bool found = false;
            for (const etf_store::ListedInstrumentRow& row : instruments.value()) {
                if (row.instrument_id == *request.reference_instrument_id) {
                    QJsonObject ref = etf_derived_instrument_json(row);
                    ref.insert(QStringLiteral("declared_by"), QStringLiteral("caller"));
                    reference_json = ref;
                    found = true;
                }
            }
            if (!found)
                return R::err("the reference instrument was not recorded by the knowledge cutoff");
            auto ref = repo.subject_observations(SubjectType::ListedInstrument, *request.reference_instrument_id);
            if (ref.is_err())
                return R::err(ref.error());
            reference_obs = ref.value();
        }
        bool requested_found = false;
        for (const etf_store::ListedInstrumentRow& row : instruments.value()) {
            if (request.instrument_id && *request.instrument_id != row.instrument_id)
                continue;
            requested_found = true;
            auto obs = repo.subject_observations(SubjectType::ListedInstrument, row.instrument_id);
            if (obs.is_err())
                return R::err(obs.error());
            auto link = repo.nport_link_relationship_known_at(row.instrument_id, known_at);
            if (link.is_err())
                return R::err(link.error());
            const bool is_reference =
                request.reference_instrument_id && *request.reference_instrument_id == row.instrument_id;
            const RotationMeasures m =
                compute_rotation_measures(obs.value(), request.frame, sessions,
                                          request.reference_instrument_id ? &reference_obs : nullptr, is_reference);
            rotation.append(QJsonObject{
                {QStringLiteral("instrument"), etf_derived_instrument_json(row)},
                {QStringLiteral("flow_routes"), etf_derived_flow_routes_json(link.value())},
                {QStringLiteral("measures"), rotation_measures_json(m, request.output_from, request.output_to)}});
        }
        if (request.instrument_id && !requested_found)
            return R::err("the listed instrument was not recorded by the knowledge cutoff");
    }

    QJsonObject out;
    out.insert(QStringLiteral("kind"), QStringLiteral("etf_derived_values"));
    out.insert(QStringLiteral("frame"),
               QJsonObject{{QStringLiteral("as_of"), derived_time_text(request.frame.as_of)},
                           {QStringLiteral("known_at"), derived_time_text(request.frame.known_at)}});
    // What was asked, so the subjects the document must hold can be derived
    // from the store and the frame alone.
    out.insert(QStringLiteral("request"),
               QJsonObject{{QStringLiteral("regulatory"), request.regulatory},
                           {QStringLiteral("rotation"), request.rotation},
                           {QStringLiteral("entity_id"), etf_derived_optional_id(request.entity_id)},
                           {QStringLiteral("instrument_id"), etf_derived_optional_id(request.instrument_id)},
                           {QStringLiteral("reference_instrument_id"),
                            etf_derived_optional_id(request.reference_instrument_id)}});
    out.insert(QStringLiteral("output_range"),
               QJsonObject{{QStringLiteral("from"), request.output_from.isValid()
                                                        ? QJsonValue(request.output_from.toString(Qt::ISODate))
                                                        : QJsonValue(QJsonValue::Null)},
                           {QStringLiteral("to"), request.output_to.isValid()
                                                      ? QJsonValue(request.output_to.toString(Qt::ISODate))
                                                      : QJsonValue(QJsonValue::Null)}});
    out.insert(QStringLiteral("methods"), derived_methods_json());
    out.insert(QStringLiteral("regulatory_flow"), regulatory);
    out.insert(QStringLiteral("rotation_proxy"), rotation);
    out.insert(QStringLiteral("rotation_reference"), reference_json);
    return R::ok(out);
}

} // namespace fincept::services::etf
