#include "services/ibkr/IbkrTwsService.h"

#include "core/config/AppPaths.h"
#include "core/logging/Logger.h"
#include "python/PythonRunner.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QtGlobal>

namespace fincept::services::ibkr {

namespace {

constexpr int kProbeWatchdogMs = 60 * 1000;
constexpr int kQuoteWatchdogMs = 120 * 1000;
constexpr int kHistoryWatchdogMs = 120 * 1000;

/// Parse the wrapper's single JSON document out of the bounded process output.
/// Returns an empty object for anything that is not a MarketLab IBKR payload,
/// so the caller falls back to the raw process error.
QJsonObject wrapper_payload(const QString& output) {
    const QString json = python::extract_json(output);
    if (json.trimmed().isEmpty())
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject())
        return {};
    const QJsonObject payload = document.object();
    if (payload.value(QLatin1String("source")).toString() != QLatin1String(kIbkrSource))
        return {};
    return payload;
}

} // namespace

IbkrTwsService::IbkrTwsService(QObject* parent) : QObject(parent) {}

IbkrTwsService& IbkrTwsService::instance() {
    static IbkrTwsService service;
    return service;
}

IbkrTwsConfig IbkrTwsService::load_config() const {
    IbkrTwsConfig cfg;
    const QString override_path = qEnvironmentVariable("MARKETLAB_IBKR_CONFIG").trimmed();
    cfg.config_path = override_path.isEmpty()
                          ? QDir(AppPaths::root()).filePath(QStringLiteral("ibkr_tws.json"))
                          : override_path;
    QFile file(cfg.config_path);
    if (!file.open(QIODevice::ReadOnly))
        return cfg;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return cfg;
    const QJsonObject object = document.object();
    cfg.trading_desk_root = object.value(QLatin1String("trading_desk_root")).toString();
    cfg.trading_desk_commit = object.value(QLatin1String("trading_desk_commit")).toString();
    cfg.ibapi_path = object.value(QLatin1String("ibapi_path")).toString();
    cfg.host = object.value(QLatin1String("host")).toString(cfg.host);
    cfg.port = object.value(QLatin1String("port")).toInt(cfg.port);
    cfg.client_id = object.value(QLatin1String("client_id")).toInt(cfg.client_id);
    cfg.tws_version = object.value(QLatin1String("tws_version")).toString();
    const QJsonArray symbols = object.value(QLatin1String("symbols")).toArray();
    for (const QJsonValue& value : symbols) {
        const QString symbol = value.toString().trimmed();
        if (!symbol.isEmpty())
            cfg.symbols.append(symbol);
    }
    return cfg;
}

bool IbkrTwsService::configured() const {
    return load_config().is_configured();
}

bool IbkrTwsService::routes_symbol(const QString& symbol) const {
    if (symbol.trimmed().isEmpty())
        return false;
    const IbkrTwsConfig cfg = load_config();
    for (const QString& candidate : cfg.symbols) {
        if (candidate.compare(symbol.trimmed(), Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

IbkrTwsConfig IbkrTwsService::config() const {
    return load_config();
}

QStringList IbkrTwsService::endpoint_arguments(const IbkrTwsConfig& cfg) {
    return {QStringLiteral("--host"), cfg.host, QStringLiteral("--port"), QString::number(cfg.port),
            QStringLiteral("--client-id"), QString::number(cfg.client_id)};
}

void IbkrTwsService::run(const IbkrTwsConfig& cfg, const QStringList& arguments, int timeout_ms,
                         std::function<void(bool, const QJsonObject&, const QString&)> cb) {
    if (cfg.config_path.trimmed().isEmpty()) {
        cb(false, QJsonObject{},
           QStringLiteral("IBKR TWS is not configured; no local ibkr_tws.json was found"));
        return;
    }
    QStringList argv{QStringLiteral("--config"), cfg.config_path};
    argv.append(arguments);

    python::PythonRunner::RunOptions options;
    options.expect_json = true;
    options.timeout_ms = timeout_ms;

    python::PythonRunner::instance().run_with_options(
        QStringLiteral("ibkr_tws_data.py"), argv, options,
        [cb = std::move(cb)](const python::PythonResult& result) {
            const QJsonObject payload = wrapper_payload(result.output);
            if (!payload.isEmpty()) {
                cb(payload.value(QLatin1String("ok")).toBool(false), payload, QString());
                return;
            }
            QString error = result.error.trimmed();
            if (error.isEmpty())
                error = result.output.trimmed();
            if (error.isEmpty())
                error = QStringLiteral("IBKR wrapper produced no result");
            cb(false, QJsonObject{}, error);
        });
}

void IbkrTwsService::probe(ProbeCallback cb) {
    const IbkrTwsConfig cfg = config();
    // The local file is the single configuration authority for a normal read:
    // the wrapper re-reads and validates it. Endpoint overrides are passed only
    // by the explicit *_with diagnostics, where the caller supplies the value.
    QStringList arguments{QStringLiteral("probe")};
    arguments.append({QStringLiteral("--timeout"), QStringLiteral("10")});
    run(cfg, arguments, kProbeWatchdogMs,
        [cb = std::move(cb)](bool ok, const QJsonObject& payload, const QString& error) {
            cb(ibkr_probe_result_from_payload(payload, ok, error));
        });
}

void IbkrTwsService::probe_with(const IbkrTwsConfig& cfg, int readiness_timeout_sec, ProbeCallback cb) {
    QStringList arguments{QStringLiteral("probe")};
    arguments.append(endpoint_arguments(cfg));
    arguments.append({QStringLiteral("--timeout"), QString::number(readiness_timeout_sec)});
    run(cfg, arguments, kProbeWatchdogMs,
        [cb = std::move(cb)](bool ok, const QJsonObject& payload, const QString& error) {
            cb(ibkr_probe_result_from_payload(payload, ok, error));
        });
}

void IbkrTwsService::fetch_quote(const QString& symbol, QuoteCallback cb) {
    const IbkrTwsConfig cfg = config();
    QStringList arguments{QStringLiteral("snapshot"), symbol};
    arguments.append({QStringLiteral("--timeout"), QStringLiteral("15")});
    run(cfg, arguments, kQuoteWatchdogMs,
        [cb = std::move(cb)](bool ok, const QJsonObject& payload, const QString& error) {
            cb(ibkr_quote_result_from_payload(payload, ok, error));
        });
}

void IbkrTwsService::fetch_quote_with(const IbkrTwsConfig& cfg, const QString& symbol, QuoteCallback cb) {
    QStringList arguments{QStringLiteral("snapshot"), symbol};
    arguments.append(endpoint_arguments(cfg));
    arguments.append({QStringLiteral("--timeout"), QStringLiteral("15")});
    run(cfg, arguments, kQuoteWatchdogMs,
        [cb = std::move(cb)](bool ok, const QJsonObject& payload, const QString& error) {
            cb(ibkr_quote_result_from_payload(payload, ok, error));
        });
}

void IbkrTwsService::fetch_history(const QString& symbol, const QString& duration, const QString& bar_size,
                                   HistoryCallback cb) {
    const IbkrTwsConfig cfg = config();
    QStringList arguments{QStringLiteral("history"), symbol, QStringLiteral("--duration"), duration,
                          QStringLiteral("--bar-size"), bar_size};
    arguments.append({QStringLiteral("--timeout"), QStringLiteral("30")});
    run(cfg, arguments, kHistoryWatchdogMs,
        [cb = std::move(cb)](bool ok, const QJsonObject& payload, const QString& error) {
            cb(ibkr_history_result_from_payload(payload, ok, error));
        });
}

} // namespace fincept::services::ibkr
