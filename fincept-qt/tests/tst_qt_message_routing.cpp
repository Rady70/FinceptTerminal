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
    // Declared first: it needs the process's first Logger call to come before
    // any application object exists.
    void logger_used_before_the_application_still_closes_with_it();
    void routes_to_the_logger_only_while_the_application_exists();
};

void TestQtMessageRouting::logger_used_before_the_application_still_closes_with_it() {
    // main() can log before the application object exists (a profile-manifest
    // error). That early call must not use up the Logger's close routine: the
    // Logger still closes its file when the application object is destroyed,
    // the moment the routing stops forwarding Qt's messages.
    fincept::Logger::instance().info(QStringLiteral("routing"), QStringLiteral("before the application"));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("early.log");

    int argc = 1;
    char name[] = "tst_qt_message_routing";
    char* argv[] = {name, nullptr};
    {
        QCoreApplication app(argc, argv);
        fincept::Logger::instance().set_file(path);
        fincept::Logger::instance().info(QStringLiteral("routing"), QStringLiteral("while the application exists"));
    }
    fincept::Logger::instance().info(QStringLiteral("routing"), QStringLiteral("after the application"));
    const QByteArray text = contents(path);
    QVERIFY(text.contains("while the application exists"));
    QVERIFY(!text.contains("after the application"));
}

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
