#pragma once

// MarketLab Terminal — hosted-destination guard for direct QNetworkAccessManager
// owners (FINCEPT_FORK_PLAN.md §5.3).
//
// Several retained surfaces fetch a URL that arrives from *configuration* rather
// than from source: the web-scraper widget, RSS feed monitoring and previews,
// the workflow HTTP/RSS bridges and the candle fetcher. They each own a
// QNetworkAccessManager, so the shared HttpClient deny-list never sees their
// requests, and because the destination is typed or imported by the user it can
// name a Fincept host without any literal ever appearing in the tree.
//
// Guarding them at `createRequest()` rather than at each call site is deliberate
// and is the smaller change: it is the single function every get/post/put on a
// manager passes through, so a request cannot be added later that forgets the
// check, and a refusal is delivered as an ordinary QNetworkReply error — which
// means each caller's EXISTING failure path reports it, with no new error
// plumbing invented at eleven call sites.
//
// `createRequest()` alone is NOT sufficient, however. It sees only the URL the
// caller asked for: Qt follows a 3xx inside the reply and does not re-enter
// this override for the redirect target, so `allowed.example` answering a
// "302 Location:" that points at a Fincept-owned host reached that host with
// the deny-list none the wiser. That is not an inference from the docs —
// it was established by observing the Qt 6.8.3 runtime this app pins, where
// createRequest() was called exactly once for a request that was redirected and
// the redirect target was contacted. So each reply is additionally put on
// QNetworkRequest::UserVerifiedRedirectPolicy and every hop is vetted before Qt
// is allowed to follow it; see the notes in the .cpp for what the vetting keeps
// (the NoLessSafeRedirectPolicy rules several callers ask for by name) and why.
//
// This is a guard, not a networking layer: it adds no transport, no policy of
// its own and no hosts. The deny-list stays in HostedPathGuard.

#include <QNetworkAccessManager>

#include <functional>

class QUrl;

namespace fincept::network {

class GuardedNetworkAccessManager : public QNetworkAccessManager {
    Q_OBJECT
  public:
    /// The predicate that decides whether a destination must not be contacted.
    /// Defaults to HostedPathGuard::is_fincept_destination; tests inject a
    /// predicate over a hermetic fake host so a broken guard can never dial
    /// the real Fincept destination it would otherwise have to name.
    using DestinationDeny = std::function<bool(const QUrl&)>;

    explicit GuardedNetworkAccessManager(QObject* parent = nullptr);

    /// Replace the denied-destination predicate (tests/tst_redirect_guard.cpp
    /// injects a fake-host deny so the redirect transport cases never need the
    /// real Fincept destination URL as a dialable target). A null callback
    /// restores the default. Both check sites — the initial URL and every
    /// redirect hop — consult the same member, so the injection cannot
    /// desynchronise them.
    void setDeniedDestination(DestinationDeny deny);

    /// True when following `from` -> `to` would weaken the transport: the
    /// target speaks something other than http/https, or https becomes http.
    /// These are exactly the two rules QNetworkRequest::NoLessSafeRedirectPolicy
    /// enforces, and re-applying them is what stops the policy swap in
    /// createRequest() from costing the callers that ask for that policy by
    /// name the protection they asked for.
    ///
    /// Public only so tests/tst_redirect_guard.cpp can assert it directly. It
    /// cannot be reached end-to-end from a hermetic test: Qt follows a redirect
    /// by restarting the SAME http reply implementation, so a non-http target
    /// dies in the transport whether or not this returns true, and exercising
    /// the https->http rule would need a local TLS server. It is a pure
    /// predicate over two URLs — no state, no hosts, no policy of its own.
    static bool redirect_is_less_safe(const QUrl& from, const QUrl& to);

  protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoing_data) override;

  private:
    DestinationDeny denied_destination_;
};

} // namespace fincept::network
