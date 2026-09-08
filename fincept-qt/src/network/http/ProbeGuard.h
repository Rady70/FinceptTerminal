#pragma once

// MarketLab Terminal — connector-probe route decision (FINCEPT_FORK_PLAN.md §5.3).
//
// The sibling of DirectRouteGuard. That leaf holds the decision the three
// DIRECT HTTP clients make; this one holds the decision the four raw-socket
// CONNECTOR PROBES make. Their destination is not a literal anywhere in the
// source — it is assembled at run time out of a saved or imported connector
// config — which is exactly the configuration-derived hosted route §5.3 names
// and the reason hiding the hosted connectors in the UI does not close it:
//
//   src/screens/data_sources/ConnectionTester.cpp           — the TEST button
//   src/screens/data_sources/DataSourcesScreen_Handlers.cpp — the status poller
//   src/mcp/tools/DataSourcesTools.cpp                      — ds_test_connection
//   src/services/workflow/nodes/DataSourceNodes.cpp         — workflow node
//
// ── Why one leaf and not four copies of four lines ───────────────────────────
//
//  1. Copied four times, the decision had already drifted. ConnectionTester
//     judged the host it resolved from the config fields and then its worker
//     dialled the host it distilled from the probe URL — a DIFFERENT value. A
//     connector with {"host":"example.com","serviceRoot":"//api.fincept.in/svc"}
//     was judged on "example.com", passed, and opened a TCP connection to
//     api.fincept.in. Any URL-shaped config value with a scheme the old guard
//     did not recognise did the same.
//  2. None of the four sites can be reached from a unit test: they drag in the
//     data-sources screen, the MCP tool registry and the workflow node
//     registry, and tests/CMakeLists.txt forbids growing a test link line to
//     haul that in ("If a unit cannot be tested without dragging in half the
//     application, that is a signal about the unit, not about this file:
//     extract the pure logic into a small header (or a leaf .cpp) and test
//     that"). Extracted, the decision links against Qt Core alone —
//     tests/tst_direct_clients.cpp exercises it in a binary with no Qt Network
//     in it at all, so a rejection proven there is by construction a rejection
//     reached before any socket could exist.
//
// Deliberately NOT a networking layer, and it must not grow into one: no
// transport, no host list of its own (that stays in HostedPathGuard, so there
// is still exactly one deny-list), no policy beyond "may this probe open a
// socket".

#include <QString>

namespace fincept::network {

/// Outcome of judging one connector probe. When `rejected` is true the caller
/// must settle its own result with `error` and must NOT open a socket.
struct ProbeDecision {
    bool rejected = false;
    /// Typed HOSTED_SERVICE_UNAVAILABLE error (HostedPathGuard::unavailable_error);
    /// empty when the probe is allowed, so it cannot leak into a caller's result.
    QString error;
    /// The host that was judged — i.e. the one the probe would dial. For the
    /// caller's log line, which is the one thing that legitimately differs
    /// between the four sites.
    QString host;
};

class ProbeGuard {
  public:
    /// The one decision, called before a worker/thread/socket is created.
    ///
    /// `url_shaped` is whatever URL-ish string the connector config yielded: it
    /// may be empty, may carry no scheme at all ("//api.fincept.in/svc"), or a
    /// scheme this app never speaks ("redis://…"). `resolved_host` is the bare
    /// host the caller distilled, which may be empty.
    ///
    /// BOTH are judged, because the call sites resolve several candidate hosts
    /// (config "host", then "serverHostname"/"zkQuorum"/"brokers", then the
    /// probe URL) and the one they judge has not always been the one they dial.
    static ProbeDecision check(const QString& url_shaped, const QString& resolved_host);

    /// The socket choke point: judge exactly the host string that is about to be
    /// handed to QTcpSocket::connectToHost(). This call is the AUTHORITATIVE
    /// one — the value judged is by construction the value dialled. check()
    /// above should still run earlier so no thread or socket object is created
    /// for the common case, but that earlier call is an optimisation, not the
    /// guarantee.
    ///
    /// It cannot decide the path-scoped Fincept-Corporation GitHub rules (a
    /// bare host carries no path); those are check()'s job, on the same URL the
    /// host was distilled from.
    static ProbeDecision check_host(const QString& host);
};

} // namespace fincept::network
