#include "app/EtfDataCli.h"

#include "core/logging/Logger.h"
#include "services/etf/EtfDataService.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"
#include "storage/sqlite/migrations/MigrationRunner.h"

#include <QDate>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>

namespace fincept::marketlab {

namespace etf_cli_detail {

constexpr int kEtfCliSecWatchdogMs = 20 * 60 * 1000;
constexpr int kEtfCliIbkrWatchdogMs = 4 * 60 * 1000;

void print_json(const QJsonObject& o) {
    const QByteArray text = QJsonDocument(o).toJson(QJsonDocument::Indented);
    std::fwrite(text.constData(), 1, static_cast<size_t>(text.size()), stdout);
    std::fflush(stdout);
}

int usage(const QString& why) {
    print_json(QJsonObject{
        {"ok", false},
        {"error", why},
        {"usage", QJsonArray{"--etf-data sec-nport --cik <cik> [--series <S#########>] [--max-filings <1-40>] "
                             "[--from <yyyy-MM-dd>] [--to <yyyy-MM-dd>]",
                             "--etf-data ibkr-daily --symbol <SYM> [--duration \"2 Y\"]",
                             "--etf-data export --out <file.json>", "--etf-data status"}}});
    return 2;
}

/// argv after `--etf-data`: the subcommand, then `--key value` pairs.
bool parse_options(int argc, char* argv[], QString& command, QHash<QString, QString>& options, QString& error) {
    int i = 1;
    while (i < argc && std::strcmp(argv[i], "--etf-data") != 0)
        ++i;
    if (i + 1 >= argc) {
        error = QStringLiteral("missing subcommand");
        return false;
    }
    command = QString::fromLocal8Bit(argv[i + 1]);
    for (int j = i + 2; j < argc; ++j) {
        const QString key = QString::fromLocal8Bit(argv[j]);
        if (key == QLatin1String("--profile")) {
            ++j; // handled by main()
            continue;
        }
        if (!key.startsWith(QLatin1String("--")) || j + 1 >= argc) {
            error = QStringLiteral("expected '--option value', got '%1'").arg(key);
            return false;
        }
        options.insert(key.mid(2), QString::fromLocal8Bit(argv[++j]));
    }
    return true;
}

QJsonObject table_counts() {
    QJsonObject counts;
    for (const char* table : {"etf_retrievals", "etf_retrieval_issues", "etf_reporting_entities", "etf_sec_filings",
                              "etf_listed_instruments", "etf_instrument_symbols", "etf_identity_links",
                              "etf_observation_starts", "etf_market_sessions", "etf_observations"}) {
        auto r = Database::instance().execute(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(table)));
        if (r.is_ok() && r.value().next())
            counts.insert(QLatin1String(table), r.value().value(0).toLongLong());
        else
            counts.insert(QLatin1String(table), QJsonValue::Null);
    }
    return counts;
}

int schema_version() {
    auto r = Database::instance().execute(QStringLiteral("SELECT MAX(version) FROM schema_version"));
    return r.is_ok() && r.value().next() ? r.value().value(0).toInt() : 0;
}

/// Wait for one asynchronous run. The state is shared so a run that finishes
/// after the watchdog cannot touch a dead stack frame.
template <typename Summary>
struct CliWait {
    QEventLoop loop;
    Summary summary;
    bool done = false;
};

} // namespace etf_cli_detail

bool etf_data_cli_requested(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--etf-data") == 0)
            return true;
    }
    return false;
}

bool etf_headless_command_requested(int argc, char* argv[]) {
    if (etf_data_cli_requested(argc, argv))
        return true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest-etf-data") == 0)
            return true;
    }
    return false;
}

int refuse_etf_command_profile_in_use(int argc, char* argv[], const QString& profile) {
    const QString detail = QStringLiteral("MarketLab is already running with profile '%1' and owns its database; "
                                          "nothing was run. Close it, or run the command with another --profile.")
                               .arg(profile);
    if (etf_data_cli_requested(argc, argv)) {
        etf_cli_detail::print_json(QJsonObject{{"ok", false}, {"error", "profile_in_use"}, {"detail", detail}});
    } else {
        std::printf("[FAIL] profile_in_use: %s\netf-data selftest: FAIL (not run)\n", detail.toUtf8().constData());
        std::fflush(stdout);
    }
    return 1;
}

int run_etf_data_cli(int argc, char* argv[]) {
    using namespace etf_cli_detail;
    using services::etf::EtfDataService;

    QString command;
    QHash<QString, QString> options;
    QString error;
    if (!parse_options(argc, argv, command, options, error))
        return usage(error);
    if (!Database::instance().is_open()) {
        print_json(QJsonObject{{"ok", false}, {"error", "the MarketLab database is not open"}});
        return 1;
    }

    if (command == QLatin1String("status")) {
        const auto sec = EtfDataService::instance().sec_config();
        print_json(QJsonObject{{"ok", true},
                               {"command", command},
                               {"schema_version", schema_version()},
                               {"highest_registered_version", MigrationRunner::highest_registered_version()},
                               {"route_policy", QLatin1String(services::etf::kEtfRoutePolicyVersion)},
                               {"calendar_version", QLatin1String(services::etf::kUsEquityCalendarVersion)},
                               // The User-Agent itself is never printed: only whether one is declared.
                               {"sec_user_agent_declared", sec.declared},
                               {"sec_min_request_interval_ms", sec.min_request_interval_ms},
                               {"ibkr_configured", EtfDataService::instance().ibkr_configured()},
                               {"table_counts", table_counts()}});
        return 0;
    }

    if (command == QLatin1String("export")) {
        const QString out = options.value(QStringLiteral("out"));
        if (out.isEmpty())
            return usage(QStringLiteral("export needs --out <file.json>"));
        auto all = EtfDataRepository::instance().export_all();
        if (all.is_err()) {
            print_json(QJsonObject{{"ok", false}, {"error", QString::fromStdString(all.error())}});
            return 1;
        }
        QJsonObject doc = all.value();
        doc.insert(QStringLiteral("schema_version"), schema_version());
        QSaveFile file(out);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(doc).toJson(QJsonDocument::Indented)) < 0 ||
            !file.commit()) {
            print_json(QJsonObject{{"ok", false}, {"error", QStringLiteral("could not write %1").arg(out)}});
            return 1;
        }
        print_json(QJsonObject{{"ok", true}, {"command", command}, {"out", out}, {"table_counts", table_counts()}});
        return 0;
    }

    if (command == QLatin1String("sec-nport")) {
        services::etf::SecNportRequest req;
        req.cik = options.value(QStringLiteral("cik"));
        req.series_id = options.value(QStringLiteral("series"));
        bool ok = true;
        if (options.contains(QStringLiteral("max-filings")))
            req.max_filings = options.value(QStringLiteral("max-filings")).toInt(&ok);
        if (!ok || req.cik.isEmpty())
            return usage(QStringLiteral("sec-nport needs --cik and an integer --max-filings"));
        if (options.contains(QStringLiteral("from")))
            req.report_period_from = QDate::fromString(options.value(QStringLiteral("from")), Qt::ISODate);
        if (options.contains(QStringLiteral("to")))
            req.report_period_to = QDate::fromString(options.value(QStringLiteral("to")), Qt::ISODate);
        if ((options.contains(QStringLiteral("from")) && !req.report_period_from.isValid()) ||
            (options.contains(QStringLiteral("to")) && !req.report_period_to.isValid()))
            return usage(QStringLiteral("--from/--to must be yyyy-MM-dd"));
        auto wait = std::make_shared<CliWait<services::etf::SecNportRunSummary>>();
        EtfDataService::instance().ingest_sec_nport(req, [wait](const services::etf::SecNportRunSummary& s) {
            wait->summary = s;
            wait->done = true;
            wait->loop.quit();
        });
        if (!wait->done) {
            QTimer::singleShot(kEtfCliSecWatchdogMs, &wait->loop, &QEventLoop::quit);
            wait->loop.exec();
        }
        if (!wait->done) {
            print_json(QJsonObject{{"ok", false}, {"command", command}, {"error", "the SEC run did not finish"}});
            return 1;
        }
        print_json(QJsonObject{{"ok", true}, {"command", command}, {"summary", wait->summary.to_json()}});
        return 0;
    }

    if (command == QLatin1String("ibkr-daily")) {
        const QString symbol = options.value(QStringLiteral("symbol"));
        const QString duration = options.value(QStringLiteral("duration"), QStringLiteral("2 Y"));
        if (symbol.isEmpty())
            return usage(QStringLiteral("ibkr-daily needs --symbol"));
        auto wait = std::make_shared<CliWait<services::etf::IbkrDailyRunSummary>>();
        EtfDataService::instance().ingest_ibkr_daily(symbol, duration,
                                                     [wait](const services::etf::IbkrDailyRunSummary& s) {
                                                         wait->summary = s;
                                                         wait->done = true;
                                                         wait->loop.quit();
                                                     });
        if (!wait->done) {
            QTimer::singleShot(kEtfCliIbkrWatchdogMs, &wait->loop, &QEventLoop::quit);
            wait->loop.exec();
        }
        if (!wait->done) {
            print_json(QJsonObject{{"ok", false}, {"command", command}, {"error", "the IBKR run did not finish"}});
            return 1;
        }
        print_json(QJsonObject{{"ok", true}, {"command", command}, {"summary", wait->summary.to_json()}});
        return 0;
    }

    return usage(QStringLiteral("unknown subcommand '%1'").arg(command));
}

} // namespace fincept::marketlab
