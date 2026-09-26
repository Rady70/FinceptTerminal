#include "core/logging/QtMessageRouting.h"

#include "core/logging/Logger.h"

#include <QCoreApplication>

namespace fincept {

namespace {

void forward_qt_message_to_logger(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const char* category = (context.category && *context.category) ? context.category : "Qt";
    switch (type) {
        case QtDebugMsg:
            Logger::instance().debug(category, message);
            break;
        case QtInfoMsg:
            Logger::instance().info(category, message);
            break;
        case QtWarningMsg:
            Logger::instance().warn(category, message);
            break;
        case QtCriticalMsg:
            Logger::instance().error(category, message);
            break;
        case QtFatalMsg:
            Logger::instance().error(category, message);
            Logger::instance().flush_and_close();
            break;
    }
}

} // namespace

void discard_qt_message(QtMsgType /*type*/, const QMessageLogContext& /*context*/, const QString& /*message*/) {}

void install_qt_message_routing() {
    qInstallMessageHandler(&forward_qt_message_to_logger);
    // Post routines run while the application object is destroyed: after the
    // last event, before static destruction and before Windows unloads Qt.
    qAddPostRoutine([]() { qInstallMessageHandler(&discard_qt_message); });
}

} // namespace fincept
