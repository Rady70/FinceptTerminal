// src/services/etf/EtfRoutePolicy.h
//
// Which acquisition routes the ETF data foundation may use, and which flow
// routes exist for a listed ETF (ETF Capital Flows Batch B). This is the
// project decision recorded by Batch A2 (ETF_FLOW_BATCH_A2_QUALIFICATION.md
// sections 3, 10 and 12) and the owner's free-only policy
// (ETF_FLOW_FREE_DATA_STRATEGY.md), in code, versioned, so every stored
// retrieval can name the policy it ran under.
//
// A disabled route is refused BEFORE any request is made and is recorded as
// ROUTE_DISABLED, which is a different state from a missing observation.
// Nothing here activates a deferred route; enabling one needs a new policy
// version, a schema change (v052 refuses its rows) and its own review.
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfDataModel.h"

#include <QString>
#include <QVector>

#include <optional>

namespace fincept::services::etf {

inline constexpr const char* kEtfRoutePolicyVersion = "etf_route_policy_v1";

struct RouteDecision {
    bool enabled = false;
    QString reason; ///< stable explanation token + text, stored with a refusal
};

/// The acquisition policy for a (source, mode) pair.
inline RouteDecision acquisition_route(SourceType source, AcquisitionMode mode) {
    if (mode == AcquisitionMode::RegulatoryApi) {
        switch (source) {
            case SourceType::SecNport:
                return {true, QStringLiteral("sec_edgar_regulatory_api: automated access permitted within the SEC "
                                             "fair-access policy with a declared User-Agent (A2 section 3)")};
            case SourceType::SecSubmissions:
                return {true, QStringLiteral("sec_edgar_regulatory_api: filing and submission metadata for "
                                             "provenance and timing (A2 section 10)")};
            case SourceType::SecPeriodicXbrl:
                return {false, QStringLiteral("d7_candidate_only: 10-Q/10-K creation and redemption facts are "
                                              "candidates, not a qualified quarterly flow stream (A2 section 6.2)")};
            case SourceType::IssuerFile:
            case SourceType::IbkrTwsReadonly:
                break;
        }
        return {false, QStringLiteral("undefined_route: this source is not reached through the SEC API")};
    }
    if (mode == AcquisitionMode::IbkrReadonlyWrapper) {
        if (source == SourceType::IbkrTwsReadonly)
            return {true, QStringLiteral("ibkr_readonly_wrapper: the user's own TWS through the existing read-only "
                                         "boundary; market-data role only (A2 section 7)")};
        return {false, QStringLiteral("undefined_route: only IBKR market data uses the IBKR wrapper")};
    }
    if (mode == AcquisitionMode::UserInitiatedImport)
        return {false, QStringLiteral("d1d_no_permission_basis: user-initiated issuer import stays disabled until a "
                                      "sufficient permission or use basis is established (A2 section 4)")};
    // A2 established the absence of a permission, not a universal prohibition:
    // four examined issuers prohibit automated collection, three grant no
    // permission for it, and others were not examined.
    return {false, QStringLiteral("issuer_automation_permission_not_established: no permission or use basis for "
                                  "automated collection from an issuer site is established (A2 sections 2 and 3)")};
}

// ── Flow-route availability per listed ETF (A2 section 10) ───────────────────
enum class FlowRoute { RegulatoryMonthly, RegulatoryQuarterly, CalculatedDaily };

inline const char* flow_route_id(FlowRoute r) {
    switch (r) {
        case FlowRoute::RegulatoryMonthly:
            return "regulatory_monthly";
        case FlowRoute::RegulatoryQuarterly:
            return "regulatory_quarterly";
        case FlowRoute::CalculatedDaily:
            return "calculated_daily";
    }
    return "";
}

enum class FlowRouteAvailability {
    Available,              ///< enabled and qualified; data may still be missing for a period
    RouteDisabled,          ///< no permitted route (calculated daily flow: D1-d)
    NotQualified,           ///< accessible candidate facts, not a qualified flow stream (D7)
    NotApplicable,          ///< the route cannot describe this ETF (an ETF class of a multi-class series)
    IdentityNotEstablished, ///< no stored link from the listed ETF to an SEC reporting entity
};

inline const char* flow_route_availability_id(FlowRouteAvailability a) {
    switch (a) {
        case FlowRouteAvailability::Available:
            return "available";
        case FlowRouteAvailability::RouteDisabled:
            return "route_disabled";
        case FlowRouteAvailability::NotQualified:
            return "not_qualified";
        case FlowRouteAvailability::NotApplicable:
            return "not_applicable";
        case FlowRouteAvailability::IdentityNotEstablished:
            return "identity_not_established";
    }
    return "";
}

/// The quality state a consumer shows for an unavailable route. An available
/// route has no state of its own: its periods are CONFIRMED or MISSING.
inline std::optional<QualityState> flow_route_quality_state(FlowRouteAvailability a) {
    switch (a) {
        case FlowRouteAvailability::Available:
            return std::nullopt;
        case FlowRouteAvailability::RouteDisabled:
            return QualityState::RouteDisabled;
        case FlowRouteAvailability::NotApplicable:
            return QualityState::NotApplicable;
        case FlowRouteAvailability::NotQualified:
        case FlowRouteAvailability::IdentityNotEstablished:
            return QualityState::Missing;
    }
    return QualityState::Missing;
}

struct FlowRouteStatus {
    FlowRoute route = FlowRoute::RegulatoryMonthly;
    FlowRouteAvailability availability = FlowRouteAvailability::IdentityNotEstablished;
    QString reason;
};

/// Flow-route availability for one listed ETF, given the relationship of its
/// stored link to an N-PORT reporting entity (nullopt: no link stored).
inline QVector<FlowRouteStatus> flow_route_availability(std::optional<LinkRelationship> nport_link) {
    QVector<FlowRouteStatus> out;
    FlowRouteStatus monthly;
    monthly.route = FlowRoute::RegulatoryMonthly;
    if (!nport_link) {
        monthly.availability = FlowRouteAvailability::IdentityNotEstablished;
        monthly.reason = QStringLiteral("no stored link to an SEC reporting entity");
    } else if (*nport_link == LinkRelationship::ClassOfMultiClassSeries) {
        monthly.availability = FlowRouteAvailability::NotApplicable;
        monthly.reason = QStringLiteral("N-PORT flows are reported per series; this ETF is one class of a multi-class "
                                        "series, so the series flow is not the ETF's flow (A2 section 6.1)");
    } else {
        monthly.availability = FlowRouteAvailability::Available;
        monthly.reason = QStringLiteral("SEC N-PORT monthly flow of the linked reporting entity");
    }
    out.append(monthly);
    out.append({FlowRoute::RegulatoryQuarterly, FlowRouteAvailability::NotQualified,
                QStringLiteral("D7: trust and pool 10-Q/10-K creation and redemption facts are candidates only")});
    out.append({FlowRoute::CalculatedDaily, FlowRouteAvailability::RouteDisabled,
                QStringLiteral("D1-d: the only free daily route is an issuer-file import, disabled until a permission "
                               "basis is established")});
    return out;
}

} // namespace fincept::services::etf
