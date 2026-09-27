#include "app/EtfDataCli.h"

#include "core/logging/Logger.h"
#include "services/etf/EtfDataService.h"
#include "services/etf/EtfDerivedAnalytics.h"
#include "services/etf/EtfRegulatoryFlowAnalytics.h"
#include "services/etf/EtfRotationMeasures.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "services/etf/SecEdgarParse.h"
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
#include <optional>
#include <utility>

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
    // One append per command: a long command wraps over adjacent literals,
    // which inside a braced list reads as a missing comma.
    QJsonArray commands;
    commands.append(QStringLiteral("--etf-data sec-nport --cik <cik> [--series <S#########>] [--max-filings <1-40>] "
                                   "[--from <yyyy-MM-dd>] [--to <yyyy-MM-dd>]"));
    commands.append(QStringLiteral("--etf-data ibkr-daily --symbol <SYM> [--duration \"2 Y\"]"));
    commands.append(QStringLiteral("--etf-data derived --out <file.json> [--as-of <ISO-8601 with Z or offset>] "
                                   "[--known-at <ISO-8601 with Z or offset>] [--cik <cik> [--series <S#########>]] "
                                   "[--symbol <SYM> | --con-id <n>] [--reference-symbol <SYM> | --reference-con-id "
                                   "<n>] [--from <yyyy-MM-dd>] [--to <yyyy-MM-dd>]"));
    commands.append(QStringLiteral("--etf-data export --out <file.json>"));
    commands.append(QStringLiteral("--etf-data status"));
    print_json(QJsonObject{{"ok", false}, {"error", why}, {"usage", commands}});
    return 2;
}

/// An instant given on the command line. It must name its zone (Z or an
/// offset): a local wall-clock time would make the decision time depend on
/// the machine that ran the command.
bool parse_instant(const QString& text, QDateTime* out) {
    const QDateTime t = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!t.isValid() || t.timeSpec() == Qt::LocalTime)
        return false;
    *out = t.toUTC();
    return true;
}

/// One listed instrument named by ticker or conId (exactly one of the two),
/// as recorded by the knowledge cutoff, or why it cannot be.
std::optional<qint64> resolve_instrument(const QString& symbol, const QString& con_id, const QDateTime& known_at,
                                         QString* why) {
    auto& repo = EtfDataRepository::instance();
    if (!con_id.isEmpty()) {
        bool ok = false;
        const qint64 id = con_id.toLongLong(&ok);
        auto instruments = repo.listed_instruments(known_at);
        if (ok && instruments.is_ok()) {
            for (const etf_store::ListedInstrumentRow& row : instruments.value()) {
                if (row.con_id == id)
                    return row.instrument_id;
            }
        }
        *why = QStringLiteral("no listed instrument with conId '%1' was recorded by the knowledge cutoff").arg(con_id);
        return std::nullopt;
    }
    auto ids = repo.find_listed_instruments_by_symbol(symbol.trimmed().toUpper(), known_at);
    if (ids.is_err() || ids.value().isEmpty()) {
        *why = QStringLiteral("no listed instrument had carried the ticker '%1' by the knowledge cutoff").arg(symbol);
        return std::nullopt;
    }
    if (ids.value().size() > 1) {
        // A ticker is not an identity; name the conId instead.
        *why = QStringLiteral("the ticker '%1' has been carried by %2 instruments; use --con-id")
                   .arg(symbol)
                   .arg(ids.value().size());
        return std::nullopt;
    }
    return ids.value().first();
}

/// Counts of a derived document, by family and state, for the console.
QJsonObject derived_summary(const QJsonObject& doc) {
    auto count_values = [](const QJsonObject& values, QHash<QString, int>& states) {
        for (auto it = values.begin(); it != values.end(); ++it)
            ++states[it.value().toObject().value(QLatin1String("state")).toString()];
    };
    auto to_json = [](const QHash<QString, int>& states) {
        QJsonObject o;
        for (auto it = states.cbegin(); it != states.cend(); ++it)
            o.insert(it.key(), it.value());
        return o;
    };
    QHash<QString, int> reg_states;
    int months = 0;
    const QJsonArray reg = doc.value(QLatin1String("regulatory_flow")).toArray();
    for (const QJsonValue& e : reg) {
        for (const QJsonValue& m :
             e.toObject().value(QLatin1String("analytics")).toObject().value(QLatin1String("months")).toArray()) {
            ++months;
            count_values(m.toObject().value(QLatin1String("values")).toObject(), reg_states);
        }
    }
    QHash<QString, int> rot_states;
    int sessions = 0;
    const QJsonArray rot = doc.value(QLatin1String("rotation_proxy")).toArray();
    for (const QJsonValue& i : rot) {
        for (const QJsonValue& s :
             i.toObject().value(QLatin1String("measures")).toObject().value(QLatin1String("sessions")).toArray()) {
            ++sessions;
            count_values(s.toObject().value(QLatin1String("values")).toObject(), rot_states);
        }
    }
    return QJsonObject{{"regulatory_entities", reg.size()},
                       {"regulatory_months", months},
                       {"regulatory_value_states", to_json(reg_states)},
                       {"rotation_instruments", rot.size()},
                       {"rotation_sessions", sessions},
                       {"rotation_value_states", to_json(rot_states)}};
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
                               {"regulatory_method", QLatin1String(services::etf::kRegulatoryFlowAnalyticsVersion)},
                               {"rotation_method", QLatin1String(services::etf::kRotationProxyMeasuresVersion)},
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

    if (command == QLatin1String("derived")) {
        // Batch C: derived values over the stored vintages. Reads only.
        // An option this command does not know is refused, not ignored: a
        // mistyped --known-at would otherwise silently mean "now".
        static const QStringList kDerivedOptions = {QStringLiteral("out"),
                                                    QStringLiteral("as-of"),
                                                    QStringLiteral("known-at"),
                                                    QStringLiteral("from"),
                                                    QStringLiteral("to"),
                                                    QStringLiteral("cik"),
                                                    QStringLiteral("series"),
                                                    QStringLiteral("symbol"),
                                                    QStringLiteral("con-id"),
                                                    QStringLiteral("reference-symbol"),
                                                    QStringLiteral("reference-con-id")};
        for (auto it = options.cbegin(); it != options.cend(); ++it) {
            if (!kDerivedOptions.contains(it.key()))
                return usage(QStringLiteral("derived does not take --%1").arg(it.key()));
        }
        for (const auto& [a, b] : {std::pair{"symbol", "con-id"}, std::pair{"reference-symbol", "reference-con-id"}}) {
            if (options.contains(QLatin1String(a)) && options.contains(QLatin1String(b)))
                return usage(
                    QStringLiteral("--%1 and --%2 are alternatives; give one").arg(QLatin1String(a), QLatin1String(b)));
        }
        const QString out = options.value(QStringLiteral("out"));
        if (out.isEmpty())
            return usage(QStringLiteral("derived needs --out <file.json>"));
        services::etf::DerivedRunRequest req;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        req.frame.as_of = now;
        req.frame.known_at = now;
        if (options.contains(QStringLiteral("as-of")) &&
            !parse_instant(options.value(QStringLiteral("as-of")), &req.frame.as_of))
            return usage(QStringLiteral("--as-of must be an ISO-8601 instant with Z or an offset"));
        if (options.contains(QStringLiteral("known-at")) &&
            !parse_instant(options.value(QStringLiteral("known-at")), &req.frame.known_at))
            return usage(QStringLiteral("--known-at must be an ISO-8601 instant with Z or an offset"));
        for (const char* key : {"from", "to"}) {
            if (!options.contains(QLatin1String(key)))
                continue;
            const QDate d = QDate::fromString(options.value(QLatin1String(key)), Qt::ISODate);
            if (!d.isValid())
                return usage(QStringLiteral("--from/--to must be yyyy-MM-dd"));
            (std::strcmp(key, "from") == 0 ? req.output_from : req.output_to) = d;
        }
        if (req.output_from.isValid() && req.output_to.isValid() && req.output_from > req.output_to)
            return usage(QStringLiteral("--from must not be after --to"));
        const bool by_entity = options.contains(QStringLiteral("cik"));
        const bool by_instrument =
            options.contains(QStringLiteral("symbol")) || options.contains(QStringLiteral("con-id"));
        if (options.contains(QStringLiteral("series")) && !by_entity)
            return usage(QStringLiteral("--series needs --cik"));
        if (by_entity || by_instrument) {
            // Only the families of the subjects named.
            req.regulatory = by_entity;
            req.rotation = by_instrument;
        }
        if (by_entity) {
            const QString cik10 = services::etf::sec_normalized_cik(options.value(QStringLiteral("cik")));
            const QString series = options.value(QStringLiteral("series"));
            if (cik10.isEmpty() || (!series.isEmpty() && !services::etf::sec_valid_series_id(series)))
                return usage(QStringLiteral("--cik must be 1-10 digits and --series S followed by 9 digits"));
            auto entities = EtfDataRepository::instance().reporting_entities(req.frame.known_at);
            if (entities.is_ok()) {
                for (const etf_store::ReportingEntityRow& e : entities.value()) {
                    if (e.cik == cik10 && e.series_id == series)
                        req.entity_id = e.entity_id;
                }
            }
            if (!req.entity_id)
                return usage(QStringLiteral("no reporting entity for CIK %1 series '%2' was recorded by the knowledge "
                                            "cutoff")
                                 .arg(cik10, series));
        }
        if (by_instrument) {
            QString why;
            const auto id = resolve_instrument(options.value(QStringLiteral("symbol")),
                                               options.value(QStringLiteral("con-id")), req.frame.known_at, &why);
            if (!id)
                return usage(why);
            req.instrument_id = *id;
        }
        if (options.contains(QStringLiteral("reference-symbol")) ||
            options.contains(QStringLiteral("reference-con-id"))) {
            QString why;
            const auto id =
                resolve_instrument(options.value(QStringLiteral("reference-symbol")),
                                   options.value(QStringLiteral("reference-con-id")), req.frame.known_at, &why);
            if (!id)
                return usage(QStringLiteral("reference: %1").arg(why));
            req.reference_instrument_id = *id;
        }
        auto doc = services::etf::run_derived_calculations(req);
        if (doc.is_err()) {
            print_json(
                QJsonObject{{"ok", false}, {"command", command}, {"error", QString::fromStdString(doc.error())}});
            return 1;
        }
        QSaveFile file(out);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(QJsonDocument(doc.value()).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
            print_json(QJsonObject{{"ok", false}, {"error", QStringLiteral("could not write %1").arg(out)}});
            return 1;
        }
        print_json(QJsonObject{{"ok", true},
                               {"command", command},
                               {"out", out},
                               {"frame", doc.value().value(QLatin1String("frame"))},
                               {"regulatory_method", QLatin1String(services::etf::kRegulatoryFlowAnalyticsVersion)},
                               {"rotation_method", QLatin1String(services::etf::kRotationProxyMeasuresVersion)},
                               {"summary", derived_summary(doc.value())}});
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
