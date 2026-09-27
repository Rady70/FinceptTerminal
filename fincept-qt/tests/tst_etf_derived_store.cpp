// tst_etf_derived_store.cpp — ETF Capital Flows Batch C: derived values over a
// real SQLite store (services/etf/EtfDerivedAnalytics).
//
// The vintages are written through the Batch B repository exactly as the
// ingestors write them, so their timing (history type, point-in-time status,
// available_from) comes from the Batch B rules, not from the test. What this
// suite proves needs a real database:
//   * a derived run reads and never writes: every ETF table is identical
//     before and after, and two runs of one request give the same bytes;
//   * a result computed for (as_of, known_at) is reproduced exactly after the
//     store has grown by a bar revision and an SEC amendment, while a later
//     frame sees both, as REVISED;
//   * a ticker change does not split an instrument's series;
//   * the flow routes of a listed ETF, the dormant calculated flow and the
//     separation of the two families are reported as stored.
// Every database is a fresh file in a temporary directory.

#include "services/etf/EtfDerivedAnalytics.h"
#include "services/etf/EtfRegulatoryFlowAnalytics.h"
#include "services/etf/EtfRotationMeasures.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <functional>

using namespace fincept;
using namespace fincept::services::etf;

namespace {

QDateTime utc(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toUTC();
}

EtfDataRepository& repo() {
    return EtfDataRepository::instance();
}

qint64 retrieval(SourceType source, AcquisitionMode mode, const char* at) {
    etf_store::RetrievalRecord r;
    r.run_id = QStringLiteral("test");
    r.source_type = source;
    r.acquisition_mode = mode;
    r.endpoint = QStringLiteral("test");
    r.request_ref = QStringLiteral("test");
    r.requested_at = utc(at);
    r.retrieved_at = utc(at);
    r.status = RetrievalStatus::Ok;
    r.response_sha256 = QStringLiteral("ab");
    r.interpretation = QStringLiteral("test");
    r.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    auto id = repo().record_retrieval(r);
    return id.is_ok() ? id.value() : -1;
}

QVector<QDate> sessions_between(const QDate& a, const QDate& b) {
    QVector<QDate> out;
    for (const MarketSessionDay& d : UsEquityCalendar::weekdays_in(a, b)) {
        if (d.is_session())
            out.append(d.date);
    }
    return out;
}

qint64 add_instrument(qint64 con_id, const char* symbol, const char* seen) {
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

/// One IBKR retrieval of daily bars for `sessions`, recorded as the Batch B
/// ingestor records it (one vintage per bar field).
bool add_bars(qint64 instrument, const QVector<QDate>& sessions, const std::function<double(int)>& close,
              const char* seen) {
    const qint64 r = retrieval(SourceType::IbkrTwsReadonly, AcquisitionMode::IbkrReadonlyWrapper, seen);
    auto start =
        repo().observation_start(SourceType::IbkrTwsReadonly, SubjectType::ListedInstrument, instrument, utc(seen), r);
    if (r <= 0 || start.is_err())
        return false;
    for (int i = 0; i < sessions.size(); ++i) {
        for (const char* measure : {"bar_close", "bar_volume"}) {
            etf_store::ObservationInput in;
            in.subject_type = SubjectType::ListedInstrument;
            in.subject_id = instrument;
            in.kind = MeasurementKind::MarketBar;
            in.measure = QLatin1String(measure);
            const bool volume = QLatin1String(measure) == QLatin1String("bar_volume");
            in.units = volume ? QStringLiteral("shares_ibkr_filtered") : QStringLiteral("USD_per_share");
            in.basis = QStringLiteral("ibkr_trades_rth_daily_split_adjusted");
            in.source_type = SourceType::IbkrTwsReadonly;
            in.acquisition_mode = AcquisitionMode::IbkrReadonlyWrapper;
            in.effective_date = sessions[i];
            in.value = FieldValue::reported_value(volume ? 1000.0 : close(i));
            in.retrieval_id = r;
            in.seen_at = utc(seen);
            in.observation_start = start.value();
            if (repo().record_observation(in).is_err())
                return false;
        }
    }
    return true;
}

struct FilingSpec {
    const char* accession;
    const char* form;
    const char* accepted;
    const char* seen;
    QDate report_date;
    double net_assets;
    double flows[3][3]; ///< per month: sales, redemption, reinvestment
    const char* amends = "";
};

/// One N-PORT filing of `entity`, recorded as the SEC ingestor records it.
bool add_filing(qint64 entity, const FilingSpec& f, const QDateTime& observation_start) {
    const qint64 r = retrieval(SourceType::SecNport, AcquisitionMode::RegulatoryApi, f.seen);
    etf_store::SecFilingFacts facts;
    facts.accession = QString::fromLatin1(f.accession);
    facts.filer_cik = QStringLiteral("0000884394");
    facts.form = QString::fromLatin1(f.form);
    facts.filing_date = utc(f.accepted).date();
    facts.accepted_at = utc(f.accepted);
    facts.entity_id = entity;
    facts.amends_accession = QString::fromLatin1(f.amends);
    facts.rep_pd_date = f.report_date;
    facts.document_sha256 = QStringLiteral("doc-") + facts.accession;
    auto filing = repo().upsert_sec_filing(facts, r, utc(f.seen));
    if (r <= 0 || filing.is_err())
        return false;
    auto record = [&](MeasurementKind kind, const char* measure, const char* basis, const QDate& effective,
                      const QDate& ps, const QDate& pe, double value) {
        etf_store::ObservationInput in;
        in.subject_type = SubjectType::ReportingEntity;
        in.subject_id = entity;
        in.kind = kind;
        in.measure = QLatin1String(measure);
        in.units = QStringLiteral("USD");
        in.basis = QLatin1String(basis);
        in.source_type = SourceType::SecNport;
        in.acquisition_mode = AcquisitionMode::RegulatoryApi;
        in.source_document = facts.accession;
        in.filing_id = filing.value().first;
        in.amended_filing = facts.form == QLatin1String("NPORT-P/A");
        in.effective_date = effective;
        in.period_start = ps;
        in.period_end = pe;
        in.report_period = f.report_date;
        in.accepted_at = facts.accepted_at;
        in.value = FieldValue::reported_value(value);
        in.retrieval_id = r;
        in.seen_at = utc(f.seen);
        in.observation_start = observation_start;
        return repo().record_observation(in).is_ok();
    };
    bool ok = record(MeasurementKind::AumObservation, "nport_net_assets", "regulatory_quarter_end_net_assets",
                     f.report_date, QDate(), QDate(), f.net_assets);
    for (int m = 0; m < 3; ++m) {
        const QDate first = QDate(f.report_date.year(), f.report_date.month(), 1).addMonths(m - 2);
        const QDate last = first.addMonths(1).addDays(-1);
        const char* measures[3] = {"nport_sales", "nport_redemption", "nport_reinvestment"};
        for (int c = 0; c < 3; ++c)
            ok = ok && record(MeasurementKind::RegulatoryReportedFlow, measures[c], "nport_monthly_flow", last, first,
                              last, f.flows[m][c]);
    }
    return ok;
}

QByteArray run_bytes(const DerivedRunRequest& req) {
    auto r = run_derived_calculations(req);
    return r.is_ok() ? QJsonDocument(r.value()).toJson(QJsonDocument::Compact) : QByteArray();
}

DerivedRunRequest request(const char* as_of, const char* known_at = nullptr) {
    DerivedRunRequest req;
    req.frame.as_of = utc(as_of);
    req.frame.known_at = utc(known_at ? known_at : as_of);
    return req;
}

QJsonObject run_json(const DerivedRunRequest& req) {
    auto r = run_derived_calculations(req);
    return r.is_ok() ? r.value() : QJsonObject();
}

QJsonObject last_session_values(const QJsonObject& doc, int instrument_index) {
    const QJsonArray sessions = doc.value(QLatin1String("rotation_proxy"))
                                    .toArray()
                                    .at(instrument_index)
                                    .toObject()
                                    .value(QLatin1String("measures"))
                                    .toObject()
                                    .value(QLatin1String("sessions"))
                                    .toArray();
    return sessions.last().toObject().value(QLatin1String("values")).toObject();
}

QJsonObject month_values(const QJsonObject& doc, const QString& month) {
    const QJsonArray months = doc.value(QLatin1String("regulatory_flow"))
                                  .toArray()
                                  .at(0)
                                  .toObject()
                                  .value(QLatin1String("analytics"))
                                  .toObject()
                                  .value(QLatin1String("months"))
                                  .toArray();
    for (const QJsonValue& m : months) {
        if (m.toObject().value(QLatin1String("month")).toString() == month)
            return m.toObject().value(QLatin1String("values")).toObject();
    }
    return {};
}

const FilingSpec kQ1{"0001410368-26-000101",
                     "NPORT-P",
                     "2026-05-28T15:00:00.000Z",
                     "2026-09-26T01:00:00.000Z",
                     QDate(2026, 3, 31),
                     1100.0,
                     {{100, 30, 5}, {10, 50, 0}, {20, 20, 0}}};
const FilingSpec kQ2{"0001410368-26-000102", "NPORT-P", "2026-08-25T15:00:00.000Z",          "2026-09-26T01:00:00.000Z",
                     QDate(2026, 6, 30),     1200.0,    {{60, 5, 0}, {40, 0, 0}, {0, 44, 0}}};

} // namespace

class TstEtfDerivedStore : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir dir_;
    int db_counter_ = 0;
    qint64 entity_ = 0;
    qint64 spy_ = 0;
    qint64 tlt_ = 0;
    QVector<QDate> sessions_;

    bool open_fresh() {
        const QString path = dir_.filePath(QStringLiteral("derived_%1.db").arg(++db_counter_));
        return Database::instance().open(path).is_ok();
    }

    /// A store with one SEC entity (two quarters) and two listed ETFs.
    void populate() {
        QVERIFY(open_fresh());
        etf_store::ReportingEntityFacts facts;
        facts.cik = QStringLiteral("0000884394");
        facts.registrant_name = QStringLiteral("Test Trust");
        facts.source_accepted_at = utc(kQ2.accepted);
        auto entity = repo().upsert_reporting_entity(facts, utc("2026-09-26T01:00:00.000Z"));
        QVERIFY(entity.is_ok());
        entity_ = entity.value();
        const qint64 r0 = retrieval(SourceType::SecNport, AcquisitionMode::RegulatoryApi, "2026-09-26T01:00:00.000Z");
        auto start = repo().observation_start(SourceType::SecNport, SubjectType::ReportingEntity, entity_,
                                              utc("2026-09-26T01:00:00.000Z"), r0);
        QVERIFY(start.is_ok());
        const FilingSpec q4{"0001410368-26-000100",           "NPORT-P",           "2026-02-25T15:00:00.000Z",
                            "2026-09-26T01:00:00.000Z",       QDate(2025, 12, 31), 1000.0,
                            {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}}};
        QVERIFY(add_filing(entity_, q4, start.value()));
        QVERIFY(add_filing(entity_, kQ1, start.value()));
        QVERIFY(add_filing(entity_, kQ2, start.value()));

        sessions_ = sessions_between(QDate(2025, 6, 2), QDate(2025, 12, 31));
        spy_ = add_instrument(756733, "SPY", "2026-09-26T02:00:00.000Z");
        tlt_ = add_instrument(15547841, "TLT", "2026-09-26T02:00:00.000Z");
        QVERIFY(spy_ > 0 && tlt_ > 0);
        QVERIFY(add_bars(spy_, sessions_, [](int i) { return 100.0 * std::pow(1.01, i); }, "2026-09-26T02:00:00.000Z"));
        QVERIFY(add_bars(tlt_, sessions_, [](int i) { return 90.0 * std::pow(1.005, i); }, "2026-09-26T02:01:00.000Z"));
    }

  private slots:
    void initTestCase();
    void cleanupTestCase();
    void a_run_reads_only_and_replays_byte_for_byte();
    void an_earlier_result_is_reproduced_after_the_store_grows();
    void a_ticker_change_keeps_one_series();
    void flow_routes_and_the_dormant_calculated_flow();
    void the_two_families_stay_apart();
    void a_declared_reference_must_be_stored();
};

void TstEtfDerivedStore::initTestCase() {
    QVERIFY(dir_.isValid());
    register_migration_v052();
    QCOMPARE(MigrationRunner::highest_registered_version(), 52);
}

void TstEtfDerivedStore::cleanupTestCase() {
    Database::instance().close();
}

void TstEtfDerivedStore::a_run_reads_only_and_replays_byte_for_byte() {
    populate();
    auto before = repo().export_all();
    QVERIFY(before.is_ok());
    const DerivedRunRequest req = request("2026-09-27T00:00:00.000Z");
    const QByteArray a = run_bytes(req);
    const QByteArray b = run_bytes(req);
    QVERIFY(!a.isEmpty());
    QCOMPARE(a, b);
    auto after = repo().export_all();
    QVERIFY(after.is_ok());
    // No table changed: not a row, not a sighting.
    QCOMPARE(QJsonDocument(after.value()).toJson(QJsonDocument::Compact),
             QJsonDocument(before.value()).toJson(QJsonDocument::Compact));
    // The frame is recorded in the output, so the run can be repeated.
    const QJsonObject doc = QJsonDocument::fromJson(a).object();
    QCOMPARE(doc.value(QLatin1String("frame")).toObject().value(QLatin1String("as_of")).toString(),
             QStringLiteral("2026-09-27T00:00:00.000Z"));
    QCOMPARE(doc.value(QLatin1String("frame")).toObject().value(QLatin1String("known_at")).toString(),
             QStringLiteral("2026-09-27T00:00:00.000Z"));
    // Values of the stored history, as the repository dated them.
    const QJsonObject apr = month_values(doc, QStringLiteral("2026-04"));
    QCOMPARE(apr.value(QLatin1String("net_flow")).toObject().value(QLatin1String("value")).toDouble(), 55.0);
    QCOMPARE(apr.value(QLatin1String("flow_pct_prior_net_assets")).toObject().value(QLatin1String("value")).toDouble(),
             55.0 / 1100.0);
    QCOMPARE(apr.value(QLatin1String("net_flow")).toObject().value(QLatin1String("point_in_time_status")).toString(),
             QStringLiteral("conservative_rule"));
    const QJsonObject spy = last_session_values(doc, 0);
    const QJsonObject r5 = spy.value(QLatin1String("price_return_5")).toObject();
    QVERIFY(std::fabs(r5.value(QLatin1String("value")).toDouble() - (std::pow(1.01, 5) - 1.0)) < 1e-12);
    QCOMPARE(r5.value(QLatin1String("point_in_time_status")).toString(), QStringLiteral("historical_assumption"));
    QCOMPARE(r5.value(QLatin1String("state")).toString(), QStringLiteral("PROXY"));
}

void TstEtfDerivedStore::an_earlier_result_is_reproduced_after_the_store_grows() {
    populate();
    const DerivedRunRequest first = request("2026-09-27T00:00:00.000Z");
    const QByteArray original = run_bytes(first);
    QVERIFY(!original.isEmpty());

    // The store grows: IBKR revises SPY's last close, and an amendment of Q2
    // with a different April is accepted and recorded.
    QVERIFY(add_bars(spy_, {sessions_.last()}, [](int) { return 1.0; }, "2026-09-28T12:00:00.000Z"));
    const FilingSpec amendment{
        "0001410368-26-000103", "NPORT-P/A", "2026-09-28T15:00:00.000Z",           "2026-09-28T16:00:00.000Z",
        QDate(2026, 6, 30),     1200.0,      {{70, 5, 0}, {40, 0, 0}, {0, 44, 0}}, "0001410368-26-000102"};
    auto start = repo().observation_start(SourceType::SecNport, SubjectType::ReportingEntity, entity_, QDateTime(), 0);
    QVERIFY(start.is_ok());
    QVERIFY(add_filing(entity_, amendment, start.value()));

    // The first request, run again, is the first result byte for byte.
    QCOMPARE(run_bytes(first), original);

    // A frame after both sees both, as revisions.
    const QJsonObject later = run_json(request("2026-09-29T00:00:00.000Z"));
    const QJsonObject apr = month_values(later, QStringLiteral("2026-04"));
    const QJsonObject net = apr.value(QLatin1String("net_flow")).toObject();
    QCOMPARE(net.value(QLatin1String("value")).toDouble(), 65.0);
    QCOMPARE(net.value(QLatin1String("state")).toString(), QStringLiteral("REVISED"));
    // Accepted after MarketLab's SEC observation start: observed, from its
    // first sighting.
    QCOMPARE(net.value(QLatin1String("point_in_time_status")).toString(), QStringLiteral("observed"));
    QCOMPARE(net.value(QLatin1String("available_from")).toString(), QStringLiteral("2026-09-28T16:00:00.000Z"));
    const QJsonObject spy = last_session_values(later, 0);
    QCOMPARE(spy.value(QLatin1String("price_return_5")).toObject().value(QLatin1String("state")).toString(),
             QStringLiteral("REVISED"));
    // A frame between the revision's close and its sighting still has the
    // original: a backfill bar's revision is usable only once it was seen.
    const QJsonObject between = run_json(request("2026-09-28T11:00:00.000Z"));
    QCOMPARE(last_session_values(between, 0)
                 .value(QLatin1String("price_return_5"))
                 .toObject()
                 .value(QLatin1String("state"))
                 .toString(),
             QStringLiteral("PROXY"));
}

void TstEtfDerivedStore::a_ticker_change_keeps_one_series() {
    QVERIFY(open_fresh());
    const QVector<QDate> sessions = sessions_between(QDate(2025, 9, 2), QDate(2025, 12, 31));
    const QVector<QDate> early = sessions.mid(0, 40);
    const QVector<QDate> late = sessions.mid(40);
    const qint64 id = add_instrument(424242, "OLDT", "2026-09-26T02:00:00.000Z");
    QVERIFY(add_bars(id, early, [](int i) { return 100.0 * std::pow(1.01, i); }, "2026-09-26T02:00:00.000Z"));
    // The same conId under a new ticker: the same instrument.
    QCOMPARE(add_instrument(424242, "NEWT", "2026-09-27T02:00:00.000Z"), id);
    QVERIFY(add_bars(id, late, [](int i) { return 100.0 * std::pow(1.01, 40 + i); }, "2026-09-27T02:00:00.000Z"));
    auto by_old = repo().find_listed_instruments_by_symbol(QStringLiteral("OLDT"));
    auto by_new = repo().find_listed_instruments_by_symbol(QStringLiteral("NEWT"));
    QVERIFY(by_old.is_ok() && by_new.is_ok());
    QCOMPARE(by_old.value(), QVector<qint64>{id});
    QCOMPARE(by_new.value(), QVector<qint64>{id});

    const QJsonObject doc = run_json(request("2026-09-28T00:00:00.000Z"));
    const QJsonArray rotation = doc.value(QLatin1String("rotation_proxy")).toArray();
    QCOMPARE(rotation.size(), 1);
    const QJsonObject instrument = rotation.at(0).toObject().value(QLatin1String("instrument")).toObject();
    QCOMPARE(instrument.value(QLatin1String("symbol")).toString(), QStringLiteral("NEWT"));
    QCOMPARE(instrument.value(QLatin1String("symbols_seen")).toArray(), (QJsonArray{"OLDT", "NEWT"}));
    // A return across the ticker change is one series: 1.01^21 - 1.
    const QJsonArray listed = rotation.at(0)
                                  .toObject()
                                  .value(QLatin1String("measures"))
                                  .toObject()
                                  .value(QLatin1String("sessions"))
                                  .toArray();
    QCOMPARE(listed.size(), sessions.size());
    const QJsonObject r21 = listed.at(50)
                                .toObject()
                                .value(QLatin1String("values"))
                                .toObject()
                                .value(QLatin1String("price_return_21"))
                                .toObject();
    QVERIFY(std::fabs(r21.value(QLatin1String("value")).toDouble() - (std::pow(1.01, 21) - 1.0)) < 1e-12);
}

void TstEtfDerivedStore::flow_routes_and_the_dormant_calculated_flow() {
    populate();
    const QJsonObject doc = run_json(request("2026-09-27T00:00:00.000Z"));
    const QJsonObject methods = doc.value(QLatin1String("methods")).toObject();
    const QJsonObject calculated = methods.value(QLatin1String("calculated_creation_redemption_flow")).toObject();
    QCOMPARE(calculated.value(QLatin1String("state")).toString(), QStringLiteral("ROUTE_DISABLED"));
    QCOMPARE(calculated.value(QLatin1String("reason")).toString(), QStringLiteral("d1d_no_permission_basis"));
    QCOMPARE(calculated.value(QLatin1String("implemented")).toBool(true), false);
    const QJsonObject rotation = methods.value(QLatin1String("rotation_proxy_measures")).toObject();
    QCOMPARE(rotation.value(QLatin1String("total_return")).toBool(true), false);
    QCOMPARE(rotation.value(QLatin1String("cross_asset_return_comparability")).toString(),
             QStringLiteral("not_established_d5"));

    // No identity link is stored: the listed ETF has no regulatory route yet.
    const QJsonArray routes = doc.value(QLatin1String("rotation_proxy"))
                                  .toArray()
                                  .at(0)
                                  .toObject()
                                  .value(QLatin1String("flow_routes"))
                                  .toArray();
    QCOMPARE(routes.size(), 3);
    QCOMPARE(routes.at(0).toObject().value(QLatin1String("availability")).toString(),
             QStringLiteral("identity_not_established"));
    QCOMPARE(routes.at(0).toObject().value(QLatin1String("state")).toString(), QStringLiteral("MISSING"));
    QCOMPARE(routes.at(1).toObject().value(QLatin1String("availability")).toString(), QStringLiteral("not_qualified"));
    QCOMPARE(routes.at(2).toObject().value(QLatin1String("route")).toString(), QStringLiteral("calculated_daily"));
    QCOMPARE(routes.at(2).toObject().value(QLatin1String("state")).toString(), QStringLiteral("ROUTE_DISABLED"));

    // A declared link makes the monthly regulatory route available.
    QVERIFY(repo()
                .declare_link(spy_, entity_, QString(), LinkRelationship::RegistrantIsInstrument,
                              QStringLiteral("test declaration"), utc("2026-09-27T00:00:00.000Z"))
                .is_ok());
    const QJsonObject linked = run_json(request("2026-09-27T00:00:00.000Z"));
    const QJsonObject monthly = linked.value(QLatin1String("rotation_proxy"))
                                    .toArray()
                                    .at(0)
                                    .toObject()
                                    .value(QLatin1String("flow_routes"))
                                    .toArray()
                                    .at(0)
                                    .toObject();
    QCOMPARE(monthly.value(QLatin1String("availability")).toString(), QStringLiteral("available"));
    QVERIFY(monthly.value(QLatin1String("state")).isNull());
}

void TstEtfDerivedStore::the_two_families_stay_apart() {
    populate();
    DerivedRunRequest regulatory_only = request("2026-09-27T00:00:00.000Z");
    regulatory_only.rotation = false;
    regulatory_only.entity_id = entity_;
    const QJsonObject reg = run_json(regulatory_only);
    QCOMPARE(reg.value(QLatin1String("regulatory_flow")).toArray().size(), 1);
    QCOMPARE(reg.value(QLatin1String("rotation_proxy")).toArray().size(), 0);
    DerivedRunRequest rotation_only = request("2026-09-27T00:00:00.000Z");
    rotation_only.regulatory = false;
    rotation_only.instrument_id = tlt_;
    const QJsonObject rot = run_json(rotation_only);
    QCOMPARE(rot.value(QLatin1String("regulatory_flow")).toArray().size(), 0);
    QCOMPARE(rot.value(QLatin1String("rotation_proxy")).toArray().size(), 1);
    QCOMPARE(rot.value(QLatin1String("rotation_proxy"))
                 .toArray()
                 .at(0)
                 .toObject()
                 .value(QLatin1String("instrument"))
                 .toObject()
                 .value(QLatin1String("symbol"))
                 .toString(),
             QStringLiteral("TLT"));
    // Every value in a full run belongs to its family's measurement kind; no
    // rotation value is in dollars.
    const QJsonObject all = run_json(request("2026-09-27T00:00:00.000Z"));
    for (const QJsonValue& e : all.value(QLatin1String("regulatory_flow")).toArray()) {
        for (const QJsonValue& m :
             e.toObject().value(QLatin1String("analytics")).toObject().value(QLatin1String("months")).toArray()) {
            const QJsonObject values = m.toObject().value(QLatin1String("values")).toObject();
            for (auto it = values.begin(); it != values.end(); ++it)
                QCOMPARE(it.value().toObject().value(QLatin1String("measurement_kind")).toString(),
                         QStringLiteral("regulatory_reported_flow"));
        }
    }
    for (const QJsonValue& i : all.value(QLatin1String("rotation_proxy")).toArray()) {
        for (const QJsonValue& s :
             i.toObject().value(QLatin1String("measures")).toObject().value(QLatin1String("sessions")).toArray()) {
            const QJsonObject values = s.toObject().value(QLatin1String("values")).toObject();
            for (auto it = values.begin(); it != values.end(); ++it) {
                QCOMPARE(it.value().toObject().value(QLatin1String("measurement_kind")).toString(),
                         QStringLiteral("rotation_proxy"));
                QVERIFY(!it.value().toObject().value(QLatin1String("units")).toString().contains(QLatin1String("USD")));
            }
        }
    }
}

void TstEtfDerivedStore::a_declared_reference_must_be_stored() {
    populate();
    DerivedRunRequest req = request("2026-09-27T00:00:00.000Z");
    req.reference_instrument_id = 999999;
    QVERIFY(run_derived_calculations(req).is_err());
    req.reference_instrument_id = tlt_;
    const QJsonObject doc = run_json(req);
    QCOMPARE(doc.value(QLatin1String("rotation_reference")).toObject().value(QLatin1String("declared_by")).toString(),
             QStringLiteral("caller"));
    // SPY against TLT: (1.01 / 1.005)^21 - 1; TLT against itself: not applicable.
    const QJsonObject spy = last_session_values(doc, 0);
    QVERIFY(
        std::fabs(
            spy.value(QLatin1String("relative_price_return_21")).toObject().value(QLatin1String("value")).toDouble() -
            (std::pow(1.01 / 1.005, 21) - 1.0)) < 1e-12);
    const QJsonObject tlt = last_session_values(doc, 1);
    QCOMPARE(tlt.value(QLatin1String("relative_price_return_21")).toObject().value(QLatin1String("reason")).toString(),
             QStringLiteral("subject_is_reference"));
}

QTEST_GUILESS_MAIN(TstEtfDerivedStore)
#include "tst_etf_derived_store.moc"
