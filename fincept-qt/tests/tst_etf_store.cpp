// tst_etf_store.cpp — ETF Capital Flows Batch B: persistence of the ETF data
// foundation (migration v052 + storage/repositories/EtfDataRepository).
//
// This suite needs a real SQLite database: what it proves is what a database
// file holds before and after it is reopened, what an upgrade of an existing
// schema leaves behind, and which rows the database itself refuses. It
// therefore links Qt6::Sql and the leaf storage sources (Database,
// MigrationRunner, Logger, v052, EtfDataRepository), plus the ETF self-test it
// runs against its database — see tests/CMakeLists.txt. Every database is a
// fresh file in a temporary directory.

#include "app/EtfDataSelftest.h"
#include "services/etf/EtfReadModel.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <utility>

using namespace fincept;
using namespace fincept::services::etf;
using etf_store::ObservationOutcome;

namespace {

QDateTime utc(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toUTC();
}

EtfDataRepository& repo() {
    return EtfDataRepository::instance();
}

qint64 add_retrieval(SourceType source, AcquisitionMode mode, const char* at) {
    etf_store::RetrievalRecord r;
    r.run_id = QStringLiteral("test");
    r.source_type = source;
    r.acquisition_mode = mode;
    r.endpoint = QStringLiteral("test");
    r.request_ref = QStringLiteral("test");
    r.requested_at = utc(at);
    r.retrieved_at = utc(at);
    r.status = RetrievalStatus::Ok;
    r.http_status = 200;
    r.response_sha256 = QStringLiteral("ab");
    r.interpretation = QStringLiteral("test");
    r.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    auto id = repo().record_retrieval(r);
    return id.is_ok() ? id.value() : -1;
}

qint64 sec_retrieval(const char* at) {
    return add_retrieval(SourceType::SecNport, AcquisitionMode::RegulatoryApi, at);
}

qint64 ibkr_retrieval(const char* at) {
    return add_retrieval(SourceType::IbkrTwsReadonly, AcquisitionMode::IbkrReadonlyWrapper, at);
}

qint64 add_entity(const char* cik, const char* series = "") {
    etf_store::ReportingEntityFacts f;
    f.cik = QString::fromLatin1(cik);
    f.series_id = QString::fromLatin1(series);
    f.registrant_name = QStringLiteral("Test Trust");
    f.source_accepted_at = utc("2026-08-28T12:25:47.000Z");
    auto id = repo().upsert_reporting_entity(f, utc("2026-09-01T00:00:00.000Z"));
    return id.is_ok() ? id.value() : -1;
}

qint64 add_filing(const char* accession, const char* form, const char* accepted, qint64 entity, qint64 retrieval,
                  const char* class_ids = "", bool returns = true) {
    etf_store::SecFilingFacts f;
    f.accession = QString::fromLatin1(accession);
    f.filer_cik = QStringLiteral("0000884394");
    f.form = QString::fromLatin1(form);
    f.filing_date = utc(accepted).date();
    f.accepted_at = utc(accepted);
    f.entity_id = entity;
    f.amends_accession = f.form == QLatin1String("NPORT-P/A") ? QStringLiteral("0001410368-26-000001") : QString();
    f.rep_pd_date = QDate(2026, 6, 30);
    f.returns_block_present = returns;
    f.class_ids = QString::fromLatin1(class_ids);
    f.document_sha256 = QStringLiteral("doc-") + f.accession;
    auto r = repo().upsert_sec_filing(f, retrieval, utc(accepted));
    return r.is_ok() ? r.value().first : -1;
}

etf_store::ObservationInput sec_input(qint64 entity, qint64 filing, const char* accession, bool amended,
                                      const FieldValue& value, qint64 retrieval, const char* seen, const char* accepted,
                                      const char* start = "2026-09-01T10:00:00.000Z") {
    etf_store::ObservationInput in;
    in.subject_type = SubjectType::ReportingEntity;
    in.subject_id = entity;
    in.kind = MeasurementKind::AumObservation;
    in.measure = QStringLiteral("nport_net_assets");
    in.units = QStringLiteral("USD");
    in.basis = QStringLiteral("regulatory_quarter_end_net_assets");
    in.source_type = SourceType::SecNport;
    in.acquisition_mode = AcquisitionMode::RegulatoryApi;
    in.source_document = QString::fromLatin1(accession);
    in.filing_id = filing;
    in.amended_filing = amended;
    in.effective_date = QDate(2026, 6, 30);
    in.report_period = QDate(2026, 6, 30);
    in.accepted_at = utc(accepted);
    in.value = value;
    in.retrieval_id = retrieval;
    in.seen_at = utc(seen);
    in.observation_start = utc(start);
    return in;
}

qint64 add_instrument(qint64 con_id, const char* symbol, const char* seen = "2026-09-25T15:00:00.000Z") {
    etf_store::ListedInstrumentFacts f;
    f.con_id = con_id;
    f.symbol = QString::fromLatin1(symbol);
    f.security_type = QStringLiteral("STK");
    f.stock_type = QStringLiteral("ETF");
    f.exchange = QStringLiteral("SMART");
    f.primary_exchange = QStringLiteral("ARCA");
    f.currency = QStringLiteral("USD");
    auto id = repo().upsert_listed_instrument(f, utc(seen));
    return id.is_ok() ? id.value() : -1;
}

etf_store::ObservationInput bar_input(qint64 instrument, double close, qint64 retrieval, const char* seen,
                                      const char* start = "2026-09-25T15:00:00.000Z", QDate date = QDate(2026, 9, 24)) {
    etf_store::ObservationInput in;
    in.subject_type = SubjectType::ListedInstrument;
    in.subject_id = instrument;
    in.kind = MeasurementKind::MarketBar;
    in.measure = QStringLiteral("bar_close");
    in.units = QStringLiteral("USD_per_share");
    in.basis = QStringLiteral("ibkr_trades_rth_daily_split_adjusted");
    in.source_type = SourceType::IbkrTwsReadonly;
    in.acquisition_mode = AcquisitionMode::IbkrReadonlyWrapper;
    in.effective_date = date;
    in.value = FieldValue::reported_value(close);
    in.retrieval_id = retrieval;
    in.seen_at = utc(seen);
    in.observation_start = utc(start);
    return in;
}

int count(const char* table) {
    auto r = Database::instance().execute(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(table)));
    return r.is_ok() && r.value().next() ? r.value().value(0).toInt() : -1;
}

int schema_version_of_open_db() {
    auto r = Database::instance().execute(QStringLiteral("SELECT MAX(version) FROM schema_version"));
    return r.is_ok() && r.value().next() ? r.value().value(0).toInt() : -1;
}

bool raw_insert_fails(const QString& sql, const QVariantList& params = {}) {
    return Database::instance().execute(sql, params).is_err();
}

} // namespace

class TstEtfStore : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir dir_;
    int db_counter_ = 0;
    QString open_fresh() {
        const QString path = dir_.filePath(QStringLiteral("etf_%1.db").arg(++db_counter_));
        auto r = Database::instance().open(path);
        if (r.is_err())
            qWarning() << "open failed:" << QString::fromStdString(r.error());
        return r.is_ok() ? path : QString();
    }

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void upgrade_from_existing_v51_database();
    void fresh_database_is_created_at_v52();
    void persisted_state_reloads_identically();
    void exact_replay_changes_nothing();
    void later_retrieval_confirms_without_a_new_vintage();
    void ibkr_revision_keeps_the_earlier_vintage();
    void older_retrieval_reapplied_is_a_noop();
    void a_value_that_flips_back_is_a_new_vintage();
    void sec_amendment_is_its_own_vintage();
    void sec_value_change_under_one_accession_is_refused();
    void confirmation_needs_the_same_meaning();
    void filing_document_change_is_refused();
    void missing_unparseable_and_zero_are_preserved();
    void instrument_identity_is_the_conid();
    void an_ordinary_stock_is_refused_twice();
    void reporting_entity_identity_is_cik_and_series();
    void identity_links_follow_the_class_structure();
    void observation_start_is_set_once();
    void session_calendar_is_stored();
    void the_database_refuses_rule_violations();
    void repository_refuses_disabled_routes();
    void vocabulary_matches_the_database();
    void export_is_deterministic();
    void selftest_writes_nothing_without_its_transaction();
};

void TstEtfStore::initTestCase() {
    QVERIFY(dir_.isValid());
    register_migration_v052();
    QCOMPARE(MigrationRunner::highest_registered_version(), 52);
}

void TstEtfStore::cleanupTestCase() {
    Database::instance().close();
}

void TstEtfStore::upgrade_from_existing_v51_database() {
    // An existing profile database at v51 with user data in a table Batch B
    // does not know. The upgrade must add v052 without touching it, after the
    // runner's pre-migration backup.
    const QString path = dir_.filePath(QStringLiteral("existing_v51.db"));
    {
        QSqlDatabase seed = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("seed_v51"));
        seed.setDatabaseName(path);
        QVERIFY(seed.open());
        QSqlQuery q(seed);
        QVERIFY(q.exec("CREATE TABLE schema_version (version INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_at TEXT "
                       "DEFAULT (datetime('now')))"));
        for (int v = 1; v <= 51; ++v) {
            q.prepare("INSERT INTO schema_version (version, name) VALUES (?, ?)");
            q.addBindValue(v);
            q.addBindValue(QStringLiteral("existing_%1").arg(v));
            QVERIFY(q.exec());
        }
        QVERIFY(q.exec("CREATE TABLE market_data (symbol TEXT, close REAL)"));
        QVERIFY(q.exec("INSERT INTO market_data VALUES ('SPY', 612.5), ('IVV', 615.25)"));
        seed.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("seed_v51"));

    auto opened = Database::instance().open(path);
    QVERIFY2(opened.is_ok(), opened.is_ok() ? "" : opened.error().c_str());
    QCOMPARE(schema_version_of_open_db(), 52);
    QCOMPARE(count("etf_observations"), 0);
    auto rows = Database::instance().execute("SELECT symbol, close FROM market_data ORDER BY symbol");
    QVERIFY(rows.is_ok());
    QVERIFY(rows.value().next());
    QCOMPARE(rows.value().value(0).toString(), QStringLiteral("IVV"));
    QCOMPARE(rows.value().value(1).toDouble(), 615.25);
    QVERIFY(rows.value().next());
    QCOMPARE(rows.value().value(1).toDouble(), 612.5);
    // The runner's pre-migration backup holds the v51 state.
    const QString backup = path + QStringLiteral(".pre-v52.bak");
    QVERIFY(QFile::exists(backup));
    {
        QSqlDatabase bak = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check_bak"));
        bak.setDatabaseName(backup);
        QVERIFY(bak.open());
        QSqlQuery q(bak);
        QVERIFY(q.exec("SELECT MAX(version) FROM schema_version") && q.next());
        QCOMPARE(q.value(0).toInt(), 51);
        QVERIFY(q.exec("SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'etf_%'") && q.next());
        QCOMPARE(q.value(0).toInt(), 0);
        bak.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("check_bak"));
    // Opening again applies nothing further.
    QVERIFY(QFile::remove(backup));
    QVERIFY(Database::instance().open(path).is_ok());
    QCOMPARE(schema_version_of_open_db(), 52);
    QVERIFY(!QFile::exists(backup));
}

void TstEtfStore::fresh_database_is_created_at_v52() {
    QVERIFY(!open_fresh().isEmpty());
    QCOMPARE(schema_version_of_open_db(), 52);
    for (const char* t : {"etf_retrievals", "etf_retrieval_issues", "etf_reporting_entities", "etf_sec_filings",
                          "etf_listed_instruments", "etf_instrument_symbols", "etf_identity_links",
                          "etf_observation_starts", "etf_market_sessions", "etf_observations"})
        QCOMPARE(count(t), 0);
}

void TstEtfStore::persisted_state_reloads_identically() {
    const QString path = open_fresh();
    QVERIFY(!path.isEmpty());
    const qint64 entity = add_entity("0000884394");
    const qint64 r1 = sec_retrieval("2026-09-01T10:00:00.000Z");
    const qint64 filing = add_filing("0001410368-26-089410", "NPORT-P", "2026-08-28T12:25:47.000Z", entity, r1);
    QVERIFY(repo()
                .observation_start(SourceType::SecNport, SubjectType::ReportingEntity, entity,
                                   utc("2026-09-01T10:00:00.000Z"), r1)
                .is_ok());
    QVERIFY(repo()
                .record_observation(sec_input(entity, filing, "0001410368-26-089410", false,
                                              parse_decimal_field("781188872106.76", true), r1,
                                              "2026-09-01T10:00:05.000Z", "2026-08-28T12:25:47.000Z"))
                .is_ok());
    etf_store::ObservationInput missing =
        sec_input(entity, filing, "0001410368-26-089410", false, FieldValue::missing(), r1, "2026-09-01T10:00:05.000Z",
                  "2026-08-28T12:25:47.000Z");
    missing.measure = QStringLiteral("nport_redemption");
    missing.kind = MeasurementKind::RegulatoryReportedFlow;
    missing.effective_date = QDate(2026, 5, 31);
    QVERIFY(repo().record_observation(missing).is_ok());
    const qint64 instrument = add_instrument(756733, "SPY");
    const qint64 r2 = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    QVERIFY(repo().upsert_sessions(UsEquityCalendar::weekdays_in(QDate(2026, 9, 21), QDate(2026, 9, 24))).is_ok());
    QVERIFY(repo().record_observation(bar_input(instrument, 612.37, r2, "2026-09-25T15:00:05.000Z")).is_ok());
    etf_store::IssueRecord issue;
    issue.subject_type = SubjectType::ListedInstrument;
    issue.subject_id = instrument;
    issue.effective_date = QDate(2026, 9, 23);
    issue.state = QualityState::Missing;
    issue.code = QStringLiteral("expected_session_without_bar");
    QVERIFY(repo().record_issue(r2, issue).is_ok());

    auto before = repo().export_all();
    QVERIFY(before.is_ok());
    // Close the connection and open the file again: a new connection reads
    // what the file holds.
    Database::instance().close();
    QVERIFY(Database::instance().open(path).is_ok());
    auto after = repo().export_all();
    QVERIFY(after.is_ok());
    QCOMPARE(after.value(), before.value());

    // And the semantics survive, not just the bytes.
    auto net = repo().vintages(SubjectType::ReportingEntity, entity, QStringLiteral("nport_net_assets"),
                               QDate(2026, 6, 30), SourceType::SecNport);
    QVERIFY(net.is_ok());
    QCOMPARE(net.value().size(), 1);
    const StoredObservation& v = net.value()[0];
    QCOMPARE(v.value.value, 781188872106.76);
    QCOMPARE(v.value.raw, QStringLiteral("781188872106.76"));
    QCOMPARE(v.source_document, QStringLiteral("0001410368-26-089410"));
    QCOMPARE(v.accepted_at, utc("2026-08-28T12:25:47.000Z"));
    QCOMPARE(v.first_seen_at, utc("2026-09-01T10:00:05.000Z"));
    QCOMPARE(v.history_type, QStringLiteral("regulatory_filing"));
    QCOMPARE(v.point_in_time_status, QStringLiteral("conservative_rule"));
    QCOMPARE(v.availability_basis, QStringLiteral("sec_next_session_after_acceptance_v1"));
    QCOMPARE(v.available_from, utc("2026-08-31T13:30:00.000Z"));
    QCOMPARE(v.revision_state, QStringLiteral("original"));
    auto red = repo().vintages(SubjectType::ReportingEntity, entity, QStringLiteral("nport_redemption"),
                               QDate(2026, 5, 31), SourceType::SecNport);
    QVERIFY(red.is_ok() && red.value().size() == 1);
    QCOMPARE(red.value()[0].value.state, ValueState::Missing);
    // The stored cell is NULL, not 0.
    auto cell = Database::instance().execute("SELECT value IS NULL, raw_text IS NULL FROM etf_observations WHERE "
                                             "measure = 'nport_redemption'");
    QVERIFY(cell.is_ok() && cell.value().next());
    QCOMPARE(cell.value().value(0).toInt(), 1);
    QCOMPARE(cell.value().value(1).toInt(), 1);
    auto bar = repo().vintages(SubjectType::ListedInstrument, instrument, QStringLiteral("bar_close"),
                               QDate(2026, 9, 24), SourceType::IbkrTwsReadonly);
    QVERIFY(bar.is_ok() && bar.value().size() == 1);
    QCOMPARE(bar.value()[0].history_type, QStringLiteral("market_backfill"));
    QCOMPARE(bar.value()[0].point_in_time_status, QStringLiteral("historical_assumption"));
    QCOMPARE(bar.value()[0].available_from, utc("2026-09-24T20:00:00.000Z"));
    QCOMPARE(count("etf_retrieval_issues"), 1);
}

void TstEtfStore::exact_replay_changes_nothing() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const auto in = bar_input(instrument, 10.0, r, "2026-09-25T15:00:05.000Z");
    QCOMPARE(repo().record_observation(in).value(), ObservationOutcome::InsertedOriginal);
    QCOMPARE(repo().record_observation(in).value(), ObservationOutcome::AlreadyRecorded);
    QCOMPARE(repo().record_observation(in).value(), ObservationOutcome::AlreadyRecorded);
    QCOMPARE(count("etf_observations"), 1);
    auto v = repo().vintages(SubjectType::ListedInstrument, instrument, QStringLiteral("bar_close"), QDate(2026, 9, 24),
                             SourceType::IbkrTwsReadonly);
    QCOMPARE(v.value()[0].seen_count, 1);
}

void TstEtfStore::later_retrieval_confirms_without_a_new_vintage() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r1 = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const qint64 r2 = ibkr_retrieval("2026-09-26T15:00:00.000Z");
    QVERIFY(repo().record_observation(bar_input(instrument, 10.0, r1, "2026-09-25T15:00:05.000Z")).is_ok());
    QCOMPARE(repo().record_observation(bar_input(instrument, 10.0, r2, "2026-09-26T15:00:05.000Z")).value(),
             ObservationOutcome::Confirmed);
    auto v = repo().vintages(SubjectType::ListedInstrument, instrument, QStringLiteral("bar_close"), QDate(2026, 9, 24),
                             SourceType::IbkrTwsReadonly);
    QCOMPARE(v.value().size(), 1);
    QCOMPARE(v.value()[0].seen_count, 2);
    QCOMPARE(v.value()[0].first_seen_at, utc("2026-09-25T15:00:05.000Z")); // first sighting unchanged
    QCOMPARE(v.value()[0].last_seen_at, utc("2026-09-26T15:00:05.000Z"));
    QCOMPARE(v.value()[0].first_retrieval_id, r1);
    QCOMPARE(v.value()[0].last_retrieval_id, r2);
}

void TstEtfStore::ibkr_revision_keeps_the_earlier_vintage() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r1 = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const qint64 r2 = ibkr_retrieval("2026-09-26T06:00:00.000Z");
    QVERIFY(repo().record_observation(bar_input(instrument, 10.0, r1, "2026-09-25T15:00:05.000Z")).is_ok());
    QCOMPARE(repo().record_observation(bar_input(instrument, 10.01, r2, "2026-09-26T06:00:05.000Z")).value(),
             ObservationOutcome::InsertedRevision);
    auto v = repo().vintages(SubjectType::ListedInstrument, instrument, QStringLiteral("bar_close"), QDate(2026, 9, 24),
                             SourceType::IbkrTwsReadonly);
    QCOMPARE(v.value().size(), 2);
    QCOMPARE(v.value()[0].value.value, 10.0); // the earlier value still exists
    QCOMPARE(v.value()[0].revision_state, QStringLiteral("original"));
    QCOMPARE(v.value()[1].value.value, 10.01);
    QCOMPARE(v.value()[1].revision_state, QStringLiteral("revised"));
    QCOMPARE(v.value()[1].source_revision, 2);
    QCOMPARE(v.value()[1].history_type, QStringLiteral("market_backfill"));
    QCOMPARE(v.value()[1].point_in_time_status, QStringLiteral("historical_assumption"));
    QCOMPARE(v.value()[1].available_from, utc("2026-09-26T06:00:05.000Z"));
    QCOMPARE(derived_quality(v.value(), 1), QualityState::Revised);
    QCOMPARE(vintage_as_of_index(v.value(), utc("2026-09-25T23:00:00.000Z")), 0);
}

void TstEtfStore::older_retrieval_reapplied_is_a_noop() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r1 = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const qint64 r2 = ibkr_retrieval("2026-09-26T15:00:00.000Z");
    const qint64 r3 = ibkr_retrieval("2026-09-27T15:00:00.000Z");
    QVERIFY(repo().record_observation(bar_input(instrument, 10.0, r1, "2026-09-25T15:00:05.000Z")).is_ok());
    QVERIFY(repo().record_observation(bar_input(instrument, 10.0, r2, "2026-09-26T15:00:05.000Z")).is_ok());
    QVERIFY(repo().record_observation(bar_input(instrument, 11.0, r3, "2026-09-27T15:00:05.000Z")).is_ok());
    // Re-applying r2 (older than the newest record) adds nothing.
    QCOMPARE(repo().record_observation(bar_input(instrument, 10.0, r2, "2026-09-26T15:00:05.000Z")).value(),
             ObservationOutcome::AlreadyRecorded);
    QCOMPARE(count("etf_observations"), 2);
}

void TstEtfStore::a_value_that_flips_back_is_a_new_vintage() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r1 = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const qint64 r2 = ibkr_retrieval("2026-09-26T15:00:00.000Z");
    const qint64 r3 = ibkr_retrieval("2026-09-27T15:00:00.000Z");
    QVERIFY(repo().record_observation(bar_input(instrument, 10.0, r1, "2026-09-25T15:00:05.000Z")).is_ok());
    QVERIFY(repo().record_observation(bar_input(instrument, 11.0, r2, "2026-09-26T15:00:05.000Z")).is_ok());
    QCOMPARE(repo().record_observation(bar_input(instrument, 10.0, r3, "2026-09-27T15:00:05.000Z")).value(),
             ObservationOutcome::InsertedRevision);
    auto v = repo().vintages(SubjectType::ListedInstrument, instrument, QStringLiteral("bar_close"), QDate(2026, 9, 24),
                             SourceType::IbkrTwsReadonly);
    QCOMPARE(v.value().size(), 3);
    QCOMPARE(vintage_as_of_index(v.value(), utc("2026-09-26T20:00:00.000Z")), 1); // 11.0 was known then
}

void TstEtfStore::sec_amendment_is_its_own_vintage() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 entity = add_entity("0001100663", "S000004310");
    const qint64 r1 = sec_retrieval("2026-09-01T10:00:00.000Z");
    const qint64 r2 = sec_retrieval("2026-09-01T10:00:02.000Z");
    const qint64 original = add_filing("0002071691-25-007634", "NPORT-P", "2025-11-26T17:01:17.000Z", entity, r1);
    const qint64 amendment = add_filing("0002071691-26-015790", "NPORT-P/A", "2026-07-13T14:48:14.000Z", entity, r2);
    const FieldValue value = parse_decimal_field("701369339021.20", true);
    QCOMPARE(repo()
                 .record_observation(sec_input(entity, original, "0002071691-25-007634", false, value, r1,
                                               "2026-09-01T10:00:01.000Z", "2025-11-26T17:01:17.000Z"))
                 .value(),
             ObservationOutcome::InsertedOriginal);
    QCOMPARE(repo()
                 .record_observation(sec_input(entity, amendment, "0002071691-26-015790", true, value, r2,
                                               "2026-09-01T10:00:03.000Z", "2026-07-13T14:48:14.000Z"))
                 .value(),
             ObservationOutcome::InsertedAmendment);
    auto v = repo().vintages(SubjectType::ReportingEntity, entity, QStringLiteral("nport_net_assets"),
                             QDate(2026, 6, 30), SourceType::SecNport);
    QCOMPARE(v.value().size(), 2);
    QCOMPARE(v.value()[0].revision_state, QStringLiteral("original"));
    QCOMPARE(v.value()[1].revision_state, QStringLiteral("amended_filing"));
    QCOMPARE(current_vintage_index(v.value()), 1);
    QCOMPARE(derived_quality(v.value(), 1), QualityState::Confirmed); // the real amendment changed nothing
    // Each vintage keeps its own acceptance-based availability.
    QCOMPARE(v.value()[0].available_from, utc("2025-11-28T14:30:00.000Z")); // Thanksgiving: the Friday
    QCOMPARE(v.value()[1].available_from, utc("2026-07-14T13:30:00.000Z"));
    // A changed amendment is REVISED, and the original is untouched.
    const qint64 r3 = sec_retrieval("2026-09-02T10:00:00.000Z");
    const qint64 amendment2 = add_filing("0002071691-26-099999", "NPORT-P/A", "2026-08-01T14:00:00.000Z", entity, r3);
    QVERIFY(repo()
                .record_observation(sec_input(entity, amendment2, "0002071691-26-099999", true,
                                              parse_decimal_field("701369339000.00", true), r3,
                                              "2026-09-02T10:00:01.000Z", "2026-08-01T14:00:00.000Z"))
                .is_ok());
    v = repo().vintages(SubjectType::ReportingEntity, entity, QStringLiteral("nport_net_assets"), QDate(2026, 6, 30),
                        SourceType::SecNport);
    QCOMPARE(v.value().size(), 3);
    QCOMPARE(v.value()[0].value.value, 701369339021.20);
    QCOMPARE(derived_quality(v.value(), current_vintage_index(v.value())), QualityState::Revised);
}

void TstEtfStore::sec_value_change_under_one_accession_is_refused() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 entity = add_entity("0000884394");
    const qint64 r1 = sec_retrieval("2026-09-01T10:00:00.000Z");
    const qint64 r2 = sec_retrieval("2026-09-02T10:00:00.000Z");
    const qint64 filing = add_filing("0001410368-26-089410", "NPORT-P", "2026-08-28T12:25:47.000Z", entity, r1);
    QVERIFY(
        repo()
            .record_observation(sec_input(entity, filing, "0001410368-26-089410", false, parse_decimal_field("1", true),
                                          r1, "2026-09-01T10:00:01.000Z", "2026-08-28T12:25:47.000Z"))
            .is_ok());
    QCOMPARE(
        repo()
            .record_observation(sec_input(entity, filing, "0001410368-26-089410", false, parse_decimal_field("2", true),
                                          r2, "2026-09-02T10:00:01.000Z", "2026-08-28T12:25:47.000Z"))
            .value(),
        ObservationOutcome::RefusedDocumentChanged);
    QCOMPARE(count("etf_observations"), 1);
}

void TstEtfStore::confirmation_needs_the_same_meaning() {
    QVERIFY(!open_fresh().isEmpty());
    // SEC: the same number under the same accession, delivered later with any
    // part of its meaning changed, is refused, never taken as a confirmation.
    const qint64 entity = add_entity("0000884394");
    const qint64 r1 = sec_retrieval("2026-09-01T10:00:00.000Z");
    const qint64 r2 = sec_retrieval("2026-09-02T10:00:00.000Z");
    const qint64 filing = add_filing("0001410368-26-089410", "NPORT-P", "2026-08-28T12:25:47.000Z", entity, r1);
    const auto first = sec_input(entity, filing, "0001410368-26-089410", false, parse_decimal_field("100", true), r1,
                                 "2026-09-01T10:00:01.000Z", "2026-08-28T12:25:47.000Z");
    QCOMPARE(repo().record_observation(first).value(), ObservationOutcome::InsertedOriginal);
    void (*const changes[])(etf_store::ObservationInput&) = {
        [](etf_store::ObservationInput& in) { in.basis = QStringLiteral("regulatory_report_date_net_assets"); },
        [](etf_store::ObservationInput& in) { in.units = QStringLiteral("EUR"); },
        [](etf_store::ObservationInput& in) { in.kind = MeasurementKind::AccountingObservation; },
        [](etf_store::ObservationInput& in) { in.period_end = QDate(2026, 6, 30); },
        [](etf_store::ObservationInput& in) { in.report_period = QDate(2026, 3, 31); },
        [](etf_store::ObservationInput& in) { in.accepted_at = utc("2026-08-28T12:25:48.000Z"); },
    };
    for (auto change : changes) {
        auto later = first;
        later.retrieval_id = r2;
        later.seen_at = utc("2026-09-02T10:00:01.000Z");
        change(later);
        QCOMPARE(repo().record_observation(later).value(), ObservationOutcome::RefusedDocumentChanged);
    }
    auto v = repo().vintages(SubjectType::ReportingEntity, entity, QStringLiteral("nport_net_assets"),
                             QDate(2026, 6, 30), SourceType::SecNport);
    QVERIFY(v.is_ok());
    QCOMPARE(v.value().size(), 1);
    QCOMPARE(v.value()[0].basis, first.basis); // the stored meaning is untouched...
    QCOMPARE(v.value()[0].seen_count, 1);      // ...and no sighting was added to it
    QCOMPARE(v.value()[0].last_retrieval_id, r1);
    auto same = first; // the same number with the same meaning still confirms
    same.retrieval_id = r2;
    same.seen_at = utc("2026-09-02T10:00:01.000Z");
    QCOMPARE(repo().record_observation(same).value(), ObservationOutcome::Confirmed);

    // IBKR: the same close in other units is a new vintage beside the old one.
    const qint64 instrument = add_instrument(756733, "SPY");
    const qint64 b1 = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const qint64 b2 = ibkr_retrieval("2026-09-26T15:00:00.000Z");
    QCOMPARE(repo().record_observation(bar_input(instrument, 10.0, b1, "2026-09-25T15:00:05.000Z")).value(),
             ObservationOutcome::InsertedOriginal);
    auto other_units = bar_input(instrument, 10.0, b2, "2026-09-26T15:00:05.000Z");
    other_units.units = QStringLiteral("EUR_per_share");
    QCOMPARE(repo().record_observation(other_units).value(), ObservationOutcome::InsertedRevision);
    auto bars = repo().vintages(SubjectType::ListedInstrument, instrument, QStringLiteral("bar_close"),
                                QDate(2026, 9, 24), SourceType::IbkrTwsReadonly);
    QVERIFY(bars.is_ok());
    QCOMPARE(bars.value().size(), 2);
    QCOMPARE(bars.value()[0].units, QStringLiteral("USD_per_share"));
    QCOMPARE(bars.value()[0].seen_count, 1);
    QCOMPARE(bars.value()[1].units, QStringLiteral("EUR_per_share"));
}

void TstEtfStore::filing_document_change_is_refused() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 entity = add_entity("0000884394");
    const qint64 r1 = sec_retrieval("2026-09-01T10:00:00.000Z");
    etf_store::SecFilingFacts f;
    f.accession = QStringLiteral("0001410368-26-089410");
    f.filer_cik = QStringLiteral("0000884394");
    f.form = QStringLiteral("NPORT-P");
    f.filing_date = QDate(2026, 8, 28);
    f.accepted_at = utc("2026-08-28T12:25:47.000Z");
    f.entity_id = entity;
    f.rep_pd_date = QDate(2026, 6, 30);
    f.document_sha256 = QStringLiteral("aaa");
    QVERIFY(!repo().stored_filing_sha256(f.accession).value().has_value());
    QCOMPARE(repo().upsert_sec_filing(f, r1, utc("2026-09-01T10:00:00.000Z")).value().second,
             etf_store::FilingOutcome::Inserted);
    QCOMPARE(repo().stored_filing_sha256(f.accession).value(), std::optional<QString>(QStringLiteral("aaa")));
    const qint64 r2 = sec_retrieval("2026-09-02T10:00:00.000Z");
    QCOMPARE(repo().upsert_sec_filing(f, r2, utc("2026-09-02T10:00:00.000Z")).value().second,
             etf_store::FilingOutcome::Confirmed);
    // Other bytes under the stored accession: an error, so the caller's
    // transaction cannot commit, and nothing written.
    const qint64 r3 = sec_retrieval("2026-09-03T10:00:00.000Z");
    f.document_sha256 = QStringLiteral("bbb");
    auto changed = repo().upsert_sec_filing(f, r3, utc("2026-09-03T10:00:00.000Z"));
    QVERIFY(changed.is_err());
    QVERIFY(QString::fromStdString(changed.error()).contains(QLatin1String("different document")));
    auto row = Database::instance().execute(
        "SELECT document_sha256 || '|' || last_retrieval_id || '|' || last_seen_at FROM etf_sec_filings");
    QVERIFY(row.is_ok() && row.value().next());
    QCOMPARE(row.value().value(0).toString(), QStringLiteral("aaa|%1|2026-09-02T10:00:00.000Z").arg(r2));
    QCOMPARE(repo().stored_filing_sha256(f.accession).value(), std::optional<QString>(QStringLiteral("aaa")));
}

void TstEtfStore::missing_unparseable_and_zero_are_preserved() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 entity = add_entity("0000884394");
    const qint64 r1 = sec_retrieval("2026-09-01T10:00:00.000Z");
    const qint64 filing = add_filing("0001410368-26-089410", "NPORT-P", "2026-08-28T12:25:47.000Z", entity, r1);
    const struct {
        const char* measure;
        FieldValue value;
    } cases[] = {{"m_missing", FieldValue::missing()},
                 {"m_unparseable", parse_decimal_field("N/A", true)},
                 {"m_zero", parse_decimal_field("0.00000000", true)}};
    for (const auto& c : cases) {
        auto in = sec_input(entity, filing, "0001410368-26-089410", false, c.value, r1, "2026-09-01T10:00:01.000Z",
                            "2026-08-28T12:25:47.000Z");
        in.measure = QString::fromLatin1(c.measure);
        QVERIFY(repo().record_observation(in).is_ok());
    }
    auto q = Database::instance().execute("SELECT measure, value IS NULL, value, value_state, raw_text FROM "
                                          "etf_observations ORDER BY measure");
    QVERIFY(q.is_ok());
    QVERIFY(q.value().next()); // m_missing
    QCOMPARE(q.value().value(1).toInt(), 1);
    QCOMPARE(q.value().value(3).toString(), QStringLiteral("missing"));
    QVERIFY(q.value().value(4).isNull());
    QVERIFY(q.value().next()); // m_unparseable
    QCOMPARE(q.value().value(1).toInt(), 1);
    QCOMPARE(q.value().value(3).toString(), QStringLiteral("unparseable"));
    QCOMPARE(q.value().value(4).toString(), QStringLiteral("N/A"));
    QVERIFY(q.value().next()); // m_zero
    QCOMPARE(q.value().value(1).toInt(), 0);
    QCOMPARE(q.value().value(2).toDouble(), 0.0);
    QCOMPARE(q.value().value(3).toString(), QStringLiteral("reported"));
}

void TstEtfStore::instrument_identity_is_the_conid() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 a = add_instrument(111, "SPLG", "2026-01-01T00:00:00.000Z");
    const qint64 renamed = add_instrument(111, "SPYM", "2026-06-01T00:00:00.000Z"); // ticker change, same conId
    QCOMPARE(renamed, a);
    const qint64 reused = add_instrument(222, "SPLG", "2026-07-01T00:00:00.000Z"); // ticker reused elsewhere
    QVERIFY(reused != a);
    auto sym = Database::instance().execute(
        "SELECT symbol, first_seen_at FROM etf_instrument_symbols WHERE instrument_id = ? ORDER BY first_seen_at", {a});
    QVERIFY(sym.is_ok());
    QVERIFY(sym.value().next());
    QCOMPARE(sym.value().value(0).toString(), QStringLiteral("SPLG"));
    QVERIFY(sym.value().next());
    QCOMPARE(sym.value().value(0).toString(), QStringLiteral("SPYM"));
    auto cur = Database::instance().execute("SELECT symbol FROM etf_listed_instruments WHERE instrument_id = ?", {a});
    QVERIFY(cur.is_ok() && cur.value().next());
    QCOMPARE(cur.value().value(0).toString(), QStringLiteral("SPYM"));
    // An older sighting processed later does not roll the symbol back.
    add_instrument(111, "SPLG", "2025-12-01T00:00:00.000Z");
    auto again = Database::instance().execute("SELECT symbol FROM etf_listed_instruments WHERE instrument_id = ?", {a});
    QVERIFY(again.is_ok() && again.value().next());
    QCOMPARE(again.value().value(0).toString(), QStringLiteral("SPYM"));
    QVERIFY(repo().upsert_listed_instrument({0, "X", "STK", "", "", "USD"}, utc("2026-01-01T00:00:00.000Z")).is_err());
}

void TstEtfStore::an_ordinary_stock_is_refused_twice() {
    QVERIFY(!open_fresh().isEmpty());
    // The repository refuses a listed instrument IBKR does not classify as an
    // ETF, whatever its secType says...
    etf_store::ListedInstrumentFacts aapl;
    aapl.con_id = 265598;
    aapl.symbol = QStringLiteral("AAPL");
    aapl.security_type = QStringLiteral("STK");
    aapl.currency = QStringLiteral("USD");
    for (const char* stock_type : {"", "COMMON", "etf"}) {
        aapl.stock_type = QString::fromLatin1(stock_type);
        auto refused = repo().upsert_listed_instrument(aapl, utc("2026-09-25T15:00:00.000Z"));
        QVERIFY2(refused.is_err(), stock_type);
        // The repository's own check refuses first; the schema's CHECK is the
        // second, independent guard behind it.
        QCOMPARE(QString::fromStdString(refused.error()),
                 QStringLiteral("a listed instrument of the ETF store needs IBKR stockType ETF"));
    }
    // ...and the database refuses it on its own, whoever writes the row.
    const QString insert = QStringLiteral(
        "INSERT INTO etf_listed_instruments (ibkr_con_id, symbol, security_type, stock_type, currency, first_seen_at, "
        "last_seen_at) VALUES (?, ?, 'STK', ?, 'USD', '2026-09-25T15:00:00.000Z', '2026-09-25T15:00:00.000Z')");
    QVERIFY(raw_insert_fails(insert, {265598, QStringLiteral("AAPL"), QStringLiteral("COMMON")}));
    QVERIFY(!raw_insert_fails(insert, {756733, QStringLiteral("SPY"), QStringLiteral("ETF")})); // the rule, not the SQL
    QCOMPARE(count("etf_listed_instruments"), 1);
}

void TstEtfStore::reporting_entity_identity_is_cik_and_series() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 registrant = add_entity("0001067839");
    const qint64 series = add_entity("0001067839", "S000101292");
    QVERIFY(registrant != series); // QQQ before and after its conversion are different reporting entities
    QCOMPARE(add_entity("0001067839"), registrant);
    QCOMPARE(add_entity("0001067839", "S000101292"), series);
    auto lvl = Database::instance().execute("SELECT reporting_level, series_id FROM etf_reporting_entities ORDER BY "
                                            "entity_id");
    QVERIFY(lvl.is_ok());
    QVERIFY(lvl.value().next());
    QCOMPARE(lvl.value().value(0).toString(), QStringLiteral("registrant"));
    QCOMPARE(lvl.value().value(1).toString(), QString(""));
    QVERIFY(lvl.value().next());
    QCOMPARE(lvl.value().value(0).toString(), QStringLiteral("series"));
    QCOMPARE(*repo().find_reporting_entity(QStringLiteral("0001067839"), QString()).value(), registrant);
    // Attributes without the acceptance time of their filing have no place in
    // the entity's chronology: refused.
    etf_store::ReportingEntityFacts undated;
    undated.cik = QStringLiteral("0001067839");
    QVERIFY(repo().upsert_reporting_entity(undated, utc("2026-09-01T00:00:00.000Z")).is_err());
}

void TstEtfStore::identity_links_follow_the_class_structure() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 multi = add_entity("0000036405", "S000002839"); // a Vanguard-like multi-class series
    const qint64 single = add_entity("0001100663", "S000004310");
    const qint64 uit = add_entity("0000884394");
    const qint64 r = sec_retrieval("2026-09-01T10:00:00.000Z");
    add_filing("0000932471-26-000001", "NPORT-P", "2026-08-27T12:00:00.000Z", multi, r, "C000007773,C000007774");
    add_filing("0002071691-26-019760", "NPORT-P", "2026-08-25T14:10:51.000Z", single, r, "C000012040");
    const qint64 voo = add_instrument(136155102, "VOO");
    const qint64 ivv = add_instrument(8991352, "IVV");
    const qint64 spy = add_instrument(756733, "SPY");
    const QDateTime at = utc("2026-09-02T00:00:00.000Z");
    QVERIFY(repo()
                .declare_link(voo, multi, QStringLiteral("C000007774"), LinkRelationship::SoleClassOfSeries,
                              QStringLiteral("test"), at)
                .is_err());
    QVERIFY(repo()
                .declare_link(voo, multi, QStringLiteral("C000007774"), LinkRelationship::ClassOfMultiClassSeries,
                              QStringLiteral("test"), at)
                .is_ok());
    QVERIFY(repo()
                .declare_link(ivv, single, QStringLiteral("C000012040"), LinkRelationship::ClassOfMultiClassSeries,
                              QStringLiteral("test"), at)
                .is_err());
    QVERIFY(repo()
                .declare_link(ivv, single, QStringLiteral("C000099999"), LinkRelationship::SoleClassOfSeries,
                              QStringLiteral("test"), at)
                .is_err()); // a class the filing does not report
    QVERIFY(repo()
                .declare_link(ivv, single, QStringLiteral("C000012040"), LinkRelationship::SoleClassOfSeries,
                              QStringLiteral("test"), at)
                .is_ok());
    QVERIFY(repo()
                .declare_link(spy, uit, QString(), LinkRelationship::SoleClassOfSeries, QStringLiteral("test"), at)
                .is_err());
    QVERIFY(repo()
                .declare_link(spy, uit, QString(), LinkRelationship::RegistrantIsInstrument, QString(), at)
                .is_err()); // a link needs a basis
    QVERIFY(repo()
                .declare_link(spy, uit, QString(), LinkRelationship::RegistrantIsInstrument, QStringLiteral("test"), at)
                .is_ok());
    // Links are never rewritten silently.
    QVERIFY(repo()
                .declare_link(voo, multi, QStringLiteral("C000007774"), LinkRelationship::SoleClassOfSeries,
                              QStringLiteral("other"), at)
                .is_err());
    auto rel = repo().nport_link_relationship(voo);
    QVERIFY(rel.is_ok() && rel.value().has_value());
    QCOMPARE(*rel.value(), LinkRelationship::ClassOfMultiClassSeries);
    bool not_applicable = false;
    for (const auto& s : flow_route_availability(rel.value()))
        not_applicable = not_applicable || (s.route == FlowRoute::RegulatoryMonthly &&
                                            s.availability == FlowRouteAvailability::NotApplicable);
    QVERIFY(not_applicable);
    QVERIFY(!repo().nport_link_relationship(add_instrument(5, "NOLINK")).value().has_value());
}

void TstEtfStore::observation_start_is_set_once() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r1 = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const qint64 r2 = ibkr_retrieval("2026-10-01T15:00:00.000Z");
    auto first = repo().observation_start(SourceType::IbkrTwsReadonly, SubjectType::ListedInstrument, instrument,
                                          utc("2026-09-25T15:00:00.000Z"), r1);
    auto second = repo().observation_start(SourceType::IbkrTwsReadonly, SubjectType::ListedInstrument, instrument,
                                           utc("2026-10-01T15:00:00.000Z"), r2);
    QVERIFY(first.is_ok() && second.is_ok());
    QCOMPARE(second.value(), utc("2026-09-25T15:00:00.000Z"));
    QCOMPARE(count("etf_observation_starts"), 1);
}

void TstEtfStore::session_calendar_is_stored() {
    QVERIFY(!open_fresh().isEmpty());
    const auto days = UsEquityCalendar::weekdays_in(QDate(2026, 11, 23), QDate(2026, 11, 27));
    QVERIFY(repo().upsert_sessions(days).is_ok());
    QVERIFY(repo().upsert_sessions(days).is_ok()); // idempotent
    QCOMPARE(count("etf_market_sessions"), 5);
    auto q = Database::instance().execute("SELECT session_date, day_type, close_local, close_utc, is_early_close FROM "
                                          "etf_market_sessions ORDER BY session_date");
    QVERIFY(q.is_ok());
    QMap<QString, QStringList> rows;
    while (q.value().next())
        rows.insert(q.value().value(0).toString(), {q.value().value(1).toString(), q.value().value(2).toString(),
                                                    q.value().value(3).toString(), q.value().value(4).toString()});
    QCOMPARE(rows.value("2026-11-26").value(0), QStringLiteral("holiday"));
    QCOMPARE(rows.value("2026-11-27").value(0), QStringLiteral("early_close"));
    QCOMPARE(rows.value("2026-11-27").value(1), QStringLiteral("13:00"));
    QCOMPARE(rows.value("2026-11-27").value(2), QStringLiteral("2026-11-27T18:00:00.000Z"));
    QCOMPARE(rows.value("2026-11-27").value(3), QStringLiteral("1"));
    QCOMPARE(rows.value("2026-11-24").value(0), QStringLiteral("regular"));
}

void TstEtfStore::the_database_refuses_rule_violations() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    const QString base = QStringLiteral(
        "INSERT INTO etf_observations (subject_type, subject_id, measurement_kind, measure, units, source_type, "
        "acquisition_mode, source_document, accepted_at, filing_id, effective_date, value, value_state, "
        "source_revision, revision_state, history_type, point_in_time_status, availability_basis, available_from, "
        "first_seen_at, last_seen_at, first_retrieval_id, last_retrieval_id) VALUES ('listed_instrument', ?, %1, "
        "'bar_close', 'USD_per_share', %2, %3, %4, %5, NULL, ?, %6, %7, 1, 'original', %8, %9, %10, "
        "%11, '2026-09-25T15:00:00.000Z', '2026-09-25T15:00:00.000Z', ?, ?)");
    // Every row below gets its own effective date, so no row can be refused by
    // the UNIQUE key instead of by the rule it is meant to break (a mutation
    // check caught exactly that vacuity in an earlier revision of this test).
    auto sql = [&](const char* kind, const char* source, const char* mode, const char* doc, const char* accepted,
                   const char* value, const char* state, const char* history, const char* pit, const char* basis,
                   const char* available) {
        return base
            .arg(QLatin1String(kind), QLatin1String(source), QLatin1String(mode), QLatin1String(doc),
                 QLatin1String(accepted), QLatin1String(value), QLatin1String(state), QLatin1String(history),
                 QLatin1String(pit))
            .arg(QLatin1String(basis), QLatin1String(available));
    };
    const char* ok_row[] = {"'market_bar'",
                            "'ibkr_tws_readonly'",
                            "'ibkr_readonly_wrapper'",
                            "''",
                            "NULL",
                            "10.0",
                            "'reported'",
                            "'market_backfill'",
                            "'historical_assumption'",
                            "'session_close_assumption'",
                            "'2026-09-24T20:00:00.000Z'"};
    // The well-formed row is accepted: the refusals below are the rules, not the SQL.
    QVERIFY(!raw_insert_fails(sql(ok_row[0], ok_row[1], ok_row[2], ok_row[3], ok_row[4], ok_row[5], ok_row[6],
                                  ok_row[7], ok_row[8], ok_row[9], ok_row[10]),
                              {instrument, QStringLiteral("2026-09-24"), r, r}));
    struct Violation {
        const char* why;
        int column;
        const char* value;
    };
    const Violation violations[] = {
        {"a calculated kind is not a source fact", 0, "'calculated_creation_redemption_flow'"},
        {"a rotation proxy is not a source fact", 0, "'rotation_proxy'"},
        {"a disabled route holds no observation", 1, "'issuer_file'"},
        {"a disabled mode holds no observation", 2, "'user_initiated_import'"},
        {"an IBKR bar has no source document", 3, "'X'"},
        {"an IBKR bar has no acceptance time", 4, "'2026-09-24T20:00:00.000Z'"},
        {"a missing value is never a number", 6, "'missing'"},
        {"backfill is never observed", 8, "'observed'"},
        {"an IBKR bar is not a regulatory filing", 7, "'regulatory_filing'"},
        {"no availability time means not point in time", 10, "NULL"},
        {"an unknown availability basis", 9, "'someday'"},
    };
    int day = 0;
    for (const Violation& v : violations) {
        const char* row[11];
        for (int i = 0; i < 11; ++i)
            row[i] = ok_row[i];
        row[v.column] = v.value;
        const QString unique_date = QDate(2026, 8, 1).addDays(day++).toString(Qt::ISODate);
        QVERIFY2(raw_insert_fails(
                     sql(row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7], row[8], row[9], row[10]),
                     {instrument, unique_date, r, r}),
                 v.why);
    }
    const char* reported_null[11] = {"'market_bar'",
                                     "'ibkr_tws_readonly'",
                                     "'ibkr_readonly_wrapper'",
                                     "''",
                                     "NULL",
                                     "NULL",
                                     "'reported'",
                                     "'market_backfill'",
                                     "'historical_assumption'",
                                     "'session_close_assumption'",
                                     "'2026-09-24T20:00:00.000Z'"};
    QVERIFY2(raw_insert_fails(sql(reported_null[0], reported_null[1], reported_null[2], reported_null[3],
                                  reported_null[4], reported_null[5], reported_null[6], reported_null[7],
                                  reported_null[8], reported_null[9], reported_null[10]),
                              {instrument, QStringLiteral("2026-07-01"), r, r}),
             "a reported value is never NULL");
    QCOMPARE(count("etf_observations"), 1); // only the well-formed row
    // A refused request has no response of any kind.
    QVERIFY(raw_insert_fails("INSERT INTO etf_retrievals (source_type, acquisition_mode, endpoint, request_ref, "
                             "requested_at, status, http_status, interpretation, route_policy) VALUES ('issuer_file', "
                             "'user_initiated_import', 'x', 'x', '2026-09-25T15:00:00.000Z', 'ROUTE_DISABLED', 200, "
                             "'x', 'x')"));
    // An SEC observation always names its accession, filing and acceptance time.
    const qint64 entity = add_entity("0000884394");
    QVERIFY(raw_insert_fails(
        "INSERT INTO etf_observations (subject_type, subject_id, measurement_kind, measure, units, source_type, "
        "acquisition_mode, source_document, effective_date, value, value_state, source_revision, revision_state, "
        "history_type, point_in_time_status, availability_basis, available_from, first_seen_at, last_seen_at, "
        "first_retrieval_id, last_retrieval_id) VALUES ('reporting_entity', ?, 'aum_observation', 'nport_net_assets', "
        "'USD', 'sec_nport', 'regulatory_api', '', '2026-06-30', 1.0, 'reported', 1, 'original', "
        "'regulatory_filing', 'conservative_rule', 'sec_next_session_after_acceptance_v1', "
        "'2026-08-31T13:30:00.000Z', '2026-09-01T10:00:00.000Z', '2026-09-01T10:00:00.000Z', ?, ?)",
        {entity, r, r}));
}

void TstEtfStore::repository_refuses_disabled_routes() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    for (auto [source, mode] : {std::pair{SourceType::IssuerFile, AcquisitionMode::UserInitiatedImport},
                                std::pair{SourceType::IssuerFile, AcquisitionMode::IssuerAutomated},
                                std::pair{SourceType::SecPeriodicXbrl, AcquisitionMode::RegulatoryApi}}) {
        auto in = bar_input(instrument, 1.0, r, "2026-09-25T15:00:05.000Z");
        in.source_type = source;
        in.acquisition_mode = mode;
        auto refused = repo().record_observation(in);
        QVERIFY(refused.is_err());
        // The repository's own route check refuses first; the schema's CHECK is
        // the second, independent guard behind it.
        QCOMPARE(QString::fromStdString(refused.error()), QStringLiteral("acquisition route disabled"));
    }
    auto proxy = bar_input(instrument, 1.0, r, "2026-09-25T15:00:05.000Z");
    proxy.kind = MeasurementKind::RotationProxy;
    auto refused_proxy = repo().record_observation(proxy);
    QVERIFY(refused_proxy.is_err());
    QCOMPARE(QString::fromStdString(refused_proxy.error()),
             QStringLiteral("calculated and proxy measurements are not source observations"));
    QCOMPARE(count("etf_observations"), 0);
    // A refused request can itself be recorded, with no response attached.
    etf_store::RetrievalRecord refused;
    refused.source_type = SourceType::IssuerFile;
    refused.acquisition_mode = AcquisitionMode::UserInitiatedImport;
    refused.endpoint = QStringLiteral("refused_before_request");
    refused.request_ref = QStringLiteral("none");
    refused.requested_at = utc("2026-09-25T15:00:00.000Z");
    refused.status = RetrievalStatus::RouteDisabled;
    refused.detail = acquisition_route(SourceType::IssuerFile, AcquisitionMode::UserInitiatedImport).reason;
    refused.interpretation = QStringLiteral("none");
    refused.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    QVERIFY(repo().record_retrieval(refused).is_ok());
}

void TstEtfStore::vocabulary_matches_the_database() {
    QVERIFY(!open_fresh().isEmpty());
    // Every retrieval status the code can produce is storable.
    for (auto s : {RetrievalStatus::Ok, RetrievalStatus::Stale, RetrievalStatus::SourceError,
                   RetrievalStatus::NotEntitled, RetrievalStatus::RouteDisabled, RetrievalStatus::NotConfigured}) {
        etf_store::RetrievalRecord r;
        r.source_type = SourceType::SecSubmissions;
        r.acquisition_mode = AcquisitionMode::RegulatoryApi;
        r.endpoint = QStringLiteral("x");
        r.request_ref = QStringLiteral("x");
        r.requested_at = utc("2026-09-25T15:00:00.000Z");
        r.status = s;
        r.interpretation = QStringLiteral("x");
        r.route_policy = QStringLiteral("x");
        QVERIFY2(repo().record_retrieval(r).is_ok(), retrieval_status_id(s));
    }
    // Every issue state the model allows is storable; quality states that are
    // display-only (CONFIRMED, CALCULATED, PROXY, REVISED) are not issues.
    const qint64 r = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    for (auto q : {QualityState::Missing, QualityState::Stale, QualityState::InProgressSession,
                   QualityState::SourceError, QualityState::NotApplicable, QualityState::NotEntitled,
                   QualityState::RouteDisabled, QualityState::ReconciliationException}) {
        etf_store::IssueRecord i;
        i.state = q;
        i.code = QStringLiteral("x");
        QVERIFY2(repo().record_issue(r, i).is_ok(), quality_state_id(q));
    }
    for (auto q : {QualityState::Confirmed, QualityState::Calculated, QualityState::Proxy, QualityState::Revised}) {
        etf_store::IssueRecord i;
        i.state = q;
        i.code = QStringLiteral("x");
        QVERIFY2(repo().record_issue(r, i).is_err(), quality_state_id(q));
    }
}

void TstEtfStore::export_is_deterministic() {
    QVERIFY(!open_fresh().isEmpty());
    const qint64 instrument = add_instrument(1, "AAA");
    const qint64 r = ibkr_retrieval("2026-09-25T15:00:00.000Z");
    QVERIFY(repo().record_observation(bar_input(instrument, 10.0, r, "2026-09-25T15:00:05.000Z")).is_ok());
    auto a = repo().export_all();
    auto b = repo().export_all();
    QVERIFY(a.is_ok() && b.is_ok());
    QCOMPARE(a.value(), b.value());
    const QJsonArray obs = a.value().value("etf_observations").toArray();
    QCOMPARE(obs.size(), 1);
    QVERIFY(obs[0].toObject().value("accepted_at").isNull());
    QCOMPARE(obs[0].toObject().value("value").toDouble(), 10.0);
}

void TstEtfStore::selftest_writes_nothing_without_its_transaction() {
    QVERIFY(!open_fresh().isEmpty());
    // A normal run passes and leaves no row behind.
    QCOMPARE(marketlab::run_etf_data_selftest(), 0);
    QCOMPARE(count("etf_retrievals"), 0);
    QCOMPARE(count("etf_observations"), 0);
    // A transaction already open on this connection makes the self-test's own
    // BEGIN fail. It must stop before its first write: going on would put its
    // writes into a transaction it does not own, and its closing rollback
    // would then end that transaction.
    QVERIFY(Database::instance().begin_transaction().is_ok());
    QCOMPARE(marketlab::run_etf_data_selftest(), 1);
    QCOMPARE(count("etf_retrievals"), 0);
    QCOMPARE(count("etf_reporting_entities"), 0);
    QVERIFY(Database::instance().commit().is_ok()); // the open transaction is still the caller's
}

QTEST_GUILESS_MAIN(TstEtfStore)
#include "tst_etf_store.moc"
