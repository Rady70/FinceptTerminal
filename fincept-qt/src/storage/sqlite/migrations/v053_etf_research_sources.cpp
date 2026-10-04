// v053_etf_research_sources — MarketLab ETF Flow & Sector Rotation
// (consolidated Batch E): source observations acquired by the user-initiated
// "Refresh ETF Research Data" run (control repository
// docs/ETF_FLOW_BATCH_E_IMPLEMENTATION.md).
//
// Adds NEW tables only; the Batch B v052 tables keep their own fail-closed
// constraints and are not touched. Derived research values are never stored:
// the workspace recomputes them from these observations for a (as_of,
// known_at) frame, like Batch C/D.
//
//   etf_research_runs            one row per manual refresh (trigger, times, summary)
//   etf_research_retrievals      one row per stage x subject request, with status,
//                                exact request/response times and response digest
//   etf_research_bars            daily bars per Yahoo symbol, append-only vintages
//   etf_research_bar_coverage    the session range each history retrieval returned,
//                                so a replay selects one consistent fetch window
//   etf_research_fund_snapshots  each manual capture of a fund's quote summary
//                                (AUM, NAV, reported shares), with the named rule
//                                that assigned its effective session
//   etf_research_holdings        top holdings per capture
//   etf_research_sector_weights  sector weights per capture
//   etf_research_fundamentals    constituent quote-summary valuation fields
//   etf_research_macro           FRED / World Bank points, append-only vintages
//   etf_research_macro_coverage  the range each macro retrieval returned
//
// CHECK constraints are the database's half of the rules: a bar has a positive
// close; an unsupplied volume is NULL (never 0); distribution and split columns
// are non-negative event amounts; a stored snapshot always names its rule; a
// refused/failed retrieval records no response digest.
//
// Historical migrations are never edited; later changes are new versions.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

// Uniquely named for unity builds (see v052).
Result<void> sql_v053(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(QString::fromUtf8(stmt)))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

const char* const kV053Statements[] = {
    "CREATE TABLE IF NOT EXISTS etf_research_runs ("
    "  run_id           TEXT PRIMARY KEY,"
    "  trigger          TEXT NOT NULL CHECK (trigger IN ('manual_ui','manual_cli')),"
    "  started_at       TEXT NOT NULL,"
    "  finished_at      TEXT,"
    "  status           TEXT NOT NULL CHECK (status IN ('RUNNING','COMPLETED','FAILED')),"
    "  script_version   TEXT NOT NULL DEFAULT '',"
    "  universe_version TEXT NOT NULL,"
    "  summary_json     TEXT NOT NULL DEFAULT '{}',"
    "  CHECK ((status = 'RUNNING') = (finished_at IS NULL))"
    ")",

    "CREATE TABLE IF NOT EXISTS etf_research_retrievals ("
    "  retrieval_id    INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  run_id          TEXT NOT NULL REFERENCES etf_research_runs (run_id),"
    "  stage           TEXT NOT NULL CHECK (stage IN ('yahoo_history','yahoo_funds','yahoo_constituent_history',"
    "                    'yahoo_fundamentals','fred','world_bank','ibkr_daily','sec_nport')),"
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
    "CREATE INDEX IF NOT EXISTS idx_etf_research_retrievals_run ON etf_research_retrievals (run_id)",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_retrievals_subject ON etf_research_retrievals (stage, subject)",

    "CREATE TABLE IF NOT EXISTS etf_research_bars ("
    "  symbol             TEXT NOT NULL,"
    "  session_date       TEXT NOT NULL,"
    "  revision           INTEGER NOT NULL CHECK (revision >= 1),"
    "  close              REAL NOT NULL CHECK (close > 0),"
    "  volume             REAL CHECK (volume IS NULL OR volume >= 0),"
    "  dividend           REAL NOT NULL CHECK (dividend >= 0),"
    "  capital_gain       REAL NOT NULL CHECK (capital_gain >= 0),"
    "  split_ratio        REAL NOT NULL CHECK (split_ratio >= 0),"
    "  source             TEXT NOT NULL CHECK (source = 'yahoo_chart'),"
    "  first_seen_at      TEXT NOT NULL,"
    "  last_seen_at       TEXT NOT NULL,"
    "  first_retrieval_id INTEGER NOT NULL REFERENCES etf_research_retrievals (retrieval_id),"
    "  last_retrieval_id  INTEGER NOT NULL REFERENCES etf_research_retrievals (retrieval_id),"
    "  seen_count         INTEGER NOT NULL DEFAULT 1 CHECK (seen_count >= 1),"
    "  PRIMARY KEY (symbol, session_date, revision)"
    ") WITHOUT ROWID",

    "CREATE TABLE IF NOT EXISTS etf_research_bar_coverage ("
    "  retrieval_id  INTEGER PRIMARY KEY REFERENCES etf_research_retrievals (retrieval_id),"
    "  symbol        TEXT NOT NULL,"
    "  first_session TEXT NOT NULL,"
    "  last_session  TEXT NOT NULL,"
    "  bars          INTEGER NOT NULL CHECK (bars > 0),"
    "  retrieved_at  TEXT NOT NULL,"
    "  in_progress_excluded INTEGER NOT NULL DEFAULT 0"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_bar_coverage_symbol ON etf_research_bar_coverage (symbol, "
    "retrieved_at)",

    "CREATE TABLE IF NOT EXISTS etf_research_fund_snapshots ("
    "  retrieval_id       INTEGER PRIMARY KEY REFERENCES etf_research_retrievals (retrieval_id),"
    "  symbol             TEXT NOT NULL,"
    "  captured_at        TEXT NOT NULL,"
    "  effective_session  TEXT,"
    "  effective_rule     TEXT NOT NULL CHECK (length(effective_rule) > 0),"
    "  total_assets       REAL,"
    "  nav                REAL,"
    "  previous_close     REAL,"
    "  shares_outstanding REAL,"
    "  market_time        TEXT,"
    "  fields_json        TEXT NOT NULL"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_fund_symbol ON etf_research_fund_snapshots (symbol, captured_at)",

    "CREATE TABLE IF NOT EXISTS etf_research_holdings ("
    "  retrieval_id   INTEGER NOT NULL REFERENCES etf_research_retrievals (retrieval_id),"
    "  symbol         TEXT NOT NULL,"
    "  rank           INTEGER NOT NULL CHECK (rank >= 1),"
    "  holding_symbol TEXT NOT NULL,"
    "  holding_name   TEXT NOT NULL DEFAULT '',"
    "  weight         REAL,"
    "  PRIMARY KEY (retrieval_id, rank)"
    ")",
    "CREATE TABLE IF NOT EXISTS etf_research_sector_weights ("
    "  retrieval_id INTEGER NOT NULL REFERENCES etf_research_retrievals (retrieval_id),"
    "  symbol       TEXT NOT NULL,"
    "  sector_key   TEXT NOT NULL,"
    "  weight       REAL NOT NULL,"
    "  PRIMARY KEY (retrieval_id, sector_key)"
    ")",
    "CREATE TABLE IF NOT EXISTS etf_research_fundamentals ("
    "  retrieval_id INTEGER PRIMARY KEY REFERENCES etf_research_retrievals (retrieval_id),"
    "  symbol       TEXT NOT NULL,"
    "  captured_at  TEXT NOT NULL,"
    "  fields_json  TEXT NOT NULL"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_fundamentals_symbol ON etf_research_fundamentals (symbol, "
    "captured_at)",

    "CREATE TABLE IF NOT EXISTS etf_research_macro ("
    "  source             TEXT NOT NULL CHECK (source IN ('fred','world_bank')),"
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
    "CREATE TABLE IF NOT EXISTS etf_research_macro_coverage ("
    "  retrieval_id   INTEGER NOT NULL REFERENCES etf_research_retrievals (retrieval_id),"
    "  source         TEXT NOT NULL,"
    "  series_id      TEXT NOT NULL,"
    "  area           TEXT NOT NULL DEFAULT '',"
    "  first_date     TEXT NOT NULL,"
    "  last_date      TEXT NOT NULL,"
    "  points         INTEGER NOT NULL CHECK (points > 0),"
    "  retrieved_at   TEXT NOT NULL,"
    "  source_updated TEXT NOT NULL DEFAULT '',"
    "  PRIMARY KEY (retrieval_id, area)"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_macro_coverage_key ON etf_research_macro_coverage "
    "(source, series_id, area, retrieved_at)",
};

Result<void> apply_v053(QSqlDatabase& db) {
    for (const char* stmt : kV053Statements) {
        auto r = sql_v053(db, stmt);
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v053() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({53, "etf_research_sources", apply_v053});
}

} // namespace fincept
