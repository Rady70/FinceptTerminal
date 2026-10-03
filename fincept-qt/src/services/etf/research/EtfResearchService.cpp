#include "services/etf/research/EtfResearchService.h"

#include "core/logging/Logger.h"
#include "python/PythonRunner.h"
#include "services/etf/EtfDataService.h"
#include "services/etf/EtfDerivedAnalytics.h"
#include "services/etf/EtfGroupAnalytics.h"
#include "services/etf/EtfSecNportIngestor.h"
#include "services/etf/EtfSessionCalendar.h"
#include "services/ibkr/IbkrTwsService.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/repositories/EtfResearchRepository.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>

#include <memory>

namespace fincept::services::etf::research {

namespace etfr_service_detail {

constexpr int kFetchWatchdogMs = 45 * 60 * 1000;

QString iso(const QDateTime& t) {
    return t.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
}

bool us_listed(const QString& sym) {
    return !sym.contains(QLatin1Char('.')) && !sym.startsWith(QLatin1Char('^'));
}

/// Batch C regulatory analytics of one reporting entity, as months.
QVector<MeasuredMonth> measured_months(qint64 entity_id, const QString& key, const QDateTime& as_of,
                                       const QDateTime& known_at) {
    QVector<MeasuredMonth> out;
    DerivedRunRequest req;
    req.frame.as_of = as_of;
    req.frame.known_at = known_at;
    req.regulatory = true;
    req.rotation = false;
    req.entity_id = entity_id;
    auto r = run_derived_calculations(req);
    if (r.is_err())
        return out;
    for (const QJsonValue& e : r.value().value(QStringLiteral("regulatory_flow")).toArray()) {
        for (const QJsonValue& mv :
             e.toObject().value(QStringLiteral("analytics")).toObject().value(QStringLiteral("months")).toArray()) {
            const QJsonObject m = mv.toObject();
            MeasuredMonth mm;
            mm.month =
                QDate::fromString(m.value(QStringLiteral("month")).toString() + QStringLiteral("-01"), Qt::ISODate);
            const QJsonObject nf =
                m.value(QStringLiteral("values")).toObject().value(QStringLiteral("net_flow")).toObject();
            if (nf.value(QStringLiteral("value")).isDouble())
                mm.flow_usd = nf.value(QStringLiteral("value")).toDouble();
            mm.quality = nf.value(QStringLiteral("state")).toString();
            mm.reason = nf.value(QStringLiteral("reason")).toString();
            mm.available_from =
                QDateTime::fromString(nf.value(QStringLiteral("available_from")).toString(), Qt::ISODateWithMs);
            mm.accession =
                m.value(QStringLiteral("inputs")).toObject().value(QStringLiteral("selected_accession")).toString();
            mm.reporting_key = key;
            out.append(mm);
        }
    }
    return out;
}

SourceStageStatus stage_status(const QString& stage, const QString& status, const QString& detail,
                               const QDateTime& requested, const QDateTime& retrieved) {
    SourceStageStatus s;
    s.stage = stage;
    s.status = status;
    s.detail = detail;
    s.requested_at = requested;
    s.retrieved_at = retrieved;
    if (stage == QLatin1String("ibkr_daily"))
        s.dependent_calculations = {QStringLiteral("ibkr_cross_check"), QStringLiteral("batch_c_rotation_proxies")};
    else if (stage == QLatin1String("sec_nport"))
        s.dependent_calculations = {QStringLiteral("measured_regulatory_flow"), QStringLiteral("estimate_validation")};
    return s;
}

} // namespace etfr_service_detail

using namespace etfr_service_detail;

EtfResearchService::EtfResearchService(QObject* parent) : QObject(parent) {
    qRegisterMetaType<RefreshResult>("fincept::services::etf::research::RefreshResult");
}

EtfResearchService& EtfResearchService::instance() {
    static EtfResearchService s;
    return s;
}

const ResearchUniverse* EtfResearchService::universe(QString* error) {
    if (!universe_ && universe_error_.isEmpty()) {
        universe_ = load_universe(QLatin1String(kUniverseResourcePath), &universe_error_);
        if (!universe_)
            LOG_ERROR("EtfResearch", QStringLiteral("research universe invalid: %1").arg(universe_error_));
    }
    if (error)
        *error = universe_error_;
    return universe_ ? &*universe_ : nullptr;
}

QDate EtfResearchService::expected_us_session(const QDateTime& as_of) {
    return completed_us_session(as_of);
}

Result<ResearchInputs> EtfResearchService::load(const QDateTime& as_of, const QDateTime& known_at, bool with_groups) {
    QString err;
    const ResearchUniverse* u = universe(&err);
    if (!u)
        return Result<ResearchInputs>::err(err.toStdString());
    QElapsedTimer phase;
    phase.start();
    auto r = EtfResearchRepository::instance().load_inputs(*u, as_of, known_at);
    if (r.is_err())
        return r;
    ResearchInputs in = r.value();
    in.load_profile.append({QStringLiteral("research_store"), phase.restart()});
    in.expected_us_session = expected_us_session(as_of);
    auto& etf = EtfDataRepository::instance();
    // Measured: SEC N-PORT regulatory flow of the declared reporting identity
    // covering each month (QQQ changes identity in 2025-10).
    for (const UniverseInstrument& i : u->instruments) {
        if (i.sec_reporting.isEmpty())
            continue;
        QVector<MeasuredMonth> months;
        for (const SecReportingIdentity& id : i.sec_reporting) {
            auto ent = etf.find_reporting_entity(id.cik, id.series_id);
            if (ent.is_err() || !ent.value()) {
                in.load_warnings.append(QStringLiteral("%1: SEC reporting entity %2/%3 is not in the store")
                                            .arg(i.symbol, id.cik, id.series_id));
                continue;
            }
            for (const MeasuredMonth& m :
                 measured_months(*ent.value(), id.cik + QLatin1Char('/') + id.series_id, in.as_of, in.known_at)) {
                const QDate month_end = m.month.addMonths(1).addDays(-1);
                if (id.covers(month_end) && id.covers(m.month))
                    months.append(m);
            }
        }
        std::sort(months.begin(), months.end(),
                  [](const MeasuredMonth& a, const MeasuredMonth& b) { return a.month < b.month; });
        in.measured.insert(i.symbol, months);
    }
    in.load_profile.append({QStringLiteral("sec_measured_batch_c"), phase.restart()});
    // IBKR completed-session closes for rows with a reviewed conId (cross-check only).
    for (const UniverseInstrument& i : u->instruments) {
        if (!i.taxonomy_con_id)
            continue;
        auto inst = etf.find_listed_instrument(*i.taxonomy_con_id);
        if (inst.is_err() || !inst.value())
            continue;
        auto obs = etf.subject_observations(SubjectType::ListedInstrument, *inst.value());
        if (obs.is_err())
            continue;
        QMap<QDate, QPair<int, double>> latest;
        for (const StoredObservation& o : obs.value()) {
            if (o.measure != QLatin1String("bar_close") || !o.value.reported() || !o.first_seen_at.isValid() ||
                o.first_seen_at > in.known_at || !in.expected_us_session.isValid() ||
                o.effective_date > in.expected_us_session) // the session must have closed by as_of
                continue;
            auto it = latest.find(o.effective_date);
            if (it == latest.end() || o.source_revision > it->first)
                latest.insert(o.effective_date, {o.source_revision, o.value.value});
        }
        QVector<QPair<QDate, double>> closes;
        for (auto it = latest.begin(); it != latest.end(); ++it)
            closes.append({it.key(), it.value().second});
        in.ibkr_close.insert(i.symbol, closes);
    }
    in.load_profile.append({QStringLiteral("ibkr_closes"), phase.restart()});
    if (with_groups) {
        auto g = group_flows(in.as_of, in.known_at, &in.load_warnings);
        if (g.is_ok())
            in.group_flows = g.value();
    } else {
        in.group_flows_loaded = false;
    }
    in.load_profile.append({QStringLiteral("batch_d_groups"), phase.restart()});
    return Result<ResearchInputs>::ok(in);
}

Result<QVector<GroupFlowRow>> EtfResearchService::group_flows(const QDateTime& as_of, const QDateTime& known_at,
                                                              QStringList* warnings) {
    QVector<GroupFlowRow> out;
    // Measured regulatory flow by Batch D category and asset class: the latest
    // 12 complete calendar months at as_of (finalized etf_group_analytics_v2).
    const QDate last_month_end = QDate(as_of.date().year(), as_of.date().month(), 1).addDays(-1);
    for (const QString& level : {QStringLiteral("asset_class"), QStringLiteral("category")}) {
        GroupRunRequest g;
        g.frame.as_of = as_of;
        g.frame.known_at = known_at;
        g.group_level = level;
        g.output_to = last_month_end;
        g.output_from = QDate(last_month_end.year(), last_month_end.month(), 1).addMonths(-11);
        auto gr = run_group_research(g);
        if (gr.is_err()) {
            if (warnings)
                warnings->append(
                    QStringLiteral("Batch D %1 groups: %2").arg(level, QString::fromStdString(gr.error())));
            continue;
        }
        const QString tax = gr.value().value(QStringLiteral("taxonomy_version")).toString();
        for (const QJsonValue& gv : gr.value().value(QStringLiteral("groups")).toArray()) {
            const QJsonObject go = gv.toObject();
            for (const QJsonValue& mv : go.value(QStringLiteral("regulatory_months")).toArray()) {
                const QJsonObject m = mv.toObject();
                GroupFlowRow row;
                row.level = level;
                row.group_id = go.value(QStringLiteral("group_id")).toString();
                row.month =
                    QDate::fromString(m.value(QStringLiteral("month")).toString() + QStringLiteral("-01"), Qt::ISODate);
                if (m.value(QStringLiteral("observed_net_flow_usd")).isDouble())
                    row.observed_net_flow_usd = m.value(QStringLiteral("observed_net_flow_usd")).toDouble();
                if (m.value(QStringLiteral("complete_net_flow_usd")).isDouble())
                    row.complete_net_flow_usd = m.value(QStringLiteral("complete_net_flow_usd")).toDouble();
                row.quality = m.value(QStringLiteral("quality")).toString();
                const QJsonObject cov = m.value(QStringLiteral("coverage")).toObject();
                row.unique_reporting_identities = cov.value(QStringLiteral("unique_reporting_identities")).toInt();
                row.observed_reporting_identities = cov.value(QStringLiteral("observed_reporting_identities")).toInt();
                row.unresolved_subjects = cov.value(QStringLiteral("unresolved_subjects")).toInt();
                row.excluded_subjects = cov.value(QStringLiteral("excluded_subjects")).toInt();
                row.taxonomy_version = tax;
                out.append(row);
            }
        }
    }
    return Result<QVector<GroupFlowRow>>::ok(out);
}

QJsonObject EtfResearchService::build_request(bool full) const {
    const ResearchUniverse* u = const_cast<EtfResearchService*>(this)->universe();
    if (!u)
        return {};
    const QStringList long_syms = u->history_symbols(QStringLiteral("long"));
    const QStringList std_syms = u->history_symbols(QStringLiteral("standard"));
    QJsonArray funds, parents;
    for (const UniverseInstrument& i : u->instruments) {
        if (!i.is_fund() || !us_listed(i.symbol))
            continue; // the snapshot's effective-session rule is the NYSE one
        funds.append(i.symbol);
        if (i.has_role("us_sector") || i.has_role("theme") || i.has_role("country"))
            parents.append(i.symbol);
    }
    QSet<QString> fetched(long_syms.begin(), long_syms.end());
    for (const auto& s : std_syms)
        fetched.insert(s);
    QJsonArray extra, fundamental_extra, exclude;
    for (const QString& m : u->intl_markets)
        for (const IntlStock& s : u->intl.value(m)) {
            if (fetched.contains(s.symbol))
                fundamental_extra.append(s.symbol);
            else
                extra.append(s.symbol);
        }
    for (const auto& s : fetched)
        exclude.append(s);
    // Incremental history: symbols whose stored coverage is continuous (the
    // latest window reaches the earliest stored session) and recent fetch only
    // from two weeks before their last stored session.
    QHash<QString, etf_research_store::HistoryCoverage> cov;
    if (!full) {
        auto c = EtfResearchRepository::instance().history_coverage();
        if (c.is_ok())
            cov = c.value();
    }
    const QDate today = QDate::currentDate();
    auto incremental = [&](const QString& s, QDate* min_last) {
        const auto it = cov.constFind(s);
        if (it == cov.constEnd() || !it->window_first.isValid() || it->window_first != it->earliest_stored ||
            it->window_last < today.addDays(-30))
            return false;
        if (!min_last->isValid() || it->window_last < *min_last)
            *min_last = it->window_last;
        return true;
    };
    QStringList long_full, std_full, inc_main;
    QDate main_last;
    for (const QString& s : long_syms)
        (incremental(s, &main_last) ? inc_main : long_full).append(s);
    for (const QString& s : std_syms)
        (incremental(s, &main_last) ? inc_main : std_full).append(s);
    QJsonArray inc_cons;
    QDate cons_last;
    for (auto it = cov.constBegin(); it != cov.constEnd(); ++it)
        if (!fetched.contains(it.key()) && incremental(it.key(), &cons_last))
            inc_cons.append(it.key());
    // Constituent fundamentals change slowly: a capture younger than three days is reused.
    constexpr int kFundamentalsFreshDays = 3;
    QJsonArray fresh;
    if (!full) {
        auto lf = EtfResearchRepository::instance().latest_fundamentals_capture();
        if (lf.is_ok())
            for (auto it = lf.value().constBegin(); it != lf.value().constEnd(); ++it)
                if (it.value().isValid() &&
                    it.value().secsTo(QDateTime::currentDateTimeUtc()) < qint64{kFundamentalsFreshDays} * 86400)
                    fresh.append(it.key());
    }
    QJsonObject holding_map;
    for (auto it = u->holding_symbol_map.constBegin(); it != u->holding_symbol_map.constEnd(); ++it)
        holding_map.insert(it.key(), it.value());
    QJsonArray excluded_holdings;
    for (auto it = u->holding_symbol_excluded.constBegin(); it != u->holding_symbol_excluded.constEnd(); ++it)
        excluded_holdings.append(it.key());
    QStringList cftc_markets;
    for (auto it = u->cftc_market.constBegin(); it != u->cftc_market.constEnd(); ++it)
        if (!cftc_markets.contains(it.value()))
            cftc_markets.append(it.value());
    std::sort(cftc_markets.begin(), cftc_markets.end());
    QJsonArray wb_codes;
    for (const UniverseInstrument& i : u->instruments)
        if (!i.wb_code.isEmpty() && i.country_type == QLatin1String("single"))
            wb_codes.append(i.wb_code);
    return QJsonObject{
        {QStringLiteral("universe_version"), u->version},
        {QStringLiteral("mode"), full ? QStringLiteral("full") : QStringLiteral("incremental")},
        {QStringLiteral("cftc"), QJsonObject{{QStringLiteral("report"), u->cftc_report},
                                             {QStringLiteral("futures_only"), u->cftc_futures_only},
                                             {QStringLiteral("markets"), QJsonArray::fromStringList(cftc_markets)}}},
        {QStringLiteral("history"),
         QJsonObject{{QStringLiteral("long"), QJsonArray::fromStringList(long_full)},
                     {QStringLiteral("standard"), QJsonArray::fromStringList(std_full)},
                     {QStringLiteral("incremental"),
                      QJsonObject{{QStringLiteral("start"), main_last.addDays(-14).toString(Qt::ISODate)},
                                  {QStringLiteral("symbols"), QJsonArray::fromStringList(inc_main)}}}}},
        {QStringLiteral("funds"), funds},
        {QStringLiteral("constituents"),
         QJsonObject{{QStringLiteral("holding_parents"), parents},
                     {QStringLiteral("max_per_parent"), 10},
                     {QStringLiteral("extra_symbols"), extra},
                     {QStringLiteral("fundamental_extra"), fundamental_extra},
                     {QStringLiteral("exclude"), exclude},
                     {QStringLiteral("history_period"), QStringLiteral("1y")},
                     {QStringLiteral("incremental"),
                      QJsonObject{{QStringLiteral("start"), cons_last.addDays(-14).toString(Qt::ISODate)},
                                  {QStringLiteral("symbols"), inc_cons}}},
                     {QStringLiteral("fundamentals_fresh"), fresh},
                     {QStringLiteral("symbol_map"), holding_map},
                     {QStringLiteral("exclude_holdings"), excluded_holdings},
                     {QStringLiteral("fundamentals_fresh_days"), kFundamentalsFreshDays},
                     {QStringLiteral("fundamentals"), true}}},
        {QStringLiteral("fred"),
         QJsonArray{QStringLiteral("T10Y2Y"), QStringLiteral("CFNAI"), QStringLiteral("BAMLH0A0HYM2"),
                    QStringLiteral("VIXCLS"), QStringLiteral("DGS10"), QStringLiteral("T10YIE"),
                    QStringLiteral("DTWEXBGS"), QStringLiteral("DCOILWTICO")}},
        {QStringLiteral("world_bank"),
         QJsonObject{{QStringLiteral("indicators"),
                      QJsonArray{QStringLiteral("NY.GDP.MKTP.KD.ZG"), QStringLiteral("BN.CAB.XOKA.GD.ZS")}},
                     {QStringLiteral("countries"), wb_codes}}}};
}

void EtfResearchService::default_fetch(const QJsonObject& request, Progress progress,
                                       std::function<void(Result<QJsonObject>)> done) {
    const QString tag = QUuid::createUuid().toString(QUuid::Id128).left(12);
    const QString req_path = QDir::temp().filePath(QStringLiteral("marketlab_etfr_req_%1.json").arg(tag));
    const QString out_path = QDir::temp().filePath(QStringLiteral("marketlab_etfr_out_%1.json").arg(tag));
    {
        QFile f(req_path);
        if (!f.open(QIODevice::WriteOnly)) {
            done(Result<QJsonObject>::err("cannot write the fetch request file"));
            return;
        }
        f.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    }
    python::PythonRunner::RunOptions opts;
    opts.expect_json = true;
    opts.timeout_ms = kFetchWatchdogMs;
    auto on_line = [progress](const QString& line, bool is_stderr) {
        if (!is_stderr || !line.startsWith(QLatin1String("ETFR_PROGRESS ")))
            return;
        const QJsonObject o = QJsonDocument::fromJson(line.mid(14).toUtf8()).object();
        if (progress)
            progress(o.value(QStringLiteral("stage")).toString(), o.value(QStringLiteral("done")).toInt(),
                     o.value(QStringLiteral("total")).toInt());
    };
    python::PythonRunner::instance().run_with_options(
        QStringLiteral("etf_research_data.py"),
        {QStringLiteral("fetch"), QStringLiteral("--request"), req_path, QStringLiteral("--out"), out_path}, opts,
        [req_path, out_path, done](const python::PythonResult& r) {
            QFile::remove(req_path);
            QFile f(out_path);
            const bool have = f.open(QIODevice::ReadOnly);
            const QByteArray bytes = have ? f.readAll() : QByteArray();
            f.close();
            QFile::remove(out_path); // licensed observations stay only in the profile database
            if (!r.success || !have) {
                done(Result<QJsonObject>::err(QStringLiteral("fetch script failed (exit %1): %2")
                                                  .arg(r.exit_code)
                                                  .arg((r.error.isEmpty() ? r.output : r.error).left(400))
                                                  .toStdString()));
                return;
            }
            QJsonParseError pe;
            const QJsonDocument d = QJsonDocument::fromJson(bytes, &pe);
            if (pe.error != QJsonParseError::NoError || !d.isObject()) {
                done(Result<QJsonObject>::err("fetch payload is not valid JSON"));
                return;
            }
            done(Result<QJsonObject>::ok(d.object()));
        },
        on_line);
}

void EtfResearchService::default_ibkr_stage(std::function<void(SourceStageStatus)> done) {
    const QDateTime started = QDateTime::currentDateTimeUtc();
    if (!EtfDataService::instance().ibkr_configured()) {
        done(stage_status(QStringLiteral("ibkr_daily"), QStringLiteral("NOT_CONFIGURED"),
                          QStringLiteral("no local read-only IBKR configuration"), started, QDateTime()));
        return;
    }
    ibkr::IbkrTwsService::instance().probe([started, done](const ibkr::IbkrTwsProbeResult& p) {
        if (!p.ok || !p.ready) {
            done(stage_status(
                QStringLiteral("ibkr_daily"), QStringLiteral("UNAVAILABLE"),
                QStringLiteral("TWS not reachable: %1 %2").arg(p.failure_type, p.failure_message).trimmed(), started,
                QDateTime::currentDateTimeUtc()));
            return;
        }
        auto instruments = EtfDataRepository::instance().listed_instruments(QDateTime::currentDateTimeUtc());
        auto symbols = std::make_shared<QStringList>();
        if (instruments.is_ok())
            for (const auto& row : instruments.value())
                if (!row.symbols.isEmpty())
                    symbols->append(row.symbols.last().symbol);
        auto agg = std::make_shared<SourceStageStatus>(
            stage_status(QStringLiteral("ibkr_daily"), QString(), QString(), started, QDateTime()));
        agg->items_requested = symbols->size();
        auto step = std::make_shared<std::function<void(int)>>();
        std::weak_ptr<std::function<void(int)>> wstep = step;
        *step = [symbols, agg, wstep, done](int idx) {
            if (idx >= symbols->size()) {
                agg->retrieved_at = QDateTime::currentDateTimeUtc();
                agg->status = agg->items_requested == 0                    ? QStringLiteral("UNAVAILABLE")
                              : agg->items_ok == 0                         ? QStringLiteral("FAILED")
                              : agg->items_ok < agg->items_requested       ? QStringLiteral("PARTIAL")
                              : agg->rows_inserted + agg->rows_revised > 0 ? QStringLiteral("UPDATED")
                                                                           : QStringLiteral("UNCHANGED");
                if (!agg->failed_subjects.isEmpty())
                    agg->detail = QStringLiteral("Failed: %1").arg(agg->failed_subjects.join(QStringLiteral(", ")));
                done(*agg);
                return;
            }
            auto self = wstep.lock(); // the pending callback keeps the chain alive, nothing else does
            EtfDataService::instance().ingest_ibkr_daily(
                symbols->at(idx), QStringLiteral("1 M"), [agg, self, idx, symbols](const IbkrDailyRunSummary& s) {
                    const bool ok = s.status == RetrievalStatus::Ok || s.status == RetrievalStatus::Stale;
                    if (ok)
                        ++agg->items_ok;
                    else
                        agg->failed_subjects.append(symbols->at(idx));
                    agg->rows_inserted += s.observations_inserted;
                    agg->rows_revised += s.observations_revised;
                    agg->rows_confirmed += s.observations_confirmed;
                    if (s.requested_last_session > agg->latest_effective)
                        agg->latest_effective = s.requested_last_session;
                    (*self)(idx + 1);
                });
        };
        (*step)(0);
    });
}

void EtfResearchService::default_sec_stage(std::function<void(SourceStageStatus)> done) {
    const QDateTime started = QDateTime::currentDateTimeUtc();
    if (!EtfDataService::instance().sec_config().declared) {
        done(stage_status(QStringLiteral("sec_nport"), QStringLiteral("NOT_CONFIGURED"),
                          QStringLiteral("no declared SEC User-Agent (sec_edgar.json / MARKETLAB_SEC_USER_AGENT)"),
                          started, QDateTime()));
        return;
    }
    auto entities = EtfDataRepository::instance().reporting_entities(QDateTime::currentDateTimeUtc());
    auto list = std::make_shared<QVector<etf_store::ReportingEntityRow>>();
    if (entities.is_ok())
        *list = entities.value();
    auto agg = std::make_shared<SourceStageStatus>(
        stage_status(QStringLiteral("sec_nport"), QString(), QString(), started, QDateTime()));
    agg->items_requested = list->size();
    auto step = std::make_shared<std::function<void(int)>>();
    std::weak_ptr<std::function<void(int)>> wstep = step;
    *step = [list, agg, wstep, done](int idx) {
        if (idx >= list->size()) {
            agg->retrieved_at = QDateTime::currentDateTimeUtc();
            agg->status = agg->items_requested == 0                    ? QStringLiteral("UNAVAILABLE")
                          : agg->items_ok == 0                         ? QStringLiteral("FAILED")
                          : agg->items_ok < agg->items_requested       ? QStringLiteral("PARTIAL")
                          : agg->rows_inserted + agg->rows_revised > 0 ? QStringLiteral("UPDATED")
                                                                       : QStringLiteral("UNCHANGED");
            if (!agg->failed_subjects.isEmpty())
                agg->detail = QStringLiteral("Failed: %1").arg(agg->failed_subjects.join(QStringLiteral(", ")));
            done(*agg);
            return;
        }
        auto self = wstep.lock();
        SecNportRequest req;
        req.cik = list->at(idx).cik;
        req.series_id = list->at(idx).series_id;
        const auto latest = EtfResearchRepository::instance().latest_sec_report_period(list->at(idx).entity_id);
        req.max_filings = sec_filings_to_request(latest.is_ok() ? latest.value() : QDate(), QDate::currentDate(),
                                                 kSecMaxFilingsPerRun);
        EtfDataService::instance().ingest_sec_nport(req, [agg, self, idx, list](const SecNportRunSummary& s) {
            const QString key = list->at(idx).cik + QLatin1Char('/') + list->at(idx).series_id;
            if (s.status == RetrievalStatus::Ok)
                ++agg->items_ok;
            else
                agg->failed_subjects.append(key);
            agg->rows_inserted += s.observations_inserted;
            agg->rows_revised += s.observations_amended;
            agg->rows_confirmed += s.observations_confirmed + s.observations_already_recorded;
            (*self)(idx + 1);
        });
    };
    (*step)(0);
}

void EtfResearchService::refresh(const QString& trigger, Progress progress, Done done, bool full) {
    if (running_) {
        RefreshResult r;
        r.error = QStringLiteral("a refresh is already running");
        if (done)
            done(r);
        return;
    }
    QString err;
    const ResearchUniverse* u = universe(&err);
    if (!u) {
        RefreshResult r;
        r.error = err;
        if (done)
            done(r);
        return;
    }
    running_ = true;
    ++fetch_invocations_;
    RefreshFetcher fetch = fetcher_ ? fetcher_
                                    : RefreshFetcher([this](const QJsonObject& rq, RefreshProgress p,
                                                            std::function<void(Result<QJsonObject>)> d) {
                                          default_fetch(rq, std::move(p), std::move(d));
                                      });
    RefreshStage ibkr = ibkr_stage_ ? ibkr_stage_ : RefreshStage([this](std::function<void(SourceStageStatus)> d) {
        default_ibkr_stage(std::move(d));
    });
    RefreshStage sec = sec_stage_ ? sec_stage_ : RefreshStage([this](std::function<void(SourceStageStatus)> d) {
        default_sec_stage(std::move(d));
    });
    emit refresh_started(QString());
    run_refresh_pipeline(
        trigger, u->version, build_request(full), fetch, ibkr, sec,
        []() { return expected_us_session(QDateTime::currentDateTimeUtc()); },
        [this, progress](const QString& s, int d, int t) {
            if (progress)
                progress(s, d, t);
            emit refresh_progress(s, d, t);
        },
        [this, done](const RefreshResult& r) {
            running_ = false;
            emit refresh_finished(r);
            if (done)
                done(r);
        },
        // The store write (hundreds of thousands of rows) runs on a worker
        // thread with its own database connection; the pipeline continues on
        // this thread when it is done, so the UI stays responsive.
        [](std::function<void()> work, std::function<void()> then) {
            auto* watcher = new QFutureWatcher<void>();
            QObject::connect(watcher, &QFutureWatcher<void>::finished, watcher, [watcher, then]() {
                then();
                watcher->deleteLater();
            });
            watcher->setFuture(QtConcurrent::run(std::move(work)));
        });
}

} // namespace fincept::services::etf::research
