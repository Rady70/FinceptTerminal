// src/services/agents/AgentService_Execution.cpp
//
// Hot path for running a single agent / team / multi-agent flow:
// run_agent, run_agent_streaming, route_query (intent routing), run_team,
// run_agent_structured, execute_routed_query, execute_multi_query.
//
// Part of the partial-class split of AgentService.cpp.

#include "auth/AuthManager.h"
#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/TopicPolicy.h"
#include "mcp/McpProvider.h"
#include "mcp/McpTypes.h"
#include "mcp/TerminalMcpBridge.h"
#include "python/PythonRunner.h"
#include "services/agents/AgentService.h"
#include "services/llm/LlmService.h"
#include "storage/cache/CacheManager.h"
#include "storage/repositories/LlmConfigRepository.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <QVariant>

#include <memory>

namespace fincept::services {

QString AgentService::run_agent(const QString& query, const QJsonObject& config) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService", QString("Running agent query [%1]: %2").arg(req_id.left(8), query.left(80)));

    QJsonObject params;
    params["query"] = query;

    QPointer<AgentService> self = this;
    run_python_stdin("run", params, config, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();

        if (r.success) {
            if (result.contains("response"))
                r.response = result["response"].toString();
            else if (result.contains("result"))
                r.response = result["result"].toString();
            else if (result.contains("data"))
                r.response = QJsonDocument(result["data"].toObject()).toJson(QJsonDocument::Indented);
            else
                r.response = QJsonDocument(result).toJson(QJsonDocument::Indented);
        } else {
            r.error = result["error"].toString("Agent execution failed");
        }

        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
    return req_id;
}

// ── Streaming agent execution ─────────────────────────────────────────────────

QString AgentService::run_agent_streaming(const QString& query, const QJsonObject& config) {
    // MarketLab (reduced AI scope): the Agents surface is disabled — no child
    // process is launched and the finagent_core entry point is never named.
    Q_UNUSED(query);
    Q_UNUSED(config);
    LOG_WARN("AgentService", "Agents are disabled in this build");
    AgentExecutionResult r;
    r.request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    r.success = false;
    r.error = "Agents are disabled in this build";
    emit agent_stream_done(r);
    publish_agent_result(r, /*final=*/true);
    return r.request_id;
}

// ── Query routing ────────────────────────────────────────────────────────────

QString AgentService::route_query(const QString& query) {
    LOG_INFO("AgentService", QString("Routing query: %1").arg(query.left(80)));

    QJsonObject params;
    params["query"] = query;

    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin("route_query", params, {}, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        RoutingResult r;
        r.request_id = req_id;
        r.success = ok && result["success"].toBool(ok);
        r.agent_id = result["agent_id"].toString();
        r.intent = result["intent"].toString();
        r.confidence = result["confidence"].toDouble();
        r.config = result["config"].toObject();

        QJsonArray kw = result["matched_keywords"].toArray();
        for (const auto& k : kw)
            r.matched_keywords.append(k.toString());

        emit self->routing_result(r);
        self->publish_routing_result(r);
    });
    return req_id;
}

// ── Team execution ───────────────────────────────────────────────────────────

QString AgentService::run_team(const QString& query, const QJsonObject& team_config) {
    // MarketLab (reduced AI scope): the Agents surface is disabled — no child
    // process is launched and the finagent_core entry point is never named.
    Q_UNUSED(query);
    Q_UNUSED(team_config);
    LOG_WARN("AgentService", "Agents are disabled in this build");
    AgentExecutionResult r;
    r.request_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    r.success = false;
    r.error = "Agents are disabled in this build";
    emit agent_stream_done(r);
    publish_agent_result(r, /*final=*/true);
    return r.request_id;
}

// ── Workflow execution ───────────────────────────────────────────────────────

QString AgentService::run_agent_structured(const QString& query, const QJsonObject& config,
                                           const QString& output_model) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    LOG_INFO("AgentService",
             QString("Running structured agent [%1] (%2): %3").arg(req_id.left(8), output_model, query.left(60)));
    QJsonObject params;
    params["query"] = query;
    params["output_model"] = output_model;

    QPointer<AgentService> self = this;
    run_python_stdin("run_structured", params, config, [self, req_id](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.request_id = req_id;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
    return req_id;
}

// ── Routed query execution ───────────────────────────────────────────────────

void AgentService::execute_routed_query(const QString& query, const QJsonObject& config, const QString& session_id) {
    LOG_INFO("AgentService", QString("Execute routed query: %1").arg(query.left(60)));
    QJsonObject params;
    params["query"] = query;
    if (!session_id.isEmpty())
        params["session_id"] = session_id;

    QPointer<AgentService> self = this;
    run_python_stdin("execute_query", params, config, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        AgentExecutionResult r;
        r.success = ok && result["success"].toBool(ok);
        r.execution_time_ms = result["execution_time_ms"].toInt();
        r.response = result.contains("response") ? result["response"].toString()
                                                 : QJsonDocument(result).toJson(QJsonDocument::Indented);
        r.error = result["error"].toString();
        emit self->agent_result(r);
        self->publish_agent_result(r, /*final=*/true);
    });
}

// ── Multi-query ──────────────────────────────────────────────────────────────

void AgentService::execute_multi_query(const QString& query, bool aggregate, const QJsonObject& config) {
    LOG_INFO("AgentService", QString("Multi-query: %1").arg(query.left(60)));
    QJsonObject params;
    params["query"] = query;
    params["aggregate"] = aggregate;

    QPointer<AgentService> self = this;
    run_python_stdin("execute_multi_query", params, config, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->multi_query_result(result);
        else
            emit self->error_occurred("multi_query", result["error"].toString());
    });
}

// ── Financial workflows (specialized) ────────────────────────────────────────

// ── Agentic Mode ─────────────────────────────────────────────────────────────
// Spawns a streaming Python subprocess that runs an AgenticRunner. Events
// arrive as AGENTIC_EVENT: <json> lines on stdout; each is forwarded to the
// `task_event` signal and DataHub topic `task:event:<task_id>`.

QString AgentService::run_agentic_streaming(const QString& action, const QJsonObject& params, const QJsonObject& config,
                                            const QString& known_task_id) {
    // MarketLab (reduced AI scope): the Agents surface is disabled — no child
    // process is launched and the finagent_core entry point is never named.
    Q_UNUSED(action);
    Q_UNUSED(params);
    Q_UNUSED(config);
    Q_UNUSED(known_task_id);
    LOG_WARN("AgentService", "Agents are disabled in this build");
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    publish_task_event(known_task_id,
                       QJsonObject{{"kind", "error"}, {"task_id", known_task_id},
                                   {"error", "Agents are disabled in this build"}});
    return req_id;
}

QString AgentService::start_task(const QString& query, const QJsonObject& config) {
    QJsonObject params;
    params["query"] = query;
    return run_agentic_streaming(QStringLiteral("agentic_start_task"), params, config);
}

QString AgentService::resume_task(const QString& task_id) {
    QJsonObject params;
    params["task_id"] = task_id;
    return run_agentic_streaming(QStringLiteral("agentic_resume_task"), params, {}, task_id);
}

QString AgentService::reply_to_question(const QString& task_id, const QString& answer) {
    QJsonObject params;
    params["task_id"] = task_id;
    params["answer"] = answer;
    return run_agentic_streaming(QStringLiteral("agentic_reply_question"), params, {}, task_id);
}

// ── Scheduled recurring tasks ────────────────────────────────────────────────

QString AgentService::schedule_create_task(const QString& name, const QString& query, const QString& schedule_expr,
                                           const QJsonObject& config, bool start_now) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["name"] = name;
    params["query"] = query;
    params["schedule"] = schedule_expr;
    params["schedule_config"] = config;
    params["start_now"] = start_now;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_create"), params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->schedule_created(result.value("schedule_id").toString());
        else
            emit self->error_occurred("schedule_create", result["error"].toString());
    });
    // Make sure the polling timer is on now that there might be schedules.
    ensure_schedule_timer();
    return req_id;
}

QString AgentService::schedule_list() {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_list"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->schedules_listed(result.value("schedules").toArray());
        else
            emit self->error_occurred("schedule_list", result["error"].toString());
    });
    return req_id;
}

QString AgentService::delete_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    // "delete_task" removes the record via the state manager; "agentic_cancel_task"
    // only sets a cooperative stop signal and leaves the row behind.
    run_python_stdin(QStringLiteral("delete_task"), params, {}, [](bool, QJsonObject) {});
    return req_id;
}

QString AgentService::schedule_delete(const QString& schedule_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["schedule_id"] = schedule_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_delete"), params, {}, [self, schedule_id](bool ok, QJsonObject) {
        if (!self || !ok)
            return;
        emit self->schedule_deleted(schedule_id);
    });
    return req_id;
}

QString AgentService::schedule_set_enabled(const QString& schedule_id, bool enabled) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["schedule_id"] = schedule_id;
    params["enabled"] = enabled;
    run_python_stdin(QStringLiteral("agentic_schedule_set_enabled"), params, {}, [](bool, QJsonObject) {});
    return req_id;
}

void AgentService::tick_schedules() {
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_schedule_tick"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        const QJsonArray due = result.value("due").toArray();
        for (const auto& v : due) {
            QJsonObject row = v.toObject();
            const QString sid = row.value("id").toString();
            const QString name = row.value("name").toString();
            const QString query = row.value("query").toString();
            const QJsonObject cfg = row.value("config").toObject();
            // Fire as a normal agentic task — streaming
            // events route through task:event:* like manual
            // starts. The schedule's id/name is forwarded so
            // UI can correlate the task with its trigger.
            const QString task_req = self->start_task(query, cfg);
            emit self->scheduled_task_fired(sid, name, task_req);
        }
    });
}

// ── Library inspection (Phase 3 management UI) ───────────────────────────────

QString AgentService::skills_list() {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_skills_list"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        emit self->skills_listed(result.value("skills").toArray());
    });
    return req_id;
}

QString AgentService::skill_delete(const QString& skill_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject p;
    p["skill_id"] = skill_id;
    run_python_stdin(QStringLiteral("agentic_skill_delete"), p, {}, [](bool, QJsonObject) {});
    return req_id;
}

QString AgentService::archival_list(const QString& user_id, const QString& type) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject p;
    if (!user_id.isEmpty())
        p["user_id"] = user_id;
    if (!type.isEmpty())
        p["type"] = type;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_memory_list"), p, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        emit self->archival_listed(result.value("memories").toArray());
    });
    return req_id;
}

QString AgentService::archival_delete(const QString& memory_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject p;
    p["memory_id"] = memory_id;
    run_python_stdin(QStringLiteral("agentic_memory_delete"), p, {}, [](bool, QJsonObject) {});
    return req_id;
}

QString AgentService::reflexion_list() {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_reflexion_list"), {}, {}, [self](bool ok, QJsonObject result) {
        if (!self || !ok)
            return;
        emit self->reflexion_listed(result.value("reflections").toArray());
    });
    return req_id;
}

void AgentService::ensure_schedule_timer() {
    if (schedule_timer_)
        return;
    auto* t = new QTimer(this);
    t->setInterval(30 * 1000); // 30s — finer than this is overkill at the minute granularity DSL
    QPointer<AgentService> self = this;
    QObject::connect(t, &QTimer::timeout, this, [self]() {
        if (self)
            self->tick_schedules();
    });
    t->start();
    schedule_timer_ = t;
    // Fire one immediate tick so a freshly-created `start_now=true` schedule
    // doesn't wait the full 30s.
    QMetaObject::invokeMethod(
        this,
        [self]() {
            if (self)
                self->tick_schedules();
        },
        Qt::QueuedConnection);
}

QString AgentService::pause_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_pause_task"), params, {},
                     [self, task_id, req_id](bool ok, QJsonObject result) {
                         if (!self || !ok)
                             return;
                         QJsonObject evt{{"kind", "control_ack"},
                                         {"task_id", task_id},
                                         {"request_id", req_id},
                                         {"signal", result.value("signal").toString("pause_requested")}};
                         emit self->task_event(task_id, evt);
                     });
    return req_id;
}

QString AgentService::cancel_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("agentic_cancel_task"), params, {},
                     [self, task_id, req_id](bool ok, QJsonObject result) {
                         if (!self || !ok)
                             return;
                         QJsonObject evt{{"kind", "control_ack"},
                                         {"task_id", task_id},
                                         {"request_id", req_id},
                                         {"signal", result.value("signal").toString("cancel_requested")}};
                         emit self->task_event(task_id, evt);
                     });
    return req_id;
}

QString AgentService::list_tasks(const QString& status_filter, int limit) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    if (!status_filter.isEmpty())
        params["status"] = status_filter;
    params["limit"] = limit;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("list_tasks"), params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->tasks_listed(result.value("tasks").toArray());
        else
            emit self->error_occurred("list_tasks", result["error"].toString());
    });
    return req_id;
}

QString AgentService::get_task(const QString& task_id) {
    const QString req_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject params;
    params["task_id"] = task_id;
    QPointer<AgentService> self = this;
    run_python_stdin(QStringLiteral("get_task"), params, {}, [self](bool ok, QJsonObject result) {
        if (!self)
            return;
        if (ok)
            emit self->task_loaded(result.value("task").toObject());
        else
            emit self->error_occurred("get_task", result["error"].toString());
    });
    return req_id;
}

} // namespace fincept::services
