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
    /// dormant markets.fincept.in.
    ///
    /// The HOST half is judged whatever the scheme is, including a
    /// scheme-relative "//host/path" and schemes this app never speaks
    /// (ftp:, redis:, mongodb:) — those shapes do arrive here, out of saved
    /// connector configuration. Only the path-scoped GitHub-org rules require
    /// http/https/ws/wss, because only there is a path meaningful. A URL with
    /// no host at all (file:///…, "api.fincept.in:443" which QUrl reads as a
    /// scheme) is false; loopback callbacks are false because 127.0.0.1 and
    /// localhost are not on the list.
    ///
    /// A trailing root-label dot is normalised away first: "api.fincept.in."
    /// is the same destination and QUrl preserves the dot. See the notes on
    /// normalized_host() in the .cpp for the verified behaviour and for the
    /// limits (punycode aliases, IP literals) that are deliberately accepted.
    static bool is_fincept_destination(const QUrl& url);

    /// True when `host` is a Fincept-owned hostname, independent of scheme.
    /// For raw host:port probes where no URL and therefore no path is
    /// available. URL-shaped input should ALSO go through
    /// is_fincept_destination(), because the Fincept-Corporation GitHub rules
    /// are path-scoped and this predicate cannot see a path.
    ///
    /// Input is trimmed, lower-cased and stripped of trailing root-label dots
    /// before comparison, so a raw config string ("  API.FINCEPT.IN. ") is
    /// judged the same as the host QUrl would hand over.
    static bool is_fincept_host(const QString& host);

    /// Typed error for a rejected destination, e.g.
    /// "HOSTED_SERVICE_UNAVAILABLE: api.fincept.in".
    static QString unavailable_error(const QUrl& url);

    /// Typed error for a rejected bare host, e.g.
    /// "HOSTED_SERVICE_UNAVAILABLE: api.fincept.in".
    static QString unavailable_error(const QString& host);

    /// True when `error` is a HostedServiceUnavailable error produced by
    /// unavailable_error() (or the bare prefix).
    static bool is_hosted_unavailable_error(const QString& error);
};

} // namespace fincept::network
