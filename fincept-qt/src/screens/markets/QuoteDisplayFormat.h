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
#include <QString>

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

} // namespace fincept::screens
