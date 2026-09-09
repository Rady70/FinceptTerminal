#pragma once

// MarketLab Terminal — central capability interface (FINCEPT_FORK_PLAN.md §5.2).
//
// One small enum-based availability source used by startup, navigation, the
// component browser, direct screen routing, restored layouts, the command
// palette, MCP tool registration, and workflow-node registration. A capability
// that is Unavailable here must be unreachable through
// every one of those surfaces.

#include <QString>

namespace fincept::capability {

enum class Capability {
    LocalWorkspace,
    PublicData,
    LocalAnalytics,
    FinceptHosted,
    CloudSync,
    BrokerReadOnly,
    BrokerExecution
};

enum class AvailabilityState { Available, Conditional, Unavailable };

struct ComponentAvailability {
    AvailabilityState state = AvailabilityState::Unavailable;
    QString reason;
    QString replacement;
};

} // namespace fincept::capability
