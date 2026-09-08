#include "network/http/GuardedNetworkAccessManager.h"

#include "core/logging/Logger.h"
#include "network/http/HostedPathGuard.h"

#include <QNetworkReply>
#include <QTimer>

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

GuardedNetworkAccessManager::GuardedNetworkAccessManager(QObject* parent) : QNetworkAccessManager(parent) {}

QNetworkReply* GuardedNetworkAccessManager::createRequest(Operation op, const QNetworkRequest& request,
                                                          QIODevice* outgoing_data) {
    const QUrl url = request.url();
    if (HostedPathGuard::is_fincept_destination(url)) {
        const QString err = HostedPathGuard::unavailable_error(url);
        LOG_WARN(TAG, QStringLiteral("Refused a configuration-derived request to a Fincept-owned "
                                     "destination before any connection was made: %1")
                          .arg(err));
        return new HostedRefusedReply(this, request, op, err);
    }
    return QNetworkAccessManager::createRequest(op, request, outgoing_data);
}

} // namespace fincept::network
