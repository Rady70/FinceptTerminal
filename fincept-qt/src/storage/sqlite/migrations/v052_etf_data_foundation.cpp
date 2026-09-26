// v052_etf_data_foundation — ETF Capital Flows, Batch B: the normalized ETF
// data foundation (control repository docs/ETF_FLOW_BATCH_B_IMPLEMENTATION.md).
//
// Adds NEW tables only. No existing table, index or row is touched, so the
// migration is safe on a populated database; MigrationRunner takes its usual
// pre-migration backup (<db>.pre-v52.bak) before applying it.
//
// The schema stores what Batch A2 section 10 requires without collapsing it:
//
//   etf_retrievals          every request, and every refused request, with its
//                           source, acquisition mode, route policy, exact
//                           request/response times, response SHA-256 and status
//   etf_retrieval_issues    what a retrieval found wrong: a missing expected
//                           session, an in-progress bar, an identity mismatch
//   etf_reporting_entities  SEC reporting identity: CIK + series ('' for a
//                           registrant that reports without a series, a UIT)
//   etf_sec_filings         N-PORT filing identity: accession, form, exact
//                           acceptance time, the accession an amendment amends
//   etf_listed_instruments  IBKR instrument identity: conId (ticker is only an
//   etf_instrument_symbols  attribute, with its own history)
//   etf_identity_links      listed instrument <-> SEC entity, with a stated
//                           relationship and basis
//   etf_observation_starts  when MarketLab began observing a subject per source
//   etf_market_sessions     the session calendar used for stored bars
//   etf_observations        one row per source vintage of one value
//
// The CHECK constraints are the database's half of the fail-closed rules: only
// enabled routes may hold observations, calculated and proxy kinds are not
// source facts, a missing value is never a number, market backfill is never
// "observed", an SEC value always names its accession and acceptance time, and
// a value without an availability time is always not_point_in_time.
//
// Historical migrations are never edited; later changes are new versions.

#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QSqlError>
#include <QSqlQuery>

namespace fincept {
namespace {

// Uniquely named (not a bare `sql`): unity builds can concatenate migration
// TUs, and two anonymous-namespace helpers sharing a name in one batch collide.
Result<void> sql_v052(QSqlDatabase& db, const char* stmt) {
    QSqlQuery q(db);
    if (!q.exec(QString::fromUtf8(stmt)))
        return Result<void>::err(q.lastError().text().toStdString());
    return Result<void>::ok();
}

const char* const kV052Statements[] = {
    // ── Retrievals ───────────────────────────────────────────────────────────
    "CREATE TABLE IF NOT EXISTS etf_retrievals ("
    "  retrieval_id     INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  run_id           TEXT NOT NULL DEFAULT '',"
    "  source_type      TEXT NOT NULL CHECK (source_type IN"
    "                     ('sec_nport','sec_submissions','sec_periodic_xbrl','issuer_file','ibkr_tws_readonly')),"
    "  acquisition_mode TEXT NOT NULL CHECK (acquisition_mode IN"
    "                     ('regulatory_api','ibkr_readonly_wrapper','user_initiated_import','issuer_automated')),"
    "  endpoint         TEXT NOT NULL,"
    "  request_ref      TEXT NOT NULL,"
    "  subject_ref      TEXT NOT NULL DEFAULT '',"
    "  requested_at     TEXT NOT NULL,"
    "  retrieved_at     TEXT,"
    "  status           TEXT NOT NULL CHECK (status IN"
    "                     ('OK','STALE','SOURCE_ERROR','NOT_ENTITLED','ROUTE_DISABLED','NOT_CONFIGURED')),"
    "  detail_code      TEXT NOT NULL DEFAULT '',"
    "  detail           TEXT NOT NULL DEFAULT '',"
    "  http_status      INTEGER,"
    "  response_sha256  TEXT,"
    "  response_bytes   INTEGER,"
    "  interpretation   TEXT NOT NULL,"
    "  route_policy     TEXT NOT NULL,"
    "  calendar_version TEXT NOT NULL DEFAULT '',"
    "  runtime_identity TEXT NOT NULL DEFAULT '',"
    // A refused request made no request: it has no response of any kind.
    "  CHECK (status NOT IN ('ROUTE_DISABLED','NOT_CONFIGURED') OR"
    "         (http_status IS NULL AND response_sha256 IS NULL AND response_bytes IS NULL AND retrieved_at IS NULL))"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_retrievals_run ON etf_retrievals (run_id)",

    "CREATE TABLE IF NOT EXISTS etf_retrieval_issues ("
    "  issue_id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  retrieval_id   INTEGER NOT NULL REFERENCES etf_retrievals (retrieval_id),"
    "  subject_type   TEXT NOT NULL DEFAULT '' CHECK (subject_type IN ('','reporting_entity','listed_instrument')),"
    "  subject_id     INTEGER,"
    "  effective_date TEXT,"
    "  state          TEXT NOT NULL CHECK (state IN ('MISSING','STALE','IN_PROGRESS_SESSION','SOURCE_ERROR',"
    "                   'NOT_APPLICABLE','NOT_ENTITLED','ROUTE_DISABLED','RECONCILIATION_EXCEPTION')),"
    "  code           TEXT NOT NULL,"
    "  detail         TEXT NOT NULL DEFAULT ''"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_issues_retrieval ON etf_retrieval_issues (retrieval_id)",
    "CREATE INDEX IF NOT EXISTS idx_etf_issues_subject ON etf_retrieval_issues (subject_type, subject_id, "
    "effective_date)",

    // ── SEC identity and filings ─────────────────────────────────────────────
    "CREATE TABLE IF NOT EXISTS etf_reporting_entities ("
    "  entity_id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  cik             TEXT NOT NULL CHECK (length(cik) = 10),"
    "  series_id       TEXT NOT NULL DEFAULT '',"
    "  reporting_level TEXT NOT NULL CHECK (reporting_level IN ('series','registrant')),"
    "  registrant_name TEXT NOT NULL DEFAULT '',"
    "  series_name     TEXT NOT NULL DEFAULT '',"
    "  reg_file_number TEXT NOT NULL DEFAULT '',"
    "  registrant_lei  TEXT NOT NULL DEFAULT '',"
    "  series_lei      TEXT NOT NULL DEFAULT '',"
    "  first_seen_at   TEXT NOT NULL,"
    "  last_seen_at    TEXT NOT NULL,"
    "  UNIQUE (cik, series_id),"
    "  CHECK ((series_id = '' AND reporting_level = 'registrant') OR"
    "         (series_id <> '' AND reporting_level = 'series'))"
    ")",

    "CREATE TABLE IF NOT EXISTS etf_sec_filings ("
    "  filing_id             INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  accession             TEXT NOT NULL UNIQUE,"
    "  filer_cik             TEXT NOT NULL CHECK (length(filer_cik) = 10),"
    "  form                  TEXT NOT NULL CHECK (form IN ('NPORT-P','NPORT-P/A')),"
    "  filing_date           TEXT NOT NULL,"
    "  report_date           TEXT NOT NULL DEFAULT '',"
    "  accepted_at           TEXT NOT NULL,"
    "  primary_document      TEXT NOT NULL DEFAULT '',"
    "  entity_id             INTEGER NOT NULL REFERENCES etf_reporting_entities (entity_id),"
    "  amends_accession      TEXT NOT NULL DEFAULT '',"
    "  rep_pd_date           TEXT NOT NULL,"
    "  rep_pd_end            TEXT NOT NULL DEFAULT '',"
    "  is_final_filing       TEXT NOT NULL DEFAULT '',"
    "  returns_block_present INTEGER NOT NULL CHECK (returns_block_present IN (0,1)),"
    "  class_ids             TEXT NOT NULL DEFAULT '',"
    "  document_sha256       TEXT NOT NULL,"
    "  first_retrieval_id    INTEGER NOT NULL REFERENCES etf_retrievals (retrieval_id),"
    "  last_retrieval_id     INTEGER NOT NULL REFERENCES etf_retrievals (retrieval_id),"
    "  first_seen_at         TEXT NOT NULL,"
    "  last_seen_at          TEXT NOT NULL,"
    "  CHECK (form = 'NPORT-P/A' OR amends_accession = '')"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_filings_entity ON etf_sec_filings (entity_id, rep_pd_date)",

    // ── IBKR identity ────────────────────────────────────────────────────────
    "CREATE TABLE IF NOT EXISTS etf_listed_instruments ("
    "  instrument_id    INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  ibkr_con_id      INTEGER NOT NULL UNIQUE CHECK (ibkr_con_id > 0),"
    "  symbol           TEXT NOT NULL,"
    "  security_type    TEXT NOT NULL,"
    "  exchange         TEXT NOT NULL DEFAULT '',"
    "  primary_exchange TEXT NOT NULL DEFAULT '',"
    "  currency         TEXT NOT NULL,"
    "  first_seen_at    TEXT NOT NULL,"
    "  last_seen_at     TEXT NOT NULL"
    ")",
    "CREATE TABLE IF NOT EXISTS etf_instrument_symbols ("
    "  instrument_id INTEGER NOT NULL REFERENCES etf_listed_instruments (instrument_id),"
    "  symbol        TEXT NOT NULL,"
    "  first_seen_at TEXT NOT NULL,"
    "  last_seen_at  TEXT NOT NULL,"
    "  PRIMARY KEY (instrument_id, symbol)"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_instrument_symbols_symbol ON etf_instrument_symbols (symbol)",

    "CREATE TABLE IF NOT EXISTS etf_identity_links ("
    "  link_id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  instrument_id INTEGER NOT NULL REFERENCES etf_listed_instruments (instrument_id),"
    "  entity_id     INTEGER NOT NULL REFERENCES etf_reporting_entities (entity_id),"
    "  class_id      TEXT NOT NULL DEFAULT '',"
    "  relationship  TEXT NOT NULL CHECK (relationship IN"
    "                  ('registrant_is_instrument','sole_class_of_series','class_of_multi_class_series')),"
    "  link_basis    TEXT NOT NULL CHECK (length(link_basis) > 0),"
    "  declared_at   TEXT NOT NULL,"
    "  UNIQUE (instrument_id, entity_id, class_id)"
    ")",

    "CREATE TABLE IF NOT EXISTS etf_observation_starts ("
    "  source_type       TEXT NOT NULL CHECK (source_type IN ('sec_nport','ibkr_tws_readonly')),"
    "  subject_type      TEXT NOT NULL CHECK (subject_type IN ('reporting_entity','listed_instrument')),"
    "  subject_id        INTEGER NOT NULL,"
    "  observation_start TEXT NOT NULL,"
    "  retrieval_id      INTEGER NOT NULL REFERENCES etf_retrievals (retrieval_id),"
    "  PRIMARY KEY (source_type, subject_type, subject_id)"
    ")",

    // ── Session calendar used by stored bars ─────────────────────────────────
    "CREATE TABLE IF NOT EXISTS etf_market_sessions ("
    "  calendar_id      TEXT NOT NULL,"
    "  calendar_version TEXT NOT NULL,"
    "  session_date     TEXT NOT NULL,"
    "  day_type         TEXT NOT NULL CHECK (day_type IN ('regular','early_close','holiday')),"
    "  open_local       TEXT,"
    "  close_local      TEXT,"
    "  open_utc         TEXT,"
    "  close_utc        TEXT,"
    "  is_early_close   INTEGER NOT NULL CHECK (is_early_close IN (0,1)),"
    "  PRIMARY KEY (calendar_id, calendar_version, session_date),"
    "  CHECK ((day_type = 'holiday') = (open_utc IS NULL)),"
    "  CHECK ((day_type = 'holiday') = (close_utc IS NULL)),"
    "  CHECK ((day_type = 'early_close') = (is_early_close = 1))"
    ")",

    // ── Observation vintages ─────────────────────────────────────────────────
    "CREATE TABLE IF NOT EXISTS etf_observations ("
    "  observation_id       INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  subject_type         TEXT NOT NULL CHECK (subject_type IN ('reporting_entity','listed_instrument')),"
    "  subject_id           INTEGER NOT NULL,"
    "  measurement_kind     TEXT NOT NULL CHECK (measurement_kind IN"
    "                         ('regulatory_reported_flow','accounting_observation','aum_observation',"
    "                          'market_bar','reference')),"
    "  measure              TEXT NOT NULL,"
    "  units                TEXT NOT NULL,"
    "  basis                TEXT NOT NULL DEFAULT '',"
    "  source_type          TEXT NOT NULL CHECK (source_type IN ('sec_nport','ibkr_tws_readonly')),"
    "  acquisition_mode     TEXT NOT NULL CHECK (acquisition_mode IN ('regulatory_api','ibkr_readonly_wrapper')),"
    "  source_document      TEXT NOT NULL DEFAULT '',"
    "  filing_id            INTEGER REFERENCES etf_sec_filings (filing_id),"
    "  effective_date       TEXT NOT NULL,"
    "  period_start         TEXT,"
    "  period_end           TEXT,"
    "  report_period        TEXT,"
    "  accepted_at          TEXT,"
    "  value                REAL,"
    "  value_state          TEXT NOT NULL CHECK (value_state IN ('reported','missing','unparseable')),"
    "  raw_text             TEXT,"
    "  source_revision      INTEGER NOT NULL CHECK (source_revision >= 1),"
    "  revision_state       TEXT NOT NULL CHECK (revision_state IN"
    "                         ('original','revised','amended_filing','split_restated')),"
    "  history_type         TEXT NOT NULL CHECK (history_type IN"
    "                         ('forward_observed','regulatory_filing','market_backfill','revised_backfill')),"
    "  point_in_time_status TEXT NOT NULL CHECK (point_in_time_status IN"
    "                         ('observed','conservative_rule','historical_assumption','not_point_in_time')),"
    "  availability_basis   TEXT NOT NULL CHECK (availability_basis IN ('recorded_first_seen',"
    "                         'recorded_first_seen_and_ibkr_next_session_v1',"
    "                         'sec_next_session_after_acceptance_v1','session_close_assumption','none')),"
    "  available_from       TEXT,"
    "  first_seen_at        TEXT NOT NULL,"
    "  last_seen_at         TEXT NOT NULL,"
    "  first_retrieval_id   INTEGER NOT NULL REFERENCES etf_retrievals (retrieval_id),"
    "  last_retrieval_id    INTEGER NOT NULL REFERENCES etf_retrievals (retrieval_id),"
    "  seen_count           INTEGER NOT NULL DEFAULT 1 CHECK (seen_count >= 1),"
    "  UNIQUE (subject_type, subject_id, measure, effective_date, source_type, source_document, source_revision),"
    // Unknown is not zero, and a number is never unknown.
    "  CHECK ((value_state = 'reported') = (value IS NOT NULL)),"
    // No availability time means not point in time, and only that.
    "  CHECK ((point_in_time_status = 'not_point_in_time') = (available_from IS NULL)),"
    "  CHECK ((availability_basis = 'none') = (available_from IS NULL)),"
    // Source identity: SEC facts describe a reporting entity and name their
    // filing; IBKR bars describe a listed instrument and have no document.
    "  CHECK ((source_type = 'sec_nport' AND acquisition_mode = 'regulatory_api' AND"
    "          subject_type = 'reporting_entity' AND source_document <> '' AND accepted_at IS NOT NULL AND"
    "          filing_id IS NOT NULL) OR"
    "         (source_type = 'ibkr_tws_readonly' AND acquisition_mode = 'ibkr_readonly_wrapper' AND"
    "          subject_type = 'listed_instrument' AND source_document = '' AND accepted_at IS NULL AND"
    "          filing_id IS NULL)),"
    "  CHECK (measurement_kind <> 'regulatory_reported_flow' OR source_type = 'sec_nport'),"
    "  CHECK (measurement_kind <> 'market_bar' OR source_type = 'ibkr_tws_readonly'),"
    // History classes per source (A2 section 5.2); backfill is never observed.
    "  CHECK (source_type <> 'sec_nport' OR history_type IN ('forward_observed','regulatory_filing')),"
    "  CHECK (source_type <> 'ibkr_tws_readonly' OR history_type IN ('forward_observed','market_backfill')),"
    "  CHECK (history_type <> 'market_backfill' OR"
    "         point_in_time_status IN ('historical_assumption','not_point_in_time')),"
    "  CHECK (history_type <> 'regulatory_filing' OR"
    "         point_in_time_status IN ('conservative_rule','not_point_in_time')),"
    "  CHECK (history_type <> 'forward_observed' OR point_in_time_status IN ('observed','not_point_in_time'))"
    ")",
    "CREATE INDEX IF NOT EXISTS idx_etf_observations_key ON etf_observations"
    " (subject_type, subject_id, measure, effective_date)",
    "CREATE INDEX IF NOT EXISTS idx_etf_observations_first_retrieval ON etf_observations (first_retrieval_id)",
};

Result<void> apply_v052(QSqlDatabase& db) {
    for (const char* stmt : kV052Statements) {
        auto r = sql_v052(db, stmt);
        if (r.is_err())
            return r;
    }
    return Result<void>::ok();
}

} // anonymous namespace

void register_migration_v052() {
    static bool done = false;
    if (done)
        return;
    done = true;
    MigrationRunner::register_migration({52, "etf_data_foundation", apply_v052});
}

} // namespace fincept
