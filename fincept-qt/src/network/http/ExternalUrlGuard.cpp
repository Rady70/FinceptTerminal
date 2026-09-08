#include "network/http/ExternalUrlGuard.h"

#include "core/logging/Logger.h"
#include "network/http/HostedPathGuard.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>

namespace fincept::network {

bool ExternalUrlGuard::open_external(const QUrl& url, QWidget* parent) {
    if (!HostedPathGuard::is_fincept_destination(url))
        return QDesktopServices::openUrl(url);

    const QString error = HostedPathGuard::unavailable_error(url);
    LOG_WARN("ExternalUrlGuard", QString("Refused browser launch of %1 — %2").arg(url.host(), error));

    // Plain QMessageBox::warning, the same call the screens that already
    // report a failure to the user make (GovDataProviderPanel export, the RSS
    // feed dialogs) — the point is that the user learns why the click did
    // nothing, not that this guard gets a dialog style of its own.
    if (parent) {
        QMessageBox::warning(
            parent, QCoreApplication::translate("ExternalUrlGuard", "Link not opened"),
            QCoreApplication::translate("ExternalUrlGuard",
                                        "This link points at a Fincept-hosted service, which this build does "
                                        "not contact.\n\n%1")
                .arg(error));
    }
    return false;
}

} // namespace fincept::network
