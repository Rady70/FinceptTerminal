#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

namespace fincept::services {

/// Symbol/asset search proxy for screen-side search-as-you-type UIs.
///
/// MarketLab: the hosted `/market/search` endpoint is replaced by the local
/// `yfinance_data.py search` path (FINCEPT_FORK_PLAN.md §7). Screens never
/// call `HttpClient` or `PythonRunner` directly for search; callers pass a
/// `request_id` (typically the current query string) so they can ignore
/// stale responses without subclassing the service.
class MarketSearchService : public QObject {
    Q_OBJECT

  public:
    struct Item {
        QString symbol;
        QString name;
        QString exchange; ///< Optional — empty when not provided by the source.
        QString type;     ///< Optional — e.g. "stock", "etf", "index". Empty when not provided.
        QString country;  ///< Optional — ISO/short country code. Empty when not provided.
    };

    static MarketSearchService& instance();

    /// Fire a search through the local yfinance search script. `type` is
    /// optional (e.g. "stock"); pass empty to omit the client-side filter.
    /// `request_id` is echoed back in `results_ready` so the caller can
    /// discard out-of-order responses. Limit is clamped to [1, 100].
    void search(const QString& query, const QString& type, int limit, const QString& request_id);

  signals:
    void results_ready(const QString& request_id, const QString& query, const QList<Item>& items);
    void search_failed(const QString& request_id, const QString& query, const QString& reason);

  private:
    MarketSearchService();
    Q_DISABLE_COPY(MarketSearchService)
};

} // namespace fincept::services

Q_DECLARE_METATYPE(fincept::services::MarketSearchService::Item)
