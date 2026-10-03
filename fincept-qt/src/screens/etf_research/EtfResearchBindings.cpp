#include "screens/etf_research/EtfResearchBindings.h"

#include "services/etf/research/EtfResearchEngine.h"
#include "services/etf/research/EtfResearchService.h"

namespace fincept::screens::etfr {

using namespace services::etf::research;

Result<ResearchSnapshot> load_research_snapshot(const QDateTime& as_of, const QDateTime& known_at) {
    auto in = EtfResearchService::instance().load(as_of, known_at);
    if (in.is_err())
        return Result<ResearchSnapshot>::err(in.error());
    return Result<ResearchSnapshot>::ok(compute_snapshot(in.value()));
}

void start_manual_refresh(RefreshProgress progress, std::function<void(const RefreshResult&)> done) {
    EtfResearchService::instance().refresh(QStringLiteral("manual_ui"), std::move(progress), std::move(done));
}

bool manual_refresh_running() {
    return EtfResearchService::instance().refresh_running();
}

} // namespace fincept::screens::etfr
