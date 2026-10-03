// MarketLab ETF Flow & Sector Rotation (consolidated Batch E) — research
// source persistence over a real SQLite store (migration v053): append-only
// vintages, confirmations and revisions, failed items that never erase earlier
// observations, consistent history windows, fund-snapshot session rule,
// null-versus-missing macro values, stage statuses, the manual-refresh
// pipeline with injected stages, and byte-identical replay after the store
// grows. Synthetic payloads only; nothing here touches the network.
#include "services/etf/research/EtfResearchEngine.h"
#include "services/etf/research/EtfResearchRefresh.h"
#include "services/etf/research/EtfResearchUniverse.h"
#include "storage/repositories/EtfResearchRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace fincept;
using namespace fincept::services::etf::research;

namespace {

QJsonArray bar(const char* d, double close, QJsonValue volume, double div = 0, double split = 0) {
    return QJsonArray{QLatin1String(d), close, volume, div, 0.0, split};
}

QJsonObject stage(const char* requested, const QJsonObject& items) {
    return QJsonObject{{"requested_at", QLatin1String(requested)},
                       {"retrieved_at", QLatin1String(requested)},
                       {"sha256", "fixture"},
                       {"items", items}};
}

QJsonObject history_item(const char* at, const QJsonArray& rows) {
    return QJsonObject{{"status", "OK"},
                       {"detail", ""},
                       {"rows", rows},
                       {"retrieved_at", QLatin1String(at)},
                       {"in_progress_excluded", 0}};
}

QJsonObject payload(const QJsonObject& stages) {
    return QJsonObject{{"script_version", "etf_research_fetch_v1"}, {"stages", stages}};
}

QDateTime utc(const char* s) {
    return QDateTime::fromString(QLatin1String(s), Qt::ISODateWithMs).toUTC();
}

int count(const char* table) {
    auto q = Database::instance().execute(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(table)));
    if (q.is_err() || !q.value().next())
        return -1;
    return q.value().value(0).toInt();
}

} // namespace

class TstEtfResearchStore : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    ResearchUniverse universe_;

  private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        register_migration_v052();
        register_migration_v053();
        QVERIFY(Database::instance().open(dir_.filePath(QStringLiteral("research.db"))).is_ok());
        QString err;
        const auto u = load_universe(QLatin1String(kUniverseResourcePath), &err);
        QVERIFY2(u, qPrintable(err));
        universe_ = *u;
    }
    void cleanupTestCase() { Database::instance().close(); }
    void schema_constraints();
    void bars_confirm_revise_and_keep_vintages();
    void failed_item_never_erases_and_is_not_this_runs_result();
    void history_window_is_one_fetch();
    void fund_snapshot_session_rule_holdings_and_fundamentals();
    void macro_null_is_missing_and_revisions_replay();
    void stage_status_stale_and_partial();
    void sec_refresh_catches_up_missed_months();
    void pipeline_records_every_stage_and_survives_fetch_failure();
    void snapshot_replay_is_byte_identical_after_growth();
};

void TstEtfResearchStore::schema_constraints() {
    auto& db = Database::instance();
    QVERIFY(db.execute(QStringLiteral("INSERT INTO etf_research_runs (run_id, trigger, started_at, status, "
                                      "universe_version) VALUES ('c1','manual_ui','2026-10-01T00:00:00.000Z',"
                                      "'RUNNING','v')"))
                .is_ok());
    // A running run has no finish time; a finished one must have it.
    QVERIFY(db.execute(QStringLiteral("UPDATE etf_research_runs SET status='COMPLETED' WHERE run_id='c1'")).is_err());
    // Unknown trigger (e.g. a scheduler) is refused by the schema.
    QVERIFY(
        db.execute(QStringLiteral("INSERT INTO etf_research_runs (run_id, trigger, started_at, status, "
                                  "universe_version) VALUES ('c2','timer','2026-10-01T00:00:00.000Z','RUNNING','v')"))
            .is_err());
    QVERIFY(db.execute(QStringLiteral("INSERT INTO etf_research_retrievals (run_id, stage, subject, requested_at, "
                                      "status, rows_received) VALUES ('c1','yahoo_history','X','t','FAILED',5)"))
                .is_err()); // a failed retrieval delivers no rows
    QVERIFY(db.execute(QStringLiteral("INSERT INTO etf_research_retrievals (run_id, stage, subject, requested_at, "
                                      "status) VALUES ('c1','yahoo_history','X','t','OK')"))
                .is_ok());
    const qint64 rid =
        db.execute(QStringLiteral("SELECT MAX(retrieval_id) FROM etf_research_retrievals")).value().next() ? 1 : 0;
    QVERIFY(rid == 1);
    auto bar_sql = [&](const QString& close, const QString& volume) {
        return db.execute(
            QStringLiteral("INSERT INTO etf_research_bars (symbol, session_date, revision, close, volume, "
                           "dividend, capital_gain, split_ratio, source, first_seen_at, last_seen_at, "
                           "first_retrieval_id, last_retrieval_id) VALUES ('X','2026-09-30',1,%1,%2,0,0,0,"
                           "'yahoo_chart','t','t',1,1)")
                .arg(close, volume));
    };
    QVERIFY(bar_sql(QStringLiteral("0"), QStringLiteral("100")).is_err());  // a bar always has a positive close
    QVERIFY(bar_sql(QStringLiteral("10"), QStringLiteral("NULL")).is_ok()); // unknown volume is NULL, not 0
    QVERIFY(db.execute(QStringLiteral("DELETE FROM etf_research_bars")).is_ok());
    QVERIFY(db.execute(QStringLiteral("DELETE FROM etf_research_retrievals")).is_ok());
    QVERIFY(db.execute(QStringLiteral("DELETE FROM etf_research_runs")).is_ok());
}

void TstEtfResearchStore::bars_confirm_revise_and_keep_vintages() {
    auto& repo = EtfResearchRepository::instance();
    QVERIFY(repo.begin_run(QStringLiteral("r1"), QStringLiteral("manual_cli"), utc("2026-09-29T22:00:00.000Z"),
                           universe_.version)
                .is_ok());
    const QJsonArray rows1{bar("2026-09-25", 100, 1e6), bar("2026-09-28", 101, 2e6),
                           bar("2026-09-29", 102, QJsonValue())};
    auto p1 = repo.persist_payload(
        QStringLiteral("r1"),
        payload({{"yahoo_history",
                  stage("2026-09-29T22:00:00.000Z", {{"SPY", history_item("2026-09-29T22:00:01.000Z", rows1)}})}}),
        QDate(2026, 9, 29));
    QVERIFY2(p1.is_ok(), p1.is_err() ? p1.error().c_str() : "");
    QCOMPARE(p1.value().first().status, QStringLiteral("UPDATED"));
    QCOMPARE(p1.value().first().rows_inserted, 3);
    QCOMPARE(count("etf_research_bars"), 3);
    // Same values again: confirmed, no new rows. One changed close: a revision.
    QVERIFY(repo.begin_run(QStringLiteral("r2"), QStringLiteral("manual_cli"), utc("2026-09-30T22:00:00.000Z"),
                           universe_.version)
                .is_ok());
    const QJsonArray rows2{bar("2026-09-25", 100, 1e6), bar("2026-09-28", 101.5, 2e6),
                           bar("2026-09-29", 102, QJsonValue()), bar("2026-09-30", 103, 1e6)};
    auto p2 = repo.persist_payload(
        QStringLiteral("r2"),
        payload({{"yahoo_history",
                  stage("2026-09-30T22:00:00.000Z", {{"SPY", history_item("2026-09-30T22:00:01.000Z", rows2)}})}}),
        QDate(2026, 9, 30));
    QVERIFY(p2.is_ok());
    QCOMPARE(p2.value().first().rows_confirmed, 2);
    QCOMPARE(p2.value().first().rows_revised, 1);
    QCOMPARE(p2.value().first().rows_inserted, 1);
    QCOMPARE(count("etf_research_bars"), 5); // the earlier 101.0 vintage is kept
    // Replay at the first cutoff sees the first vintage; now sees the revision.
    auto old = repo.load_inputs(universe_, utc("2026-09-29T23:00:00.000Z"), utc("2026-09-29T23:00:00.000Z"));
    QVERIFY(old.is_ok());
    QCOMPARE(old.value().bars.value(QStringLiteral("SPY")).bars.size(), 3);
    QCOMPARE(old.value().bars.value(QStringLiteral("SPY")).bars[1].close, 101.0);
    QVERIFY(!old.value().bars.value(QStringLiteral("SPY")).bars[2].volume.has_value());
    auto now = repo.load_inputs(universe_, utc("2026-10-01T12:00:00.000Z"), utc("2026-10-01T12:00:00.000Z"));
    QCOMPARE(now.value().bars.value(QStringLiteral("SPY")).bars.size(), 4);
    QCOMPARE(now.value().bars.value(QStringLiteral("SPY")).bars[1].close, 101.5);
    QVERIFY(now.value().bars.value(QStringLiteral("SPY")).bars[1].revised);
    QVERIFY(now.value().bars.value(QStringLiteral("SPY")).any_revised);
    // Point in time with a later knowledge cutoff: at 11:00 New York on 09-30 the
    // 09-30 session had not closed, so its bar (known by known_at) is excluded,
    // while the later-known revision of the finished 09-28 session is used.
    auto intraday = repo.load_inputs(universe_, utc("2026-09-30T15:00:00.000Z"), utc("2026-10-01T12:00:00.000Z"));
    QCOMPARE(intraday.value().bars.value(QStringLiteral("SPY")).bars.size(), 3);
    QCOMPARE(intraday.value().bars.value(QStringLiteral("SPY")).bars.last().date, QDate(2026, 9, 29));
    QCOMPARE(intraday.value().bars.value(QStringLiteral("SPY")).bars[1].close, 101.5);
}

void TstEtfResearchStore::failed_item_never_erases_and_is_not_this_runs_result() {
    auto& repo = EtfResearchRepository::instance();
    QVERIFY(repo.begin_run(QStringLiteral("r3"), QStringLiteral("manual_cli"), utc("2026-10-01T22:00:00.000Z"),
                           universe_.version)
                .is_ok());
    const int before = count("etf_research_bars");
    auto p = repo.persist_payload(
        QStringLiteral("r3"),
        payload(
            {{"yahoo_history",
              stage("2026-10-01T22:00:00.000Z",
                    {{"SPY", QJsonObject{{"status", "FAILED"}, {"detail", "HTTP 429"}, {"rows", QJsonArray()}}}})}}),
        QDate(2026, 10, 1));
    QVERIFY(p.is_ok());
    QCOMPARE(p.value().first().status, QStringLiteral("FAILED"));
    QVERIFY(p.value().first().failed_subjects.contains(QStringLiteral("SPY")));
    QCOMPARE(count("etf_research_bars"), before);
    // The stored SPY history is still the 2026-09-30 window; its latest session is now stale.
    auto in = repo.load_inputs(universe_, utc("2026-10-02T12:00:00.000Z"), utc("2026-10-02T12:00:00.000Z"));
    QCOMPARE(in.value().bars.value(QStringLiteral("SPY")).bars.last().date, QDate(2026, 9, 30));
    ResearchInputs x = in.value();
    x.expected_us_session = QDate(2026, 10, 1);
    const ResearchSnapshot s = compute_snapshot(x);
    QVERIFY(s.row(QStringLiteral("SPY"))->stale);
}

void TstEtfResearchStore::history_window_is_one_fetch() {
    auto& repo = EtfResearchRepository::instance();
    QVERIFY(repo.begin_run(QStringLiteral("r4"), QStringLiteral("manual_cli"), utc("2026-10-02T22:00:00.000Z"),
                           universe_.version)
                .is_ok());
    // A later fetch restated after a 2:1 split returns only two sessions; the replay
    // must not mix its halved closes with the older unrestated sessions.
    const QJsonArray rows{bar("2026-09-30", 51.5, 2e6), bar("2026-10-01", 52, 2e6, 0, 0)};
    QVERIFY(repo.persist_payload(
                    QStringLiteral("r4"),
                    payload({{"yahoo_history", stage("2026-10-02T22:00:00.000Z",
                                                     {{"SPY", history_item("2026-10-02T22:00:01.000Z", rows)}})}}),
                    QDate(2026, 10, 2))
                .is_ok());
    auto in = repo.load_inputs(universe_, utc("2026-10-03T12:00:00.000Z"), utc("2026-10-03T12:00:00.000Z"));
    const auto bars = in.value().bars.value(QStringLiteral("SPY")).bars;
    QCOMPARE(bars.size(), 2);
    QCOMPARE(bars.first().date, QDate(2026, 9, 30));
    QCOMPARE(bars.first().close, 51.5);
}

void TstEtfResearchStore::fund_snapshot_session_rule_holdings_and_fundamentals() {
    auto& repo = EtfResearchRepository::instance();
    // Friday 2026-10-02 14:30 UTC (10:30 New York): the prior completed session is Thursday.
    QCOMPARE(EtfResearchRepository::prior_completed_session(utc("2026-10-02T14:30:00.000Z")), QDate(2026, 10, 1));
    // Monday after Good Friday 2026-04-03: Thursday 04-02.
    QCOMPARE(EtfResearchRepository::prior_completed_session(utc("2026-04-06T15:00:00.000Z")), QDate(2026, 4, 2));
    QVERIFY(repo.begin_run(QStringLiteral("r5"), QStringLiteral("manual_cli"), utc("2026-10-02T14:30:00.000Z"),
                           universe_.version)
                .is_ok());
    const QJsonObject fund{
        {"status", "OK"},
        {"captured_at", "2026-10-02T14:30:05.000Z"},
        {"fields", QJsonObject{{"totalAssets", 1.2e11},
                               {"navPrice", 195.5},
                               {"sharesOutstanding", 6.1e8},
                               {"netExpenseRatio", 0.08},
                               {"yield", 0.0043},
                               {"currency", "USD"}}},
        {"holdings", QJsonArray{QJsonArray{1, "NVDA", "NVIDIA", 0.14}, QJsonArray{2, "AAPL", "Apple", 0.12}}},
        {"sector_weights", QJsonObject{{"technology", 0.99}}},
        {"holdings_status", "OK"}};
    const QJsonObject fund_failed{{"status", "FAILED"}, {"detail", "quote summary failed"}};
    const QJsonObject fundamentals{{"status", "OK"},
                                   {"captured_at", "2026-10-02T14:31:00.000Z"},
                                   {"fields", QJsonObject{{"trailingPE", 50.0}, {"currency", "USD"}}}};
    auto p = repo.persist_payload(
        QStringLiteral("r5"),
        payload({{"yahoo_funds", stage("2026-10-02T14:30:00.000Z", {{"XLK", fund}, {"XLF", fund_failed}})},
                 {"yahoo_fundamentals", stage("2026-10-02T14:31:00.000Z", {{"NVDA", fundamentals}})}}),
        QDate(2026, 10, 1));
    QVERIFY(p.is_ok());
    QCOMPARE(p.value()[0].status, QStringLiteral("PARTIAL"));
    auto in = repo.load_inputs(universe_, utc("2026-10-03T00:00:00.000Z"), utc("2026-10-03T00:00:00.000Z"));
    const auto caps = in.value().funds.value(QStringLiteral("XLK"));
    QCOMPARE(caps.size(), 1);
    QCOMPARE(caps.first().effective_session, QDate(2026, 10, 1));
    QCOMPARE(caps.first().effective_rule, QStringLiteral("prior_completed_session_v1"));
    QCOMPARE(*caps.first().total_assets, 1.2e11);
    QVERIFY(!in.value().funds.contains(QStringLiteral("XLF"))); // a failed capture is not an observation
    QCOMPARE(in.value().holdings.value(QStringLiteral("XLK")).holdings.size(), 2);
    QCOMPARE(in.value().holdings.value(QStringLiteral("XLK")).sector_weights.value(QStringLiteral("technology")), 0.99);
    QCOMPARE(
        in.value().fundamentals.value(QStringLiteral("NVDA")).fields.value(QStringLiteral("trailingPE")).toDouble(),
        50.0);
    // Point in time: a frame whose decision time precedes the capture must not
    // see it, even when the knowledge cutoff is later (as_of < capture <= known_at).
    auto early = repo.load_inputs(universe_, utc("2026-10-02T12:00:00.000Z"), utc("2026-10-03T00:00:00.000Z"));
    QVERIFY(early.is_ok());
    QVERIFY(!early.value().funds.contains(QStringLiteral("XLK")));
    QVERIFY(!early.value().holdings.contains(QStringLiteral("XLK")));
    QVERIFY(!early.value().holdings_history.contains(QStringLiteral("XLK")));
    QVERIFY(!early.value().fundamentals.contains(QStringLiteral("NVDA")));
    // Between the two captures: the fund capture (14:30) is visible, the
    // constituent fundamentals capture (14:31) is not.
    auto mid = repo.load_inputs(universe_, utc("2026-10-02T14:30:30.000Z"), utc("2026-10-03T00:00:00.000Z"));
    QVERIFY(mid.value().funds.contains(QStringLiteral("XLK")));
    QCOMPARE(mid.value().holdings.value(QStringLiteral("XLK")).holdings.size(), 2);
    QVERIFY(!mid.value().fundamentals.contains(QStringLiteral("NVDA")));
    // Units: netExpenseRatio is already percent; yield is a fraction.
    const FundFacts f = fund_facts_from_captures(caps, nullptr, nullptr, QDate(2026, 10, 1));
    QCOMPARE(*f.expense_pct.value, 0.08);
    QVERIFY(std::abs(*f.yield_pct.value - 0.43) < 1e-12);
    QVERIFY(f.aum.has_flag("ASSUMED_EFFECTIVE_DATE"));
    QVERIFY(std::abs(*f.shares_gap_pct.value - (1.2e11 / 195.5 / 6.1e8 - 1) * 100) < 1e-9);
}

void TstEtfResearchStore::macro_null_is_missing_and_revisions_replay() {
    auto& repo = EtfResearchRepository::instance();
    QVERIFY(repo.begin_run(QStringLiteral("r6"), QStringLiteral("manual_cli"), utc("2026-10-02T15:00:00.000Z"),
                           universe_.version)
                .is_ok());
    const QJsonObject fred{{"status", "OK"},
                           {"retrieved_at", "2026-10-02T15:00:01.000Z"},
                           {"rows", QJsonArray{QJsonArray{"2026-07-01", -0.10}, QJsonArray{"2026-08-01", 0.05}}},
                           {"missing_points", 0},
                           {"response_sha256", "x"}};
    const QJsonObject wb{{"status", "OK"},
                         {"retrieved_at", "2026-10-02T15:00:02.000Z"},
                         {"rows", QJsonArray{QJsonArray{"JP", 2024, 0.4}, QJsonArray{"JP", 2025, QJsonValue()}}},
                         {"source_last_updated", "2026-07-13"},
                         {"countries_absent", QJsonArray{"TW"}}};
    QVERIFY(
        repo.persist_payload(QStringLiteral("r6"),
                             payload({{"fred", stage("2026-10-02T15:00:00.000Z", {{"CFNAI", fred}})},
                                      {"world_bank", stage("2026-10-02T15:00:00.000Z", {{"NY.GDP.MKTP.KD.ZG", wb}})}}),
                             QDate(2026, 10, 1))
            .is_ok());
    QVERIFY(repo.begin_run(QStringLiteral("r7"), QStringLiteral("manual_cli"), utc("2026-11-02T15:00:00.000Z"),
                           universe_.version)
                .is_ok());
    QJsonObject fred2 = fred;
    fred2.insert("retrieved_at", "2026-11-02T15:00:01.000Z");
    fred2.insert("rows", QJsonArray{QJsonArray{"2026-07-01", -0.12}, QJsonArray{"2026-08-01", 0.05},
                                    QJsonArray{"2026-09-01", 0.20}});
    QVERIFY(repo.persist_payload(QStringLiteral("r7"),
                                 payload({{"fred", stage("2026-11-02T15:00:00.000Z", {{"CFNAI", fred2}})}}),
                                 QDate(2026, 10, 30))
                .is_ok());
    auto before = repo.load_inputs(universe_, utc("2026-10-03T00:00:00.000Z"), utc("2026-10-03T00:00:00.000Z"));
    auto after = repo.load_inputs(universe_, utc("2026-11-03T00:00:00.000Z"), utc("2026-11-03T00:00:00.000Z"));
    QCOMPARE(before.value().fred.value(QStringLiteral("CFNAI")).points.size(), 2);
    QCOMPARE(before.value().fred.value(QStringLiteral("CFNAI")).points.first().value, -0.10);
    QCOMPARE(after.value().fred.value(QStringLiteral("CFNAI")).points.size(), 3);
    QCOMPARE(after.value().fred.value(QStringLiteral("CFNAI")).points.first().value, -0.12);
    QVERIFY(after.value().fred.value(QStringLiteral("CFNAI")).points.first().revised);
    // A value known by known_at but not yet published at as_of is excluded:
    // CFNAI for September (dated 09-01) is assumed available from 11-01.
    auto early = repo.load_inputs(universe_, utc("2026-10-03T00:00:00.000Z"), utc("2026-11-03T00:00:00.000Z"));
    QCOMPARE(early.value().fred.value(QStringLiteral("CFNAI")).points.size(), 2);
    QCOMPARE(early.value().fred.value(QStringLiteral("CFNAI")).points.first().value, -0.12); // later-known revision
    // World Bank 2024 (dated 2024-12-31) is usable from 2025-07-19, not before.
    auto wb_early = repo.load_inputs(universe_, utc("2025-07-01T00:00:00.000Z"), utc("2026-11-03T00:00:00.000Z"));
    QVERIFY(wb_early.value()
                .world_bank.value(QStringLiteral("NY.GDP.MKTP.KD.ZG"))
                .value(QStringLiteral("JP"))
                .points.isEmpty());
    // World Bank null: stored (the source said "no value") but never read as a number.
    const auto jp = after.value().world_bank.value(QStringLiteral("NY.GDP.MKTP.KD.ZG")).value(QStringLiteral("JP"));
    QCOMPARE(jp.points.size(), 1);
    QCOMPARE(jp.points.first().date, QDate(2024, 12, 31));
    QVERIFY(!after.value().world_bank.value(QStringLiteral("NY.GDP.MKTP.KD.ZG")).contains(QStringLiteral("TW")));
}

void TstEtfResearchStore::sec_refresh_catches_up_missed_months() {
    // A fresh profile asks for the route's maximum; five months away asks for
    // enough filings to bridge the gap; an up-to-date profile still asks for a
    // few (amendments, the quarter in flight); never more than the route allows.
    QCOMPARE(sec_filings_to_request(QDate(), QDate(2026, 10, 3)), 40);
    QCOMPARE(sec_filings_to_request(QDate(2026, 4, 30), QDate(2026, 10, 3)), 9);
    QCOMPARE(sec_filings_to_request(QDate(2026, 8, 31), QDate(2026, 10, 3)), 5);
    QCOMPARE(sec_filings_to_request(QDate(2026, 9, 30), QDate(2026, 10, 3)), 4);
    QCOMPARE(sec_filings_to_request(QDate(2021, 1, 31), QDate(2026, 10, 3)), 40);
    // The latest stored period is read from Batch B's filings table.
    auto none = EtfResearchRepository::instance().latest_sec_report_period(987654);
    QVERIFY(none.is_ok());
    QVERIFY(!none.value().isValid());
}

void TstEtfResearchStore::stage_status_stale_and_partial() {
    auto& repo = EtfResearchRepository::instance();
    QVERIFY(repo.begin_run(QStringLiteral("r8"), QStringLiteral("manual_cli"), utc("2026-10-05T22:00:00.000Z"),
                           universe_.version)
                .is_ok());
    // XLU's history ends before the expected session: STALE, not UPDATED.
    auto p = repo.persist_payload(
        QStringLiteral("r8"),
        payload({{"yahoo_history", stage("2026-10-05T22:00:00.000Z",
                                         {{"XLU", history_item("2026-10-05T22:00:01.000Z",
                                                               QJsonArray() << bar("2026-10-01", 80, 1e6))}})}}),
        QDate(2026, 10, 5));
    QVERIFY2(p.is_ok(), p.is_err() ? p.error().c_str() : "");
    QCOMPARE(p.value().first().status, QStringLiteral("STALE"));
    QCOMPARE(p.value().first().stale_items, 1);
    QVERIFY(p.value().first().dependent_calculations.contains(QStringLiteral("rrg")));
}

void TstEtfResearchStore::pipeline_records_every_stage_and_survives_fetch_failure() {
    auto not_configured = [](const char* name) {
        return RefreshStage([name](std::function<void(SourceStageStatus)> done) {
            SourceStageStatus s;
            s.stage = QLatin1String(name);
            s.status = QStringLiteral("NOT_CONFIGURED");
            s.detail = QStringLiteral("no local configuration");
            done(s);
        });
    };
    int fetches = 0;
    RefreshFetcher ok_fetch = [&](const QJsonObject& request, RefreshProgress,
                                  std::function<void(Result<QJsonObject>)> d) {
        ++fetches;
        QVERIFY(request.contains(QStringLiteral("funds")));
        d(Result<QJsonObject>::ok(
            payload({{"yahoo_history", stage("2026-10-06T22:00:00.000Z",
                                             {{"XLK", history_item("2026-10-06T22:00:01.000Z",
                                                                   QJsonArray() << bar("2026-10-06", 200, 1e6))}})}})));
    };
    RefreshResult first;
    run_refresh_pipeline(
        QStringLiteral("manual_cli"), universe_.version, QJsonObject{{"funds", QJsonArray()}}, ok_fetch,
        not_configured("ibkr_daily"), not_configured("sec_nport"), []() { return QDate(2026, 10, 6); }, nullptr,
        [&](const RefreshResult& r) { first = r; });
    QVERIFY2(first.ok, qPrintable(first.error));
    QCOMPARE(fetches, 1);
    QStringList stages;
    for (const auto& s : first.stages)
        stages << s.stage + QLatin1Char('=') + s.status;
    QVERIFY2(stages.contains(QStringLiteral("yahoo_history=UPDATED")), qPrintable(stages.join(',')));
    QVERIFY(stages.contains(QStringLiteral("ibkr_daily=NOT_CONFIGURED")));
    QVERIFY(stages.contains(QStringLiteral("sec_nport=NOT_CONFIGURED")));
    const int bars_before = count("etf_research_bars");
    RefreshFetcher bad_fetch = [&](const QJsonObject&, RefreshProgress, std::function<void(Result<QJsonObject>)> d) {
        d(Result<QJsonObject>::err("fetch script failed (exit 1): network unreachable"));
    };
    RefreshResult second;
    run_refresh_pipeline(
        QStringLiteral("manual_ui"), universe_.version, QJsonObject(), bad_fetch, not_configured("ibkr_daily"),
        not_configured("sec_nport"), []() { return QDate(2026, 10, 7); }, nullptr,
        [&](const RefreshResult& r) { second = r; });
    QVERIFY(second.ok); // the run completed and recorded its failures
    int failed = 0;
    for (const auto& s : second.stages)
        failed += s.status == QLatin1String("FAILED") ? 1 : 0;
    QCOMPARE(failed, 6);
    QCOMPARE(count("etf_research_bars"), bars_before); // nothing was erased
    // The last completed run is the failed one, and the loader reports it as such.
    auto in = EtfResearchRepository::instance().load_inputs(universe_, QDateTime::currentDateTimeUtc(),
                                                            QDateTime::currentDateTimeUtc());
    QCOMPARE(in.value().last_refresh_run_id, second.run_id);
    bool saw_failed_history = false;
    for (const auto& s : in.value().last_refresh)
        saw_failed_history =
            saw_failed_history || (s.stage == QLatin1String("yahoo_history") && s.status == QLatin1String("FAILED"));
    QVERIFY(saw_failed_history);
    // A stage whose STORE write fails (here a negative volume violates the
    // table's CHECK) is recorded FAILED; the other stages are still stored and
    // the IBKR and SEC stages still run. The write runs through the executor:
    // nothing completes until its continuation runs.
    int extra_stages = 0;
    auto counted = [&](const char* name) {
        return RefreshStage([name, &extra_stages](std::function<void(SourceStageStatus)> done) {
            ++extra_stages;
            SourceStageStatus s;
            s.stage = QLatin1String(name);
            s.status = QStringLiteral("UNAVAILABLE");
            done(s);
        });
    };
    const QJsonObject fred_ok{{"status", "OK"},
                              {"retrieved_at", "2026-10-08T15:00:01.000Z"},
                              {"rows", QJsonArray() << QJsonArray{"2026-10-07", 4.1}}, // << nests a single row
                              {"missing_points", 0},
                              {"response_sha256", "y"}};
    RefreshFetcher mixed_fetch = [&](const QJsonObject&, RefreshProgress, std::function<void(Result<QJsonObject>)> d) {
        d(Result<QJsonObject>::ok(payload(
            {{"yahoo_history",
              stage("2026-10-08T22:00:00.000Z",
                    {{"XLK", history_item("2026-10-08T22:00:01.000Z", QJsonArray() << bar("2026-10-08", 201, -5.0))}})},
             {"fred", stage("2026-10-08T15:00:00.000Z", {{"DGS10", fred_ok}})}})));
    };
    std::function<void()> pending_work, pending_then;
    RefreshExecutor deferred = [&](std::function<void()> work, std::function<void()> then) {
        pending_work = std::move(work);
        pending_then = std::move(then);
    };
    bool finished = false;
    RefreshResult third;
    run_refresh_pipeline(
        QStringLiteral("manual_ui"), universe_.version, QJsonObject(), mixed_fetch, counted("ibkr_daily"),
        counted("sec_nport"), []() { return QDate(2026, 10, 8); }, nullptr,
        [&](const RefreshResult& r) {
            third = r;
            finished = true;
        },
        deferred);
    QVERIFY(!finished);
    QVERIFY(pending_work && pending_then);
    QCOMPARE(extra_stages, 0);
    pending_work();
    QVERIFY(!finished); // the continuation runs on the caller's thread, after the write
    pending_then();
    QVERIFY(finished);
    QCOMPARE(extra_stages, 2);
    QStringList st;
    for (const auto& x : third.stages)
        st << x.stage + QLatin1Char('=') + x.status + QLatin1Char('(') + x.detail + QLatin1Char(')');
    QVERIFY2(st.join(',').contains(QStringLiteral("yahoo_history=FAILED")), qPrintable(st.join(',')));
    QVERIFY2(st.join(',').contains(QStringLiteral("fred=UPDATED")), qPrintable(st.join(',')));
    QVERIFY(st.join(',').contains(QStringLiteral("ibkr_daily=UNAVAILABLE")));
    QVERIFY(st.join(',').contains(QStringLiteral("sec_nport=UNAVAILABLE")));
    QVERIFY(third.error.contains(QStringLiteral("yahoo_history")));
}

void TstEtfResearchStore::snapshot_replay_is_byte_identical_after_growth() {
    auto& repo = EtfResearchRepository::instance();
    const QDateTime cut = utc("2026-10-07T00:00:00.000Z");
    auto in1 = repo.load_inputs(universe_, cut, cut);
    QVERIFY(in1.is_ok());
    const QByteArray a = QJsonDocument(snapshot_to_json(compute_snapshot(in1.value()))).toJson(QJsonDocument::Compact);
    const QJsonObject exp1 = repo.export_all().value();
    // Grow the store after the cutoff: new bars, a revision and a new fund capture.
    QVERIFY(repo.begin_run(QStringLiteral("r9"), QStringLiteral("manual_cli"), utc("2026-10-09T22:00:00.000Z"),
                           universe_.version)
                .is_ok());
    QVERIFY(repo.persist_payload(QStringLiteral("r9"),
                                 payload({{"yahoo_history",
                                           stage("2026-10-09T22:00:00.000Z",
                                                 {{"SPY", history_item("2026-10-09T22:00:01.000Z",
                                                                       QJsonArray{bar("2026-10-01", 53, 2e6),
                                                                                  bar("2026-10-09", 55, 1e6)})}})}}),
                                 QDate(2026, 10, 9))
                .is_ok());
    auto in2 = repo.load_inputs(universe_, cut, cut);
    const QByteArray b = QJsonDocument(snapshot_to_json(compute_snapshot(in2.value()))).toJson(QJsonDocument::Compact);
    QCOMPARE(a, b);
    QVERIFY(repo.export_all().value() != exp1);
}

QTEST_GUILESS_MAIN(TstEtfResearchStore)
#include "tst_etf_research_store.moc"
