// src/screens/etf_research/EtfResearchBindings.h
//
// The two service calls the ETF Flow & Sector Rotation screen makes. The
// application defines them in EtfResearchBindings.cpp (the research service);
// UI tests define stubs that count calls, so a test can prove that showing the
// screen performs a store read and no acquisition.
#pragma once
#include "core/result/Result.h"
#include "services/etf/research/EtfResearchRefreshTypes.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <functional>

namespace fincept::screens::etfr {

/// Read stored observations and compute the snapshot (worker thread). No acquisition.
Result<services::etf::research::ResearchSnapshot> load_research_snapshot(const QDateTime& as_of,
                                                                         const QDateTime& known_at);

/// Start the user-initiated manual refresh (main thread).
void start_manual_refresh(services::etf::research::RefreshProgress progress,
                          std::function<void(const services::etf::research::RefreshResult&)> done);

/// True while a manual refresh runs.
bool manual_refresh_running();

} // namespace fincept::screens::etfr
