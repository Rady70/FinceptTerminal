#include "services/ResearchDataEvents.h"

namespace fincept::services {

ResearchDataEvents& ResearchDataEvents::instance() {
    static ResearchDataEvents events;
    return events;
}

} // namespace fincept::services
