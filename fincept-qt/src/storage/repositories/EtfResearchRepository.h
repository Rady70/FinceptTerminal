#pragma once
// EtfResearchRepository — persistence of the ETF Flow & Sector Rotation
// research sources (consolidated Batch E; schema in migration
// v053_etf_research_sources).
//
// Writes are append-only for source values, with the Batch B vintage rule: a
// later retrieval that delivers the same value confirms the stored vintage
// (last_seen_at, seen_count); a different value becomes a new revision and the
// earlier one is kept unchanged. Nothing here deletes or rewrites a stored
// observation. A failed or unavailable item is recorded as a retrieval with
// its status and no rows; it never removes or replaces earlier observations,
// and an earlier observation is never presented as this run's result.
//
// Reads take a frame (as_of, known_at): only what was recorded by known_at
// exists, and for daily history only the session window of the latest history
// retrieval at known_at is used (one consistent fetch window per symbol, so a
// split restatement can never mix with older, unrestated rows).
//
// Derived research values are not stored.

#include "core/result/Result.h"
#include "services/etf/research/EtfResearchInputs.h"
#include "storage/repositories/BaseRepository.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace fincept {

namespace etf_research_store {

/// What one persisted stage produced.
struct StageOutcome {
    services::etf::research::SourceStageStatus status;
};

} // namespace etf_research_store

class EtfResearchRepository : public BaseRepository<services::etf::research::DailyBar> {
  public:
    static EtfResearchRepository& instance();

    Result<void> begin_run(const QString& run_id, const QString& trigger, const QDateTime& started_at,
                           const QString& universe_version);
    Result<void> finish_run(const QString& run_id, const QString& status, const QDateTime& finished_at,
                            const QString& script_version,
                            const QVector<services::etf::research::SourceStageStatus>& stages);

    /// Persist one fetch payload (scripts/etf_research_data.py) inside one
    /// transaction. `expected_us_session` decides the history stage's STALE
    /// state. Returns one status per stage present in the payload.
    Result<QVector<services::etf::research::SourceStageStatus>>
    persist_payload(const QString& run_id, const QJsonObject& payload, const QDate& expected_us_session);

    /// Record a stage that made no request (not configured / unavailable) or
    /// was run by another subsystem (IBKR, SEC), with its summary.
    Result<qint64> record_stage_retrieval(const QString& run_id, const QString& stage, const QString& subject,
                                          const QDateTime& requested_at, const QDateTime& retrieved_at,
                                          const QString& status, const QString& detail, int rows);

    /// Everything the research engine reads, for the frame. Measured SEC flow
    /// and IBKR closes are added by the service (they live in the v052 store).
    Result<services::etf::research::ResearchInputs>
    load_inputs(const services::etf::research::ResearchUniverse& universe, const QDateTime& as_of,
                const QDateTime& known_at);

    /// The last completed run recorded by known_at, or an empty id.
    Result<QJsonObject> last_run(const QDateTime& known_at);

    /// Every research table, every row, in key order, as JSON (restart / replay checks).
    Result<QJsonObject> export_all();

    /// Row counts per research table.
    Result<QJsonObject> table_counts();

    /// The rule assigning an effective NYSE session to an undated snapshot:
    /// the last NYSE session completed before the capture's New York date.
    static QDate prior_completed_session(const QDateTime& captured_at);

  private:
    EtfResearchRepository() = default;
};

} // namespace fincept
