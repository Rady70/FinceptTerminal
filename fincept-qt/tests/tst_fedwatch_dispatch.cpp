// Exercise the production EconomicsService and DataHub. Only process launch
// and persistent cache are replaced at link time: no Python/network/profile
// writes. These small recorders use the existing interfaces, not a new framework.
#include "datahub/DataHub.h"
#include "python/PythonRunner.h"
#include "services/economics/EconomicsService.h"
#include "storage/cache/CacheManager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

namespace {
struct Call {
    QString script;
    QStringList args;
};
QList<Call> calls;
int cache_reads = 0;
int cache_writes = 0;
} // namespace

namespace fincept::python {
PythonRunner::PythonRunner() = default;
PythonRunner& PythonRunner::instance() {
    static PythonRunner runner;
    return runner;
}
void PythonRunner::run(const QString& script, const QStringList& args, Callback cb, StreamCallback, const QByteArray&) {
    calls.append({script, args});
    cb({true, QStringLiteral("{\"success\":true,\"data\":{}}"), {}, 0});
}
QString extract_json(const QString& output) {
    return output;
}
} // namespace fincept::python

namespace fincept {
CacheManager::CacheManager(QObject* parent) : QObject(parent) {}
CacheManager& CacheManager::instance() {
    static CacheManager cache;
    return cache;
}
QVariant CacheManager::get(const QString&) const {
    ++cache_reads;
    return {};
}
void CacheManager::put(const QString&, const QVariant&, int, const QString&) {
    ++cache_writes;
}
void CacheManager::remove(const QString&) {}
} // namespace fincept

class FedwatchDispatchTest : public QObject {
    Q_OBJECT
  private slots:
    void explicit_dispatch_cannot_be_replayed() {
        auto& service = fincept::services::EconomicsService::instance();
        auto& hub = fincept::datahub::DataHub::instance();
        hub.set_coalesce_window_ms(0);
        service.ensure_registered_with_hub();
        QSignalSpy ready(&service, &fincept::services::EconomicsService::result_ready);
        const QString topic = QStringLiteral("econ:fedwatch:manual_october");
        service.execute("fedwatch", "fedwatch_data.py", "collect", {"--meeting", "2026-10-28"}, "manual_october");
        QCOMPARE(ready.count(), 1);
        QCOMPARE(calls.size(), 1);
        QCOMPARE(calls[0].args, (QStringList{"collect", "--meeting", "2026-10-28"}));
        QCOMPARE(cache_reads, 0);
        QCOMPARE(cache_writes, 0);
        QVERIFY(hub.peek_raw(topic).isValid());

        // Direct producer replay and actual hub dispatch both remain inert.
        service.refresh({topic});
        hub.request(topic, true);
        QCOMPARE(calls.size(), 1);
        QCOMPARE(ready.count(), 1);

        // Positive control: the real hub can replay another economics script.
        service.execute("fred", "fred_data.py", "series", {"GDP"}, "control", true);
        QCOMPARE(calls.size(), 2);
        hub.request("econ:fred:control", true);
        QTRY_COMPARE(calls.size(), 3); // Process the hub's queued coalesce dispatch.
        QCOMPARE(calls.last().script, QStringLiteral("fred_data.py"));
        QCOMPARE(cache_reads, 1);
        QCOMPARE(cache_writes, 1);
    }
};

QTEST_GUILESS_MAIN(FedwatchDispatchTest)
#include "tst_fedwatch_dispatch.moc"
