// tst_etf_ingest.cpp — ETF Capital Flows Batch B: the SEC N-PORT and IBKR
// daily-bar ingestion runs, end to end into a real SQLite database.
//
// The transports are injected (services/etf/EtfSecNportIngestor.h,
// EtfIbkrDailyIngestor.h): a fake SEC server answering from the trimmed real
// filings of etf_test_fixtures.h, and a fake IBKR wrapper answering with
// synthetic envelopes. Both answer asynchronously, like the real ones, and a
// fixed clock makes every recorded time deterministic. No network, no TWS.
//
// Covered: registrant (UIT) and series targets, older submissions pages,
// amendments, replay, forward observation after the observation start, the
// declared User-Agent, and every degraded case of the Batch B runbook: SEC
// unavailable, throttled or malformed; missing N-PORT fields; unknown or
// mismatched identity; TWS unavailable; entitlement failure; in-progress
// session; stale history; missing expected session; historical revision.

#include "etf_test_fixtures.h"
#include "services/etf/EtfIbkrDailyIngestor.h"
#include "services/etf/EtfReadModel.h"
#include "services/etf/EtfSecNportIngestor.h"
#include "services/etf/EtfTiming.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QEventLoop>
#include <QHash>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

using namespace fincept;
using namespace fincept::services::etf;
using namespace etf_fixtures;

namespace {

const QString kUa = QStringLiteral("MarketLab test admin@example.com");

QDateTime utc(const char* iso) {
    return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODateWithMs).toUTC();
}

/// A clock that starts at `start` and advances one second per reading.
struct FakeClock {
    QDateTime now;
    QDateTime operator()() {
        const QDateTime t = now;
        now = now.addSecs(1);
        return t;
    }
};

/// A fake SEC server: URL -> response, answered on the event loop.
struct FakeSec {
    QHash<QString, SecHttpResponse> routes;
    QStringList requested;
    QList<QByteArray> user_agents;

    void ok(const QString& url, const QByteArray& body) { routes.insert(url, SecHttpResponse{true, 200, body, {}}); }
    void status(const QString& url, int http) { routes.insert(url, SecHttpResponse{true, http, "error", {}}); }
    void fail(const QString& url) { routes.insert(url, SecHttpResponse{false, 0, {}, QStringLiteral("timed out")}); }

    SecHttpGet transport() {
        return [this](const QString& url, const QByteArray& ua, std::function<void(const SecHttpResponse&)> cb) {
            requested.append(url);
            user_agents.append(ua);
            const SecHttpResponse r = routes.value(url, SecHttpResponse{true, 404, "not found", {}});
            QTimer::singleShot(0, [cb, r]() { cb(r); });
        };
    }
};

const QString kSpySubs = QStringLiteral("https://data.sec.gov/submissions/CIK0000884394.json");
const QString kSpyDocJune =
    QStringLiteral("https://www.sec.gov/Archives/edgar/data/884394/000141036826089410/primary_doc.xml");
const QString kSpyDocMarch =
    QStringLiteral("https://www.sec.gov/Archives/edgar/data/884394/000141036826055357/primary_doc.xml");
const QString kIvvIndex = sec_series_index_url(QStringLiteral("S000004310"));
const QString kIvvSubs = QStringLiteral("https://data.sec.gov/submissions/CIK0001100663.json");
const QString kIvvPage = QStringLiteral("https://data.sec.gov/submissions/CIK0001100663-submissions-001.json");

NportSpec spy_2026_03() {
    // Synthetic values in the SPY shape (the March 2026 document is not a
    // retained capture); identity and dates follow the real listing.
    NportSpec s = spy_2026_06();
    s.rep_pd_date = QStringLiteral("2026-03-31");
    s.net_assets = QStringLiteral("700000000000.00");
    return s;
}

void serve_spy(FakeSec& sec, const NportSpec& june = spy_2026_06()) {
    sec.ok(kSpySubs,
           submissions_json(
               QStringLiteral("0000884394"), QStringLiteral("SPDR S&P 500 ETF TRUST"),
               {{"0001410368-26-089410", "NPORT-P", "2026-08-28", "2026-06-30", "2026-08-28T12:25:47.000Z"},
                {"0000000000-26-000009", "N-CSR", "2026-06-01", "", "2026-06-01T20:00:00.000Z"},
                {"0001410368-26-055357", "NPORT-P", "2026-05-28", "2026-03-31", "2026-05-28T19:11:03.000Z"}}));
    sec.ok(kSpyDocJune, nport_xml(june));
    sec.ok(kSpyDocMarch, nport_xml(spy_2026_03()));
}

void serve_ivv(FakeSec& sec) {
    sec.ok(kIvvIndex, series_index_atom(QStringLiteral("0001100663"), QStringLiteral("iSHARES TRUST"),
                                        {{"0002071691-26-019760", "NPORT-P", "2026-08-25"},
                                         {"0002071691-26-015790", "NPORT-P/A", "2026-07-13"},
                                         {"0002071691-25-007634", "NPORT-P", "2025-11-26"}}));
    // The recent window holds the two newer filings; the original is on the
    // older page, which must be read for its acceptance time.
    sec.ok(kIvvSubs,
           submissions_json(
               QStringLiteral("0001100663"), QStringLiteral("iSHARES TRUST"),
               {{"0002071691-26-019760", "NPORT-P", "2026-08-25", "2026-06-30", "2026-08-25T14:10:51.000Z"},
                {"0002071691-26-015790", "NPORT-P/A", "2026-07-13", "2025-09-30", "2026-07-13T14:48:14.000Z"}},
               {{"CIK0001100663-submissions-001.json", "2024-12-26", "2025-11-30"}}));
    sec.ok(kIvvPage, submissions_page_json({{"0002071691-25-007634", "NPORT-P", "2025-11-26", "2025-09-30",
                                             "2025-11-26T17:01:17.000Z"}}));
    NportSpec june = ivv_2025_09(false);
    june.rep_pd_date = QStringLiteral("2026-06-30"); // synthetic values for the newest quarter
    sec.ok(sec_nport_primary_doc_url(QStringLiteral("0001100663"), QStringLiteral("0002071691-26-019760")),
           nport_xml(june));
    sec.ok(sec_nport_primary_doc_url(QStringLiteral("0001100663"), QStringLiteral("0002071691-26-015790")),
           nport_xml(ivv_2025_09(true)));
    sec.ok(sec_nport_primary_doc_url(QStringLiteral("0001100663"), QStringLiteral("0002071691-25-007634")),
           nport_xml(ivv_2025_09(false)));
}

SecNportRunSummary run_sec(FakeSec& sec, const SecNportRequest& req, const char* clock_start, const QString& ua = kUa) {
    auto clock = std::make_shared<FakeClock>(FakeClock{utc(clock_start)});
    EtfSecNportIngestor ingestor(sec.transport(), ua, 0, [clock]() { return (*clock)(); });
    SecNportRunSummary out;
    bool done = false;
    QEventLoop loop;
    ingestor.run(req, [&](const SecNportRunSummary& s) {
        out = s;
        done = true;
        loop.quit();
    });
    if (!done) {
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!done)
        qWarning("SEC run did not finish");
    return out;
}

SecNportRequest request(const char* cik, const char* series = "", int max_filings = 4) {
    SecNportRequest r;
    r.cik = QString::fromLatin1(cik);
    r.series_id = QString::fromLatin1(series);
    r.max_filings = max_filings;
    return r;
}

/// A fake IBKR wrapper that answers with `payload` and records the request.
struct FakeIbkr {
    QJsonObject payload;
    QVector<IbkrDailyRequest> requests;
    IbkrDailyFetch fetch() {
        return [this](const IbkrDailyRequest& r, std::function<void(const QJsonObject&)> cb) {
            requests.append(r);
            const QJsonObject p = payload;
            QTimer::singleShot(0, [cb, p]() { cb(p); });
        };
    }
};

IbkrDailyRunSummary run_ibkr(FakeIbkr& ibkr, const char* clock_start, bool configured = true,
                             const QString& symbol = QStringLiteral("SPY"), const QString& duration = "2 Y") {
    auto clock = std::make_shared<FakeClock>(FakeClock{utc(clock_start)});
    EtfIbkrDailyIngestor ingestor(ibkr.fetch(), configured, [clock]() { return (*clock)(); });
    IbkrDailyRunSummary out;
    bool done = false;
    QEventLoop loop;
    ingestor.run(symbol, duration, [&](const IbkrDailyRunSummary& s) {
        out = s;
        done = true;
        loop.quit();
    });
    if (!done) {
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return out;
}

int count(const char* table, const QString& where = QString()) {
    auto r = Database::instance().execute(
        QStringLiteral("SELECT COUNT(*) FROM %1%2")
            .arg(QLatin1String(table), where.isEmpty() ? QString() : QStringLiteral(" WHERE ") + where));
    return r.is_ok() && r.value().next() ? r.value().value(0).toInt() : -1;
}

QString scalar(const QString& sql) {
    auto r = Database::instance().execute(sql);
    return r.is_ok() && r.value().next() ? r.value().value(0).toString() : QStringLiteral("<none>");
}

} // namespace

class TstEtfIngest : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir dir_;
    int counter_ = 0;

  private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void sec_registrant_target_ingests_real_filings();
    void sec_series_target_reads_an_older_page_and_keeps_the_amendment();
    void sec_replay_confirms_without_duplicates();
    void sec_forward_observation_after_the_observation_start();
    void sec_unavailable_records_a_source_error();
    void sec_throttle_stops_the_run();
    void sec_transport_failure_and_malformed_submissions();
    void sec_malformed_document_is_skipped_not_guessed();
    void sec_missing_nport_fields_stay_missing();
    void sec_unknown_or_mismatched_identity_is_refused();
    void sec_report_date_not_month_end_keeps_flows_unmapped();
    void sec_requires_a_declared_user_agent();
    void sec_invalid_request_touches_nothing();

    void ibkr_backfill_is_stored_with_sessions_and_identity();
    void ibkr_replay_confirms_and_revision_is_kept();
    void ibkr_forward_observation_uses_the_next_session();
    void ibkr_in_progress_session_is_rejected_and_recorded();
    void ibkr_missing_session_and_stale_history_are_recorded();
    void ibkr_entitlement_and_tws_failures_store_no_data();
    void ibkr_not_configured_or_invalid_request_makes_no_request();
};

void TstEtfIngest::initTestCase() {
    QVERIFY(dir_.isValid());
    register_migration_v052();
}

void TstEtfIngest::init() {
    QVERIFY(Database::instance().open(dir_.filePath(QStringLiteral("ingest_%1.db").arg(++counter_))).is_ok());
}

void TstEtfIngest::cleanupTestCase() {
    Database::instance().close();
}

// ── SEC ──────────────────────────────────────────────────────────────────────

void TstEtfIngest::sec_registrant_target_ingests_real_filings() {
    FakeSec sec;
    serve_spy(sec);
    const SecNportRunSummary s = run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(s.filings_stored, 2);
    QCOMPARE(s.observations_inserted, 20); // per filing: net assets + 3 months x 3 flow elements
    QCOMPARE(sec.requested, (QStringList{kSpySubs, kSpyDocMarch, kSpyDocJune})); // acceptance order, no index
    for (const QByteArray& ua : sec.user_agents)
        QCOMPARE(ua, kUa.toUtf8());
    QCOMPARE(count("etf_reporting_entities"), 1);
    QCOMPARE(scalar("SELECT reporting_level || '|' || series_id || '|' || cik FROM etf_reporting_entities"),
             QStringLiteral("registrant||0000884394"));
    QCOMPARE(count("etf_sec_filings"), 2);
    QCOMPARE(scalar("SELECT accepted_at FROM etf_sec_filings WHERE accession = '0001410368-26-089410'"),
             QStringLiteral("2026-08-28T12:25:47.000Z"));
    // Real values, exact, with their timing class.
    QCOMPARE(scalar("SELECT raw_text FROM etf_observations WHERE measure = 'nport_sales' AND effective_date = "
                    "'2026-04-30'"),
             QStringLiteral("121393713302.60000000"));
    QCOMPARE(scalar("SELECT period_start || '|' || period_end || '|' || report_period FROM etf_observations WHERE "
                    "measure = 'nport_sales' AND effective_date = '2026-04-30'"),
             QStringLiteral("2026-04-01|2026-04-30|2026-06-30"));
    QCOMPARE(scalar("SELECT history_type || '|' || point_in_time_status || '|' || available_from FROM "
                    "etf_observations WHERE measure = 'nport_net_assets' AND effective_date = '2026-06-30'"),
             QStringLiteral("regulatory_filing|conservative_rule|2026-08-31T13:30:00.000Z"));
    QCOMPARE(scalar("SELECT basis FROM etf_observations WHERE measure = 'nport_net_assets' AND effective_date = "
                    "'2026-06-30'"),
             QStringLiteral("regulatory_quarter_end_net_assets"));
    QCOMPARE(count("etf_retrievals", "status = 'OK'"), 3);
    QCOMPARE(scalar("SELECT runtime_identity FROM etf_retrievals LIMIT 1"),
             QStringLiteral("{\"user_agent\":\"MarketLab test <contact>\"}")); // the contact is never stored
}

void TstEtfIngest::sec_series_target_reads_an_older_page_and_keeps_the_amendment() {
    FakeSec sec;
    serve_ivv(sec);
    const SecNportRunSummary s = run_sec(sec, request("1100663", "S000004310", 3), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(s.filings_stored, 3);
    QVERIFY(sec.requested.contains(kIvvPage));
    QCOMPARE(sec.requested.first(), kIvvIndex);
    QCOMPARE(scalar("SELECT reporting_level || '|' || series_id FROM etf_reporting_entities"),
             QStringLiteral("series|S000004310"));
    QCOMPARE(scalar("SELECT amends_accession FROM etf_sec_filings WHERE form = 'NPORT-P/A'"),
             QStringLiteral("0002071691-25-007634"));
    QCOMPARE(scalar("SELECT class_ids FROM etf_sec_filings WHERE form = 'NPORT-P/A'"), QStringLiteral("C000012040"));
    // Two vintages for the 2025-09-30 net assets: the original and its amendment.
    const qint64 entity = scalar("SELECT entity_id FROM etf_reporting_entities").toLongLong();
    auto v =
        EtfDataRepository::instance().vintages(SubjectType::ReportingEntity, entity, QStringLiteral("nport_net_assets"),
                                               QDate(2025, 9, 30), SourceType::SecNport);
    QVERIFY(v.is_ok());
    QCOMPARE(v.value().size(), 2);
    QCOMPARE(v.value()[0].source_document, QStringLiteral("0002071691-25-007634"));
    QCOMPARE(v.value()[1].revision_state, QStringLiteral("amended_filing"));
    QCOMPARE(derived_quality(v.value(), current_vintage_index(v.value())), QualityState::Confirmed);
    QCOMPARE(s.observations_amended, 10);
}

void TstEtfIngest::sec_replay_confirms_without_duplicates() {
    FakeSec sec;
    serve_spy(sec);
    run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z");
    const int rows = count("etf_observations");
    const SecNportRunSummary again = run_sec(sec, request("884394"), "2026-09-27T10:00:00.000Z");
    QCOMPARE(again.status, RetrievalStatus::Ok);
    QCOMPARE(again.observations_inserted, 0);
    QCOMPARE(again.observations_confirmed, 20);
    QCOMPARE(count("etf_observations"), rows);
    QCOMPARE(count("etf_sec_filings"), 2);
    QCOMPARE(count("etf_retrievals"), 6); // every retrieval is recorded
    QCOMPARE(scalar("SELECT MIN(seen_count) || '|' || MAX(seen_count) FROM etf_observations"), QStringLiteral("2|2"));
    QCOMPARE(scalar("SELECT first_seen_at FROM etf_observations WHERE measure = 'nport_net_assets' AND "
                    "effective_date = '2026-06-30'")
                 .left(10),
             QStringLiteral("2026-09-26"));
}

void TstEtfIngest::sec_forward_observation_after_the_observation_start() {
    FakeSec sec;
    serve_spy(sec);
    run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z"); // observation start
    // A new filing accepted after the observation start, seen by a later run.
    sec.ok(kSpySubs,
           submissions_json(
               QStringLiteral("0000884394"), QStringLiteral("SPDR S&P 500 ETF TRUST"),
               {{"0001410368-26-120000", "NPORT-P", "2026-11-25", "2026-09-30", "2026-11-25T15:00:00.000Z"},
                {"0001410368-26-089410", "NPORT-P", "2026-08-28", "2026-06-30", "2026-08-28T12:25:47.000Z"}}));
    NportSpec sept = spy_2026_06();
    sept.rep_pd_date = QStringLiteral("2026-09-30");
    sec.ok(QStringLiteral("https://www.sec.gov/Archives/edgar/data/884394/000141036826120000/primary_doc.xml"),
           nport_xml(sept));
    const SecNportRunSummary s = run_sec(sec, request("884394", "", 2), "2026-11-30T09:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(scalar("SELECT history_type || '|' || point_in_time_status || '|' || availability_basis FROM "
                    "etf_observations WHERE measure = 'nport_net_assets' AND effective_date = '2026-09-30'"),
             QStringLiteral("forward_observed|observed|recorded_first_seen"));
    const QDateTime available = utc(
        scalar("SELECT available_from FROM etf_observations WHERE measure = 'nport_net_assets' AND effective_date = "
               "'2026-09-30'")
            .toLatin1()
            .constData());
    const QDateTime first_seen =
        utc(scalar("SELECT first_seen_at FROM etf_observations WHERE measure = 'nport_net_assets' AND effective_date = "
                   "'2026-09-30'")
                .toLatin1()
                .constData());
    QCOMPARE(available, first_seen); // never before MarketLab saw it
    QVERIFY(available >= utc("2026-11-30T09:00:00.000Z"));
}

void TstEtfIngest::sec_unavailable_records_a_source_error() {
    FakeSec sec;
    sec.status(kSpySubs, 503);
    const SecNportRunSummary s = run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::SourceError);
    QCOMPARE(s.detail_code, QStringLiteral("http_503"));
    QCOMPARE(sec.requested.size(), 1);
    QCOMPARE(count("etf_retrievals", "status = 'SOURCE_ERROR' AND http_status = 503"), 1);
    QCOMPARE(count("etf_observations"), 0);
    QCOMPARE(count("etf_reporting_entities"), 0);
}

void TstEtfIngest::sec_throttle_stops_the_run() {
    FakeSec sec;
    serve_spy(sec);
    sec.status(kSpyDocMarch, 429); // the first document requested
    const SecNportRunSummary s = run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::SourceError);
    QCOMPARE(s.detail_code, QStringLiteral("http_429"));
    QVERIFY(!sec.requested.contains(kSpyDocJune)); // no further request after the throttle
    QCOMPARE(count("etf_observations"), 0);
}

void TstEtfIngest::sec_transport_failure_and_malformed_submissions() {
    FakeSec sec;
    sec.fail(kSpySubs);
    SecNportRunSummary s = run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.detail_code, QStringLiteral("transport_error"));
    QCOMPARE(count("etf_retrievals", "retrieved_at IS NULL AND http_status IS NULL"), 1);

    sec.ok(kSpySubs, "<html><body>Request Rate Threshold Exceeded</body></html>");
    s = run_sec(sec, request("884394"), "2026-09-26T11:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::SourceError);
    QCOMPARE(s.detail_code, QStringLiteral("submissions_not_json"));
    QCOMPARE(count("etf_observations"), 0);
}

void TstEtfIngest::sec_malformed_document_is_skipped_not_guessed() {
    FakeSec sec;
    serve_spy(sec);
    QByteArray broken = nport_xml(spy_2026_03());
    broken.truncate(broken.size() - 40);
    sec.ok(kSpyDocMarch, broken);
    const SecNportRunSummary s = run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(s.filings_stored, 1);
    QCOMPARE(s.filings_skipped, 1);
    QCOMPARE(count("etf_retrievals", "detail_code = 'nport_xml_malformed'"), 1);
    QCOMPARE(count("etf_observations", "report_period = '2026-03-31'"), 0);
}

void TstEtfIngest::sec_missing_nport_fields_stay_missing() {
    FakeSec sec;
    NportSpec june = spy_2026_06();
    june.months[1].present = false;
    june.months[2].redemption = QStringLiteral("N/A");
    june.net_assets_present = false;
    serve_spy(sec, june);
    const SecNportRunSummary s = run_sec(sec, request("884394", "", 1), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(count("etf_observations", "effective_date = '2026-05-31' AND value_state = 'missing' AND value IS NULL"),
             3);
    QCOMPARE(scalar("SELECT value_state || '|' || raw_text || '|' || (value IS NULL) FROM etf_observations WHERE "
                    "measure = 'nport_redemption' AND effective_date = '2026-06-30'"),
             QStringLiteral("unparseable|N/A|1"));
    QCOMPARE(scalar("SELECT value_state FROM etf_observations WHERE measure = 'nport_net_assets'"),
             QStringLiteral("missing"));
    QCOMPARE(count("etf_observations", "value = 0 AND value_state <> 'reported'"), 0);
}

void TstEtfIngest::sec_unknown_or_mismatched_identity_is_refused() {
    {
        FakeSec sec;
        sec.ok(kIvvIndex, series_index_atom(QString(), QString(), {}));
        const SecNportRunSummary s = run_sec(sec, request("1100663", "S000004310"), "2026-09-26T10:00:00.000Z");
        QCOMPARE(s.status, RetrievalStatus::SourceError);
        QCOMPARE(s.detail_code, QStringLiteral("index_without_company"));
        QCOMPARE(sec.requested.size(), 1);
    }
    {
        // EDGAR's real answer to an unknown series id (HTTP 200, an HTML page):
        // recorded as the identity the SEC does not know, with nothing stored.
        FakeSec sec;
        sec.ok(kIvvIndex, edgar_no_match_page());
        const int entities_before = count("etf_reporting_entities");
        const SecNportRunSummary s = run_sec(sec, request("1100663", "S000004310"), "2026-09-26T10:30:00.000Z");
        QCOMPARE(s.status, RetrievalStatus::SourceError);
        QCOMPARE(s.detail_code, QStringLiteral("series_not_found"));
        QCOMPARE(sec.requested.size(), 1);
        QCOMPARE(count("etf_retrievals", "detail_code = 'series_not_found' AND http_status = 200"), 1);
        QCOMPARE(count("etf_reporting_entities"), entities_before);
    }
    {
        FakeSec sec;
        sec.ok(kIvvIndex, series_index_atom(QStringLiteral("0001064642"), QStringLiteral("SPDR SERIES TRUST"), {}));
        const SecNportRunSummary s = run_sec(sec, request("1100663", "S000004310"), "2026-09-26T11:00:00.000Z");
        QCOMPARE(s.detail_code, QStringLiteral("series_registrant_mismatch"));
    }
    {
        // The document is filed under the requested CIK but for another series.
        FakeSec sec;
        serve_ivv(sec);
        NportSpec other = ivv_2025_09(false);
        other.series_id = QStringLiteral("S000004344");
        sec.ok(sec_nport_primary_doc_url(QStringLiteral("0001100663"), QStringLiteral("0002071691-25-007634")),
               nport_xml(other));
        const SecNportRunSummary s = run_sec(sec, request("1100663", "S000004310", 3), "2026-09-26T12:00:00.000Z");
        QCOMPARE(s.filings_stored, 2);
        QCOMPARE(count("etf_retrievals", "detail_code = 'series_mismatch'"), 1);
    }
    {
        // A registrant mismatch and a form mismatch are refused as well.
        FakeSec sec;
        serve_spy(sec);
        NportSpec wrong_cik = spy_2026_06();
        wrong_cik.cik = QStringLiteral("0000000001");
        sec.ok(kSpyDocJune, nport_xml(wrong_cik));
        NportSpec wrong_form = spy_2026_03();
        wrong_form.submission_type = QStringLiteral("NPORT-P/A");
        sec.ok(kSpyDocMarch, nport_xml(wrong_form));
        const int observations_before = count("etf_observations"); // the blocks above share this database
        const SecNportRunSummary s = run_sec(sec, request("884394"), "2026-09-26T13:00:00.000Z");
        QCOMPARE(s.filings_stored, 0);
        QCOMPARE(count("etf_retrievals", "detail_code = 'registrant_mismatch'"), 1);
        QCOMPARE(count("etf_retrievals", "detail_code = 'form_mismatch'"), 1);
        QCOMPARE(count("etf_observations"), observations_before);
        QCOMPARE(count("etf_reporting_entities", "cik = '0000884394'"), 0);
    }
}

void TstEtfIngest::sec_report_date_not_month_end_keeps_flows_unmapped() {
    FakeSec sec;
    NportSpec odd = spy_2026_06();
    odd.rep_pd_date = QStringLiteral("2026-06-15");
    sec.ok(kSpySubs, submissions_json(QStringLiteral("0000884394"), QStringLiteral("SPDR S&P 500 ETF TRUST"),
                                      {{"0001410368-26-089410", "NPORT-P", "2026-08-28", "2026-06-15",
                                        "2026-08-28T12:25:47.000Z"}}));
    sec.ok(kSpyDocJune, nport_xml(odd));
    const SecNportRunSummary s = run_sec(sec, request("884394", "", 1), "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(count("etf_observations", "measurement_kind = 'regulatory_reported_flow'"), 0);
    QCOMPARE(scalar("SELECT basis FROM etf_observations WHERE measure = 'nport_net_assets'"),
             QStringLiteral("regulatory_report_date_net_assets"));
    QCOMPARE(count("etf_retrieval_issues", "code = 'report_date_not_month_end'"), 1);
}

void TstEtfIngest::sec_requires_a_declared_user_agent() {
    FakeSec sec;
    serve_spy(sec);
    const SecNportRunSummary s =
        run_sec(sec, request("884394"), "2026-09-26T10:00:00.000Z", QStringLiteral("MarketLab/0.1 (research)"));
    QCOMPARE(s.status, RetrievalStatus::NotConfigured);
    QVERIFY(sec.requested.isEmpty());
    QCOMPARE(count("etf_retrievals", "status = 'NOT_CONFIGURED' AND http_status IS NULL AND retrieved_at IS NULL"), 1);
}

void TstEtfIngest::sec_invalid_request_touches_nothing() {
    FakeSec sec;
    for (const SecNportRequest& r : {request("88439A"), request("884394", "C000012040"), request("884394", "", 41)}) {
        const SecNportRunSummary s = run_sec(sec, r, "2026-09-26T10:00:00.000Z");
        QCOMPARE(s.detail_code, QStringLiteral("invalid_request"));
    }
    QVERIFY(sec.requested.isEmpty());
    QCOMPARE(count("etf_retrievals"), 0);
}

// ── IBKR ─────────────────────────────────────────────────────────────────────

void TstEtfIngest::ibkr_backfill_is_stored_with_sessions_and_identity() {
    FakeIbkr ibkr;
    // Saturday 05:00 ET: the last requestable session is Friday 2026-09-25.
    const QString end = QStringLiteral("20260925 23:59:59 US/Eastern");
    ibkr.payload = ibkr_history_envelope("SPY", 756733, bars_for_sessions(QDate(2026, 9, 1), QDate(2026, 9, 25)), end);
    const IbkrDailyRunSummary s = run_ibkr(ibkr, "2026-09-26T09:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(ibkr.requests.size(), 1);
    QCOMPARE(ibkr.requests[0].end_date_time, end);
    QCOMPARE(ibkr.requests[0].duration, QStringLiteral("2 Y"));
    QCOMPARE(s.bars_accepted, 18);
    QCOMPARE(s.observations_inserted, 90);
    QCOMPARE(scalar("SELECT ibkr_con_id || '|' || symbol || '|' || primary_exchange FROM etf_listed_instruments"),
             QStringLiteral("756733|SPY|ARCA"));
    QCOMPARE(count("etf_observations", "history_type = 'market_backfill' AND point_in_time_status = "
                                       "'historical_assumption'"),
             90);
    QCOMPARE(scalar("SELECT units FROM etf_observations WHERE measure = 'bar_volume' LIMIT 1"),
             QStringLiteral("shares_ibkr_filtered"));
    QCOMPARE(count("etf_market_sessions", "day_type = 'holiday'"), 1); // Labor Day
    QCOMPARE(count("etf_market_sessions"), 19);
    QVERIFY(scalar("SELECT runtime_identity FROM etf_retrievals").contains(QLatin1String("\"commit\":\"4a3c606e\"")));
    QVERIFY(scalar("SELECT interpretation FROM etf_retrievals").contains(QLatin1String(kIbkrRequestEndRule)));
}

void TstEtfIngest::ibkr_replay_confirms_and_revision_is_kept() {
    FakeIbkr ibkr;
    const QString end = QStringLiteral("20260925 23:59:59 US/Eastern");
    QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), QDate(2026, 9, 25));
    ibkr.payload = ibkr_history_envelope("SPY", 756733, bars, end);
    run_ibkr(ibkr, "2026-09-26T09:00:00.000Z");
    const IbkrDailyRunSummary replay = run_ibkr(ibkr, "2026-09-26T18:00:00.000Z");
    QCOMPARE(replay.observations_inserted, 0);
    QCOMPARE(replay.observations_confirmed, 50);
    bars[3].volume += 2; // a late correction of 2 shares on 2026-09-17 (cf. A2 section 7.8)
    ibkr.payload = ibkr_history_envelope("SPY", 756733, bars, end);
    const IbkrDailyRunSummary revised = run_ibkr(ibkr, "2026-09-27T09:00:00.000Z");
    QCOMPARE(revised.observations_revised, 1);
    QCOMPARE(revised.observations_confirmed, 49);
    QCOMPARE(count("etf_observations", "measure = 'bar_volume' AND effective_date = '2026-09-17'"), 2);
    QCOMPARE(scalar("SELECT revision_state || '|' || availability_basis FROM etf_observations WHERE measure = "
                    "'bar_volume' AND effective_date = '2026-09-17' AND source_revision = 2"),
             QStringLiteral("revised|recorded_first_seen"));
    QCOMPARE(scalar("SELECT CAST(value AS INTEGER) FROM etf_observations WHERE measure = 'bar_volume' AND "
                    "effective_date = '2026-09-17' AND source_revision = 1"),
             QStringLiteral("1000000")); // the earlier value still exists
}

void TstEtfIngest::ibkr_forward_observation_uses_the_next_session() {
    FakeIbkr ibkr;
    ibkr.payload = ibkr_history_envelope("SPY", 756733, bars_for_sessions(QDate(2026, 9, 21), QDate(2026, 9, 25)),
                                         QStringLiteral("20260925 23:59:59 US/Eastern"));
    run_ibkr(ibkr, "2026-09-26T09:00:00.000Z");
    // Tuesday 05:00 ET: the Monday 2026-09-28 session closed after the start.
    ibkr.payload = ibkr_history_envelope("SPY", 756733, bars_for_sessions(QDate(2026, 9, 21), QDate(2026, 9, 28)),
                                         QStringLiteral("20260928 23:59:59 US/Eastern"));
    const IbkrDailyRunSummary s = run_ibkr(ibkr, "2026-09-29T09:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Ok);
    QCOMPARE(s.observations_inserted, 5);
    QCOMPARE(scalar("SELECT history_type || '|' || point_in_time_status || '|' || availability_basis || '|' || "
                    "available_from FROM etf_observations WHERE measure = 'bar_close' AND effective_date = "
                    "'2026-09-28'"),
             QStringLiteral("forward_observed|observed|recorded_first_seen_and_ibkr_next_session_v1|"
                            "2026-09-29T13:30:00.000Z"));
    QCOMPARE(scalar("SELECT history_type FROM etf_observations WHERE measure = 'bar_close' AND effective_date = "
                    "'2026-09-25'"),
             QStringLiteral("market_backfill"));
}

void TstEtfIngest::ibkr_in_progress_session_is_rejected_and_recorded() {
    FakeIbkr ibkr;
    // Friday 10:51 ET: the request asks up to Thursday; the response also
    // carries the Friday bar of the session still trading.
    ibkr.payload = ibkr_history_envelope("SPY", 756733, bars_for_sessions(QDate(2026, 9, 21), QDate(2026, 9, 25)),
                                         QStringLiteral("20260924 23:59:59 US/Eastern"));
    const IbkrDailyRunSummary s = run_ibkr(ibkr, "2026-09-25T14:51:00.000Z");
    QCOMPARE(ibkr.requests[0].end_date_time, QStringLiteral("20260924 23:59:59 US/Eastern"));
    QCOMPARE(s.in_progress_rejected, 1);
    QCOMPARE(count("etf_observations", "effective_date = '2026-09-25'"), 0);
    QCOMPARE(count("etf_retrieval_issues", "state = 'IN_PROGRESS_SESSION' AND effective_date = '2026-09-25' AND "
                                           "subject_type = 'listed_instrument'"),
             1);
}

void TstEtfIngest::ibkr_missing_session_and_stale_history_are_recorded() {
    FakeIbkr ibkr;
    QVector<BarSpec> bars = bars_for_sessions(QDate(2026, 9, 14), QDate(2026, 9, 22));
    bars.removeAt(2); // 2026-09-16
    ibkr.payload = ibkr_history_envelope("SPY", 756733, bars, QStringLiteral("20260925 23:59:59 US/Eastern"));
    const IbkrDailyRunSummary s = run_ibkr(ibkr, "2026-09-26T09:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Stale);
    QCOMPARE(s.missing_sessions, 4); // 16, 23, 24, 25
    QCOMPARE(count("etf_retrieval_issues", "state = 'MISSING'"), 4);
    QCOMPARE(count("etf_retrievals", "status = 'STALE'"), 1);
    QCOMPARE(s.bars_accepted, 6); // what was returned is still stored
    QCOMPARE(count("etf_observations", "effective_date = '2026-09-16'"), 0);
}

void TstEtfIngest::ibkr_entitlement_and_tws_failures_store_no_data() {
    FakeIbkr ibkr;
    const QString end = QStringLiteral("20260925 23:59:59 US/Eastern");
    ibkr.payload = ibkr_history_envelope("SPY", 756733, {}, end, "2 Y", "NOT_ENTITLED", false);
    IbkrDailyRunSummary s = run_ibkr(ibkr, "2026-09-26T09:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::NotEntitled);
    ibkr.payload = ibkr_failure_envelope("IBKR_CONNECT_FAILED", "connect", "TWS is not running");
    s = run_ibkr(ibkr, "2026-09-26T10:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::SourceError);
    QCOMPARE(s.detail_code, QStringLiteral("IBKR_CONNECT_FAILED"));
    ibkr.payload = ibkr_history_envelope("SPY", 756733, {}, end, "2 Y", "STALE", false);
    s = run_ibkr(ibkr, "2026-09-26T11:00:00.000Z");
    QCOMPARE(s.status, RetrievalStatus::Stale);
    QCOMPARE(count("etf_observations"), 0);
    QCOMPARE(count("etf_listed_instruments"), 0);
    QCOMPARE(count("etf_retrievals"), 3);
    QCOMPARE(count("etf_retrievals", "status = 'NOT_ENTITLED'"), 1);
}

void TstEtfIngest::ibkr_not_configured_or_invalid_request_makes_no_request() {
    FakeIbkr ibkr;
    IbkrDailyRunSummary s = run_ibkr(ibkr, "2026-09-26T09:00:00.000Z", /*configured=*/false);
    QCOMPARE(s.status, RetrievalStatus::NotConfigured);
    QCOMPARE(count("etf_retrievals", "status = 'NOT_CONFIGURED' AND retrieved_at IS NULL"), 1);
    s = run_ibkr(ibkr, "2026-09-26T09:00:00.000Z", true, QStringLiteral("SPY;DEL"));
    QCOMPARE(s.detail_code, QStringLiteral("invalid_request"));
    s = run_ibkr(ibkr, "2026-09-26T09:00:00.000Z", true, QStringLiteral("SPY"), QStringLiteral("20 Y"));
    QCOMPARE(s.detail_code, QStringLiteral("invalid_request"));
    QVERIFY(ibkr.requests.isEmpty());
    QCOMPARE(count("etf_retrievals"), 1);
}

QTEST_GUILESS_MAIN(TstEtfIngest)
#include "tst_etf_ingest.moc"
