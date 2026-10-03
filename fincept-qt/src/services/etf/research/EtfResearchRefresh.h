// src/services/etf/research/EtfResearchRefresh.h
//
// The manual-refresh pipeline, separated from its transports so it can be
// exercised with injected stages:
//
//   begin_run → fetch (Yahoo/FRED/World Bank payload) → persist append-only →
//   IBKR stage → SEC stage → finish_run (one status per stage, summary stored)
//
// A failed fetch marks every network stage FAILED for this run and still runs
// the IBKR and SEC stages; earlier observations stay stored and are never
// reported as this run's result. Nothing here schedules anything.
#pragma once
#include "core/result/Result.h"
#include "services/etf/research/EtfResearchInputs.h"
#include "services/etf/research/EtfResearchRefreshTypes.h"
#include "storage/repositories/EtfResearchRepository.h"

#include <QDateTime>
#include <QJsonObject>
#include <QRandomGenerator>

#include <functional>
#include <memory>

namespace fincept::services::etf::research {

inline QString refresh_retrieval_status(const QString& stage_status) {
    if (stage_status == QLatin1String("NOT_CONFIGURED") || stage_status == QLatin1String("UNAVAILABLE") ||
        stage_status == QLatin1String("FAILED") || stage_status == QLatin1String("PARTIAL"))
        return stage_status;
    return QStringLiteral("OK");
}

/// Run one manual refresh. `expected_session` gives the latest completed NYSE
/// session at persist time (for the STALE rule).
inline void run_refresh_pipeline(const QString& trigger, const QString& universe_version, const QJsonObject& request,
                                 RefreshFetcher fetch, RefreshStage ibkr, RefreshStage sec,
                                 std::function<QDate()> expected_session, RefreshProgress progress,
                                 std::function<void(const RefreshResult&)> done) {
    auto result = std::make_shared<RefreshResult>();
    result->started = QDateTime::currentDateTimeUtc();
    result->run_id = QStringLiteral("etfr-%1-%2")
                         .arg(result->started.toString(QStringLiteral("yyyyMMddTHHmmsszzz")))
                         .arg(QRandomGenerator::global()->generate() & 0xffff, 4, 16, QLatin1Char('0'));
    auto& repo = EtfResearchRepository::instance();
    if (auto b = repo.begin_run(result->run_id, trigger, result->started, universe_version); b.is_err()) {
        result->error = QString::fromStdString(b.error());
        done(*result);
        return;
    }
    auto report = [progress](const QString& s, int d, int t) {
        if (progress)
            progress(s, d, t);
    };
    auto finish = [result, done](const QString& status) {
        result->finished = QDateTime::currentDateTimeUtc();
        auto f = EtfResearchRepository::instance().finish_run(result->run_id, status, result->finished,
                                                              result->script_version, result->stages);
        if (f.is_err() && result->error.isEmpty())
            result->error = QString::fromStdString(f.error());
        result->ok = status == QLatin1String("COMPLETED") && f.is_ok();
        done(*result);
    };
    auto record = [result](const SourceStageStatus& s, const QString& subject) {
        EtfResearchRepository::instance().record_stage_retrieval(
            result->run_id, s.stage, subject, s.requested_at.isValid() ? s.requested_at : result->started,
            s.retrieved_at, refresh_retrieval_status(s.status), s.detail,
            s.rows_inserted + s.rows_revised + s.rows_confirmed);
        result->stages.append(s);
    };
    auto run_extra = [result, finish, record, report, ibkr, sec]() {
        report(QStringLiteral("ibkr_daily"), 0, 1);
        ibkr([result, finish, record, report, sec](SourceStageStatus s) {
            record(s, QStringLiteral("stored_listed_instruments"));
            report(QStringLiteral("sec_nport"), 0, 1);
            sec([finish, record](SourceStageStatus s2) {
                record(s2, QStringLiteral("stored_reporting_entities"));
                finish(QStringLiteral("COMPLETED"));
            });
        });
    };
    report(QStringLiteral("yahoo_history"), 0, 1);
    fetch(request, report, [result, finish, run_extra, expected_session](Result<QJsonObject> payload) {
        if (payload.is_err()) {
            for (const QString& st : {QStringLiteral("yahoo_history"), QStringLiteral("yahoo_constituent_history"),
                                      QStringLiteral("yahoo_funds"), QStringLiteral("yahoo_fundamentals"),
                                      QStringLiteral("fred"), QStringLiteral("world_bank")}) {
                SourceStageStatus s;
                s.stage = st;
                s.status = QStringLiteral("FAILED");
                s.detail = QString::fromStdString(payload.error());
                s.requested_at = result->started;
                EtfResearchRepository::instance().record_stage_retrieval(result->run_id, st, QStringLiteral("*"),
                                                                         result->started, QDateTime(),
                                                                         QStringLiteral("FAILED"), s.detail, 0);
                result->stages.append(s);
            }
            run_extra();
            return;
        }
        result->script_version = payload.value().value(QStringLiteral("script_version")).toString();
        auto persisted =
            EtfResearchRepository::instance().persist_payload(result->run_id, payload.value(), expected_session());
        if (persisted.is_err()) {
            result->error = QString::fromStdString(persisted.error());
            finish(QStringLiteral("FAILED"));
            return;
        }
        result->stages = persisted.value();
        run_extra();
    });
}

} // namespace fincept::services::etf::research
