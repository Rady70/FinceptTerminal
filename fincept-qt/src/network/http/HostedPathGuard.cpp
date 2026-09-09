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

// Normalise a hostname to the form the deny-list compares against: trimmed,
// lower-cased, and with the empty root label removed.
//
// The root label is the reason this function exists. "api.fincept.in." is a
// valid ABSOLUTE FQDN — it resolves to exactly the same address as
// "api.fincept.in", and QTcpSocket/QNAM dial it happily — and QUrl PRESERVES
// the dot rather than folding it away. Verified empirically against the Qt
// this app pins (6.8.3, msvc2022_64):
//
//     QUrl("https://api.fincept.in./v1/x").host()  ==  "api.fincept.in."
//     QUrl("//api.fincept.in./svc").host()         ==  "api.fincept.in."
//
// so before this normalisation a connector row, provider base_url or shipped
// default with one extra character walked past EVERY branch below, and with
// it past the shared HttpClient deny-list, CloudClient, QuantLibClient,
// ArenaLlmClient and all four connector probes at once. Qt also folds the
// Unicode full-stop variants (U+3002, U+FF0E, U+FF61) to ASCII '.' while
// parsing, so "api.fincept.in。" arrives here as an ordinary trailing dot
// and is stripped by the same loop. More than one trailing dot is stripped
// too: QUrl rejects that form outright (host() comes back empty), but a raw
// "host" field read straight out of a saved connector config never passes
// through QUrl at all.
//
// ── Accepted limits, stated rather than left silent ──────────────────────────
//
//  * Userinfo needs nothing here. QUrl splits it off before host(), verified:
//    QUrl("http://x@api.fincept.in/").host() == "api.fincept.in" (rejected),
//    and QUrl("http://api.fincept.in@api.anthropic.com/").host() ==
//    "api.anthropic.com" (allowed — the destination really is Anthropic, the
//    Fincept-looking text is only a username).
//  * Case and IDN folding of a parsed URL is Qt's: QUrl lower-cases and
//    ACE-normalises the host while parsing. normalized_host() lower-cases
//    again for the raw-string form (a config "host" field) that never went
//    through QUrl.
//  * A punycode label that DECODES to a Fincept domain is not matched. No
//    Fincept domain has a non-ASCII form, so there is no alias to fold to; a
//    general IDN-fold here would be new machinery for an empty set.
//  * An IP literal for a Fincept host is not matched, and deliberately so.
//    This is a NAME deny-list; it cannot enumerate hosting addresses that
//    change under it. The containment this guard implements is against
//    configuration and shipped defaults that NAME Fincept, not against a user
//    who deliberately types the address behind one.
QString normalized_host(const QString& host) {
    QString h = host.trimmed().toLower();
    while (h.endsWith(QLatin1Char('.')))
        h.chop(1);
    return h;
}

// `host` must already have been through normalized_host().
bool host_is_or_under(const QString& host, const QString& domain) {
    return host == domain || host.endsWith(QLatin1Char('.') + domain);
}

} // namespace

const QString& HostedPathGuard::unavailable_prefix() {
    static const QString kPrefix = QStringLiteral("HOSTED_SERVICE_UNAVAILABLE");
    return kPrefix;
}

bool HostedPathGuard::is_fincept_destination(const QUrl& url) {
    const QString host = normalized_host(url.host());
    if (host.isEmpty())
        return false;

    // Domain matching lives in is_fincept_host() so there is one list, not two:
    // a new Fincept domain added there is picked up by both predicates.
    //
    // Judged BEFORE the scheme is looked at, on purpose: a Fincept-owned host
    // is Fincept-owned whatever protocol is spoken to it, and the URL-shaped
    // values that reach this guard are not all http(s) — a connector config can
    // name "ftp://api.fincept.in/x", "redis://api.fincept.in:6379" or the
    // scheme-relative "//api.fincept.in/svc" (QUrl parses the host out of all
    // three; verified). Gating the host check on the scheme, as this used to,
    // made every one of those forms a clean pass.
    if (is_fincept_host(host))
        return true;

    // The Fincept-Corporation GitHub rules below are PATH-scoped, and a path is
    // only meaningful over a scheme this app actually requests with. Anything
    // else (file:, data:, qrc:, a bare "host:port" that QUrl reads as a scheme)
    // stops here.
    if (!is_network_scheme(url.scheme()))
        return false;

    // Fincept-Corporation GitHub content (update manifest, docs assets).
    if (host == QLatin1String("raw.githubusercontent.com")) {
        return url.path().startsWith(QLatin1String("/Fincept-Corporation/"), Qt::CaseInsensitive) ||
               url.path().compare(QLatin1String("/Fincept-Corporation"), Qt::CaseInsensitive) == 0;
    }
    if (host == QLatin1String("github.com")) {
        return url.path().startsWith(QLatin1String("/Fincept-Corporation/"), Qt::CaseInsensitive) ||
               url.path().compare(QLatin1String("/Fincept-Corporation"), Qt::CaseInsensitive) == 0;
    }

    return false;
}

bool HostedPathGuard::is_fincept_host(const QString& host) {
    const QString h = normalized_host(host);
    if (h.isEmpty())
        return false;

    // Deliberately absent here: github.com and raw.githubusercontent.com. Those
    // hosts are Fincept-owned only under /Fincept-Corporation/ paths, and a
    // bare host:port probe carries no path to judge — denying them on the host
    // alone would block unrelated third-party GitHub endpoints. Callers that
    // do hold a URL must run it through is_fincept_destination() as well.
    return host_is_or_under(h, QStringLiteral("fincept.in")) || host_is_or_under(h, QStringLiteral("fincept.com")) ||
           host_is_or_under(h, QStringLiteral("fincept.app")) || host_is_or_under(h, QStringLiteral("fincept.ai"));
}

// The two error builders report the host AS IT WAS GIVEN, not normalized_host()'d:
// the message is a diagnostic, and "HOSTED_SERVICE_UNAVAILABLE: api.fincept.in."
// tells whoever reads the log which exact string was configured. Callers match
// the prefix, never the host half.
QString HostedPathGuard::unavailable_error(const QUrl& url) {
    return QStringLiteral("%1: %2").arg(unavailable_prefix(), url.host().isEmpty() ? url.toString() : url.host());
}

QString HostedPathGuard::unavailable_error(const QString& host) {
    return QStringLiteral("%1: %2").arg(unavailable_prefix(), host.trimmed());
}

bool HostedPathGuard::is_hosted_unavailable_error(const QString& error) {
    return error.startsWith(unavailable_prefix());
}

} // namespace fincept::network
