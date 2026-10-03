// src/services/etf/research/EtfResearchService.h
//
// The ETF Flow & Sector Rotation workspace's entry point:
//
//   * load(as_of, known_at)  — reads the stored observations (research tables,
//     Batch C regulatory flow for declared SEC identities, Batch B IBKR closes)
//     and nothing else. Opening the workspace calls only this. It never starts
//     an acquisition.
//   * refresh(...)           — the user-initiated "Refresh ETF Research Data":
//     one manual run that fetches the approved free sources through
//     scripts/etf_research_data.py (Yahoo via the shipped yfinance, FRED via the
//     FedWatch transport, World Bank), persists them append-only, then runs the
//     existing read-only IBKR completed-session route and the SEC N-PORT route
//     when (and only when) their local configuration exists, and records one
//     status per source: UPDATED, UNCHANGED, STALE, PARTIAL, FAILED,
//     UNAVAILABLE or NOT_CONFIGURED, with observation and retrieval times and
//     the calculations that depend on it.
//
// There is no timer, no polling and no background schedule anywhere here.
#pragma once
#include "core/result/Result.h"
#include "services/etf/research/EtfResearchInputs.h"
#include "services/etf/research/EtfResearchRefresh.h"
#include "services/etf/research/EtfResearchUniverse.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace fincept::services::etf::research {

class EtfResearchService : public QObject {
    Q_OBJECT
  public:
    static EtfResearchService& instance();

    /// The bundled universe (parsed once). nullptr with `error` set if invalid.
    const ResearchUniverse* universe(QString* error = nullptr);

    /// Latest NYSE session whose close (plus 30 minutes) is at or before `as_of`.
    static QDate expected_us_session(const QDateTime& as_of);

    /// Read-only. Safe on a worker thread (per-thread database connection).
    /// Stored inputs at (as_of, known_at). `with_groups` false defers the Batch D
    /// group research (most of the read time) to group_flows().
    Result<ResearchInputs> load(const QDateTime& as_of, const QDateTime& known_at, bool with_groups = true);
    /// Batch D measured group flows (asset class, category) for the latest 12
    /// complete months at as_of.
    Result<QVector<GroupFlowRow>> group_flows(const QDateTime& as_of, const QDateTime& known_at,
                                              QStringList* warnings = nullptr);

    /// The acquisition request the manual refresh sends to the fetch script.
    /// The fetch request. Incremental by default: a symbol whose stored history
    /// is continuous and recent fetches only its last weeks (the store joins
    /// them when consistent); `full` re-fetches every history at full depth.
    QJsonObject build_request(bool full = false) const;

    // ── Manual refresh ───────────────────────────────────────────────────────
    using Progress = RefreshProgress;
    using Done = std::function<void(const RefreshResult&)>;

    /// Fetch transport: run the fetch for `request` and deliver the payload
    /// (or an error). Default: scripts/etf_research_data.py via PythonRunner.
    using Fetcher = RefreshFetcher;
    /// Optional extra stages (IBKR, SEC). Each reports its own status.
    using StageRunner = RefreshStage;

    void refresh(const QString& trigger, Progress progress, Done done, bool full = false);
    bool refresh_running() const { return running_; }

    /// How many acquisitions this process has started (tests assert that
    /// opening the workspace starts none).
    int fetch_invocations() const { return fetch_invocations_; }

    void set_fetcher(Fetcher f) { fetcher_ = std::move(f); }
    void set_ibkr_stage(StageRunner r) { ibkr_stage_ = std::move(r); }
    void set_sec_stage(StageRunner r) { sec_stage_ = std::move(r); }

  signals:
    void refresh_started(const QString& run_id);
    void refresh_progress(const QString& stage, int done, int total);
    void refresh_finished(const fincept::services::etf::research::RefreshResult& result);

  private:
    explicit EtfResearchService(QObject* parent = nullptr);
    void default_fetch(const QJsonObject& request, Progress progress, std::function<void(Result<QJsonObject>)> done);
    void default_ibkr_stage(std::function<void(SourceStageStatus)> done);
    void default_sec_stage(std::function<void(SourceStageStatus)> done);

    std::optional<ResearchUniverse> universe_;
    QString universe_error_;
    bool running_ = false;
    int fetch_invocations_ = 0;
    Fetcher fetcher_;
    StageRunner ibkr_stage_;
    StageRunner sec_stage_;
};

} // namespace fincept::services::etf::research

Q_DECLARE_METATYPE(fincept::services::etf::research::RefreshResult)
