#include "app/IbkrTwsSelftest.h"

#include "core/logging/Logger.h"
#include "services/ibkr/IbkrTwsService.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

namespace fincept::marketlab {

// File-local helpers live in a uniquely named namespace: the release-style
// unity build concatenates this file with MarketLabBoundarySelftest.cpp into
// one TU, and an anonymous namespace would merge with that file's anonymous
// namespace and collide on its `failures`/`CHECK` definitions.
namespace ibkr_selftest_detail {

int ibkr_selftest_failures = 0;

#define IBKR_TWS_SELFTEST_CHECK(cond, what)                                                                            \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++ibkr_selftest_failures;                                                                                  \
            std::fprintf(stderr, "[ibkr-selftest] FAIL: %s\n", what);                                                  \
            LOG_ERROR("IbkrSelftest", QStringLiteral("FAIL: %1").arg(QString::fromUtf8(what)));                        \
        } else {                                                                                                       \
            LOG_INFO("IbkrSelftest", QStringLiteral("ok: %1").arg(QString::fromUtf8(what)));                           \
        }                                                                                                              \
    } while (0)

/// State shared between a bounded wait and the asynchronous service callback.
///
/// The service callback can outlive the wait when the watchdog fires, so the
/// callback must never capture stack references: it owns this state through a
/// shared_ptr, and once the wait has returned without completion the state is
/// marked abandoned and a late callback becomes a no-op instead of touching a
/// dead frame.
template <typename Result>
struct AsyncWait {
    QEventLoop loop;
    Result result{};
    bool completed = false;
    bool abandoned = false;
};

/// Run one asynchronous service operation to completion, bounded by a watchdog.
/// Returns whether the operation completed and the value it produced.
template <typename Result, typename Start>
std::pair<bool, Result> wait_result(Start&& start, int timeout_ms) {
    auto wait = std::make_shared<AsyncWait<Result>>();
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &wait->loop, &QEventLoop::quit);
    start([wait](const Result& result) {
        if (wait->abandoned)
            return;
        wait->result = result;
        wait->completed = true;
        wait->loop.quit();
    });
    if (!wait->completed) {
        watchdog.start(timeout_ms);
        wait->loop.exec();
    }
    wait->abandoned = true;
    return {wait->completed, wait->result};
}

QJsonObject classification_json(const services::ibkr::IbkrTwsClassification& classification) {
    QJsonObject object;
    object["usable"] = classification.usable;
    object["feed"] = classification.feed;
    object["status"] = classification.status;
    object["entitlement"] = classification.entitlement;
    object["value_present"] = classification.value_present;
    object["delayed_fallback"] = classification.delayed_fallback;
    object["timed_out"] = classification.timed_out;
    object["validation_reason"] = classification.validation_reason;
    if (!classification.error_code.isUndefined() && !classification.error_code.isNull())
        object["error_code"] = classification.error_code;
    if (!classification.error_message.isEmpty())
        object["error_message"] = classification.error_message;
    // Both snapshot attempts stay visible in the recorded evidence.
    object["live_market_data_status"] = classification.live_status;
    object["live_usable_market_data"] = classification.live_usable;
    object["delayed_attempted"] = classification.delayed_attempted;
    object["delayed_market_data_status"] = classification.delayed_status;
    object["delayed_usable_market_data"] = classification.delayed_usable;
    object["delayed_validation_reason"] = classification.delayed_validation_reason;
    if (!classification.delayed_error_code.isUndefined() && !classification.delayed_error_code.isNull())
        object["delayed_error_code"] = classification.delayed_error_code;
    if (!classification.delayed_error_message.isEmpty())
        object["delayed_error_message"] = classification.delayed_error_message;
    return object;
}

QJsonObject identity_json(const services::ibkr::IbkrTwsIdentity& identity) {
    QJsonObject object;
    object["adapter_commit"] = identity.commit;
    object["adapter_sha256"] = identity.adapter_sha256;
    object["ibapi_version"] = identity.ibapi_version;
    object["ibapi_version_source"] = identity.ibapi_version_source;
    object["ibapi_location"] = identity.ibapi_location;
    object["ibapi_runtime_path_verified"] = identity.ibapi_runtime_path_verified;
    object["uses_official_runtime"] = identity.uses_official_runtime;
    object["tws_version"] = identity.tws_version;
    object["tws_version_source"] = identity.tws_version_source;
    object["host"] = identity.host;
    object["port"] = identity.port;
    object["client_id"] = identity.client_id;
    return object;
}

QJsonObject failure_json(const QString& type, const QString& stage, const QString& message) {
    QJsonObject object;
    object["type"] = type;
    object["stage"] = stage;
    object["message"] = message;
    return object;
}

} // namespace ibkr_selftest_detail

int run_ibkr_tws_selftest() {
    using namespace ibkr_selftest_detail;
    using services::ibkr::IbkrTwsHistoryResult;
    using services::ibkr::IbkrTwsProbeResult;
    using services::ibkr::IbkrTwsQuoteResult;
    using services::ibkr::IbkrTwsService;

    LOG_INFO("IbkrSelftest", "Starting MarketLab IBKR TWS read-only self-test");

    auto& service = IbkrTwsService::instance();
    const services::ibkr::IbkrTwsConfig cfg = service.config();

    QJsonObject summary;
    summary["config_path"] = cfg.config_path;
    summary["endpoint_host"] = cfg.host;
    summary["endpoint_port"] = cfg.port;
    summary["client_id"] = cfg.client_id;

    if (!service.configured()) {
        summary["configured"] = false;
        summary["skipped"] = true;
        summary["reason"] = QStringLiteral("no local ibkr_tws.json configuration");
        std::printf("IBKR_SELFTEST_JSON: %s\n", QJsonDocument(summary).toJson(QJsonDocument::Compact).constData());
        LOG_INFO("IbkrSelftest", "No local IBKR configuration; suite reports skipped (exit 0)");
        return 0;
    }
    summary["configured"] = true;

    // ── 1. Connection, readiness, observed runtime identity, clean disconnect ─
    const auto [probe_done, probe] = wait_result<IbkrTwsProbeResult>(
        [&](std::function<void(const IbkrTwsProbeResult&)> deliver) {
            service.probe([deliver = std::move(deliver)](const IbkrTwsProbeResult& result) { deliver(result); });
        },
        60 * 1000);
    IBKR_TWS_SELFTEST_CHECK(probe_done, "probe completed within its bounded watchdog");
    IBKR_TWS_SELFTEST_CHECK(probe.ok, "probe reported ok");
    IBKR_TWS_SELFTEST_CHECK(probe.connected && probe.ready, "TWS socket and readiness signal observed");
    IBKR_TWS_SELFTEST_CHECK(probe.clean_disconnect, "clean disconnect observed between commands");
    IBKR_TWS_SELFTEST_CHECK(probe.identity.uses_official_runtime == QLatin1String("true"),
                            "adapter reported the official ibapi runtime");
    IBKR_TWS_SELFTEST_CHECK(!probe.identity.commit.isEmpty(), "adapter pin commit reported");
    summary["probe"] =
        QJsonObject{{"ok", probe.ok},
                    {"connected", probe.connected},
                    {"ready", probe.ready},
                    {"clean_disconnect", probe.clean_disconnect},
                    {"identity", identity_json(probe.identity)},
                    {"failure", failure_json(probe.failure_type, probe.failure_stage, probe.failure_message)}};

    // ── 2. Contract resolution and quote snapshot for one representative equity
    const auto [quote_done, quote] = wait_result<IbkrTwsQuoteResult>(
        [&](std::function<void(const IbkrTwsQuoteResult&)> deliver) {
            service.fetch_quote(QStringLiteral("AAPL"),
                                [deliver = std::move(deliver)](const IbkrTwsQuoteResult& result) { deliver(result); });
        },
        120 * 1000);
    IBKR_TWS_SELFTEST_CHECK(quote_done, "quote request completed within its bounded watchdog");
    IBKR_TWS_SELFTEST_CHECK(quote.ok, "quote request reported a classified outcome");
    IBKR_TWS_SELFTEST_CHECK(quote.con_id > 0, "AAPL contract resolved to a conId");
    IBKR_TWS_SELFTEST_CHECK(!quote.contract.isEmpty(), "resolved contract identity carried to the consumer");
    QJsonObject quote_json = classification_json(quote.classification);
    quote_json["ok"] = quote.ok;
    quote_json["con_id"] = quote.con_id;
    quote_json["contract_symbol"] = quote.contract.value(QLatin1String("symbol")).toString();
    quote_json["contract_currency"] = quote.contract.value(QLatin1String("currency")).toString();
    quote_json["contract_security_type"] = quote.contract.value(QLatin1String("security_type")).toString();
    quote_json["contract_exchange"] = quote.contract.value(QLatin1String("exchange")).toString();
    quote_json["contract_primary_exchange"] = quote.contract.value(QLatin1String("primary_exchange")).toString();
    quote_json["source"] = quote.quote.source;
    quote_json["retrieved_at"] = static_cast<double>(quote.quote.retrieved_at);
    quote_json["status"] = quote.quote.status;
    quote_json["has_price"] = quote.quote.has_price;
    quote_json["has_change"] = quote.quote.has_change;
    quote_json["has_volume"] = quote.quote.has_volume;
    quote_json["failure"] = failure_json(quote.failure_type, quote.failure_stage, quote.failure_message);
    summary["quote"] = quote_json;

    if (quote.ok) {
        // Contract identity is part of the result, not an assumption.
        IBKR_TWS_SELFTEST_CHECK(
            quote.contract.value(QLatin1String("security_type")).toString() == QLatin1String("STK") ||
                quote.contract.value(QLatin1String("security_type")).toString() == QLatin1String("ETF") ||
                quote.contract.value(QLatin1String("security_type")).toString() == QLatin1String("FUND"),
            "resolved contract carries a supported security type");
        IBKR_TWS_SELFTEST_CHECK(!quote.contract.value(QLatin1String("exchange")).toString().isEmpty(),
                                "resolved contract names an exchange");
        if (quote.classification.usable) {
            // A usable quote must name the feed it actually came from, carry
            // provenance, and must not call delayed data live.
            IBKR_TWS_SELFTEST_CHECK(quote.classification.feed == QLatin1String("LIVE") ||
                                        quote.classification.feed == QLatin1String("FROZEN") ||
                                        quote.classification.feed == QLatin1String("DELAYED") ||
                                        quote.classification.feed == QLatin1String("DELAYED_FROZEN"),
                                    "usable quote names its observed IBKR feed");
            IBKR_TWS_SELFTEST_CHECK(quote.quote.source == QLatin1String("ibkr_tws"), "quote provenance names ibkr_tws");
            IBKR_TWS_SELFTEST_CHECK(quote.quote.retrieved_at > 0, "quote carries a retrieval timestamp");
            IBKR_TWS_SELFTEST_CHECK(quote.classification.feed != QLatin1String("LIVE") ||
                                        quote.quote.status == QLatin1String("LIVE"),
                                    "no feed is reported as live unless IBKR said live");
        } else {
            // The stopping rule accepts an entitlement limitation, not a
            // generic provider/API failure, as qualification evidence.
            IBKR_TWS_SELFTEST_CHECK(quote.classification.status == QLatin1String("NOT_ENTITLED"),
                                    "an unusable quote is an explicit entitlement limitation, not a generic failure");
            IBKR_TWS_SELFTEST_CHECK(!quote.classification.entitlement.isEmpty(),
                                    "unusable quote exposes an entitlement state");
            IBKR_TWS_SELFTEST_CHECK(!quote.quote.has_price && !quote.quote.has_change && !quote.quote.has_volume,
                                    "unusable quote carries no fabricated readings");
        }
        if (quote.classification.delayed_attempted) {
            // A live block must not hide the delayed attempt that followed it.
            IBKR_TWS_SELFTEST_CHECK(!quote.classification.delayed_status.isEmpty(),
                                    "delayed attempt status is exposed");
            IBKR_TWS_SELFTEST_CHECK(quote.classification.delayed_usable != QLatin1String("PASS") ||
                                        quote.quote.source == QLatin1String("ibkr_tws"),
                                    "delayed attempt outcome is visible to the consumer");
        }
    }

    // ── 3. Bounded historical bars for the retained quote/history path ────────
    const auto [history_done, history] = wait_result<IbkrTwsHistoryResult>(
        [&](std::function<void(const IbkrTwsHistoryResult&)> deliver) {
            service.fetch_history(
                QStringLiteral("AAPL"), QStringLiteral("1 M"), QStringLiteral("1 day"),
                [deliver = std::move(deliver)](const IbkrTwsHistoryResult& result) { deliver(result); });
        },
        120 * 1000);
    IBKR_TWS_SELFTEST_CHECK(history_done, "history request completed within its bounded watchdog");
    IBKR_TWS_SELFTEST_CHECK(history.ok, "history request reported a classified outcome");
    IBKR_TWS_SELFTEST_CHECK(history.con_id > 0, "AAPL contract resolved for the history request");
    QJsonObject history_json = classification_json(history.classification);
    history_json["ok"] = history.ok;
    history_json["con_id"] = history.con_id;
    history_json["contract_security_type"] = history.contract.value(QLatin1String("security_type")).toString();
    history_json["contract_exchange"] = history.contract.value(QLatin1String("exchange")).toString();
    history_json["contract_primary_exchange"] = history.contract.value(QLatin1String("primary_exchange")).toString();
    history_json["bar_count"] = history.bars.size();
    history_json["dropped_bars"] = history.dropped_bars;
    history_json["failure"] = failure_json(history.failure_type, history.failure_stage, history.failure_message);
    summary["history"] = history_json;

    if (history.ok && history.classification.usable && !history.bars.isEmpty()) {
        bool ordered = true;
        bool complete_ohlc = true;
        bool positive_prices = true;
        for (int i = 0; i < history.bars.size(); ++i) {
            if (i > 0 && history.bars[i].timestamp <= history.bars[i - 1].timestamp)
                ordered = false;
            if (!history.bars[i].has_open || !history.bars[i].has_high || !history.bars[i].has_low)
                complete_ohlc = false;
            if (!(history.bars[i].close > 0.0) || !(history.bars[i].open > 0.0) || !(history.bars[i].high > 0.0) ||
                !(history.bars[i].low > 0.0))
                positive_prices = false;
        }
        IBKR_TWS_SELFTEST_CHECK(ordered, "history bars are strictly ordered by timestamp");
        IBKR_TWS_SELFTEST_CHECK(complete_ohlc, "history bars carry open/high/low presence");
        IBKR_TWS_SELFTEST_CHECK(positive_prices, "history bars carry positive price readings");
        IBKR_TWS_SELFTEST_CHECK(history.dropped_bars == 0, "no history bar was dropped by the model boundary");
    } else if (history.ok) {
        // The stopping rule accepts an entitlement limitation, not a generic
        // provider/API failure or an unvalidated series, as qualification.
        IBKR_TWS_SELFTEST_CHECK(
            history.classification.status == QLatin1String("NOT_ENTITLED"),
            "an unusable history result is an explicit entitlement limitation, not a generic failure");
        IBKR_TWS_SELFTEST_CHECK(history.bars.isEmpty(), "unusable history carries no bars");
    }

    // ── 4. Failure paths through the same service boundary ───────────────────
    QJsonObject failure_paths;

    // 4a. Refused connection: bind a port, release it, then point the service at
    // it. Nothing listens, so the attempt must end in a bounded typed failure.
    int closed_port = 0;
    {
        QTcpServer probe_server;
        if (probe_server.listen(QHostAddress::LocalHost, 0))
            closed_port = probe_server.serverPort();
    }
    if (closed_port > 0) {
        services::ibkr::IbkrTwsConfig refused = cfg;
        refused.port = closed_port;
        const auto [refused_done, refused_result] = wait_result<IbkrTwsProbeResult>(
            [&](std::function<void(const IbkrTwsProbeResult&)> deliver) {
                service.probe_with(
                    refused, 3, [deliver = std::move(deliver)](const IbkrTwsProbeResult& result) { deliver(result); });
            },
            60 * 1000);
        IBKR_TWS_SELFTEST_CHECK(refused_done, "refused-connection probe completed within its bounded watchdog");
        IBKR_TWS_SELFTEST_CHECK(!refused_result.ok, "refused connection is a failure, not a success");
        IBKR_TWS_SELFTEST_CHECK(refused_result.failure_type == QLatin1String("IBKR_CONNECTION_FAILED") ||
                                    refused_result.failure_type == QLatin1String("IBKR_TIMEOUT"),
                                "refused connection carries a bounded transport failure type");
        failure_paths["refused_connection"] =
            failure_json(refused_result.failure_type, refused_result.failure_stage, refused_result.failure_message);
    } else {
        IBKR_TWS_SELFTEST_CHECK(false, "could not allocate a loopback port for the refused-connection check");
    }

    // 4b. Readiness timeout: a socket that accepts but never sends nextValidId.
    {
        QTcpServer silent_server;
        const bool listening = silent_server.listen(QHostAddress::LocalHost, 0);
        IBKR_TWS_SELFTEST_CHECK(listening, "silent loopback listener started for the readiness-timeout check");
        if (listening) {
            services::ibkr::IbkrTwsConfig silent = cfg;
            silent.port = silent_server.serverPort();
            const auto [timeout_done, timeout_result] = wait_result<IbkrTwsProbeResult>(
                [&](std::function<void(const IbkrTwsProbeResult&)> deliver) {
                    service.probe_with(silent, 2, [deliver = std::move(deliver)](const IbkrTwsProbeResult& result) {
                        deliver(result);
                    });
                },
                60 * 1000);
            IBKR_TWS_SELFTEST_CHECK(timeout_done, "readiness-timeout probe completed within its bounded watchdog");
            IBKR_TWS_SELFTEST_CHECK(!timeout_result.ok, "readiness timeout is a failure, not a success");
            IBKR_TWS_SELFTEST_CHECK(timeout_result.failure_type == QLatin1String("IBKR_TIMEOUT"),
                                    "readiness timeout carries the typed timeout failure");
            failure_paths["readiness_timeout"] =
                failure_json(timeout_result.failure_type, timeout_result.failure_stage, timeout_result.failure_message);
        }
    }

    // 4c. Unresolved contract: an instrument TWS cannot resolve must be an
    // explicit contract failure, never a fabricated quote.
    {
        const auto [missing_done, missing] = wait_result<IbkrTwsQuoteResult>(
            [&](std::function<void(const IbkrTwsQuoteResult&)> deliver) {
                service.fetch_quote_with(
                    cfg, QStringLiteral("ZZZZ_MARKETLAB_NO_SUCH_SYMBOL"),
                    [deliver = std::move(deliver)](const IbkrTwsQuoteResult& result) { deliver(result); });
            },
            120 * 1000);
        IBKR_TWS_SELFTEST_CHECK(missing_done, "unresolved-contract request completed within its bounded watchdog");
        IBKR_TWS_SELFTEST_CHECK(!missing.ok, "unresolved contract is an explicit failure");
        IBKR_TWS_SELFTEST_CHECK(missing.failure_type == QLatin1String("IBKR_CONTRACT_NOT_RESOLVED") ||
                                    missing.failure_type == QLatin1String("IBKR_REQUEST_REJECTED"),
                                "unresolved contract carries a typed contract failure");
        failure_paths["unresolved_contract"] =
            failure_json(missing.failure_type, missing.failure_stage, missing.failure_message);
    }

    summary["failure_paths"] = failure_paths;
    summary["failed_checks"] = ibkr_selftest_failures;
    summary["result"] = ibkr_selftest_failures == 0 ? QStringLiteral("PASS") : QStringLiteral("FAIL");
    std::printf("IBKR_SELFTEST_JSON: %s\n", QJsonDocument(summary).toJson(QJsonDocument::Compact).constData());
    if (ibkr_selftest_failures > 0)
        std::fprintf(stderr, "[ibkr-selftest] %d check(s) failed\n", ibkr_selftest_failures);
    return ibkr_selftest_failures == 0 ? 0 : 1;
}

} // namespace fincept::marketlab
