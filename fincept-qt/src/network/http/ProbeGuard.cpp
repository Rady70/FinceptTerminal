#include "network/http/ProbeGuard.h"

#include "network/http/HostedPathGuard.h"

#include <QUrl>

namespace fincept::network {

namespace {

ProbeDecision rejected_for(const QString& host) {
    ProbeDecision d;
    d.rejected = true;
    d.host = host.trimmed();
    d.error = HostedPathGuard::unavailable_error(host);
    return d;
}

ProbeDecision allowed_for(const QString& host) {
    ProbeDecision d;
    d.host = host.trimmed();
    return d;
}

} // namespace

ProbeDecision ProbeGuard::check_host(const QString& host) {
    const QString h = host.trimmed();
    if (!h.isEmpty() && HostedPathGuard::is_fincept_host(h))
        return rejected_for(h);
    return allowed_for(h);
}

ProbeDecision ProbeGuard::check(const QString& url_shaped, const QString& resolved_host) {
    // 1. The bare host the caller resolved. Judged first because for three of
    //    the four call sites this IS the argument connectToHost() receives, and
    //    because it is the only shape available when the config supplies
    //    host + port with no URL anywhere.
    if (const auto by_host = check_host(resolved_host); by_host.rejected)
        return by_host;

    const QString raw_url = url_shaped.trimmed();
    if (raw_url.isEmpty())
        return allowed_for(resolved_host);

    const QUrl url(raw_url);

    // 2. The URL form. Only this shape can decide the path-scoped
    //    Fincept-Corporation GitHub rules (github.com and
    //    raw.githubusercontent.com are Fincept-owned only under
    //    /Fincept-Corporation/, so they are absent from the bare-host list and
    //    a host-only check would let them through). is_fincept_destination()
    //    judges the host half whatever the scheme is, so a scheme-relative
    //    "//api.fincept.in/svc" or an "ftp://api.fincept.in/x" is caught here
    //    too.
    if (HostedPathGuard::is_fincept_destination(url))
        return rejected_for(url.host().isEmpty() ? raw_url : url.host());

    // 3. The host the URL form distils, judged on its own. For ConnectionTester's
    //    URL branch this string is literally the argument connectToHost()
    //    receives, and it is NOT always equal to `resolved_host` — that
    //    divergence is the bug this leaf exists to make impossible. Step 2
    //    already covers every URL that parses to a host, so this is defence in
    //    depth against a future change to is_fincept_destination()'s scheme
    //    handling rather than a second rule; it costs one comparison.
    if (const auto by_url_host = check_host(url.host()); by_url_host.rejected)
        return by_url_host;

    return allowed_for(resolved_host.trimmed().isEmpty() ? url.host() : resolved_host);
}

} // namespace fincept::network
