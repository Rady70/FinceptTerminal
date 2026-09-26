// Qt's own messages reach the Logger only while the application object exists
// (src/core/logging/QtMessageRouting.h). Qt keeps calling the installed handler
// until the process is gone, including while Windows unloads Qt's DLLs inside
// ExitProcess, after the Logger is destroyed and every other thread has been
// terminated. Forwarding a message there hung the headless commands at exit.

#include "core/logging/Logger.h"
#include "core/logging/QtMessageRouting.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QtMessageHandler installed_handler() {
    const QtMessageHandler handler = qInstallMessageHandler(nullptr);
    qInstallMessageHandler(handler);
    return handler;
}

QByteArray contents(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TestQtMessageRouting : public QObject {
    Q_OBJECT

  private slots:
    void routes_to_the_logger_only_while_the_application_exists();
};

void TestQtMessageRouting::routes_to_the_logger_only_while_the_application_exists() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString during = dir.filePath("during.log");
    const QString after = dir.filePath("after.log");

    int argc = 1;
    char name[] = "tst_qt_message_routing";
    char* argv[] = {name, nullptr};
    {
        QCoreApplication app(argc, argv);
        fincept::Logger::instance().set_file(during);
        fincept::install_qt_message_routing();
        qWarning("routing probe: the application exists");
        QVERIFY(installed_handler() != &fincept::discard_qt_message);
        QVERIFY(contents(during).contains("routing probe: the application exists"));
    }

    // The application object is gone. Even with the Logger writable again, a Qt
    // message no longer reaches it.
    QVERIFY(installed_handler() == &fincept::discard_qt_message);
    fincept::Logger::instance().set_file(after);
    qWarning("routing probe: the application is gone");
    fincept::Logger::instance().flush_and_close();
    QVERIFY(!contents(after).contains("routing probe: the application is gone"));
}

QTEST_APPLESS_MAIN(TestQtMessageRouting)
#include "tst_qt_message_routing.moc"
