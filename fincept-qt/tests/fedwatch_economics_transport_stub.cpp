// Deterministic widget transport seam only. The production panel, base widgets,
// chart and presentation model are linked unchanged; these tests do NOT claim
// EconomicsService/PythonRunner end-to-end coverage. Any accidental call to the
// default transport fails immediately rather than making a network request.
#include "core/config/ProfileManager.h"
#include "core/session/ScreenStateManager.h"
#include "services/economics/EconomicsService.h"

#include <QDir>

namespace fincept {
ProfileManager& ProfileManager::instance() {
    static ProfileManager profile;
    return profile;
}
QString ProfileManager::profile_root() const {
    return QDir::tempPath() + "/fedwatch-widget-isolated-profile";
}
ScreenStateManager::ScreenStateManager(QObject* parent) : QObject(parent) {}
ScreenStateManager& ScreenStateManager::instance() {
    static ScreenStateManager state;
    return state;
}
void ScreenStateManager::notify_changed(screens::IStatefulScreen*) {
    qFatal("Standalone FedWatch widget test unexpectedly reached application screen-state persistence");
}
} // namespace fincept

namespace fincept::services {
EconomicsService::EconomicsService(QObject* parent) : QObject(parent) {}
EconomicsService& EconomicsService::instance() {
    static EconomicsService service;
    return service;
}
void EconomicsService::execute(const QString&, const QString&, const QString&, const QStringList&, const QString&,
                               bool) {
    qFatal("FedWatch widget test unexpectedly invoked production economics transport");
}
void EconomicsService::invalidate(const QString&) {}
void EconomicsService::ensure_registered_with_hub() {}
QStringList EconomicsService::topic_patterns() const {
    return {};
}
void EconomicsService::refresh(const QStringList&) {}
int EconomicsService::max_requests_per_sec() const {
    return 2;
}
} // namespace fincept::services
