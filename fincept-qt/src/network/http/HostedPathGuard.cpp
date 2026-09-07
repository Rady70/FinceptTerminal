#include "network/http/HostedPathGuard.h"

#include <QStringList>
#include <QUrl>

namespace fincept::network {

namespace {

bool is_network_scheme(const QString& scheme) {
    const QString s = scheme.toLower();
    return s == QLatin1String("http") || s == QLatin1String("https") || s == QLatin1String("ws") ||
           s == QLatin1String("wss");
}

bool host_is_or_under(const QString& host, const QString& domain) {
    const QString h = host.toLower();
    return h == domain || h.endsWith(QLatin1Char('.') + domain);
}

} // namespace

const QString& HostedPathGuard::unavailable_prefix() {
    static const QString kPrefix = QStringLiteral("HOSTED_SERVICE_UNAVAILABLE");
    return kPrefix;
}

bool HostedPathGuard::is_fincept_destination(const QUrl& url) {
    if (!is_network_scheme(url.scheme()))
        return false;

    const QString host = url.host().toLower();
    if (host.isEmpty())
        return false;

    if (host_is_or_under(host, QStringLiteral("fincept.in")) ||
        host_is_or_under(host, QStringLiteral("fincept.com")) ||
        host_is_or_under(host, QStringLiteral("fincept.app")) ||
        host_is_or_under(host, QStringLiteral("fincept.ai")))
        return true;

    // Fincept-Corporation GitHub content (update manifest, docs assets).
    if (host == QLatin1String("raw.githubusercontent.com")) {
        return url.path().startsWith(QLatin1String("/Fincept-Corporation/"), Qt::CaseInsensitive) ||
               url.path().compare(QLatin1String("/Fincept-Corporation"), Qt::CaseInsensitive) == 0;
    }
    if (host == QLatin1String("github.com")) {
        return url.path().startsWith(QLatin1String("/Fincept-Corporation"), Qt::CaseInsensitive);
    }

    return false;
}

QString HostedPathGuard::unavailable_error(const QUrl& url) {
    return QStringLiteral("%1: %2").arg(unavailable_prefix(), url.host().isEmpty() ? url.toString() : url.host());
}

bool HostedPathGuard::is_hosted_unavailable_error(const QString& error) {
    return error.startsWith(unavailable_prefix());
}

} // namespace fincept::network
