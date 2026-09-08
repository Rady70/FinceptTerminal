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
// This is a guard, not a networking layer: it adds no transport, no policy of
// its own and no hosts. The deny-list stays in HostedPathGuard.

#include <QNetworkAccessManager>

namespace fincept::network {

class GuardedNetworkAccessManager : public QNetworkAccessManager {
    Q_OBJECT
  public:
    explicit GuardedNetworkAccessManager(QObject* parent = nullptr);

  protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoing_data) override;
};

} // namespace fincept::network
