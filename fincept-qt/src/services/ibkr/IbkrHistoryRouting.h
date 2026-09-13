// src/services/ibkr/IbkrHistoryRouting.h
//
// The routing policy for the retained Equity Research candle route's optional
// IBKR provider. Header-only and pure so the decision is unit-testable without
// the service's cache, Python and signal dependencies.
//
// Phase 5 accepts IBKR history only for explicitly routed symbols and only for
// periods that map to a single bounded IBKR request; every other case stays on
// the public provider, whose provenance names it. A routed-history failure is
// never allowed to leave the previously displayed series plotted under the
// failed provider's provenance.
#pragma once
#include <QLatin1String>
#include <QString>

namespace fincept::services::ibkr {

enum class IbkrHistoryRoute { Ibkr, PublicProvider };

enum class IbkrHistoryFailureDisposition {
    /// Emit an empty series before the error metadata so no old data remains
    /// visible with the failed provider's source label.
    ClearSeriesAndReport,
};

/// The IBKR duration for a chart period, or empty when the period is not a
/// single bounded request.
inline QString ibkr_duration_for_period(const QString& period) {
    const QString p = period.trimmed().toLower();
    if (p == QLatin1String("1mo"))
        return QStringLiteral("1 M");
    if (p == QLatin1String("3mo"))
        return QStringLiteral("3 M");
    if (p == QLatin1String("6mo"))
        return QStringLiteral("6 M");
    if (p == QLatin1String("1y"))
        return QStringLiteral("1 Y");
    return {};
}

inline IbkrHistoryRoute ibkr_history_route(bool configured, bool symbol_routed, const QString& period) {
    if (!configured || !symbol_routed)
        return IbkrHistoryRoute::PublicProvider;
    return ibkr_duration_for_period(period).isEmpty() ? IbkrHistoryRoute::PublicProvider : IbkrHistoryRoute::Ibkr;
}

/// A cached series may satisfy the IBKR route only when the sidecar that
/// produced it named IBKR; a public-provider cache entry must not be relabelled.
inline bool ibkr_cache_origin_is_ibkr(const QString& origin) {
    return origin == QLatin1String("ibkr_tws");
}

inline IbkrHistoryFailureDisposition ibkr_history_failure_disposition() {
    return IbkrHistoryFailureDisposition::ClearSeriesAndReport;
}

} // namespace fincept::services::ibkr
