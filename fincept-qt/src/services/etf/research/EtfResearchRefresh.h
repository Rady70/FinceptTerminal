// src/services/etf/research/EtfResearchRefresh.h
//
// The manual-refresh pipeline, separated from its transports so it can be
// exercised with injected stages:
//
//   begin_run → fetch (Yahoo/FRED/World Bank payload) → persist, one
//   transaction per source stage, off the UI thread → IBKR stage → SEC stage →
//   finish_run (one status per stage, summary stored)
//
// Source stages are independent: a failed fetch marks every network stage
// FAILED, and a stage whose persistence fails is recorded FAILED while the
// other stages are still persisted; in both cases the IBKR and SEC stages still
// run. Earlier observations stay stored and are never reported as this run's
// result. Nothing here schedules anything.
#pragma once
#include "core/result/Result.h"
#include "services/etf/research/EtfResearchInputs.h"
#include "services/etf/research/EtfResearchRefreshTypes.h"
#include "storage/repositories/EtfResearchRepository.h"

#include <QDateTime>
#include <QJsonObject>
#include <QRandomGenerator>

#include <algorithm>
#include <functional>
#include <memory>

namespace fincept::services::etf::research {

/// Filings to request for one reporting entity so a manual refresh catches up
/// on every month missed since the latest stored report period: N-PORT is a
/// monthly report, so the months behind plus three (amendments, the quarter
/// in flight), at least four, at most the route's per-run maximum (40). With
/// nothing stored, the maximum.
inline int sec_filings_to_request(const QDate& latest_report_period, const QDate& today, int route_max = 40) {
    if (!latest_report_period.isValid())
        return route_max;
    const int behind = (today.year() - latest_report_period.year()) * 12 + today.month() - latest_report_period.month();
    return std::clamp(behind + 3, 4, route_max);
}

/// Whether a symbol's stored history may be refreshed incrementally: its latest
/// coverage window is recent and continuous, i.e. it reaches back over the stored
/// history (a symbol younger than the requested depth) or over the requested
/// depth (a rolling window). A restated history leaves a window of a few weeks,
/// which is neither, so the next request fetches the full history.
inline bool history_incremental_eligible(const etf_research_store::HistoryCoverage& c, const QDate& today,
                                         int required_days) {
    if (!c.window_first.isValid() || !c.window_last.isValid() || c.window_last < today.addDays(-30))
        return false;
    const bool covers_stored = c.earliest_stored.isValid() && c.window_first <= c.earliest_stored.addDays(30);
    const bool covers_depth = c.window_first.daysTo(c.window_last) >= required_days;
    return covers_stored || covers_depth;
}

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
                                 std::function<void(const RefreshResult&)> done, RefreshExecutor executor = {}) {
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
    fetch(request, report, [result, finish, run_extra, expected_session, executor](Result<QJsonObject> payload) {
        if (payload.is_err()) {
            for (const QString& st : {QStringLiteral("yahoo_history"), QStringLiteral("yahoo_constituent_history"),
                                      QStringLiteral("yahoo_funds"), QStringLiteral("yahoo_fundamentals"),
                                      QStringLiteral("fred"), QStringLiteral("world_bank"), QStringLiteral("cftc")}) {
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
        const QDate expected = expected_session();
        const QJsonObject pl = payload.value();
        auto persisted = std::make_shared<QVector<SourceStageStatus>>();
        auto errors = std::make_shared<QStringList>();
        const QString run_id = result->run_id;
        const QDateTime started = result->started;
        // One transaction per source stage: a stage that cannot be stored is
        // recorded FAILED and the others are still persisted.
        auto work = [pl, expected, persisted, errors, run_id, started]() {
            auto& repo = EtfResearchRepository::instance();
            const QJsonObject stages = pl.value(QStringLiteral("stages")).toObject();
            for (const QString& st : {QStringLiteral("yahoo_history"), QStringLiteral("yahoo_constituent_history"),
                                      QStringLiteral("yahoo_funds"), QStringLiteral("yahoo_fundamentals"),
                                      QStringLiteral("fred"), QStringLiteral("world_bank"), QStringLiteral("cftc")}) {
                if (!stages.contains(st))
                    continue;
                QJsonObject one = pl;
                one.insert(QStringLiteral("stages"), QJsonObject{{st, stages.value(st)}});
                auto p = repo.persist_payload(run_id, one, expected);
                if (p.is_ok()) {
                    *persisted += p.value();
                    continue;
                }
                SourceStageStatus s;
                s.stage = st;
                s.status = QStringLiteral("FAILED");
                s.detail = QStringLiteral("persistence failed: ") + QString::fromStdString(p.error());
                s.requested_at = started;
                repo.record_stage_retrieval(run_id, st, QStringLiteral("*"), started, QDateTime(),
                                            QStringLiteral("FAILED"), s.detail, 0);
                persisted->append(s);
                errors->append(st);
            }
        };
        auto then = [result, persisted, errors, run_extra]() {
            result->stages = *persisted;
            if (!errors->isEmpty())
                result->error = QStringLiteral("persistence failed for: ") + errors->join(QStringLiteral(", "));
            run_extra();
        };
        if (executor) {
            executor(std::move(work), std::move(then));
        } else {
            work();
            then();
        }
    });
}

} // namespace fincept::services::etf::research
