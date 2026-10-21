// v056_etf_research_scheduled_runs — MarketLab daily update (2026-10-21): an
// ETF Flow & Sector Rotation refresh can now be started by the owner's daily
// update (the Windows scheduled task, or the open app at the same time), and
// the run records that truthfully as trigger 'scheduled'.
//
// v053's CHECK allowed only 'manual_ui' and 'manual_cli'. SQLite cannot alter
// a CHECK, so etf_research_runs is rebuilt with 'scheduled' added, every row
// preserved:
//
//   etf_research_runs  trigger IN ('manual_ui','manual_cli','scheduled')
//
// etf_research_runs is the parent of etf_research_retrievals (run_id); the
// migration runs inside the runner's transaction with deferred foreign keys
// and re-inserts every run with its own run_id before commit (the v054
// pattern).
//
// Historical migrations are never edited; later changes are new versions.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

// Uniquely named for unity builds (see v052).
Result<void> sql_v056(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(QString::fromUtf8(stmt)))
        return Result<void>::err(std::string(stmt).substr(0, 60) + ": " + q.lastError().text().toStdString());
    return Result<void>::ok();
}

const char* const kV056Statements[] = {
    "PRAGMA defer_foreign_keys = ON",
    "CREATE TABLE etf_research_runs_v056_copy AS SELECT * FROM etf_research_runs",
    "DROP TABLE etf_research_runs",
    "CREATE TABLE etf_research_runs ("
    "  run_id           TEXT PRIMARY KEY,"
    "  trigger          TEXT NOT NULL CHECK (trigger IN ('manual_ui','manual_cli','scheduled')),"
    "  started_at       TEXT NOT NULL,"
    "  finished_at      TEXT,"
    "  status           TEXT NOT NULL CHECK (status IN ('RUNNING','COMPLETED','FAILED')),"
    "  script_version   TEXT NOT NULL DEFAULT '',"
    "  universe_version TEXT NOT NULL,"
    "  summary_json     TEXT NOT NULL DEFAULT '{}',"
    "  CHECK ((status = 'RUNNING') = (finished_at IS NULL))"
    ")",
    ("INSERT INTO etf_research_runs (run_id, trigger, started_at, finished_at, status, script_version, "
     "universe_version, summary_json) SELECT run_id, trigger, started_at, finished_at, status, script_version, "
     "universe_version, summary_json FROM etf_research_runs_v056_copy"),
    "DROP TABLE etf_research_runs_v056_copy",
};

Result<void> apply_v056(QSqlDatabase& db) {
    for (const char* stmt : kV056Statements) {
        auto r = sql_v056(db, stmt);
        if (r.is_err())
            return r;
    }
    // Every reference must still resolve before the runner commits.
    QSqlQuery fk(db);
    if (!fk.exec(QStringLiteral("PRAGMA foreign_key_check")))
        return Result<void>::err(fk.lastError().text().toStdString());
    if (fk.next())
        return Result<void>::err("v056: foreign_key_check reports a dangling reference in " +
                                 fk.value(0).toString().toStdString());
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v056() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({56, "etf_research_scheduled_runs", apply_v056});
}

} // namespace fincept
