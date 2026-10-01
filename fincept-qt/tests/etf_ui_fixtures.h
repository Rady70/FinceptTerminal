// Synthetic SQLite fixtures adapted from the finalized Batch D store tests.
#pragma once
#include "services/etf/EtfGroupAnalytics.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/EtfDataRepository.h"

namespace etf_ui_fixtures {
using namespace fincept;
using namespace fincept::services::etf;
QDateTime utc(const char* s) {
    return QDateTime::fromString(QLatin1String(s), Qt::ISODateWithMs).toUTC();
}
EtfDataRepository& repo() {
    return EtfDataRepository::instance();
}

qint64 retrieval(const char* seen) {
    etf_store::RetrievalRecord r;
    r.run_id = QStringLiteral("group-test");
    r.source_type = SourceType::SecNport;
    r.acquisition_mode = AcquisitionMode::RegulatoryApi;
    r.endpoint = QStringLiteral("test");
    r.request_ref = QStringLiteral("test");
    r.requested_at = utc(seen);
    r.retrieved_at = utc(seen);
    r.status = RetrievalStatus::Ok;
    r.response_sha256 = QStringLiteral("fixture");
    r.interpretation = QStringLiteral("test");
    r.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    const auto id = repo().record_retrieval(r);
    return id.is_ok() ? id.value() : -1;
}

qint64 entity(const char* cik, const char* series, const char* seen) {
    etf_store::ReportingEntityFacts f;
    f.cik = QLatin1String(cik);
    f.series_id = QLatin1String(series);
    f.registrant_name = QStringLiteral("Fixture Trust");
    f.source_accepted_at = utc("2026-11-25T15:00:00.000Z");
    const auto r = repo().upsert_reporting_entity(f, utc(seen));
    return r.is_ok() ? r.value() : -1;
}

qint64 instrument(qint64 con_id, const char* symbol, const char* seen) {
    etf_store::ListedInstrumentFacts f;
    f.con_id = con_id;
    f.symbol = QLatin1String(symbol);
    f.security_type = QStringLiteral("STK");
    f.stock_type = QStringLiteral("ETF");
    f.exchange = QStringLiteral("SMART");
    f.primary_exchange = QStringLiteral("ARCA");
    f.currency = QStringLiteral("USD");
    const auto r = repo().upsert_listed_instrument(f, utc(seen));
    return r.is_ok() ? r.value() : -1;
}

bool filing(qint64 subject, const char* cik, const char* accession, const char* form, const char* accepted,
            const char* seen, double net_flow, const char* amends = "") {
    const qint64 retrieval_id = retrieval(seen);
    if (retrieval_id <= 0)
        return false;
    const auto start =
        repo().observation_start(SourceType::SecNport, SubjectType::ReportingEntity, subject, utc(seen), retrieval_id);
    if (start.is_err())
        return false;
    etf_store::SecFilingFacts f;
    f.accession = QLatin1String(accession);
    f.filer_cik = QLatin1String(cik);
    f.form = QLatin1String(form);
    f.filing_date = utc(accepted).date();
    f.report_date = QStringLiteral("2026-10-31");
    f.accepted_at = utc(accepted);
    f.entity_id = subject;
    f.amends_accession = QLatin1String(amends);
    f.rep_pd_date = QDate(2026, 10, 31);
    f.document_sha256 = QStringLiteral("hash-") + f.accession;
    const auto saved = repo().upsert_sec_filing(f, retrieval_id, utc(seen));
    if (saved.is_err())
        return false;
    const char* measures[3] = {kNportSales, kNportRedemption, kNportReinvestment};
    for (int i = 0; i < 3; ++i) {
        etf_store::ObservationInput in;
        in.subject_type = SubjectType::ReportingEntity;
        in.subject_id = subject;
        in.kind = MeasurementKind::RegulatoryReportedFlow;
        in.measure = QLatin1String(measures[i]);
        in.units = QStringLiteral("USD");
        in.basis = QStringLiteral("nport_monthly_flow");
        in.source_type = SourceType::SecNport;
        in.acquisition_mode = AcquisitionMode::RegulatoryApi;
        in.source_document = f.accession;
        in.filing_id = saved.value().first;
        in.amended_filing = f.form == QLatin1String("NPORT-P/A");
        in.effective_date = QDate(2026, 10, 31);
        in.period_start = QDate(2026, 10, 1);
        in.period_end = QDate(2026, 10, 31);
        in.report_period = QDate(2026, 10, 31);
        in.accepted_at = f.accepted_at;
        in.value = FieldValue::reported_value(i == 0 ? net_flow : 0.0);
        in.retrieval_id = retrieval_id;
        in.seen_at = utc(seen);
        in.observation_start = start.value();
        if (repo().record_observation(in).is_err())
            return false;
    }
    return true;
}

GroupRunRequest request(const char* time) {
    GroupRunRequest r;
    r.frame = {utc(time), utc(time)};
    r.group_level = QStringLiteral("complex");
    r.group_id = QStringLiteral("sp500");
    r.expected_taxonomy_version = QStringLiteral("etf-taxonomy-v2");
    r.output_from = QDate(2026, 10, 1);
    r.output_to = QDate(2026, 10, 31);
    return r;
}

} // namespace etf_ui_fixtures
