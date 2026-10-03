#include "app/EtfResearchCli.h"

#include "screens/etf_research/EtfResearchScreen.h"
#include "services/etf/research/EtfResearchEngine.h"
#include "services/etf/research/EtfResearchService.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/repositories/EtfResearchRepository.h"
#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>

namespace fincept::marketlab {

namespace etfr_cli_detail {

void print(const QJsonObject& o) {
    const QByteArray t = QJsonDocument(o).toJson(QJsonDocument::Indented);
    std::fwrite(t.constData(), 1, static_cast<size_t>(t.size()), stdout);
    std::fflush(stdout);
}

int usage(const QString& why) {
    print(QJsonObject{
        {"ok", false},
        {"error", why},
        {"usage",
         QJsonArray{QStringLiteral("--etf-research request [--out <file.json>]"),
                    QStringLiteral("--etf-research refresh"),
                    QStringLiteral("--etf-research snapshot --out <file.json> [--as-of <instant>] "
                                   "[--known-at <instant>] [--compact]"),
                    QStringLiteral("--etf-research export --out <file.json>"), QStringLiteral("--etf-research status"),
                    QStringLiteral("--etf-research capture --out-dir <dir> [--width <px>] "
                                   "[--height <px>] [--as-of <instant>] [--known-at <instant>]")}}});
    return 2;
}

bool parse(int argc, char* argv[], QString& command, QHash<QString, QString>& options, QString& error) {
    int i = 1;
    for (; i < argc; ++i)
        if (std::strcmp(argv[i], "--etf-research") == 0)
            break;
    if (i + 1 >= argc) {
        error = QStringLiteral("missing subcommand");
        return false;
    }
    command = QString::fromLocal8Bit(argv[i + 1]);
    for (int k = i + 2; k < argc; ++k) {
        const QString a = QString::fromLocal8Bit(argv[k]);
        if (!a.startsWith(QLatin1String("--")))
            continue;
        const QString key = a.mid(2);
        if (key == QLatin1String("profile")) {
            ++k; // handled by the profile manager
            continue;
        }
        if (options.contains(key)) {
            error = QStringLiteral("duplicate option --%1").arg(key);
            return false;
        }
        if (key == QLatin1String("compact")) {
            options.insert(key, QStringLiteral("true"));
            continue;
        }
        if (k + 1 >= argc) {
            error = QStringLiteral("--%1 needs a value").arg(key);
            return false;
        }
        options.insert(key, QString::fromLocal8Bit(argv[++k]));
    }
    return true;
}

bool instant(const QString& text, QDateTime* out) {
    const QDateTime t = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!t.isValid() || t.timeSpec() == Qt::LocalTime)
        return false;
    *out = t.toUTC();
    return true;
}

bool write_json(const QString& path, const QJsonObject& o, QByteArray* bytes_out = nullptr) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Indented);
    f.write(b);
    if (bytes_out)
        *bytes_out = b;
    return f.commit();
}

QJsonObject stages_json(const QVector<services::etf::research::SourceStageStatus>& stages) {
    QJsonArray a;
    for (const auto& s : stages)
        a.append(QJsonObject{{"stage", s.stage},
                             {"status", s.status},
                             {"detail", s.detail},
                             {"requested_at", s.requested_at.toString(Qt::ISODateWithMs)},
                             {"retrieved_at", s.retrieved_at.toString(Qt::ISODateWithMs)},
                             {"latest_effective", s.latest_effective},
                             {"items_requested", s.items_requested},
                             {"items_ok", s.items_ok},
                             {"rows_inserted", s.rows_inserted},
                             {"rows_revised", s.rows_revised},
                             {"rows_confirmed", s.rows_confirmed},
                             {"stale_items", s.stale_items},
                             {"failed_subjects", QJsonArray::fromStringList(s.failed_subjects)},
                             {"dependent_calculations", QJsonArray::fromStringList(s.dependent_calculations)}});
    return QJsonObject{{"stages", a}};
}

} // namespace etfr_cli_detail

bool etf_research_cli_requested(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--etf-research") == 0)
            return true;
    return false;
}

int run_etf_research_cli(int argc, char* argv[]) {
    using namespace etfr_cli_detail;
    using namespace services::etf::research;
    QString command, error;
    QHash<QString, QString> options;
    if (!parse(argc, argv, command, options, error))
        return usage(error);
    if (!Database::instance().is_open()) {
        print(QJsonObject{{"ok", false}, {"error", "the MarketLab database is not open"}});
        return 1;
    }
    auto& service = EtfResearchService::instance();
    QString uerr;
    if (!service.universe(&uerr)) {
        print(QJsonObject{{"ok", false}, {"error", "research universe invalid"}, {"detail", uerr}});
        return 1;
    }
    QDateTime as_of = QDateTime::currentDateTimeUtc();
    QDateTime known_at = as_of;
    if (options.contains(QStringLiteral("as-of")) && !instant(options.value(QStringLiteral("as-of")), &as_of))
        return usage(QStringLiteral("--as-of must be ISO-8601 with Z or an offset"));
    if (options.contains(QStringLiteral("known-at")) && !instant(options.value(QStringLiteral("known-at")), &known_at))
        return usage(QStringLiteral("--known-at must be ISO-8601 with Z or an offset"));
    if (!options.contains(QStringLiteral("known-at")) && options.contains(QStringLiteral("as-of")))
        known_at = as_of;

    auto allowed = [&](const QStringList& keys) {
        for (auto it = options.begin(); it != options.end(); ++it)
            if (!keys.contains(it.key()))
                return it.key();
        return QString();
    };

    if (command == QLatin1String("request")) {
        if (const QString bad = allowed({QStringLiteral("out")}); !bad.isEmpty())
            return usage(QStringLiteral("unknown option --%1 for request").arg(bad));
        const QJsonObject req = service.build_request();
        if (options.contains(QStringLiteral("out")) && !write_json(options.value(QStringLiteral("out")), req)) {
            print(QJsonObject{{"ok", false}, {"error", "could not write --out"}});
            return 1;
        }
        print(QJsonObject{{"ok", true}, {"command", command}, {"request", req}});
        return 0;
    }
    if (command == QLatin1String("status")) {
        if (const QString bad = allowed({}); !bad.isEmpty())
            return usage(QStringLiteral("unknown option --%1 for status").arg(bad));
        auto counts = EtfResearchRepository::instance().table_counts();
        auto last = EtfResearchRepository::instance().last_run(QDateTime::currentDateTimeUtc());
        print(QJsonObject{{"ok", counts.is_ok()},
                          {"command", command},
                          {"tables", counts.is_ok() ? counts.value() : QJsonObject()},
                          {"last_run", last.is_ok() ? last.value() : QJsonObject()}});
        return counts.is_ok() ? 0 : 1;
    }
    if (command == QLatin1String("export")) {
        if (const QString bad = allowed({QStringLiteral("out")}); !bad.isEmpty() || !options.contains("out"))
            return usage(QStringLiteral("export needs --out and nothing else"));
        auto all = EtfResearchRepository::instance().export_all();
        if (all.is_err()) {
            print(QJsonObject{{"ok", false}, {"error", QString::fromStdString(all.error())}});
            return 1;
        }
        QByteArray bytes;
        if (!write_json(options.value(QStringLiteral("out")), all.value(), &bytes)) {
            print(QJsonObject{{"ok", false}, {"error", "could not write --out"}});
            return 1;
        }
        print(QJsonObject{
            {"ok", true},
            {"command", command},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
        return 0;
    }
    if (command == QLatin1String("snapshot")) {
        if (const QString bad = allowed({QStringLiteral("out"), QStringLiteral("as-of"), QStringLiteral("known-at"),
                                         QStringLiteral("compact")});
            !bad.isEmpty() || !options.contains(QStringLiteral("out")))
            return usage(QStringLiteral("snapshot needs --out [--as-of] [--known-at] [--compact]"));
        QElapsedTimer timer;
        timer.start();
        auto in = service.load(as_of, known_at);
        if (in.is_err()) {
            print(QJsonObject{{"ok", false}, {"error", QString::fromStdString(in.error())}});
            return 1;
        }
        const qint64 load_ms = timer.restart();
        const ResearchSnapshot snap = compute_snapshot(in.value());
        const qint64 compute_ms = timer.elapsed();
        const QJsonObject doc = snapshot_to_json(snap, !options.contains(QStringLiteral("compact")));
        QByteArray bytes;
        if (!write_json(options.value(QStringLiteral("out")), doc, &bytes)) {
            print(QJsonObject{{"ok", false}, {"error", "could not write --out"}});
            return 1;
        }
        QJsonObject counts;
        for (auto it = snap.counts.begin(); it != snap.counts.end(); ++it)
            counts.insert(it.key(), it.value());
        print(QJsonObject{
            {"ok", true},
            {"command", command},
            {"as_of", snap.as_of.toString(Qt::ISODateWithMs)},
            {"known_at", snap.known_at.toString(Qt::ISODateWithMs)},
            {"counts", counts},
            {"load_ms", load_ms},
            {"compute_ms", compute_ms},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}});
        return 0;
    }
    if (command == QLatin1String("refresh")) {
        if (const QString bad = allowed({}); !bad.isEmpty())
            return usage(QStringLiteral("unknown option --%1 for refresh").arg(bad));
        QEventLoop loop;
        auto result = std::make_shared<RefreshResult>();
        bool finished = false;
        service.refresh(
            QStringLiteral("manual_cli"),
            [](const QString& stage, int d, int t) {
                std::fprintf(stderr, "progress %s %d/%d\n", stage.toUtf8().constData(), d, t);
                std::fflush(stderr);
            },
            [&](const RefreshResult& r) {
                *result = r;
                finished = true;
                loop.quit();
            });
        if (!finished) {
            QTimer::singleShot(60 * 60 * 1000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        if (!finished) {
            print(QJsonObject{{"ok", false}, {"command", command}, {"error", "the refresh did not finish"}});
            return 1;
        }
        QJsonObject o{{"ok", result->ok},
                      {"command", command},
                      {"run_id", result->run_id},
                      {"started", result->started.toString(Qt::ISODateWithMs)},
                      {"finished", result->finished.toString(Qt::ISODateWithMs)},
                      {"script_version", result->script_version},
                      {"error", result->error}};
        o.insert(QStringLiteral("summary"), stages_json(result->stages));
        print(o);
        return result->ok ? 0 : 1;
    }
    if (command == QLatin1String("capture")) {
        if (const QString bad = allowed({QStringLiteral("out-dir"), QStringLiteral("width"), QStringLiteral("height"),
                                         QStringLiteral("as-of"), QStringLiteral("known-at")});
            !bad.isEmpty() || !options.contains(QStringLiteral("out-dir")))
            return usage(QStringLiteral("capture needs --out-dir [--width] [--height] [--as-of] [--known-at]"));
        const int w = options.value(QStringLiteral("width"), QStringLiteral("1680")).toInt();
        const int h = options.value(QStringLiteral("height"), QStringLiteral("980")).toInt();
        const int before = service.fetch_invocations();
        auto in = service.load(as_of, known_at);
        if (in.is_err()) {
            print(QJsonObject{{"ok", false}, {"error", QString::fromStdString(in.error())}});
            return 1;
        }
        const ResearchSnapshot snap = compute_snapshot(in.value());
        QDir().mkpath(options.value(QStringLiteral("out-dir")));
        const QJsonArray files =
            screens::capture_etf_research_views(snap, options.value(QStringLiteral("out-dir")), w, h);
        print(QJsonObject{{"ok", !files.isEmpty()},
                          {"command", command},
                          {"files", files},
                          {"acquisitions_started", service.fetch_invocations() - before}});
        return files.isEmpty() ? 1 : 0;
    }
    return usage(QStringLiteral("unknown subcommand '%1'").arg(command));
}

int run_etf_research_selftest() {
    using namespace services::etf::research;
    int failures = 0;
    auto check = [&failures](bool ok, const char* what) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
        std::fflush(stdout);
        if (!ok)
            ++failures;
    };
    auto& service = EtfResearchService::instance();
    QString uerr;
    check(service.universe(&uerr) != nullptr, "research universe resource parses and validates");
    auto& repo = EtfResearchRepository::instance();
    auto counts_before = repo.table_counts();
    check(counts_before.is_ok(), "migration v053 research tables are present");
    auto etf_before = EtfDataRepository::instance().export_all();
    const int fetches_before = service.fetch_invocations();
    {
        screens::EtfResearchScreen screen;
        screen.setAttribute(Qt::WA_DontShowOnScreen, true);
        screen.resize(1400, 860);
        QEventLoop loop;
        bool applied = false;
        QObject::connect(&screen, &screens::EtfResearchScreen::snapshot_applied, &loop, [&]() {
            applied = true;
            loop.quit();
        });
        screen.show();
        if (!applied) {
            QTimer::singleShot(180000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        check(applied, "workspace opened and completed its stored-data read");
        check(screen.load_requests() == 1, "opening performed exactly one store read");
        for (const QString& v : screen.view_ids())
            screen.show_view(v);
        check(!screen.grab().isNull(), "every view rendered natively");
    }
    check(service.fetch_invocations() == fetches_before, "opening the workspace started no acquisition");
    auto counts_after = repo.table_counts();
    check(counts_after.is_ok() && counts_before.is_ok() && counts_after.value() == counts_before.value(),
          "no research-source row was written");
    auto etf_after = EtfDataRepository::instance().export_all();
    check(etf_before.is_ok() && etf_after.is_ok() && etf_before.value() == etf_after.value(),
          "no ETF data-foundation row was written");
    const QDateTime frame = QDateTime::currentDateTimeUtc();
    auto a = service.load(frame, frame);
    auto b = service.load(frame, frame);
    check(a.is_ok() && b.is_ok() &&
              QJsonDocument(snapshot_to_json(compute_snapshot(a.value()))).toJson(QJsonDocument::Compact) ==
                  QJsonDocument(snapshot_to_json(compute_snapshot(b.value()))).toJson(QJsonDocument::Compact),
          "snapshot of a fixed frame is byte-identical twice");
    std::printf("etf-research selftest: %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}

} // namespace fincept::marketlab
