// src/screens/markets/QuoteDisplayFormat.h
//
// QuoteData → the text a markets-domain table cell shows.
//
// Extracted from MarketPanel/ScreenerScreen so the "missing is not zero" rule
// for these retained public-data surfaces is unit-testable without linking the
// screens, the DataHub and the widget tree (the same reason MarketQuoteParse.h
// and ReportQuoteFormat.h exist). Header-only, Qt Core only.
//
// A field the provider did not return renders as the table's existing "--"
// placeholder; a genuine zero renders as "0.00" / "0". The per-field presence
// flags are the only thing that tells the two apart — formatting the bare
// double would print "$0.00" for a value that was simply never received
// (FINCEPT_FORK_PLAN.md §4).
#pragma once
#include "services/markets/MarketDataService.h"
#include "ui/formatting/NumberFormat.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

/// The markets tables' existing "no reading" placeholder.
inline QString quote_na() {
    return QStringLiteral("--");
}

/// A numeric field with an optional currency prefix and unit suffix, or the
/// placeholder when the quote does not carry the field.
inline QString quote_field_text(bool has, double value, int precision, const QString& prefix = {},
                                const QString& suffix = {}) {
    if (!has)
        return quote_na();
    return prefix + QString::number(value, 'f', precision) + suffix;
}

/// A signed change/percent cell ("+1.23" / "-0.45%"), or the placeholder.
/// Zero is neither positive nor negative: the sign is added only when the
/// value is strictly greater than zero, so an exact zero renders "0.00".
inline QString quote_signed_text(bool has, double value, int precision, const QString& suffix = {}) {
    if (!has)
        return quote_na();
    return QStringLiteral("%1%2%3")
        .arg(value > 0 ? QStringLiteral("+") : QString())
        .arg(value, 0, 'f', precision)
        .arg(suffix);
}

/// The Markets panels' arrow-prefixed change cell ("▲ 1.23" / "▼ 3.75"), or
/// the placeholder. Zero gets a flat marker, not an up or down arrow: an
/// unchanged reading must not be presented as a move in either direction.
inline QString quote_arrow_text(bool has, double value, int precision, const QString& suffix = {}) {
    if (!has)
        return quote_na();
    const QString arrow = value > 0   ? QString::fromUtf8("\xe2\x96\xb2")
                          : value < 0 ? QString::fromUtf8("\xe2\x96\xbc")
                                      : QString::fromUtf8("\xe2\x80\xa2");
    return QStringLiteral("%1 %2%3").arg(arrow).arg(std::abs(value), 0, 'f', precision).arg(suffix);
}

/// Missing-last ordering for an optional numeric reading, independent of the
/// table's sort direction: a missing reading never sorts above a present one in
/// either order. Returns true when `a` should come before `b`.
inline bool quote_missing_last_before(bool has_a, double a, bool has_b, double b, bool descending) {
    if (has_a != has_b)
        return has_a; // present first (missing last) in both directions
    if (!has_a || a == b)
        return false;
    return descending ? a > b : a < b;
}

/// The name a watchlist row actually displays: the provider's name when it
/// supplied one, otherwise the locally stored name. Sorting must use this same
/// string, or the table would order by a value the user cannot see.
inline QString quote_watchlist_row_name(const QString& provider_name, const QString& stored_name) {
    return provider_name.isEmpty() ? stored_name : provider_name;
}

/// One watchlist row's sort inputs. `name` is the *displayed* name; a numeric
/// column's reading is present only when `has_value` is true (a missing cell
/// carries no number, so it sorts last in either direction).
struct QuoteSortKey {
    QString symbol;
    QString name;
    bool has_value = false;
    double value = 0.0;
};

/// Comparator for the watchlist table's screen-owned ordering. Returns true
/// when `a` should come before `b`; missing numeric readings stay last in both
/// ascending and descending order. `column`: 0 SYMBOL, 1 NAME, 2..7 numerics.
inline bool quote_sort_before(int column, bool descending, const QuoteSortKey& a, const QuoteSortKey& b) {
    if (column <= 1) {
        const QString& as = column == 0 ? a.symbol : a.name;
        const QString& bs = column == 0 ? b.symbol : b.name;
        const int c = QString::compare(as, bs, Qt::CaseInsensitive);
        return descending ? c > 0 : c < 0;
    }
    if (a.has_value != b.has_value)
        return a.has_value;
    if (!a.has_value || a.value == b.value)
        return false;
    return descending ? a.value > b.value : a.value < b.value;
}

/// A volume cell. Missing stays the placeholder; a genuine zero is a reading
/// and renders as "0" (format_compact_volume's "--" for <= 0 would make an
/// actual zero volume indistinguishable from an absent one). A negative
/// volume is malformed — it cannot be a real reading, so it is treated as
/// unavailable rather than attributed to the instrument as zero.
inline QString quote_volume_text(const services::QuoteData& q) {
    if (!q.has_volume || q.volume < 0)
        return quote_na();
    if (q.volume == 0)
        return QStringLiteral("0");
    return fincept::ui::formatting::format_compact_volume(static_cast<qint64>(q.volume));
}

/// Combine a cell's existing tooltip (e.g. the Markets panel's full-name
/// tooltip) with the row provenance instead of replacing it.
inline QString merge_quote_provenance(const QString& existing, const QString& provenance) {
    return existing.isEmpty() ? provenance : existing + QLatin1Char('\n') + provenance;
}

/// Which provider produced the row, when it answered, and whether the reading
/// is live, partial, or the last cached value — the same facts the Watchlist
/// row tooltip carries (FINCEPT_FORK_PLAN.md §4).
inline QString quote_provenance_text(const services::QuoteData& q) {
    const QString source = q.source.isEmpty() ? QCoreApplication::translate("QuoteDisplayFormat", "unknown") : q.source;
    QString text = QCoreApplication::translate("QuoteDisplayFormat", "Source: %1").arg(source);
    text += QLatin1Char('\n');
    text += q.retrieved_at > 0 ? QCoreApplication::translate("QuoteDisplayFormat", "Retrieved: %1")
                                     .arg(QDateTime::fromSecsSinceEpoch(q.retrieved_at).toString(Qt::ISODate))
                               : QCoreApplication::translate("QuoteDisplayFormat", "Retrieved: unknown");
    text += QLatin1Char('\n');
    text += QCoreApplication::translate("QuoteDisplayFormat", "Status: %1")
                .arg(q.status.isEmpty() ? QStringLiteral("UNKNOWN") : q.status);
    if (q.status == QLatin1String(services::kQuoteStatusStale)) {
        text += QLatin1Char('\n');
        text +=
            QCoreApplication::translate("QuoteDisplayFormat", "The refresh failed; this row is the last cached value.");
    }
    return text;
}

/// Decimals for a price cell: `base`, or at least 4 for a price below 1 so a
/// coin at 0.0793 is not shown as 0.08 (and its change as 0.00).
inline int quote_price_decimals(bool has, double price, int base) {
    if (has && price != 0.0 && std::abs(price) < 1.0)
        return std::max(base, 4);
    return base;
}

/// "19 Oct" — the trading day a price belongs to; empty when unknown.
inline QString quote_session_text(const QDate& date) {
    return date.isValid() ? QLocale::c().toString(date, QStringLiteral("d MMM")) : QString();
}

/// What a quote's daily change is measured against, in words.
inline QString quote_basis_text(const services::QuoteData& q) {
    const QString b = q.day_basis;
    if (b == QLatin1String("helsinki_midnight"))
        return QCoreApplication::translate("QuoteDisplayFormat", "change since 00:00 Helsinki time");
    if (b == QLatin1String("prior_settlement"))
        return q.contract.isEmpty()
                   ? QCoreApplication::translate("QuoteDisplayFormat", "change vs previous settlement")
                   : QCoreApplication::translate("QuoteDisplayFormat", "change vs previous settlement of %1")
                         .arg(q.contract);
    if (b == QLatin1String("prior_bar_close_unverified"))
        return QCoreApplication::translate("QuoteDisplayFormat",
                                           "change vs previous daily bar (settlement unavailable; may span a "
                                           "contract roll)");
    if (b == QLatin1String("prior_session_close") || b == QLatin1String("provider_previous_close"))
        return QCoreApplication::translate("QuoteDisplayFormat", "change vs previous session close");
    return QCoreApplication::translate("QuoteDisplayFormat", "change reference not reported");
}

/// One symbol's line in a widget footer tooltip.
inline QString quote_status_line(const QString& symbol, const services::QuoteData* q, const QString& error) {
    QString line = symbol + QStringLiteral(" — ");
    if (!q)
        return line + QCoreApplication::translate("QuoteDisplayFormat", "failed: %1").arg(error);
    QStringList parts;
    const QString day = quote_session_text(q->session_date);
    parts << (day.isEmpty() ? QCoreApplication::translate("QuoteDisplayFormat", "session date unknown") : day);
    parts << quote_basis_text(*q);
    if (q->retrieved_at > 0)
        parts << QCoreApplication::translate("QuoteDisplayFormat", "fetched %1")
                     .arg(QDateTime::fromSecsSinceEpoch(q->retrieved_at).toString(QStringLiteral("HH:mm")));
    if (!error.isEmpty())
        parts << QCoreApplication::translate("QuoteDisplayFormat", "last refresh failed: %1").arg(error);
    return line + parts.join(QStringLiteral(" · "));
}

/// The footer a quote widget shows: which trading day(s) its numbers belong
/// to, which symbols failed (no value at all) and which kept an older value
/// after a failed refresh. `warn` is set when anything failed.
struct QuoteFeedSummary {
    QString text;
    QString tooltip;
    bool warn = false;
    QStringList failed;      ///< no value, last request failed
    QStringList not_updated; ///< showing an older value, last refresh failed
    int settled = 0;         ///< symbols with a value or a failure (for the loading progress)
};

inline QuoteFeedSummary quote_feed_summary(const QStringList& symbols, const QHash<QString, services::QuoteData>& cache,
                                           const QHash<QString, QString>& errors) {
    QuoteFeedSummary out;
    QVector<QDate> days;
    QStringList lines;
    for (const QString& sym : symbols) {
        const auto it = cache.constFind(sym);
        const services::QuoteData* q = it == cache.constEnd() ? nullptr : &it.value();
        const QString err = errors.value(sym);
        if (!q && err.isEmpty())
            continue;
        ++out.settled;
        if (!q)
            out.failed << sym;
        else if (!err.isEmpty())
            out.not_updated << sym;
        if (q && q->session_date.isValid() && !days.contains(q->session_date))
            days << q->session_date;
        lines << quote_status_line(sym, q, err);
    }
    std::sort(days.begin(), days.end());
    QStringList parts;
    if (!days.isEmpty()) {
        QStringList names;
        const int first = std::max<int>(0, static_cast<int>(days.size()) - 3);
        for (int i = first; i < days.size(); ++i)
            names << quote_session_text(days[i]);
        parts << QCoreApplication::translate("QuoteDisplayFormat", "Data: %1")
                     .arg((first > 0 ? QStringLiteral("… ") : QString()) + names.join(QStringLiteral(" · ")));
    }
    auto named = [](const QStringList& list) {
        QStringList shown = list.mid(0, 3);
        return shown.join(QStringLiteral(", ")) + (list.size() > 3 ? QStringLiteral(" …") : QString());
    };
    if (!out.failed.isEmpty())
        parts << QCoreApplication::translate("QuoteDisplayFormat", "%1 failed: %2")
                     .arg(out.failed.size())
                     .arg(named(out.failed));
    if (!out.not_updated.isEmpty())
        parts << QCoreApplication::translate("QuoteDisplayFormat", "%1 not refreshed: %2")
                     .arg(out.not_updated.size())
                     .arg(named(out.not_updated));
    out.text = parts.join(QStringLiteral(" · "));
    out.tooltip = lines.join(QLatin1Char('\n'));
    out.warn = !out.failed.isEmpty() || !out.not_updated.isEmpty();
    return out;
}

} // namespace fincept::screens
