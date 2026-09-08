#pragma once

// MarketLab Terminal — direct-client route decision (FINCEPT_FORK_PLAN.md §5.3).
//
// HostedPathGuard answers one question: "is this URL a Fincept-owned
// destination?". This leaf holds the *decision* the DIRECT clients make with
// that answer — the clients that own their own QNetworkAccessManager, so the
// shared HttpClient deny-list never sees their requests and each one has to
// reject for itself:
//
//   src/services/alpha_arena/ArenaLlmClient.cpp  — arena completion POST
//   src/services/quantlib/QuantLibClient.cpp     — hosted QuantLib suite
//   src/network/cloud/CloudClient.cpp            — finceptgo cloud sync
//
// Why this is its own translation unit rather than four lines repeated three
// times. Repeated, the decision is three independent places for the rejection
// to drift or be dropped, and NONE of them can be reached from a unit test:
// each client drags in AppConfig, AuthManager, a provider catalog, a cache and
// Qt Network, and tests/CMakeLists.txt forbids growing a test link line to
// haul that in ("extract the pure logic into a small header (or a leaf .cpp)
// and test that"). Extracted, there is one implementation, it is the one the
// production clients call, and tests/tst_direct_clients.cpp can link it
// against Qt Core alone — a binary with no Qt Network in it at all, so a
// rejection proven there is by construction a rejection reached before any
// socket could exist.
//
// Deliberately NOT a networking layer, and it must not grow into one: it
// composes a URL the way those clients compose one, and returns the guard's
// verdict plus the typed error the caller hands back. No transport, no
// policy of its own, no new hosts — the deny-list stays in HostedPathGuard.

#include <QString>
#include <QUrl>

namespace fincept::network {

/// Outcome of judging one direct-client route. When `rejected` is true the
/// caller must return `error` to its own callback WITHOUT touching the
/// network; `url` is the composed destination that was judged (useful for the
/// caller's log line, which is the one thing that legitimately differs
/// between the three clients).
struct DirectRouteDecision {
    bool rejected = false;
    QString error;
    QUrl url;
};

class DirectRouteGuard {
  public:
    /// Judge an already-composed absolute URL (ArenaLlmClient's case: the
    /// provider catalog hands it a finished endpoint).
    static DirectRouteDecision check_url(const QString& absolute_url);

    /// Compose `base` + `endpoint` and judge the result. An `endpoint` that is
    /// already absolute (starts with "http") wins over `base`, which is what
    /// CloudClient's callers rely on. QuantLibClient passes its
    /// "/quantlib/<endpoint>" path as `endpoint`.
    static DirectRouteDecision check_route(const QString& base, const QString& endpoint);

    /// The composition rule on its own, exposed so the composition and the
    /// verdict can be asserted separately in tests.
    static QString compose(const QString& base, const QString& endpoint);
};

} // namespace fincept::network
