// src/screens/report_builder/ReportQuoteFormat.h
//
// QuoteData → report-builder market_data component config.
//
// Extracted from ReportBuilderScreen.cpp so the "missing is not zero" rule for
// this retained surface is unit-testable without linking the screen, the
// DataHub and the whole widget tree (the same reason MarketQuoteParse.h
// exists for the live market-data paths). Header-only, Qt Core only.
#pragma once
#include "services/markets/MarketDataService.h"

#include <QMap>
#include <QString>

namespace fincept::screens {

/// The market_data component config keys the canvas renders. An empty string
/// means "no reading to print", a formatted number means the reading that
/// actually arrived — the per-field presence flags decide which it is, so a
/// missing price renders as nothing while a genuine zero renders as "0.00"
/// (FINCEPT_FORK_PLAN.md §4: missing must never become zero, and zero must
/// never be mistaken for missing).
///
/// `status` is the QuoteData retrieval-status token (OK / PARTIAL / STALE)
/// propagated verbatim, rather than flattened to "ok" — a PARTIAL row says so.
inline QMap<QString, QString> quote_report_config(const fincept::services::QuoteData& q) {
    QMap<QString, QString> c;
    c[QStringLiteral("price")] = q.has_price ? QString::number(q.price, 'f', 2) : QString();
    c[QStringLiteral("change")] = q.has_change ? QString::number(q.change, 'f', 2) : QString();
    c[QStringLiteral("change_pct")] = q.has_change_pct ? QString::number(q.change_pct, 'f', 2) : QString();
    c[QStringLiteral("name")] = q.name;
    c[QStringLiteral("high")] = q.has_high ? QString::number(q.high, 'f', 2) : QString();
    c[QStringLiteral("low")] = q.has_low ? QString::number(q.low, 'f', 2) : QString();
    c[QStringLiteral("volume")] = q.has_volume ? QString::number(q.volume, 'f', 0) : QString();
    c[QStringLiteral("status")] = q.status;
    return c;
}

} // namespace fincept::screens
