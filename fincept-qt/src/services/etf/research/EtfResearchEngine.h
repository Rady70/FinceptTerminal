// src/services/etf/research/EtfResearchEngine.h
//
// Assemble one ResearchSnapshot from ResearchInputs. Pure: no database, no
// network, no clock (the frame's as_of/known_at come from the inputs and
// `computed_at` is the only wall-clock field, excluded from the JSON digest).
#pragma once
#include "services/etf/research/EtfResearchInputs.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <QJsonObject>

namespace fincept::services::etf::research {

ResearchSnapshot compute_snapshot(const ResearchInputs& in);

/// Full machine-readable export of a snapshot (headless CLI, tests, review).
/// `include_series` adds RRG trails, flow intervals and correlation matrices.
QJsonObject snapshot_to_json(const ResearchSnapshot& s, bool include_series = true);

/// Map Yahoo's quote-summary fields to fund facts with an explicit unit table.
FundFacts fund_facts_from_captures(const QVector<FundCapture>& caps, const BarSeries* bars,
                                   const HoldingsCapture* holdings, const QDate& expected_session);

} // namespace fincept::services::etf::research
