// src/services/etf/EtfDerivedAnalytics.h
//
// ETF Capital Flows, Batch C: the entry point to derived values over the
// stored ETF data foundation (docs/ETF_FLOW_BATCH_C_IMPLEMENTATION.md in the
// control repository).
//
// It reads the append-only vintage store through EtfDataRepository and runs
// the two calculation families, which stay apart in every respect:
//
//   * regulatory-flow analytics (EtfRegulatoryFlowAnalytics.h): per SEC
//     reporting entity, monthly, measurement kind regulatory_reported_flow;
//   * market-rotation proxy measures (EtfRotationMeasures.h): per IBKR listed
//     instrument, per exchange session, measurement kind rotation_proxy.
//
// They describe different subjects (a reporting entity is not a listed
// instrument; Batch B stores no identity link between them yet), different
// frequencies and different measurement classes, and no value of one family
// is ever added to, weighted with or written into the other. Calculated daily
// creation/redemption flow has no enabled input route (D1-d): it is reported
// as ROUTE_DISABLED and no formula is run.
//
// Nothing is written. A run is a pure function of its request and time frame
// (as_of, known_at), both recorded in its output, and of what the store had
// recorded by known_at: the subjects, their tickers and identity links, and
// the vintages (used only once available at as_of). Names, exchange and
// currency are rewritten in place by later sightings, so no earlier value of
// them exists and the output leaves them out. The same request over the same
// store therefore gives the same bytes, and a result computed earlier is
// recomputed exactly after the store grows (for the persisted session rows,
// which carry no recording time, see EtfRotationMeasures.h).
#pragma once
#include "core/result/Result.h"
#include "services/etf/EtfDerivedModel.h"

#include <QDate>
#include <QJsonObject>
#include <QString>

#include <optional>

namespace fincept::services::etf {

struct DerivedRunRequest {
    DerivedTimeFrame frame;
    bool regulatory = true;                        ///< compute the regulatory-flow family
    bool rotation = true;                          ///< compute the rotation-proxy family
    std::optional<qint64> entity_id;               ///< only this reporting entity
    std::optional<qint64> instrument_id;           ///< only this listed instrument
    std::optional<qint64> reference_instrument_id; ///< the caller's declared reference for relative measures
    QDate output_from; ///< series output bounds (the calculation always uses the full history)
    QDate output_to;
};

/// The method block every output carries: versions, parameters, bases.
QJsonObject derived_methods_json();

/// Why a request cannot be run as stated, or an empty string when it can.
/// A request must name its frame and at least one family, and every part of
/// it must apply to a family it runs: an entity filter needs the regulatory
/// family, an instrument filter or a reference the rotation family. An output
/// range must not end before it starts. Nothing is dropped silently.
QString derived_request_problem(const DerivedRunRequest& request);

/// Compute the derived values of the subjects recorded by the knowledge
/// cutoff. Reads only. A request with a problem (derived_request_problem), or
/// a requested entity, instrument or reference that was not recorded by the
/// cutoff, is an error, not an empty result.
Result<QJsonObject> run_derived_calculations(const DerivedRunRequest& request);

} // namespace fincept::services::etf
