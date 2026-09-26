#include "services/etf/EtfDataService.h"

#include "core/config/AppPaths.h"
#include "network/http/HttpClient.h"
#include "services/ibkr/IbkrTwsService.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtGlobal>

#include <algorithm>

namespace fincept::services::etf {

EtfDataService::EtfDataService(QObject* parent) : QObject(parent) {}

EtfDataService& EtfDataService::instance() {
    static EtfDataService service;
    return service;
}

EtfDataService::SecConfig EtfDataService::sec_config() const {
    SecConfig cfg;
    const QString override_path = qEnvironmentVariable("MARKETLAB_SEC_CONFIG").trimmed();
    cfg.config_path =
        override_path.isEmpty() ? QDir(AppPaths::root()).filePath(QStringLiteral("sec_edgar.json")) : override_path;
    QFile file(cfg.config_path);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            const QJsonObject o = doc.object();
            cfg.user_agent = o.value(QLatin1String("user_agent")).toString();
            const int interval = o.value(QLatin1String("min_request_interval_ms")).toInt(kSecDefaultRequestIntervalMs);
            cfg.min_request_interval_ms = interval;
        }
    }
    const QString env_ua = qEnvironmentVariable("MARKETLAB_SEC_USER_AGENT").trimmed();
    if (!env_ua.isEmpty())
        cfg.user_agent = env_ua;
    // The floor keeps a run well inside the SEC's 10 requests per second
    // whatever the local file says.
    cfg.min_request_interval_ms = std::clamp(cfg.min_request_interval_ms, kSecMinRequestIntervalFloorMs, 10000);
    cfg.declared = sec_user_agent_valid(cfg.user_agent);
    return cfg;
}

bool EtfDataService::ibkr_configured() const {
    return ibkr::IbkrTwsService::instance().configured();
}

void EtfDataService::ingest_sec_nport(const SecNportRequest& request,
                                      std::function<void(const SecNportRunSummary&)> done) {
    const SecConfig cfg = sec_config();
    SecHttpGet http = [this](const QString& url, const QByteArray& user_agent,
                             std::function<void(const SecHttpResponse&)> cb) {
        HttpClient::Headers headers;
        headers.insert("User-Agent", user_agent);
        headers.insert("Accept", "application/json, application/atom+xml, application/xml, text/xml");
        HttpClient::instance().get_raw(
            url,
            [cb = std::move(cb)](const HttpClient::RawResponse& r) {
                SecHttpResponse s;
                s.transport_ok = r.transport_ok;
                s.http_status = r.status;
                s.body = r.body;
                s.error = r.error;
                cb(s);
            },
            this, headers);
    };
    auto* ingestor = new EtfSecNportIngestor(
        std::move(http), cfg.user_agent, cfg.min_request_interval_ms, []() { return QDateTime::currentDateTimeUtc(); },
        this);
    ingestor->run(request, [ingestor, done = std::move(done)](const SecNportRunSummary& summary) {
        if (done)
            done(summary);
        ingestor->deleteLater();
    });
}

void EtfDataService::ingest_ibkr_daily(const QString& symbol, const QString& duration,
                                       std::function<void(const IbkrDailyRunSummary&)> done) {
    IbkrDailyFetch fetch = [](const IbkrDailyRequest& r, std::function<void(const QJsonObject&)> cb) {
        ibkr::IbkrTwsService::instance().fetch_history_envelope(r.symbol, r.duration, QLatin1String(kIbkrBarSize),
                                                                r.end_date_time, QLatin1String(kIbkrWhatToShow),
                                                                /*use_rth=*/true, std::move(cb));
    };
    auto* ingestor = new EtfIbkrDailyIngestor(
        std::move(fetch), ibkr_configured(), []() { return QDateTime::currentDateTimeUtc(); }, this);
    ingestor->run(symbol, duration, [ingestor, done = std::move(done)](const IbkrDailyRunSummary& summary) {
        if (done)
            done(summary);
        ingestor->deleteLater();
    });
}

} // namespace fincept::services::etf
