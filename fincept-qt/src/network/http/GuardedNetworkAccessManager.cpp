#include "network/http/GuardedNetworkAccessManager.h"

#include "core/logging/Logger.h"
#include "network/http/HostedPathGuard.h"

#include <QNetworkReply>
#include <QTimer>
#include <QVariant>

#include <memory>

namespace fincept::network {

namespace {

const QString TAG = QStringLiteral("GuardedNAM");

/// A reply that never touches the network: it is born finished and in error.
///
/// Qt gives no way to fail a request from createRequest() other than returning
/// a reply, and returning nullptr crashes the caller. The error is delivered on
/// the next event-loop turn rather than inside the constructor because every
/// caller connects its handlers to the returned reply *after* createRequest()
/// returns — signalling immediately would fire into nothing and the caller
/// would wait forever for a finished() that already happened.
/// Deliberately no Q_OBJECT: this declares no new signal or slot, it only
/// overrides virtuals and emits signals it inherits from QNetworkReply. Adding
/// the macro would require a "GuardedNetworkAccessManager.moc" include, an
/// idiom used nowhere else in src/ and one that can break the unity build the
/// release preset turns on (CMAKE_UNITY_BUILD=ON batches this TU with others).
class HostedRefusedReply : public QNetworkReply {
  public:
    // Operation is nested in QNetworkAccessManager, not in QNetworkReply, so it
    // has to be named in full here even though the manager passes it straight in.
    HostedRefusedReply(QObject* parent, const QNetworkRequest& request,
                       QNetworkAccessManager::Operation op, const QString& text)
        : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        setError(QNetworkReply::ContentAccessDenied, text);
        setFinished(true);
        QTimer::singleShot(0, this, [this, text]() {
            emit errorOccurred(QNetworkReply::ContentAccessDenied);
            emit finished();
        });
    }

    void abort() override {}
    qint64 readData(char*, qint64) override { return -1; }
};

} // namespace

GuardedNetworkAccessManager::GuardedNetworkAccessManager(QObject* parent)
    : QNetworkAccessManager(parent),
      denied_destination_(&HostedPathGuard::is_fincept_destination) {}

void GuardedNetworkAccessManager::setDeniedDestination(DestinationDeny deny) {
    denied_destination_ = deny ? std::move(deny)
                               : DestinationDeny(&HostedPathGuard::is_fincept_destination);
}

// This predicate exists because createRequest() replaces whatever policy the
// caller asked for with UserVerifiedRedirectPolicy, and several callers ask for
// NoLessSafeRedirectPolicy by name (WebScraperWidget, FeedMonitor, NewsService,
// the two RSS dialogs). Silently swapping their policy for one that only checks
// the deny-list would DROP the protection they requested and leave the app
// strictly less safe than before this guard existed. So the vetting callback
// re-applies the same two rules Qt applies:
//
//   - the target must speak http or https — a redirect that hands the URL to
//     some other scheme is a protocol change the caller never agreed to;
//   - https must not become http — a downgrade strips TLS from a route the
//     caller chose to encrypt.
//
// `from` is the URL the redirect is being followed FROM, not the original
// request URL, so a chain (https -> https -> http) is judged hop by hop the way
// Qt judges it.
bool GuardedNetworkAccessManager::redirect_is_less_safe(const QUrl& from, const QUrl& to) {
    const QString to_scheme = to.scheme().toLower();
    if (to_scheme != QLatin1String("http") && to_scheme != QLatin1String("https"))
        return true;
    return from.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 &&
           to_scheme == QLatin1String("http");
}

QNetworkReply* GuardedNetworkAccessManager::createRequest(Operation op, const QNetworkRequest& request,
                                                          QIODevice* outgoing_data) {
    const QUrl url = request.url();
    if (denied_destination_(url)) {
        const QString err = HostedPathGuard::unavailable_error(url);
        LOG_WARN(TAG, QStringLiteral("Refused a configuration-derived request to a Fincept-owned "
                                     "destination before any connection was made: %1")
                          .arg(err));
        return new HostedRefusedReply(this, request, op, err);
    }

    // The check above sees the URL the caller asked for and nothing else. Qt
    // follows a 3xx itself, inside the reply, and does NOT call createRequest()
    // again for the redirect target — established by observation against the Qt
    // 6.8.3 runtime this app pins, not assumed from the documentation. So an
    // allowed host answering a "302 Location:" that points at a Fincept-owned
    // destination reached that server with the deny-list none the wiser.
    //
    // UserVerifiedRedirectPolicy is the seam Qt provides for this: the reply
    // stops at each hop, emits redirected() with the resolved target, and makes
    // no connection until redirectAllowed() is emitted back. Refusing simply
    // means never emitting it and aborting, which was verified to leave the
    // forbidden server uncontacted (the reply finishes with
    // OperationCanceledError).
    //
    // ManualRedirectPolicy is left alone: a caller that asked for it is reading
    // the Location header itself and issuing the next request through this
    // manager, so that request re-enters createRequest() and is checked here.
    // Overriding it would auto-follow a hop the caller expects to handle.
    //
    // The attribute is read for VALIDITY first and only then for its value:
    // QNetworkRequest::ManualRedirectPolicy is 0, so an unset attribute — which
    // is most requests — would compare equal to it and silently opt every
    // ordinary caller out of the vetting below.
    const QVariant policy_attr = request.attribute(QNetworkRequest::RedirectPolicyAttribute);
    const bool caller_handles_redirects =
        policy_attr.isValid() &&
        policy_attr.value<QNetworkRequest::RedirectPolicy>() == QNetworkRequest::ManualRedirectPolicy;
    if (caller_handles_redirects)
        return QNetworkAccessManager::createRequest(op, request, outgoing_data);

    QNetworkRequest vetted(request);
    vetted.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                        QNetworkRequest::UserVerifiedRedirectPolicy);

    QNetworkReply* reply = QNetworkAccessManager::createRequest(op, vetted, outgoing_data);
    if (!reply)
        return reply;

    // The hop currently being followed FROM. Qt resolves each Location against
    // it before emitting redirected(), and the downgrade rule below needs the
    // previous hop's scheme, not the original request's.
    auto current = std::make_shared<QUrl>(url);
    QObject::connect(reply, &QNetworkReply::redirected, reply, [reply, current, this](const QUrl& target) {
        if (denied_destination_(target)) {
            LOG_WARN(TAG, QStringLiteral("Refused a redirect to a Fincept-owned destination before any "
                                         "connection was made: %1 -> %2")
                              .arg(current->toString(QUrl::RemoveQuery),
                                   HostedPathGuard::unavailable_error(target)));
            reply->abort();
            return;
        }
        if (redirect_is_less_safe(*current, target)) {
            LOG_WARN(TAG, QStringLiteral("Refused a redirect that weakens the transport before any "
                                         "connection was made: %1 -> %2")
                              .arg(current->toString(QUrl::RemoveQuery),
                                   target.toString(QUrl::RemoveQuery)));
            reply->abort();
            return;
        }
        *current = target;
        emit reply->redirectAllowed();
    });
    return reply;
}

} // namespace fincept::network
