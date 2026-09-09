#pragma once

// MarketLab Terminal — single availability source (FINCEPT_FORK_PLAN.md §5.2).
//
// Default capabilities (plan §5.2):
//   - enabled:    local workspace, public data, user-configured providers,
//                 local analytics;
//   - conditional: external read-only data provider after its interface is
//                 integrated and qualified;
//   - disabled:   Fincept-hosted services, Fincept cloud sync, and external
//                 broker or exchange execution.
//
// Screens follow the plan's feature disposition (§6). Screens whose
// disposition is Unavailable (or Conditional-without-conditions-met) are
// denied by is_screen_allowed(), and every navigation surface must consult
// that function so unavailable screens stay unreachable through shortcuts,
// restored layouts, command aliases, MCP calls, workflows, and agents.

#include "core/capability/Capability.h"

#include <QString>
#include <QStringList>

namespace fincept::capability {

class CapabilityManager {
  public:
    static CapabilityManager& instance();

    ComponentAvailability availability(Capability cap) const;

    /// True when the capability's state is not Unavailable.
    bool is_available(Capability cap) const;

    /// Availability of a dock-screen id. Unknown ids are Unavailable.
    ComponentAvailability screen_availability(const QString& screen_id) const;

    /// True when a direct navigation to `screen_id` may proceed. This is the
    /// single gate every navigation surface (router, tab bar, command bar,
    /// MCP navigation tools, layout restore, component browser) must call.
    bool is_screen_allowed(const QString& screen_id) const;

    /// Filter `ids`, keeping only the allowed ones, in order.
    QStringList allowed_screens(const QStringList& ids) const;

  private:
    CapabilityManager() = default;
};

} // namespace fincept::capability
