#include "network/http/DirectRouteGuard.h"

#include "network/http/HostedPathGuard.h"

namespace fincept::network {

QString DirectRouteGuard::compose(const QString& base, const QString& endpoint) {
    // An absolute endpoint wins over the configured base — CloudClient's
    // callers pass full URLs for a few endpoints and relative paths for the
    // rest, and both must be judged, not just the relative ones.
    return endpoint.startsWith(QLatin1String("http")) ? endpoint : (base + endpoint);
}

DirectRouteDecision DirectRouteGuard::check_url(const QString& absolute_url) {
    DirectRouteDecision d;
    d.url = QUrl(absolute_url);
    if (!HostedPathGuard::is_fincept_destination(d.url))
        return d;
    d.rejected = true;
    d.error = HostedPathGuard::unavailable_error(d.url);
    return d;
}

DirectRouteDecision DirectRouteGuard::check_route(const QString& base, const QString& endpoint) {
    return check_url(compose(base, endpoint));
}

} // namespace fincept::network
