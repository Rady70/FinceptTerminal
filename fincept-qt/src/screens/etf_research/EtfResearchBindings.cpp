#include "screens/etf_research/EtfResearchBindings.h"

#include "services/etf/research/EtfResearchEngine.h"
#include "services/etf/research/EtfResearchService.h"

namespace fincept::screens::etfr {

using namespace services::etf::research;

Result<ResearchSnapshot> load_research_snapshot(const QDateTime& as_of, const QDateTime& known_at) {
    // The Batch D groups take most of the read time and only the FLOW view's
    // group table uses them: they are read when that view first opens.
    auto in = EtfResearchService::instance().load(as_of, known_at, false);
    if (in.is_err())
        return Result<ResearchSnapshot>::err(in.error());
    return Result<ResearchSnapshot>::ok(compute_snapshot(in.value()));
}

Result<QVector<GroupFlowRow>> load_group_flows(const QDateTime& as_of, const QDateTime& known_at) {
    return EtfResearchService::instance().group_flows(as_of, known_at);
}

void start_manual_refresh(RefreshProgress progress, std::function<void(const RefreshResult&)> done) {
    EtfResearchService::instance().refresh(QStringLiteral("manual_ui"), std::move(progress), std::move(done));
}

bool manual_refresh_running() {
    return EtfResearchService::instance().refresh_running();
}

} // namespace fincept::screens::etfr
