#pragma once
#include <QString>
#include <QtGlobal>

namespace fincept {

/// Routes Qt's own messages (qDebug, qInfo, qWarning, qCritical, qFatal) into
/// the Logger while the application object exists, so framework and
/// third-party warnings reach the log file in Release builds. Call once, after
/// the QCoreApplication is constructed.
///
/// When the application object is destroyed, the routing switches to
/// discard_qt_message(). Qt calls the installed handler until the process is
/// gone, including from its own DLLs' static destructors while Windows unloads
/// them inside ExitProcess. By then the Logger (a function-local static) has
/// been destroyed, and Windows has terminated every other thread wherever it
/// was. A "QMutex: destroying locked mutex" warning from that unload reached
/// Logger::write, whose timestamp (QDateTime::currentDateTime) then waited
/// forever on a Qt lock that a terminated thread still held: the intermittent
/// hang of the headless commands at exit. The Logger closes its file at the
/// same point (its own post routine), so nothing it would write is dropped.
void install_qt_message_routing();

/// The handler once the application object is gone. It takes no lock,
/// allocates nothing and writes nothing.
void discard_qt_message(QtMsgType type, const QMessageLogContext& context, const QString& message);

} // namespace fincept
