// v054_etf_research_cftc — MarketLab ETF Flow & Sector Rotation (consolidated
// Batch E): CFTC Commitments of Traders positioning as a research source.
//
// The manual refresh reads the CFTC positioning of the futures markets that
// some research ETFs track directly (gold, the S&P 500, Treasury notes, ...)
// through MarketLab's existing CFTC tool, and stores the weekly series it
// needs (open interest, non-commercial long and short) as research
// observations so the workspace reads them locally. Two v053 CHECK lists name
// the allowed sources and stages; SQLite cannot alter a CHECK, so both tables
// are rebuilt with 'cftc' added, every row preserved:
//
//   etf_research_retrievals  stage IN (..., 'cftc')
//   etf_research_macro       source IN ('fred','world_bank','cftc')
//
// The migration runs inside the runner's transaction with deferred foreign
// keys: the retrievals table is the parent of every observation table, and
// each parent row is re-inserted with its own retrieval_id before commit.
//
// Historical migrations are never edited; later changes are new versions.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

// Uniquely named for unity builds (see v052).
Result<void> sql_v054(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(QString::fromUtf8(stmt)))
        return Result<void>::err(std::string(stmt).substr(0, 60) + ": " + q.lastError().text().toStdString());
    return Result<void>::ok();
}

const char* const kV054Statements[] = {
    "PRAGMA defer_foreign_keys = ON",

    // ── etf_research_retrievals (parent of every observation table) ──────────
    "CREATE TABLE etf_research_retrievals_v054_copy AS SELECT * FROM etf_research_retrievals",
    "DROP TABLE etf_research_retrievals",
    "CREATE TABLE etf_research_retrievals ("
    "  retrieval_id    INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  run_id          TEXT NOT NULL REFERENCES etf_research_runs (run_id),"
    "  stage           TEXT NOT NULL CHECK (stage IN ('yahoo_history','yahoo_funds','yahoo_constituent_history',"
    "                    'yahoo_fundamentals','fred','world_bank','ibkr_daily','sec_nport','cftc')),"
    "  subject         TEXT NOT NULL,"
    "  requested_at    TEXT NOT NULL,"
    "  retrieved_at    TEXT,"
    "  status          TEXT NOT NULL CHECK (status IN ('OK','FAILED','UNAVAILABLE','NOT_CONFIGURED','PARTIAL')),"
    "  detail          TEXT NOT NULL DEFAULT '',"
    "  response_sha256 TEXT NOT NULL DEFAULT '',"
    "  rows_received   INTEGER NOT NULL DEFAULT 0 CHECK (rows_received >= 0),"
    "  first_effective TEXT,"
    "  last_effective  TEXT,"
    "  CHECK (status IN ('OK','PARTIAL') OR rows_received = 0)"
    ")",
    "INSERT INTO etf_research_retrievals (retrieval_id, run_id, stage, subject, requested_at, retrieved_at, status, "
    "detail, response_sha256, rows_received, first_effective, last_effective) SELECT retrieval_id, run_id, stage, "
    "subject, requested_at, retrieved_at, status, detail, response_sha256, rows_received, first_effective, "
    "last_effective FROM etf_research_retrievals_v054_copy ORDER BY retrieval_id",
    "DROP TABLE etf_research_retrievals_v054_copy",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_retrievals_run ON etf_research_retrievals (run_id)",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_retrievals_subject ON etf_research_retrievals (stage, subject)",

    // ── etf_research_macro ───────────────────────────────────────────────────
    "CREATE TABLE etf_research_macro_v054_copy AS SELECT * FROM etf_research_macro",
    "DROP TABLE etf_research_macro",
    "CREATE TABLE etf_research_macro ("
    "  source             TEXT NOT NULL CHECK (source IN ('fred','world_bank','cftc')),"
    "  series_id          TEXT NOT NULL,"
    "  area               TEXT NOT NULL DEFAULT '',"
    "  obs_date           TEXT NOT NULL,"
    "  revision           INTEGER NOT NULL CHECK (revision >= 1),"
    "  value              REAL,"
    "  first_seen_at      TEXT NOT NULL,"
    "  last_seen_at       TEXT NOT NULL,"
    "  first_retrieval_id INTEGER NOT NULL REFERENCES etf_research_retrievals (retrieval_id),"
    "  last_retrieval_id  INTEGER NOT NULL REFERENCES etf_research_retrievals (retrieval_id),"
    "  seen_count         INTEGER NOT NULL DEFAULT 1 CHECK (seen_count >= 1),"
    "  PRIMARY KEY (source, series_id, area, obs_date, revision)"
    ") WITHOUT ROWID",
    "INSERT INTO etf_research_macro SELECT source, series_id, area, obs_date, revision, value, first_seen_at, "
    "last_seen_at, first_retrieval_id, last_retrieval_id, seen_count FROM etf_research_macro_v054_copy",
    "DROP TABLE etf_research_macro_v054_copy",
};

Result<void> apply_v054(QSqlDatabase& db) {
    for (const char* stmt : kV054Statements) {
        auto r = sql_v054(db, stmt);
        if (r.is_err())
            return r;
    }
    // Every reference must still resolve before the runner commits.
    QSqlQuery fk(db);
    if (!fk.exec(QStringLiteral("PRAGMA foreign_key_check")))
        return Result<void>::err(fk.lastError().text().toStdString());
    if (fk.next())
        return Result<void>::err("v054: foreign_key_check reports a dangling reference in " +
                                 fk.value(0).toString().toStdString());
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v054() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({54, "etf_research_cftc", apply_v054});
}

} // namespace fincept
