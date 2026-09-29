// Batch D group query over a real v052 store. The fixture writes source facts
// through Batch B's repository and reads them through the Batch D service.
#include "services/etf/EtfGroupAnalytics.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace fincept;
using namespace fincept::services::etf;

namespace {
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
    r.expected_taxonomy_version = QStringLiteral("etf-taxonomy-v1");
    r.output_from = QDate(2026, 10, 1);
    r.output_to = QDate(2026, 10, 31);
    return r;
}

QJsonObject group_month(const QJsonObject& document) {
    const QJsonArray groups = document.value(QStringLiteral("groups")).toArray();
    return groups.isEmpty() ? QJsonObject()
                            : groups[0].toObject().value(QStringLiteral("regulatory_months")).toArray()[0].toObject();
}

QJsonObject constituent(const QJsonObject& month, const QString& key) {
    for (const QJsonValue& value : month.value(QStringLiteral("constituents")).toArray()) {
        const QJsonObject row = value.toObject();
        if (row.value(QStringLiteral("stable_key")).toString() == key)
            return row;
    }
    return {};
}
} // namespace

class EtfGroupStoreTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
  private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        register_migration_v052();
        QVERIFY(Database::instance().open(dir_.filePath(QStringLiteral("groups.db"))).is_ok());
    }
    void cleanupTestCase() { Database::instance().close(); }
    void identity_link_cutoff_conflict_and_amendment_replay();
    void request_refusals_and_historical_intervals();
    void rotation_staleness_inputs_and_leverage_policy();
};

void EtfGroupStoreTest::identity_link_cutoff_conflict_and_amendment_replay() {
    const qint64 spy_entity = entity("0000884394", "", "2026-12-02T00:00:00.000Z");
    const qint64 ivv_entity = entity("0001100663", "S000004310", "2026-12-02T00:00:00.000Z");
    const qint64 spy_instrument = instrument(756733, "SPY", "2026-12-02T00:00:00.000Z");
    QVERIFY(spy_entity > 0 && ivv_entity > 0 && spy_instrument > 0);
    QVERIFY(filing(spy_entity, "0000884394", "0001193125-26-100001", "NPORT-P", "2026-11-25T15:00:00.000Z",
                   "2026-12-02T00:00:00.000Z", 100.0));
    QVERIFY(filing(ivv_entity, "0001100663", "0001193125-26-100002", "NPORT-P", "2026-11-25T15:00:00.000Z",
                   "2026-12-02T00:00:00.000Z", 50.0));
    const GroupRunRequest old = request("2026-12-03T00:00:00.000Z");
    const auto store_before = repo().export_all();
    QVERIFY(store_before.is_ok());
    const auto initial = run_group_research(old);
    QVERIFY2(initial.is_ok(), initial.is_ok() ? "" : initial.error().c_str());
    const QByteArray old_bytes = QJsonDocument(initial.value()).toJson(QJsonDocument::Compact);
    const QJsonObject initial_month = group_month(initial.value());
    QCOMPARE(initial_month.value(QStringLiteral("observed_net_flow_usd")).toDouble(), 150.0);
    QVERIFY(initial_month.value(QStringLiteral("complete_net_flow_usd")).isNull());
    QCOMPARE(constituent(initial_month, QStringLiteral("756733")).value(QStringLiteral("reason")).toString(),
             QStringLiteral("identity_not_established"));
    QCOMPARE(initial.value().value(QStringLiteral("taxonomy_version")).toString(), QStringLiteral("etf-taxonomy-v1"));
    QCOMPARE(initial.value().value(QStringLiteral("taxonomy_sha256")).toString().size(), 64);
    const auto store_after = repo().export_all();
    QVERIFY(store_after.is_ok());
    QCOMPARE(QJsonDocument(store_before.value()).toJson(), QJsonDocument(store_after.value()).toJson());

    const auto one_link = repo().declare_link(spy_instrument, spy_entity, {}, LinkRelationship::RegistrantIsInstrument,
                                              QStringLiteral("fixture declaration"), utc("2026-12-04T00:00:00.000Z"));
    QVERIFY2(one_link.is_ok(), one_link.is_ok() ? "" : one_link.error().c_str());
    auto before = repo().nport_links_known_at(spy_instrument, old.frame.known_at);
    auto after = repo().nport_links_known_at(spy_instrument, utc("2026-12-05T00:00:00.000Z"));
    QVERIFY(before.is_ok() && after.is_ok());
    QCOMPARE(before.value().size(), 0);
    QCOMPARE(after.value().size(), 1);
    const auto old_again = run_group_research(old);
    QVERIFY(old_again.is_ok());
    QCOMPARE(QJsonDocument(old_again.value()).toJson(QJsonDocument::Compact), old_bytes);
    const auto linked = run_group_research(request("2026-12-05T00:00:00.000Z"));
    QVERIFY(linked.is_ok());
    QCOMPARE(group_month(linked.value()).value(QStringLiteral("observed_net_flow_usd")).toDouble(), 150.0);
    QCOMPARE(
        constituent(group_month(linked.value()), QStringLiteral("756733")).value(QStringLiteral("status")).toString(),
        QStringLiteral("duplicate_reporting_identity"));

    const auto conflict = repo().declare_link(
        spy_instrument, ivv_entity, QStringLiteral("C000012040"), LinkRelationship::SoleClassOfSeries,
        QStringLiteral("conflicting fixture declaration"), utc("2026-12-06T00:00:00.000Z"));
    QVERIFY2(conflict.is_ok(), conflict.is_ok() ? "" : conflict.error().c_str());
    const auto ambiguous = run_group_research(request("2026-12-07T00:00:00.000Z"));
    QVERIFY(ambiguous.is_ok());
    const QJsonObject ambiguous_month = group_month(ambiguous.value());
    QCOMPARE(constituent(ambiguous_month, QStringLiteral("756733")).value(QStringLiteral("reason")).toString(),
             QStringLiteral("ambiguous_identity_links"));
    QVERIFY(ambiguous_month.value(QStringLiteral("complete_net_flow_usd")).isNull());
    QCOMPARE(ambiguous_month.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("basis")).toString(),
             QStringLiteral("unresolved_constituent"));

    QVERIFY(filing(spy_entity, "0000884394", "0001193125-26-100003", "NPORT-P/A", "2026-12-08T15:00:00.000Z",
                   "2026-12-09T00:00:00.000Z", 130.0, "0001193125-26-100001"));
    const auto original_replay = run_group_research(old);
    QVERIFY(original_replay.is_ok());
    QCOMPARE(QJsonDocument(original_replay.value()).toJson(QJsonDocument::Compact), old_bytes);
    const auto amended = run_group_research(request("2026-12-10T00:00:00.000Z"));
    QVERIFY(amended.is_ok());
    const QJsonObject amended_month = group_month(amended.value());
    QCOMPARE(amended_month.value(QStringLiteral("observed_net_flow_usd")).toDouble(), 180.0);
    QCOMPARE(constituent(amended_month, QStringLiteral("0000884394/"))
                 .value(QStringLiteral("selected_accession"))
                 .toString(),
             QStringLiteral("0001193125-26-100003"));
    QCOMPARE(constituent(amended_month, QStringLiteral("0000884394/"))
                 .value(QStringLiteral("net_flow"))
                 .toObject()
                 .value(QStringLiteral("state"))
                 .toString(),
             QStringLiteral("REVISED"));

    QVERIFY(instrument(756733, "RENAMED_SPY", "2026-12-11T00:00:00.000Z") > 0);
    const auto renamed = run_group_research(request("2026-12-12T00:00:00.000Z"));
    QVERIFY(renamed.is_ok());
    QVERIFY(!constituent(group_month(renamed.value()), QStringLiteral("756733")).isEmpty());
    QCOMPARE(QJsonDocument(run_group_research(old).value()).toJson(QJsonDocument::Compact), old_bytes);
}

void EtfGroupStoreTest::request_refusals_and_historical_intervals() {
    auto r = request("2026-12-03T00:00:00.000Z");
    r.expected_taxonomy_version = QStringLiteral("etf-taxonomy-v2");
    const auto mismatch = run_group_research(r);
    QVERIFY(mismatch.is_err());
    QCOMPARE(QString::fromStdString(mismatch.error()), QStringLiteral("taxonomy_version_mismatch"));
    r = request("2026-12-03T00:00:00.000Z");
    r.group_id = QStringLiteral("not_a_group");
    QVERIFY(run_group_research(r).is_err());
    r = request("2026-12-03T00:00:00.000Z");
    r.output_from = QDate(2026, 10, 2);
    QVERIFY(!group_request_problem(r).isEmpty());
    r.output_from = QDate(2026, 11, 1);
    QVERIFY(!group_request_problem(r).isEmpty());
    r.output_from = QDate(2026, 10, 1);
    r.output_to = QDate(2027, 1, 31);
    QVERIFY(!group_request_problem(r).isEmpty());

    r = request("2026-12-03T00:00:00.000Z");
    r.group_level = QStringLiteral("category");
    r.group_id = QStringLiteral("nasdaq_100");
    r.output_from = QDate(2025, 12, 1);
    r.output_to = QDate(2025, 12, 31);
    const auto history = run_group_research(r);
    QVERIFY2(history.is_ok(), history.is_ok() ? "" : history.error().c_str());
    const auto m = group_month(history.value());
    const auto qqq = constituent(m, QStringLiteral("0001067839/S000101292"));
    QCOMPARE(qqq.value(QStringLiteral("reason")).toString(), QStringLiteral("classification_month_partial"));
    QSet<QString> identities;
    for (const auto& row : m.value(QStringLiteral("constituents")).toArray()) {
        const QString key = row.toObject().value(QStringLiteral("stable_key")).toString();
        QVERIFY(!identities.contains(key));
        identities.insert(key);
    }
}

void EtfGroupStoreTest::rotation_staleness_inputs_and_leverage_policy() {
    const char* seen = "2026-12-02T23:00:00.000Z";
    const qint64 gold = instrument(51529211, "GLD", seen);
    QVERIFY(gold > 0);
    QVERIFY(instrument(738523410, "SH", seen) > 0);
    QVERIFY(instrument(99999999, "UNCLASSIFIED", seen) > 0);
    etf_store::RetrievalRecord ret;
    ret.run_id = QStringLiteral("rotation-fixture");
    ret.source_type = SourceType::IbkrTwsReadonly;
    ret.acquisition_mode = AcquisitionMode::IbkrReadonlyWrapper;
    ret.endpoint = QStringLiteral("fixture");
    ret.requested_at = utc(seen);
    ret.retrieved_at = utc(seen);
    ret.status = RetrievalStatus::Ok;
    ret.response_sha256 = QStringLiteral("fixture");
    ret.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    ret.interpretation = QStringLiteral("fixture");
    const auto retrieval_id = repo().record_retrieval(ret);
    QVERIFY(retrieval_id.is_ok());
    const auto days = UsEquityCalendar::weekdays_in(QDate(2026, 8, 3), QDate(2026, 11, 30));
    QVERIFY(repo().upsert_sessions(days).is_ok());
    int n = 0;
    for (const auto& d : days) {
        if (!d.is_session())
            continue;
        ++n;
        for (const char* measure : {"bar_close", "bar_volume"}) {
            etf_store::ObservationInput in;
            in.subject_type = SubjectType::ListedInstrument;
            in.subject_id = gold;
            in.kind = MeasurementKind::MarketBar;
            in.measure = QLatin1String(measure);
            in.units = in.measure == QLatin1String("bar_close") ? QStringLiteral("USD_per_share")
                                                                : QStringLiteral("shares_ibkr_filtered");
            in.basis = QStringLiteral("ibkr_trades_rth_daily_split_adjusted");
            in.source_type = ret.source_type;
            in.acquisition_mode = ret.acquisition_mode;
            in.effective_date = d.date;
            in.value = FieldValue::reported_value(100.0 + n);
            in.retrieval_id = retrieval_id.value();
            in.seen_at = utc(seen);
            in.observation_start = utc(seen);
            QVERIFY(repo().record_observation(in).is_ok());
        }
    }
    auto r = request("2026-12-03T22:00:00.000Z");
    r.group_level = QStringLiteral("cross_asset");
    r.group_id.clear();
    const auto document = run_group_research(r);
    QVERIFY(document.is_ok());
    bool found_gold = false;
    bool found_inverse = false;
    for (const auto& group : document.value().value(QStringLiteral("groups")).toArray()) {
        QVERIFY(group.toObject().value(QStringLiteral("rotation_aggregate")).isNull());
        for (const auto& item : group.toObject().value(QStringLiteral("rotation_constituents")).toArray()) {
            const auto row = item.toObject();
            if (row.value(QStringLiteral("stable_key")).toString() == QLatin1String("51529211")) {
                found_gold = true;
                QCOMPARE(row.value(QStringLiteral("status")).toString(), QStringLiteral("stale"));
                QCOMPARE(row.value(QStringLiteral("total_return")).toBool(), false);
                QVERIFY(!row.value(QStringLiteral("source_sessions")).toArray().isEmpty());
                const auto input = row.value(QStringLiteral("source_sessions"))
                                       .toArray()[0]
                                       .toObject()
                                       .value(QStringLiteral("inputs"))
                                       .toObject()
                                       .value(QStringLiteral("bar_close"))
                                       .toObject();
                QVERIFY(input.value(QStringLiteral("observation_id")).toInteger() > 0);
            }
            if (row.value(QStringLiteral("stable_key")).toString() == QLatin1String("738523410")) {
                found_inverse = true;
                QCOMPARE(row.value(QStringLiteral("status")).toString(), QStringLiteral("excluded"));
            }
        }
    }
    QVERIFY(found_gold && found_inverse);
    r.include_leveraged = true;
    const auto explicit_include = run_group_research(r);
    QVERIFY(explicit_include.is_ok());
    for (const auto& group : explicit_include.value().value(QStringLiteral("groups")).toArray())
        for (const auto& item : group.toObject().value(QStringLiteral("rotation_constituents")).toArray())
            if (item.toObject().value(QStringLiteral("stable_key")).toString() == QLatin1String("738523410"))
                QCOMPARE(item.toObject().value(QStringLiteral("status")).toString(), QStringLiteral("missing"));
}

QTEST_GUILESS_MAIN(EtfGroupStoreTest)
#include "tst_etf_group_store.moc"
