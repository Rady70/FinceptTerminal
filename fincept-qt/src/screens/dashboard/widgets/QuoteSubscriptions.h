#pragma once
// QuoteSubscriptions — the per-symbol quote cache a dashboard widget keeps,
// together with the failures the hub reports for those symbols.
//
// Every quote widget used to subscribe to `market:quote:<sym>` values only. The
// hub reports a failed refresh through `publish_error`, which no widget
// listened to, so a symbol that never arrived simply vanished from the tile and
// a symbol whose later refresh failed kept its old number with no warning.
// This helper subscribes to both and keeps the two maps the footer summary
// (quote_feed_summary) is built from.
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "services/markets/MarketDataService.h"

#include <QHash>
#include <QObject>
#include <QStringList>

#include <functional>

namespace fincept::screens::widgets {

class QuoteSubscriptions {
  public:
    /// Subscribe `owner` to the quote and error topics of `symbols`. `changed`
    /// runs after every delivery (value or failure); widgets pass a lambda that
    /// coalesces through BaseWidget::schedule_render. The hub drops every
    /// subscription when `owner` unsubscribes or is destroyed.
    void subscribe(QObject* owner, const QStringList& symbols, std::function<void()> changed) {
        auto& hub = datahub::DataHub::instance();
        for (const QString& sym : symbols) {
            const QString topic = QStringLiteral("market:quote:") + sym;
            hub.subscribe(owner, topic, [this, sym, changed](const QVariant& v) {
                if (!v.canConvert<services::QuoteData>())
                    return;
                const auto delivered = v.value<services::QuoteData>();
                const auto it = cache_.constFind(sym);
                if (quote_value_clears_error(it == cache_.constEnd() ? nullptr : &it.value(), delivered))
                    errors_.remove(sym);
                cache_.insert(sym, delivered);
                if (changed)
                    changed();
            });
            hub.subscribe_errors(owner, topic, [this, sym, changed](const QString& error) {
                errors_.insert(sym, error.isEmpty() ? QStringLiteral("refresh failed") : error);
                if (changed)
                    changed();
            });
        }
    }

    static void unsubscribe(QObject* owner) { datahub::DataHub::instance().unsubscribe(owner); }

    /// Forget everything (symbol set changed).
    void clear() {
        cache_.clear();
        errors_.clear();
    }

    const services::QuoteData* quote(const QString& sym) const {
        const auto it = cache_.constFind(sym);
        return it == cache_.constEnd() ? nullptr : &it.value();
    }
    QString error(const QString& sym) const { return errors_.value(sym); }
    const QHash<QString, services::QuoteData>& cache() const { return cache_; }
    const QHash<QString, QString>& errors() const { return errors_; }

    QuoteFeedSummary summary(const QStringList& symbols) const { return quote_feed_summary(symbols, cache_, errors_); }

  private:
    QHash<QString, services::QuoteData> cache_;
    QHash<QString, QString> errors_;
};

/// Loading progress counts failures as settled (a failed symbol is an answer,
/// not a reason to spin), and the footer names the data date and failures.
inline void show_feed_status(BaseWidget* widget, const QuoteFeedSummary& s, int expected) {
    widget->set_loading_progress(s.settled, expected);
    widget->set_footer(s.text, s.tooltip, s.warn);
}

} // namespace fincept::screens::widgets
