#pragma once

// MarketLab Terminal — hosted-service containment guard (FINCEPT_FORK_PLAN.md §5.3).
//
// One deny-list for Fincept-owned destinations, applied in every shared or
// directly owned network client. The rejections in shared clients are one
// defense; inventoried direct paths (QNAM, QWebSocket, Python, embedded web,
// external-browser launch) are removed or disabled at their own sites, and
// the static source audit (marketlab/audit_hosted_paths.py) fails when a new
// Fincept-owned host appears without an explicit disposition.
//
// The error text is a typed code: callers match the prefix
// kHostedUnavailablePrefix and the host name, not the human wording.

#include <QString>

class QUrl;

namespace fincept::network {

class HostedPathGuard {
  public:
    /// Typed error prefix used by every client so callers can detect the
    /// hosted-unavailable class without string-matching prose.
    static const QString& unavailable_prefix();

    /// True when `url` names a Fincept-owned destination and must not be
    /// contacted: fincept.in (and subdomains), fincept.com / .app / .ai, the
    /// Fincept GitHub org (raw.githubusercontent + github.com paths), and the
    /// dormant markets.fincept.in. Only http/https/ws/wss schemes are
    /// considered (localhost loopback callbacks for auth flows are excluded).
    static bool is_fincept_destination(const QUrl& url);

    /// Typed error for a rejected destination, e.g.
    /// "HOSTED_SERVICE_UNAVAILABLE: api.fincept.in".
    static QString unavailable_error(const QUrl& url);

    /// True when `error` is a HostedServiceUnavailable error produced by
    /// unavailable_error() (or the bare prefix).
    static bool is_hosted_unavailable_error(const QString& error);
};

} // namespace fincept::network
