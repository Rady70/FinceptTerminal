#include "app/EtfDataSelftest.h"

#include "services/etf/EtfReadModel.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"

#include <QDate>
#include <QDateTime>
#include <QString>
#include <QVariantList>

#include <cstdio>

namespace fincept::marketlab {

namespace etf_selftest_detail {

int etf_selftest_failures = 0;

void etf_check(const char* what, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok)
        ++etf_selftest_failures;
}

QDateTime etf_utc(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toUTC();
}

qint64 etf_row_total() {
    qint64 total = 0;
    for (const char* table : {"etf_retrievals", "etf_retrieval_issues", "etf_reporting_entities", "etf_sec_filings",
                              "etf_listed_instruments", "etf_instrument_symbols", "etf_identity_links",
                              "etf_observation_starts", "etf_market_sessions", "etf_observations"}) {
        auto r = Database::instance().execute(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(table)));
        if (r.is_err() || !r.value().next())
            return -1;
        total += r.value().value(0).toLongLong();
    }
    return total;
}

} // namespace etf_selftest_detail

int run_etf_data_selftest() {
    using namespace etf_selftest_detail;
    using namespace services::etf;
    using etf_store::ObservationOutcome;
    etf_selftest_failures = 0;
    auto& db = Database::instance();
    auto& repo = EtfDataRepository::instance();

    etf_check("database is open", db.is_open());
    if (!db.is_open())
        return 1;
    auto version = db.execute(QStringLiteral("SELECT MAX(version) FROM schema_version"));
    etf_check("schema is at v52 or later",
              version.is_ok() && version.value().next() && version.value().value(0).toInt() >= 52);
    const qint64 rows_before = etf_row_total();
    etf_check("every ETF table is present", rows_before >= 0);
    if (rows_before < 0)
        return 1;

    // Every write below relies on this transaction being rolled back at the
    // end. Without it they would land in the profile's real tables: stop.
    const bool in_transaction = db.begin_transaction().is_ok();
    etf_check("transaction opened", in_transaction);
    if (!in_transaction) {
        std::printf("etf-data selftest: FAIL (no transaction, nothing written)\n");
        std::fflush(stdout);
        return 1;
    }

    // ── SEC: original, replay, later retrieval, amendment, missing value ─────
    auto retrieval = [&](SourceType source, AcquisitionMode mode, const char* at) {
        etf_store::RetrievalRecord r;
        r.run_id = QStringLiteral("selftest");
        r.source_type = source;
        r.acquisition_mode = mode;
        r.endpoint = QStringLiteral("selftest");
        r.request_ref = QStringLiteral("selftest");
        r.requested_at = etf_utc(at);
        r.retrieved_at = etf_utc(at);
        r.status = RetrievalStatus::Ok;
        r.interpretation = QStringLiteral("selftest");
        r.route_policy = QLatin1String(kEtfRoutePolicyVersion);
        auto id = repo.record_retrieval(r);
        return id.is_ok() ? id.value() : 0;
    };
    const qint64 r1 = retrieval(SourceType::SecNport, AcquisitionMode::RegulatoryApi, "2026-09-01T10:00:00.000Z");
    etf_store::ReportingEntityFacts facts;
    facts.cik = QStringLiteral("9999999999");
    facts.registrant_name = QStringLiteral("MarketLab self-test registrant");
    facts.source_accepted_at = etf_utc("2026-08-28T12:25:47.000Z");
    auto entity = repo.upsert_reporting_entity(facts, etf_utc("2026-09-01T10:00:00.000Z"));
    etf_check("registrant-level entity stored with an empty series id", entity.is_ok());
    const qint64 entity_id = entity.is_ok() ? entity.value() : 0;
    auto start = repo.observation_start(SourceType::SecNport, SubjectType::ReportingEntity, entity_id,
                                        etf_utc("2026-09-01T10:00:00.000Z"), r1);
    etf_store::SecFilingFacts filing;
    filing.accession = QStringLiteral("9999999999-26-000001");
    filing.filer_cik = facts.cik;
    filing.form = QStringLiteral("NPORT-P");
    filing.filing_date = QDate(2026, 8, 28);
    filing.accepted_at = etf_utc("2026-08-28T12:25:47.000Z");
    filing.entity_id = entity_id;
    filing.rep_pd_date = QDate(2026, 6, 30);
    filing.document_sha256 = QStringLiteral("00");
    auto f1 = repo.upsert_sec_filing(filing, r1, etf_utc("2026-09-01T10:00:00.000Z"));
    etf_check("filing stored", f1.is_ok());

    etf_store::ObservationInput net;
    net.subject_type = SubjectType::ReportingEntity;
    net.subject_id = entity_id;
    net.kind = MeasurementKind::AumObservation;
    net.measure = QStringLiteral("nport_net_assets");
    net.units = QStringLiteral("USD");
    net.basis = QStringLiteral("regulatory_quarter_end_net_assets");
    net.source_type = SourceType::SecNport;
    net.acquisition_mode = AcquisitionMode::RegulatoryApi;
    net.source_document = filing.accession;
    net.filing_id = f1.is_ok() ? f1.value().first : 0;
    net.effective_date = QDate(2026, 6, 30);
    net.report_period = QDate(2026, 6, 30);
    net.accepted_at = filing.accepted_at;
    net.value = parse_decimal_field(QStringLiteral("100.50"), true);
    net.retrieval_id = r1;
    net.seen_at = etf_utc("2026-09-01T10:00:00.000Z");
    net.observation_start = start.is_ok() ? start.value() : QDateTime();
    auto o1 = repo.record_observation(net);
    etf_check("SEC original recorded", o1.is_ok() && o1.value() == ObservationOutcome::InsertedOriginal);
    auto replay = repo.record_observation(net);
    etf_check("exact replay changes nothing", replay.is_ok() && replay.value() == ObservationOutcome::AlreadyRecorded);

    etf_store::ObservationInput flow = net;
    flow.kind = MeasurementKind::RegulatoryReportedFlow;
    flow.measure = QStringLiteral("nport_sales");
    flow.basis = QStringLiteral("nport_monthly_flow");
    flow.effective_date = QDate(2026, 4, 30);
    flow.period_start = QDate(2026, 4, 1);
    flow.period_end = QDate(2026, 4, 30);
    flow.value = FieldValue::missing();
    etf_check("missing flow recorded", repo.record_observation(flow).is_ok());

    const qint64 r2 = retrieval(SourceType::SecNport, AcquisitionMode::RegulatoryApi, "2026-09-02T10:00:00.000Z");
    net.retrieval_id = r2;
    net.seen_at = etf_utc("2026-09-02T10:00:00.000Z");
    auto confirm = repo.record_observation(net);
    etf_check("a later retrieval of the same value confirms",
              confirm.is_ok() && confirm.value() == ObservationOutcome::Confirmed);

    etf_store::SecFilingFacts amendment = filing;
    amendment.accession = QStringLiteral("9999999999-26-000002");
    amendment.form = QStringLiteral("NPORT-P/A");
    amendment.amends_accession = filing.accession;
    amendment.accepted_at = etf_utc("2026-09-10T15:00:00.000Z");
    const qint64 r3 = retrieval(SourceType::SecNport, AcquisitionMode::RegulatoryApi, "2026-09-11T10:00:00.000Z");
    auto f2 = repo.upsert_sec_filing(amendment, r3, etf_utc("2026-09-11T10:00:00.000Z"));
    net.source_document = amendment.accession;
    net.filing_id = f2.is_ok() ? f2.value().first : 0;
    net.amended_filing = true;
    net.accepted_at = amendment.accepted_at;
    net.value = parse_decimal_field(QStringLiteral("101"), true);
    net.retrieval_id = r3;
    net.seen_at = etf_utc("2026-09-11T10:00:00.000Z");
    auto amended = repo.record_observation(net);
    etf_check("amendment recorded as its own vintage",
              amended.is_ok() && amended.value() == ObservationOutcome::InsertedAmendment);

    auto sec_vintages = repo.vintages(SubjectType::ReportingEntity, entity_id, QStringLiteral("nport_net_assets"),
                                      QDate(2026, 6, 30), SourceType::SecNport);
    const bool two = sec_vintages.is_ok() && sec_vintages.value().size() == 2;
    etf_check("the original filing is kept beside its amendment", two);
    if (two) {
        const auto& v = sec_vintages.value();
        etf_check("original: regulatory_filing under the conservative rule",
                  v[0].history_type == QLatin1String("regulatory_filing") &&
                      v[0].point_in_time_status == QLatin1String("conservative_rule") &&
                      v[0].available_from == etf_utc("2026-08-31T13:30:00.000Z") && v[0].seen_count == 2);
        etf_check("amendment: accepted after the observation start, so observed from its first sighting",
                  v[1].history_type == QLatin1String("forward_observed") &&
                      v[1].point_in_time_status == QLatin1String("observed") &&
                      v[1].available_from == etf_utc("2026-09-11T10:00:00.000Z") &&
                      v[1].revision_state == QLatin1String("amended_filing"));
        etf_check("the current vintage is the amendment, derived as REVISED",
                  current_vintage_index(v) == 1 && derived_quality(v, 1) == QualityState::Revised);
    }
    auto missing = repo.vintages(SubjectType::ReportingEntity, entity_id, QStringLiteral("nport_sales"),
                                 QDate(2026, 4, 30), SourceType::SecNport);
    etf_check("a missing flow stays missing (NULL, not zero)",
              missing.is_ok() && missing.value().size() == 1 && !missing.value()[0].value.reported() &&
                  missing.value()[0].value.state == ValueState::Missing);

    // ── IBKR: backfill vintage, revision kept beside it ─────────────────────
    const qint64 r4 =
        retrieval(SourceType::IbkrTwsReadonly, AcquisitionMode::IbkrReadonlyWrapper, "2026-09-25T15:00:00.000Z");
    etf_store::ListedInstrumentFacts inst;
    inst.con_id = 999999991;
    inst.symbol = QStringLiteral("ZZSELFTEST");
    inst.security_type = QStringLiteral("STK");
    inst.stock_type = QStringLiteral("ETF");
    inst.currency = QStringLiteral("USD");
    auto instrument = repo.upsert_listed_instrument(inst, etf_utc("2026-09-25T15:00:00.000Z"));
    etf_check("instrument stored by conId", instrument.is_ok());
    const qint64 instrument_id = instrument.is_ok() ? instrument.value() : 0;
    auto ibkr_start = repo.observation_start(SourceType::IbkrTwsReadonly, SubjectType::ListedInstrument, instrument_id,
                                             etf_utc("2026-09-25T15:00:00.000Z"), r4);
    etf_check("session days stored",
              repo.upsert_sessions(UsEquityCalendar::weekdays_in(QDate(2026, 9, 21), QDate(2026, 9, 24))).is_ok());
    etf_store::ObservationInput bar;
    bar.subject_type = SubjectType::ListedInstrument;
    bar.subject_id = instrument_id;
    bar.kind = MeasurementKind::MarketBar;
    bar.measure = QStringLiteral("bar_close");
    bar.units = QStringLiteral("USD_per_share");
    bar.source_type = SourceType::IbkrTwsReadonly;
    bar.acquisition_mode = AcquisitionMode::IbkrReadonlyWrapper;
    bar.effective_date = QDate(2026, 9, 24);
    bar.value = FieldValue::reported_value(10.0);
    bar.retrieval_id = r4;
    bar.seen_at = etf_utc("2026-09-25T15:00:00.000Z");
    bar.observation_start = ibkr_start.is_ok() ? ibkr_start.value() : QDateTime();
    auto b1 = repo.record_observation(bar);
    etf_check("IBKR backfill bar recorded", b1.is_ok() && b1.value() == ObservationOutcome::InsertedOriginal);
    const qint64 r5 =
        retrieval(SourceType::IbkrTwsReadonly, AcquisitionMode::IbkrReadonlyWrapper, "2026-09-26T15:00:00.000Z");
    bar.value = FieldValue::reported_value(10.5);
    bar.retrieval_id = r5;
    bar.seen_at = etf_utc("2026-09-26T15:00:00.000Z");
    auto b2 = repo.record_observation(bar);
    etf_check("a changed bar is a new revision", b2.is_ok() && b2.value() == ObservationOutcome::InsertedRevision);
    auto bars = repo.vintages(SubjectType::ListedInstrument, instrument_id, QStringLiteral("bar_close"),
                              QDate(2026, 9, 24), SourceType::IbkrTwsReadonly);
    if (bars.is_ok() && bars.value().size() == 2) {
        const auto& v = bars.value();
        etf_check("backfill is a historical assumption, available from the session close",
                  v[0].history_type == QLatin1String("market_backfill") &&
                      v[0].point_in_time_status == QLatin1String("historical_assumption") &&
                      v[0].available_from == etf_utc("2026-09-24T20:00:00.000Z"));
        etf_check("the earlier vintage is still what was known before the revision was seen",
                  vintage_as_of_index(v, etf_utc("2026-09-25T20:00:00.000Z")) == 0 &&
                      vintage_as_of_index(v, etf_utc("2026-09-26T16:00:00.000Z")) == 1);
    } else {
        etf_check("both IBKR vintages kept", false);
    }

    // ── Refusals: route policy and the database's own rules ─────────────────
    etf_store::ObservationInput issuer = bar;
    issuer.source_type = SourceType::IssuerFile;
    issuer.acquisition_mode = AcquisitionMode::UserInitiatedImport;
    etf_check("a disabled route is refused", repo.record_observation(issuer).is_err());
    auto observed_backfill = db.execute(
        QStringLiteral(
            "INSERT INTO etf_observations (subject_type, subject_id, measurement_kind, measure, units, "
            "source_type, acquisition_mode, effective_date, value, value_state, source_revision, "
            "revision_state, history_type, point_in_time_status, availability_basis, available_from, "
            "first_seen_at, last_seen_at, first_retrieval_id, last_retrieval_id) VALUES ('listed_instrument', "
            "?, 'market_bar', 'bar_open', 'USD_per_share', 'ibkr_tws_readonly', 'ibkr_readonly_wrapper', "
            "'2026-09-23', 1.0, 'reported', 1, 'original', 'market_backfill', 'observed', "
            "'recorded_first_seen', '2026-09-25T15:00:00.000Z', '2026-09-25T15:00:00.000Z', "
            "'2026-09-25T15:00:00.000Z', ?, ?)"),
        {instrument_id, r4, r4});
    etf_check("the database refuses a backfilled value labelled observed", observed_backfill.is_err());
    auto zero_missing = db.execute(
        QStringLiteral(
            "INSERT INTO etf_observations (subject_type, subject_id, measurement_kind, measure, units, "
            "source_type, acquisition_mode, effective_date, value, value_state, source_revision, "
            "revision_state, history_type, point_in_time_status, availability_basis, available_from, "
            "first_seen_at, last_seen_at, first_retrieval_id, last_retrieval_id) VALUES ('listed_instrument', "
            "?, 'market_bar', 'bar_low', 'USD_per_share', 'ibkr_tws_readonly', 'ibkr_readonly_wrapper', "
            "'2026-09-23', 0.0, 'missing', 1, 'original', 'market_backfill', 'historical_assumption', "
            "'session_close_assumption', '2026-09-23T20:00:00.000Z', '2026-09-25T15:00:00.000Z', "
            "'2026-09-25T15:00:00.000Z', ?, ?)"),
        {instrument_id, r4, r4});
    etf_check("the database refuses a missing value stored as zero", zero_missing.is_err());
    auto ordinary_stock = db.execute(
        QStringLiteral("INSERT INTO etf_listed_instruments (ibkr_con_id, symbol, security_type, stock_type, currency, "
                       "first_seen_at, last_seen_at) VALUES (999999992, 'ZZCOMMON', 'STK', 'COMMON', 'USD', "
                       "'2026-09-25T15:00:00.000Z', '2026-09-25T15:00:00.000Z')"));
    etf_check("the database refuses an instrument IBKR does not classify as an ETF", ordinary_stock.is_err());

    etf_check("transaction rolled back", db.rollback().is_ok());
    etf_check("the self-test left no ETF row behind", etf_row_total() == rows_before);

    std::printf("etf-data selftest: %s (%d failure(s))\n", etf_selftest_failures == 0 ? "PASS" : "FAIL",
                etf_selftest_failures);
    std::fflush(stdout);
    return etf_selftest_failures == 0 ? 0 : 1;
}

} // namespace fincept::marketlab
