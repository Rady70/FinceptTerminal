#include "services/markets/MarketSearchService.h"

#include "core/logging/Logger.h"
#include "python/PythonRunner.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPointer>
#include <QSet>

#include <algorithm>

namespace fincept::services {

namespace {

constexpr int kMinLimit = 1;
constexpr int kMaxLimit = 100;
constexpr const char* kScript = "yfinance_data.py";

/// Map yfinance quoteType values to the short type names the UI used to
/// receive from the hosted search endpoint.
QString short_type(const QString& quote_type) {
    const QString t = quote_type.toUpper();
    if (t == QLatin1String("EQUITY") || t == QLatin1String("STOCK"))
        return QStringLiteral("stock");
    if (t == QLatin1String("ETF"))
        return QStringLiteral("etf");
    if (t == QLatin1String("MUTUALFUND") || t == QLatin1String("FUND"))
        return QStringLiteral("fund");
    if (t == QLatin1String("INDEX"))
        return QStringLiteral("index");
    if (t == QLatin1String("CURRENCY"))
        return QStringLiteral("forex");
    if (t == QLatin1String("CRYPTOCURRENCY"))
        return QStringLiteral("crypto");
    if (t == QLatin1String("FUTURE") || t == QLatin1String("FUTURES"))
        return QStringLiteral("futures");
    if (t == QLatin1String("BOND"))
        return QStringLiteral("bond");
    return quote_type.toLower();
}

/// Which short types satisfy a requested asset type. Yahoo's search does not
/// carry every legacy /market/search type, so each UI slash-command maps to an
/// acceptable set; "/economic" has no Yahoo equivalent and yields no results
/// (the UI shows the truthful empty state instead of unrelated instruments).
QSet<QString> acceptable_for(const QString& requested) {
    static const QHash<QString, QSet<QString>> kMap = {
        {QStringLiteral("stock"), {QStringLiteral("stock")}},
        {QStringLiteral("fund"), {QStringLiteral("fund"), QStringLiteral("etf")}},
        {QStringLiteral("dr"), {QStringLiteral("stock")}},
        {QStringLiteral("index"), {QStringLiteral("index")}},
        {QStringLiteral("forex"), {QStringLiteral("forex")}},
        {QStringLiteral("crypto"), {QStringLiteral("crypto")}},
        {QStringLiteral("futures"), {QStringLiteral("futures")}},
        {QStringLiteral("bond"), {QStringLiteral("bond")}},
        {QStringLiteral("economic"), {}},
    };
    return kMap.value(requested, {requested});
}

QList<MarketSearchService::Item> parse_items(const QJsonObject& doc, const QString& requested_type) {
    QJsonArray arr;
    if (doc.contains("results"))
        arr = doc["results"].toArray();
    else if (doc.contains("data"))
        arr = doc["data"].toArray();

    const QSet<QString> accepted = acceptable_for(requested_type);

    QList<MarketSearchService::Item> out;
    out.reserve(arr.size());
    for (const auto& v : arr) {
        const auto obj = v.toObject();
        const QString sym = obj["symbol"].toString();
        if (sym.isEmpty())
            continue;
        const QString type = short_type(obj["type"].toString());
        if (!requested_type.isEmpty() && !accepted.contains(type))
            continue;
        out.push_back({sym, obj["name"].toString(), obj["exchange"].toString(), type, obj["country"].toString()});
    }
    return out;
}

} // namespace

MarketSearchService& MarketSearchService::instance() {
    static MarketSearchService s;
    return s;
}

MarketSearchService::MarketSearchService() = default;

void MarketSearchService::search(const QString& query, const QString& type, int limit, const QString& request_id) {
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        emit results_ready(request_id, query, {});
        return;
    }
    const int clamped = std::clamp(limit, kMinLimit, kMaxLimit);

    QPointer<MarketSearchService> self = this;
    fincept::python::PythonRunner::instance().run(
        QString::fromLatin1(kScript), {QStringLiteral("search"), q, QString::number(clamped)},
        [self, request_id, query, type](const fincept::python::PythonResult& result) {
            if (!self)
                return;
            if (!result.success) {
                const QString reason = result.error.isEmpty() ? QStringLiteral("Local search failed") : result.error;
                LOG_WARN("MarketSearch", "local search failed: " + reason.left(300));
                emit self->search_failed(request_id, query, reason);
                return;
            }

            const QString json_str = fincept::python::extract_json(result.output);
            QJsonParseError err;
            const auto doc = QJsonDocument::fromJson(json_str.toUtf8(), &err);
            if (doc.isNull() || !doc.isObject()) {
                LOG_WARN("MarketSearch", QString("local search returned invalid JSON: %1").arg(err.errorString()));
                emit self->search_failed(request_id, query, QStringLiteral("Local search returned invalid JSON"));
                return;
            }
            const auto obj = doc.object();
            if (obj.contains("error")) {
                emit self->search_failed(request_id, query, obj["error"].toString());
                return;
            }
            emit self->results_ready(request_id, query, parse_items(obj, type));
        });
}

} // namespace fincept::services
