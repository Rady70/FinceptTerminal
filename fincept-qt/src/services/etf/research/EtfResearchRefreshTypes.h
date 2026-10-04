// src/services/etf/research/EtfResearchRefreshTypes.h
//
// Types shared by the manual-refresh pipeline, the service and the UI
// bindings (no storage dependency).
#pragma once
#include "core/result/Result.h"
#include "services/etf/research/EtfResearchInputs.h"

#include <QDateTime>
#include <QJsonObject>

#include <functional>

namespace fincept::services::etf::research {

struct RefreshResult {
    QString run_id;
    bool ok = false; ///< the run completed and its outcome is recorded (sources may still have failed)
    QString error;   ///< why the run itself could not complete
    QDateTime started;
    QDateTime finished;
    QVector<SourceStageStatus> stages;
    QString script_version;
};

using RefreshProgress = std::function<void(const QString& stage, int done, int total)>;
using RefreshFetcher = std::function<void(const QJsonObject& request, RefreshProgress progress,
                                          std::function<void(Result<QJsonObject>)> done)>;
using RefreshStage = std::function<void(std::function<void(SourceStageStatus)> done)>;
/// Runs `work` (the heavy store write) and then `then` on the caller's thread.
/// The application runs `work` on a worker thread; tests may run both inline.
using RefreshExecutor = std::function<void(std::function<void()> work, std::function<void()> then)>;

} // namespace fincept::services::etf::research
