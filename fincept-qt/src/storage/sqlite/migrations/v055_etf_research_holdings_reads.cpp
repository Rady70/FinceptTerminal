// v055_etf_research_holdings_reads — MarketLab ETF Flow & Sector Rotation
// (consolidated Batch E): the outcome of each fund holdings read.
//
// Yahoo's quote summary and its fund holdings are separate reads of one
// yahoo_funds retrieval. Whether the holdings read succeeded, found none
// published, or failed was kept only inside the retrieval's free-text detail,
// so a reader had to guess it from the wording. This table records it as data,
// one row per yahoo_funds retrieval that attempted a holdings read:
//
//   etf_research_holdings_reads  retrieval_id, symbol, status, detail
//
// status: OK (holdings or sector weights read), UNAVAILABLE (read, Yahoo
// published none), FAILED (the read failed; detail says why). Retrievals stored
// before this version have no row: their holdings outcome is unknown, never
// guessed.
//
// Historical migrations are never edited; later changes are new versions.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

// Uniquely named for unity builds (see v052).
Result<void> sql_v055(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(QString::fromUtf8(stmt)))
        return Result<void>::err(std::string(stmt).substr(0, 60) + ": " + q.lastError().text().toStdString());
    return Result<void>::ok();
}

const char* const kV055Statements[] = {
    "CREATE TABLE IF NOT EXISTS etf_research_holdings_reads ("
    "  retrieval_id INTEGER PRIMARY KEY REFERENCES etf_research_retrievals (retrieval_id),"
    "  symbol       TEXT NOT NULL,"
    "  status       TEXT NOT NULL CHECK (status IN ('OK','UNAVAILABLE','FAILED')),"
    "  detail       TEXT NOT NULL DEFAULT ''"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_research_holdings_reads_symbol ON etf_research_holdings_reads (symbol)",
};

Result<void> apply_v055(QSqlDatabase& db) {
    for (const char* stmt : kV055Statements) {
        auto r = sql_v055(db, stmt);
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v055() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({55, "etf_research_holdings_reads", apply_v055});
}

} // namespace fincept
