#include "storage/repositories/EtfResearchRepository.h"

#include "services/etf/EtfSessionCalendar.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSet>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

#include <cmath>
#include <optional>

namespace fincept {

namespace {

using namespace services::etf::research;
using services::etf::UsEquityCalendar;

QString etfr_iso(const QDateTime& t) {
    return t.isValid() ? t.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'")) : QString();
}

QDateTime etfr_parse_time(const QString& s) {
    if (s.isEmpty())
        return {};
    const QDateTime t = QDateTime::fromString(s, Qt::ISODateWithMs);
    return t.isValid() ? t.toUTC() : QDateTime();
}

QVariant etfr_opt(const QJsonValue& v) {
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        return QVariant();
    return QVariant(v.toDouble());
}

/// A TEXT value that must never become SQL NULL: a null QString binds NULL in
/// the SQLite driver (see EtfDataRepository's etf_text).
QVariant etfr_text(const QString& s) {
    return s.isNull() ? QVariant(QString::fromUtf8("")) : QVariant(s);
}

QVariant etfr_text_or_null(const QString& s) {
    return s.isEmpty() ? QVariant() : QVariant(s);
}

bool etfr_same(double a, double b) {
    if (a == b)
        return true;
    return std::abs(a - b) <= 1e-12 * std::max(std::abs(a), std::abs(b));
}

bool etfr_same_opt(const QVariant& a, const QVariant& b) {
    if (a.isNull() || b.isNull())
        return a.isNull() && b.isNull();
    return etfr_same(a.toDouble(), b.toDouble());
}

/// Which calculations depend on each stage (reported after a refresh).
QStringList etfr_dependents(const QString& stage) {
    if (stage == QLatin1String("yahoo_history"))
        return {QStringLiteral("returns"),
                QStringLiteral("relative_returns"),
                QStringLiteral("rrg"),
                QStringLiteral("turnover_tilt"),
                QStringLiteral("momentum"),
                QStringLiteral("breadth"),
                QStringLiteral("correlation_regime"),
                QStringLiteral("regime_hmm"),
                QStringLiteral("thai_baskets"),
                QStringLiteral("country_carry"),
                QStringLiteral("estimated_flow_e3")};
    if (stage == QLatin1String("yahoo_funds"))
        return {QStringLiteral("fund_facts"), QStringLiteral("estimated_flow"), QStringLiteral("sector_valuation"),
                QStringLiteral("holdings")};
    if (stage == QLatin1String("yahoo_constituent_history"))
        return {QStringLiteral("constituent_rows"), QStringLiteral("intl_sector_heatmap")};
    if (stage == QLatin1String("yahoo_fundamentals"))
        return {QStringLiteral("constituent_aggregates"), QStringLiteral("intl_sector_fundamentals")};
    if (stage == QLatin1String("fred"))
        return {QStringLiteral("business_cycle"), QStringLiteral("macro_factor"), QStringLiteral("valuation_spread"),
                QStringLiteral("regime_hmm"), QStringLiteral("confluence")};
    if (stage == QLatin1String("world_bank"))
        return {QStringLiteral("country_quality"), QStringLiteral("country_composite")};
    if (stage == QLatin1String("cftc"))
        return {QStringLiteral("cftc_positioning_context")};
    if (stage == QLatin1String("ibkr_daily"))
        return {QStringLiteral("ibkr_cross_check"), QStringLiteral("batch_c_rotation_proxies")};
    if (stage == QLatin1String("sec_nport"))
        return {QStringLiteral("measured_regulatory_flow"), QStringLiteral("estimate_validation")};
    return {};
}

struct EtfrWriteCounts {
    int inserted = 0;
    int revised = 0;
    int confirmed = 0;
};

Result<qint64> etfr_insert_retrieval(const QString& run_id, const QString& stage, const QString& subject,
                                     const QString& requested_at, const QString& retrieved_at, const QString& status,
                                     const QString& detail, const QString& sha, int rows, const QString& first_eff,
                                     const QString& last_eff) {
    auto r = Database::instance().execute(
        QStringLiteral(
            "INSERT INTO etf_research_retrievals (run_id, stage, subject, requested_at, retrieved_at, status, "
            "detail, response_sha256, rows_received, first_effective, last_effective) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,?)"),
        {etfr_text(run_id), etfr_text(stage), etfr_text(subject), etfr_text(requested_at),
         etfr_text_or_null(retrieved_at), etfr_text(status), etfr_text(detail), etfr_text(sha),
         status == QLatin1String("OK") || status == QLatin1String("PARTIAL") ? rows : 0, etfr_text_or_null(first_eff),
         etfr_text_or_null(last_eff)});
    if (r.is_err())
        return Result<qint64>::err(r.error());
    return Result<qint64>::ok(r.value().lastInsertId().toLongLong());
}

/// Append-only vintage write of one symbol's bars.
Result<EtfrWriteCounts> etfr_write_bars(const QString& symbol, const QJsonArray& rows, qint64 retrieval_id,
                                        const QString& seen_at) {
    EtfrWriteCounts c;
    struct Latest {
        int revision = 0;
        double close = 0, dividend = 0, cg = 0, split = 0;
        QVariant volume;
    };
    QHash<QString, Latest> latest;
    {
        auto q = Database::instance().execute(
            QStringLiteral("SELECT session_date, revision, close, volume, dividend, capital_gain, split_ratio "
                           "FROM etf_research_bars WHERE symbol = ? ORDER BY session_date, revision"),
            {symbol});
        if (q.is_err())
            return Result<EtfrWriteCounts>::err(q.error());
        auto& s = q.value();
        while (s.next()) {
            Latest l;
            l.revision = s.value(1).toInt();
            l.close = s.value(2).toDouble();
            l.volume = s.value(3);
            l.dividend = s.value(4).toDouble();
            l.cg = s.value(5).toDouble();
            l.split = s.value(6).toDouble();
            latest.insert(s.value(0).toString(), l);
        }
    }
    QVector<QVariantList> inserts, confirms;
    for (const QJsonValue& v : rows) {
        const QJsonArray r = v.toArray();
        if (r.size() < 6)
            continue;
        const QString date = r[0].toString();
        const double close = r[1].toDouble();
        const QVariant volume = etfr_opt(r[2]);
        const double div = std::max(0.0, r[3].toDouble());
        const double cg = std::max(0.0, r[4].toDouble());
        const double split = std::max(0.0, r[5].toDouble());
        if (!(close > 0.0) || !QDate::fromString(date, Qt::ISODate).isValid())
            continue;
        const auto it = latest.constFind(date);
        if (it != latest.constEnd() && etfr_same(it->close, close) && etfr_same_opt(it->volume, volume) &&
            etfr_same(it->dividend, div) && etfr_same(it->cg, cg) && etfr_same(it->split, split)) {
            confirms.append({seen_at, retrieval_id, symbol, date, it->revision});
            ++c.confirmed;
            continue;
        }
        const int revision = it == latest.constEnd() ? 1 : it->revision + 1;
        if (revision > 1)
            ++c.revised;
        else
            ++c.inserted;
        inserts.append({symbol, date, revision, close, volume, div, cg, split, QStringLiteral("yahoo_chart"), seen_at,
                        seen_at, retrieval_id, retrieval_id});
    }
    auto ins = Database::instance().execute_many(
        QStringLiteral("INSERT INTO etf_research_bars (symbol, session_date, revision, close, volume, dividend, "
                       "capital_gain, split_ratio, source, first_seen_at, last_seen_at, first_retrieval_id, "
                       "last_retrieval_id) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)"),
        inserts);
    if (ins.is_err())
        return Result<EtfrWriteCounts>::err(ins.error());
    auto upd = Database::instance().execute_many(
        QStringLiteral(
            "UPDATE etf_research_bars SET last_seen_at = ?, last_retrieval_id = ?, seen_count = seen_count + 1 "
            "WHERE symbol = ? AND session_date = ? AND revision = ?"),
        confirms);
    if (upd.is_err())
        return Result<EtfrWriteCounts>::err(upd.error());
    return Result<EtfrWriteCounts>::ok(c);
}

/// Append-only vintage write of macro points of one (source, series, area).
Result<EtfrWriteCounts> etfr_write_macro(const QString& source, const QString& series, const QString& area,
                                         const QVector<QPair<QString, QVariant>>& points, qint64 retrieval_id,
                                         const QString& seen_at) {
    EtfrWriteCounts c;
    QHash<QString, QPair<int, QVariant>> latest;
    auto q =
        Database::instance().execute(QStringLiteral("SELECT obs_date, revision, value FROM etf_research_macro WHERE "
                                                    "source = ? AND series_id = ? AND area = ? "
                                                    "ORDER BY obs_date, revision"),
                                     {source, series, etfr_text(area)});
    if (q.is_err())
        return Result<EtfrWriteCounts>::err(q.error());
    while (q.value().next())
        latest.insert(q.value().value(0).toString(), {q.value().value(1).toInt(), q.value().value(2)});
    QVector<QVariantList> inserts, confirms;
    for (const auto& p : points) {
        const auto it = latest.constFind(p.first);
        if (it != latest.constEnd() && etfr_same_opt(it->second, p.second)) {
            confirms.append({seen_at, retrieval_id, source, series, etfr_text(area), p.first, it->first});
            ++c.confirmed;
            continue;
        }
        const int revision = it == latest.constEnd() ? 1 : it->first + 1;
        if (revision > 1)
            ++c.revised;
        else
            ++c.inserted;
        inserts.append({source, series, etfr_text(area), p.first, revision, p.second, seen_at, seen_at, retrieval_id,
                        retrieval_id});
    }
    auto ins = Database::instance().execute_many(
        QStringLiteral(
            "INSERT INTO etf_research_macro (source, series_id, area, obs_date, revision, value, "
            "first_seen_at, last_seen_at, first_retrieval_id, last_retrieval_id) VALUES (?,?,?,?,?,?,?,?,?,?)"),
        inserts);
    if (ins.is_err())
        return Result<EtfrWriteCounts>::err(ins.error());
    auto upd = Database::instance().execute_many(
        QStringLiteral(
            "UPDATE etf_research_macro SET last_seen_at = ?, last_retrieval_id = ?, seen_count = seen_count + 1 "
            "WHERE source = ? AND series_id = ? AND area = ? AND obs_date = ? AND revision = ?"),
        confirms);
    if (upd.is_err())
        return Result<EtfrWriteCounts>::err(upd.error());
    return Result<EtfrWriteCounts>::ok(c);
}

/// Repeated keys (dates) within one delivery. An identical repeat is kept once;
/// when the repeats of a key differ, every row of that key is left out (which
/// one is right is unknown), never the item. Order is kept; `repeated` counts
/// the identical repeats dropped, `conflicting` the keys left out.
template <typename Row, typename KeyOf, typename Same>
QVector<Row> etfr_unique_rows(const QVector<Row>& rows, KeyOf key_of, Same same, int* repeated, int* conflicting) {
    QHash<QString, qsizetype> first;
    QSet<QString> conflict;
    QVector<Row> kept;
    for (const Row& r : rows) {
        const QString k = key_of(r);
        const auto f = first.constFind(k);
        if (f == first.constEnd()) {
            first.insert(k, kept.size());
            kept.append(r);
        } else if (same(kept.at(*f), r)) {
            ++*repeated;
        } else {
            conflict.insert(k);
        }
    }
    *conflicting += static_cast<int>(conflict.size());
    if (conflict.isEmpty())
        return kept;
    QVector<Row> out;
    for (const Row& r : kept)
        if (!conflict.contains(key_of(r)))
            out.append(r);
    return out;
}

using EtfrPoints = QVector<QPair<QString, QVariant>>;

EtfrPoints etfr_unique_points(const EtfrPoints& pts, int* repeated, int* conflicting) {
    return etfr_unique_rows(
        pts, [](const QPair<QString, QVariant>& x) { return x.first; },
        [](const QPair<QString, QVariant>& a, const QPair<QString, QVariant>& b) {
            return etfr_same_opt(a.second, b.second);
        },
        repeated, conflicting);
}

/// Records the outcome of a fund's holdings read (v055) for a yahoo_funds
/// retrieval; nothing when the item carries no recognised holdings status.
Result<void> etfr_record_holdings_read(qint64 retrieval_id, const QString& symbol, const QJsonObject& item) {
    const QString status = item.value(QStringLiteral("holdings_status")).toString();
    if (status != QLatin1String("OK") && status != QLatin1String("UNAVAILABLE") && status != QLatin1String("FAILED"))
        return Result<void>::ok();
    auto r = Database::instance().execute(
        QStringLiteral(
            "INSERT INTO etf_research_holdings_reads (retrieval_id, symbol, status, detail) VALUES (?,?,?,?)"),
        {retrieval_id, symbol, status, etfr_text(item.value(QStringLiteral("holdings_detail")).toString())});
    if (r.is_err())
        return Result<void>::err(r.error());
    return Result<void>::ok();
}

/// Why an item that was obtained is only partly usable ('' when it is whole):
/// rows left out as unparseable, World Bank pages that failed, a fund whose
/// quote summary failed while its holdings came, or corporate actions on rows
/// without a close (kept as provenance, not applied).
QString etfr_partial_reason(const QJsonObject& it) {
    QStringList why;
    if (const int n = it.value(QStringLiteral("unparseable_points")).toInt(); n > 0)
        why << QStringLiteral("%1 unparseable row(s) left out").arg(n);
    if (!it.value(QStringLiteral("pages_failed")).toArray().isEmpty())
        why << QStringLiteral("%1 page(s) not read").arg(it.value(QStringLiteral("pages_failed")).toArray().size());
    if (it.value(QStringLiteral("holdings_status")).toString() == QLatin1String("FAILED"))
        why << QStringLiteral("holdings could not be read");
    if (it.value(QStringLiteral("provider_not_read")).toBool())
        why << QStringLiteral("provider not read; stored CFTC archive used");
    if (const int n = it.value(QStringLiteral("sector_weights_unparseable")).toInt(); n > 0)
        why << QStringLiteral("%1 non-numeric sector weight(s) left out").arg(n);
    if (it.value(QStringLiteral("quote_status")).toString() == QLatin1String("FAILED"))
        why << QStringLiteral("quote summary failed, holdings kept");
    if (const int n = it.value(QStringLiteral("actions_without_close")).toArray().size(); n > 0)
        why << QStringLiteral("%1 corporate action(s) on rows without a close").arg(n);
    return why.join(QStringLiteral(", "));
}

/// The corporate actions of rows without a close, as text for the provenance.
QString etfr_actions_text(const QJsonObject& it) {
    QStringList a;
    for (const auto& v : it.value(QStringLiteral("actions_without_close")).toArray()) {
        const QJsonArray r = v.toArray();
        if (r.size() >= 4)
            a << QStringLiteral("%1 dividend %2 capital gain %3 split %4")
                     .arg(r.at(0).toString())
                     .arg(r.at(1).toDouble())
                     .arg(r.at(2).toDouble())
                     .arg(r.at(3).toDouble());
    }
    return a.isEmpty() ? QString()
                       : QStringLiteral("corporate action(s) on rows without a close, kept here, not applied: ") +
                             a.join(QStringLiteral("; "));
}

SourceStageStatus etfr_finish_status(SourceStageStatus st) {
    st.dependent_calculations = etfr_dependents(st.stage);
    if (st.items_requested == 0) {
        st.status = QStringLiteral("UNAVAILABLE");
        if (st.detail.isEmpty())
            st.detail = QStringLiteral("nothing requested");
    } else if (st.items_ok == 0) {
        st.status = QStringLiteral("FAILED");
    } else if (st.items_ok < st.items_requested) {
        st.status = QStringLiteral("PARTIAL");
    } else if (st.stale_items > 0) {
        st.status = QStringLiteral("STALE");
    } else if (st.rows_inserted + st.rows_revised > 0) {
        st.status = QStringLiteral("UPDATED");
    } else {
        st.status = QStringLiteral("UNCHANGED");
    }
    if (st.stale_items > 0 && st.status != QLatin1String("STALE"))
        st.detail += QStringLiteral(" %1 item(s) older than their freshness rule.").arg(st.stale_items);
    if (!st.failed_subjects.isEmpty())
        st.detail += QStringLiteral(" Failed: %1.")
                         .arg(st.failed_subjects.mid(0, 12).join(QStringLiteral(", ")) +
                              (st.failed_subjects.size() > 12 ? QStringLiteral(", …") : QString()));
    st.detail = st.detail.trimmed();
    return st;
}

QJsonObject etfr_status_json(const SourceStageStatus& st) {
    return {{QStringLiteral("stage"), st.stage},
            {QStringLiteral("status"), st.status},
            {QStringLiteral("detail"), st.detail},
            {QStringLiteral("requested_at"), etfr_iso(st.requested_at)},
            {QStringLiteral("retrieved_at"), etfr_iso(st.retrieved_at)},
            {QStringLiteral("latest_effective"), st.latest_effective},
            {QStringLiteral("items_requested"), st.items_requested},
            {QStringLiteral("items_ok"), st.items_ok},
            {QStringLiteral("rows_inserted"), st.rows_inserted},
            {QStringLiteral("rows_revised"), st.rows_revised},
            {QStringLiteral("rows_confirmed"), st.rows_confirmed},
            {QStringLiteral("stale_items"), st.stale_items},
            {QStringLiteral("failed_subjects"), QJsonArray::fromStringList(st.failed_subjects)},
            {QStringLiteral("dependent_calculations"), QJsonArray::fromStringList(st.dependent_calculations)}};
}

SourceStageStatus etfr_status_from_json(const QJsonObject& o) {
    SourceStageStatus st;
    st.stage = o.value(QStringLiteral("stage")).toString();
    st.status = o.value(QStringLiteral("status")).toString();
    st.detail = o.value(QStringLiteral("detail")).toString();
    st.requested_at = etfr_parse_time(o.value(QStringLiteral("requested_at")).toString());
    st.retrieved_at = etfr_parse_time(o.value(QStringLiteral("retrieved_at")).toString());
    st.latest_effective = o.value(QStringLiteral("latest_effective")).toString();
    st.items_requested = o.value(QStringLiteral("items_requested")).toInt();
    st.items_ok = o.value(QStringLiteral("items_ok")).toInt();
    st.rows_inserted = o.value(QStringLiteral("rows_inserted")).toInt();
    st.rows_revised = o.value(QStringLiteral("rows_revised")).toInt();
    st.rows_confirmed = o.value(QStringLiteral("rows_confirmed")).toInt();
    st.stale_items = o.value(QStringLiteral("stale_items")).toInt();
    for (const auto& v : o.value(QStringLiteral("failed_subjects")).toArray())
        st.failed_subjects.append(v.toString());
    for (const auto& v : o.value(QStringLiteral("dependent_calculations")).toArray())
        st.dependent_calculations.append(v.toString());
    return st;
}

} // namespace

EtfResearchRepository& EtfResearchRepository::instance() {
    static EtfResearchRepository s;
    return s;
}

QDate EtfResearchRepository::prior_completed_session(const QDateTime& captured_at) {
    const QDate ny = UsEquityCalendar::exchange_date(captured_at);
    const auto s = UsEquityCalendar::last_session_before(ny);
    return s ? s->date : QDate();
}

Result<void> EtfResearchRepository::begin_run(const QString& run_id, const QString& trigger,
                                              const QDateTime& started_at, const QString& universe_version) {
    auto r = db().execute(QStringLiteral("INSERT INTO etf_research_runs (run_id, trigger, started_at, status, "
                                         "universe_version) VALUES (?,?,?,'RUNNING',?)"),
                          {run_id, trigger, etfr_iso(started_at), universe_version});
    if (r.is_err())
        return Result<void>::err(r.error());
    return Result<void>::ok();
}

Result<void> EtfResearchRepository::finish_run(const QString& run_id, const QString& status,
                                               const QDateTime& finished_at, const QString& script_version,
                                               const QVector<SourceStageStatus>& stages) {
    QJsonArray a;
    for (const auto& st : stages)
        a.append(etfr_status_json(st));
    auto r = db().execute(
        QStringLiteral("UPDATE etf_research_runs SET status = ?, finished_at = ?, script_version = ?, "
                       "summary_json = ? WHERE run_id = ?"),
        {status, etfr_iso(finished_at), etfr_text(script_version),
         QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("stages"), a}}).toJson(QJsonDocument::Compact)),
         run_id});
    if (r.is_err())
        return Result<void>::err(r.error());
    return Result<void>::ok();
}

Result<qint64> EtfResearchRepository::record_stage_retrieval(const QString& run_id, const QString& stage,
                                                             const QString& subject, const QDateTime& requested_at,
                                                             const QDateTime& retrieved_at, const QString& status,
                                                             const QString& detail, int rows) {
    return etfr_insert_retrieval(run_id, stage, subject, etfr_iso(requested_at), etfr_iso(retrieved_at), status, detail,
                                 QString(), rows, QString(), QString());
}

Result<QVector<SourceStageStatus>> EtfResearchRepository::persist_payload(const QString& run_id,
                                                                          const QJsonObject& payload,
                                                                          const QDate& expected_us_session) {
    using R = Result<QVector<SourceStageStatus>>;
    QVector<SourceStageStatus> out;
    const QJsonObject stages = payload.value(QStringLiteral("stages")).toObject();
    auto tx = db().begin_transaction();
    if (tx.is_err())
        return R::err(tx.error());
    auto fail = [&](const std::string& e) {
        db().rollback();
        return R::err(e);
    };
    for (const QString& stage : {QStringLiteral("yahoo_history"), QStringLiteral("yahoo_constituent_history"),
                                 QStringLiteral("yahoo_funds"), QStringLiteral("yahoo_fundamentals"),
                                 QStringLiteral("fred"), QStringLiteral("world_bank"), QStringLiteral("cftc")}) {
        if (!stages.contains(stage))
            continue;
        const QJsonObject so = stages.value(stage).toObject();
        const QJsonObject items = so.value(QStringLiteral("items")).toObject();
        SourceStageStatus st;
        st.stage = stage;
        if (so.value(QStringLiteral("skipped_fresh")).toInt() > 0)
            st.detail = QStringLiteral("%1 constituent(s) captured within %2 days were not re-fetched.")
                            .arg(so.value(QStringLiteral("skipped_fresh")).toInt())
                            .arg(so.value(QStringLiteral("fresh_days")).toInt());
        st.requested_at = etfr_parse_time(so.value(QStringLiteral("requested_at")).toString());
        st.retrieved_at = etfr_parse_time(so.value(QStringLiteral("retrieved_at")).toString());
        st.items_requested = items.size();
        const QString req_at = so.value(QStringLiteral("requested_at")).toString();
        QString latest_eff;
        QStringList partly; // obtained items that are only partly usable
        QStringList keys = items.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString& subject : keys) {
            // Each item is stored behind its own savepoint: an item that cannot be
            // stored is rolled back alone and recorded FAILED with the reason,
            // and the stage's other items are kept (one bad row or item never
            // undoes the rest of the delivery).
            const SourceStageStatus st_before = st;
            const QString latest_before = latest_eff;
            const qsizetype partly_before = partly.size();
            if (auto sp = db().execute(QStringLiteral("SAVEPOINT etfr_item")); sp.is_err())
                return fail(sp.error());
            const Result<void> item = [&]() -> Result<void> {
                const QJsonObject it = items.value(subject).toObject();
                // An item obtained in part is PARTIAL, with what was left out named;
                // what was obtained is stored (2026-10-04 data-preservation rule).
                QString partial = etfr_partial_reason(it);
                const bool obtained = it.value(QStringLiteral("status")).toString() == QLatin1String("OK");
                QString status = !obtained           ? QStringLiteral("FAILED")
                                 : partial.isEmpty() ? QStringLiteral("OK")
                                                     : QStringLiteral("PARTIAL");
                QString detail = it.value(QStringLiteral("detail")).toString();
                if (const QString acts = etfr_actions_text(it); !acts.isEmpty())
                    detail = detail.isEmpty() ? acts : detail + QStringLiteral("; ") + acts;
                // Rows this store cannot read are left out on their own, counted and
                // named; the item is then PARTIAL. Never silently dropped.
                // `lost`: something received is not stored, so the item is PARTIAL;
                // an identical repeat kept once loses nothing and is only noted.
                auto noted = [&](int n, const QString& what, bool lost = true) {
                    if (n <= 0)
                        return;
                    const QString note = QStringLiteral("%1 %2").arg(n).arg(what);
                    detail = detail.isEmpty() ? note : detail + QStringLiteral("; ") + note;
                    if (!lost)
                        return;
                    partial = partial.isEmpty() ? note : partial + QStringLiteral(", ") + note;
                    status = QStringLiteral("PARTIAL");
                };
                auto left_out = [&](int n, const QString& what) { noted(n, what + QStringLiteral(" left out")); };
                // `partly` is filled when the item is done, once every exclusion is known.
                const auto note_partly = qScopeGuard([&] {
                    if (!partial.isEmpty() && status != QLatin1String("FAILED"))
                        partly << QStringLiteral("%1 (%2)").arg(subject, partial);
                });
                const QString ret_at = it.value(QStringLiteral("retrieved_at"))
                                           .toString(it.value(QStringLiteral("captured_at")).toString());
                // An item reported as obtained whose rows are all unusable here is a
                // failed item, recorded with its reason (never left without a retrieval).
                auto fail_item = [&](const QString& why) -> Result<void> {
                    status = QStringLiteral("FAILED");
                    auto rid = etfr_insert_retrieval(run_id, stage, subject, req_at, QString(), status,
                                                     detail.isEmpty() ? why : why + QStringLiteral("; ") + detail,
                                                     QString(), 0, QString(), QString());
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    --st.items_ok;
                    st.failed_subjects.append(subject);
                    return Result<void>::ok();
                };
                if (status == QLatin1String("FAILED")) {
                    auto rid = etfr_insert_retrieval(run_id, stage, subject, req_at, QString(), status, detail,
                                                     QString(), 0, QString(), QString());
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    if (stage == QLatin1String("yahoo_funds"))
                        if (auto hr = etfr_record_holdings_read(rid.value(), subject, it); hr.is_err())
                            return hr;
                    st.failed_subjects.append(subject);
                    return Result<void>::ok();
                }
                ++st.items_ok;
                if (stage == QLatin1String("yahoo_history") || stage == QLatin1String("yahoo_constituent_history")) {
                    QVector<QJsonArray> parsed;
                    int malformed = 0, bad_volume = 0;
                    for (const QJsonValue& rv : it.value(QStringLiteral("rows")).toArray()) {
                        QJsonArray r = rv.toArray();
                        if (!(r.size() >= 6 && QDate::fromString(r.at(0).toString(), Qt::ISODate).isValid() &&
                              r.at(1).toDouble() > 0.0)) {
                            ++malformed;
                            continue;
                        }
                        // A volume that is not a non-negative number is unusable: the
                        // bar is kept with its volume unknown (NULL), never 0.
                        if (!r.at(2).isNull() && !(r.at(2).isDouble() && r.at(2).toDouble() >= 0.0)) {
                            r[2] = QJsonValue();
                            ++bad_volume;
                        }
                        parsed.append(r);
                    }
                    int repeated = 0, conflicting = 0;
                    parsed = etfr_unique_rows(
                        parsed, [](const QJsonArray& r) { return r.at(0).toString(); },
                        [](const QJsonArray& a, const QJsonArray& b) { return a == b; }, &repeated, &conflicting);
                    std::sort(parsed.begin(), parsed.end(), [](const QJsonArray& a, const QJsonArray& b) {
                        return a.at(0).toString() < b.at(0).toString();
                    });
                    QJsonArray rows;
                    for (const QJsonArray& r : parsed)
                        rows.append(r);
                    left_out(malformed, QStringLiteral("malformed bar row(s)"));
                    noted(bad_volume, QStringLiteral("unusable volume(s) stored as unknown"));
                    noted(repeated, QStringLiteral("identical repeated bar(s) kept once"), false);
                    left_out(conflicting, QStringLiteral("date(s) with conflicting bars"));
                    if (rows.isEmpty()) {
                        // A delivery without one well-formed completed bar is a failed item.
                        if (auto f = fail_item(QStringLiteral("no well-formed bar in the response")); f.is_err())
                            return Result<void>::err(f.error());
                        return Result<void>::ok();
                    }
                    const QString first = rows.first().toArray().at(0).toString();
                    const QString last = rows.last().toArray().at(0).toString();
                    // An incremental delivery (recent sessions only) extends the
                    // symbol's latest coverage window back to its start when its
                    // overlap with the stored closes is consistent. A split, or most
                    // overlapping closes moved by one common factor, is a restatement
                    // of the whole history: the window stays this delivery's range,
                    // so no restated close is ever joined to unrestated ones, and the
                    // next refresh fetches the full history again.
                    QString window_first = first;
                    QString stitch_note;
                    if (it.value(QStringLiteral("incremental")).toBool()) {
                        auto pw = db().execute(QStringLiteral("SELECT first_session, last_session FROM "
                                                              "etf_research_bar_coverage WHERE symbol = ? ORDER BY "
                                                              "retrieved_at DESC, retrieval_id DESC LIMIT 1"),
                                               {subject});
                        if (pw.is_err())
                            return Result<void>::err(pw.error());
                        QString prev_first, prev_last;
                        if (pw.value().next()) {
                            prev_first = pw.value().value(0).toString();
                            prev_last = pw.value().value(1).toString();
                        }
                        bool split = false;
                        QHash<QString, double> fetched;
                        for (const QJsonValue& rv : rows) {
                            const QJsonArray r = rv.toArray();
                            fetched.insert(r.at(0).toString(), r.at(1).toDouble());
                            split = split || r.at(5).toDouble() > 0.0;
                        }
                        int overlap = 0, moved = 0;
                        QVector<double> ratios;
                        if (!prev_last.isEmpty()) {
                            auto ov = db().execute(
                                QStringLiteral(
                                    "SELECT b.session_date, b.close FROM etf_research_bars b WHERE b.symbol = ? "
                                    "AND b.session_date >= ? AND b.session_date <= ? AND b.revision = (SELECT "
                                    "MAX(x.revision) FROM etf_research_bars x WHERE x.symbol = b.symbol AND "
                                    "x.session_date = b.session_date)"),
                                {subject, first, prev_last});
                            if (ov.is_err())
                                return Result<void>::err(ov.error());
                            while (ov.value().next()) {
                                const auto f = fetched.constFind(ov.value().value(0).toString());
                                if (f == fetched.constEnd())
                                    continue;
                                ++overlap;
                                const double ratio = *f / ov.value().value(1).toDouble();
                                if (std::abs(ratio - 1.0) > 1e-6) {
                                    ++moved;
                                    ratios.append(ratio);
                                }
                            }
                        }
                        bool common_factor = false;
                        if (moved >= 2 && moved * 2 >= overlap) {
                            std::sort(ratios.begin(), ratios.end());
                            const double median = ratios[ratios.size() / 2];
                            int near = 0;
                            for (double r : ratios)
                                near += std::abs(r / median - 1.0) < 1e-3 ? 1 : 0;
                            common_factor = near * 2 >= moved;
                        }
                        if (!prev_first.isEmpty() && overlap >= 3 && !split && !common_factor) {
                            window_first = prev_first;
                            stitch_note = QStringLiteral("incremental; joined to the stored history from %1 (%2 of %3 "
                                                         "overlapping closes revised)")
                                              .arg(prev_first)
                                              .arg(moved)
                                              .arg(overlap);
                        } else {
                            stitch_note =
                                QStringLiteral("incremental; NOT joined (%1): this delivery's range only, full "
                                               "history on the next refresh")
                                    .arg(split                 ? QStringLiteral("split in the delivery")
                                         : common_factor       ? QStringLiteral("history restated")
                                         : prev_last.isEmpty() ? QStringLiteral("no stored window")
                                                               : QStringLiteral("overlap too short"));
                        }
                    }
                    auto rid = etfr_insert_retrieval(
                        run_id, stage, subject, req_at, ret_at, status,
                        stitch_note.isEmpty()
                            ? detail
                            : (detail.isEmpty() ? stitch_note : detail + QStringLiteral("; ") + stitch_note),
                        so.value(QStringLiteral("sha256")).toString(), rows.size(), first, last);
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    auto w = etfr_write_bars(subject, rows, rid.value(), ret_at);
                    if (w.is_err())
                        return Result<void>::err(w.error());
                    st.rows_inserted += w.value().inserted;
                    st.rows_revised += w.value().revised;
                    st.rows_confirmed += w.value().confirmed;
                    auto cov = db().execute(
                        QStringLiteral(
                            "INSERT INTO etf_research_bar_coverage (retrieval_id, symbol, first_session, "
                            "last_session, bars, retrieved_at, in_progress_excluded) VALUES (?,?,?,?,?,?,?)"),
                        {rid.value(), subject, window_first, last, static_cast<int>(rows.size()), ret_at,
                         it.value(QStringLiteral("in_progress_excluded")).toInt()});
                    if (cov.is_err())
                        return Result<void>::err(cov.error());
                    const bool us = !subject.contains(QLatin1Char('.')) && !subject.startsWith(QLatin1Char('^'));
                    const QDate ld = QDate::fromString(last, Qt::ISODate);
                    if (us && expected_us_session.isValid() && ld < expected_us_session)
                        ++st.stale_items;
                    if (us && last > latest_eff)
                        latest_eff = last;
                } else if (stage == QLatin1String("yahoo_funds")) {
                    const QJsonObject f = it.value(QStringLiteral("fields")).toObject();
                    const QDateTime captured = etfr_parse_time(it.value(QStringLiteral("captured_at")).toString());
                    const QDate eff = prior_completed_session(captured);
                    // A holding Yahoo lists without a ticker but with its name is kept
                    // (empty symbol); one without a rank, with a repeated rank, or
                    // without symbol and name cannot be identified and is left out,
                    // counted. A sector weight that is not a number is left out alone.
                    QVector<QVariantList> hrows;
                    int unidentified = 0;
                    QSet<int> ranks;
                    for (const auto& hv : it.value(QStringLiteral("holdings")).toArray()) {
                        const QJsonArray h = hv.toArray();
                        const int rank = h.size() >= 4 && h[0].isDouble() ? h[0].toInt() : 0;
                        if (rank < 1 || ranks.contains(rank) ||
                            (h[1].toString().isEmpty() && h[2].toString().isEmpty())) {
                            ++unidentified;
                            continue;
                        }
                        ranks.insert(rank);
                        hrows.append({subject, rank, h[1].toString(), etfr_text(h[2].toString()), etfr_opt(h[3])});
                    }
                    QVector<QVariantList> wrows;
                    int bad_weights = 0;
                    const QJsonObject sw = it.value(QStringLiteral("sector_weights")).toObject();
                    for (auto k = sw.begin(); k != sw.end(); ++k) {
                        if (k.value().isDouble())
                            wrows.append({subject, k.key(), k.value().toDouble()});
                        else
                            ++bad_weights;
                    }
                    left_out(unidentified, QStringLiteral("unidentifiable holding(s)"));
                    left_out(bad_weights, QStringLiteral("non-numeric sector weight(s)"));
                    auto rid = etfr_insert_retrieval(run_id, stage, subject, req_at, etfr_iso(captured), status,
                                                     it.value(QStringLiteral("holdings_status")).toString() +
                                                         QLatin1Char(' ') +
                                                         it.value(QStringLiteral("holdings_detail")).toString() +
                                                         (detail.isEmpty() ? QString() : QStringLiteral("; ") + detail),
                                                     so.value(QStringLiteral("sha256")).toString(), 1,
                                                     eff.toString(Qt::ISODate), eff.toString(Qt::ISODate));
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    QString market_time;
                    if (f.value(QStringLiteral("regularMarketTime")).isDouble())
                        market_time = etfr_iso(QDateTime::fromSecsSinceEpoch(
                            static_cast<qint64>(f.value(QStringLiteral("regularMarketTime")).toDouble()),
                            QTimeZone::UTC));
                    const QVariant aum = f.contains(QStringLiteral("totalAssets"))
                                             ? etfr_opt(f.value(QStringLiteral("totalAssets")))
                                             : etfr_opt(f.value(QStringLiteral("netAssets")));
                    // A fund whose quote summary failed while its holdings came keeps
                    // the holdings; no capture of empty fund facts is written.
                    const bool quote = !f.isEmpty();
                    if (quote) {
                        auto ins = db().execute(
                            QStringLiteral(
                                "INSERT INTO etf_research_fund_snapshots (retrieval_id, symbol, captured_at, "
                                "effective_session, effective_rule, total_assets, nav, previous_close, "
                                "shares_outstanding, market_time, fields_json) VALUES (?,?,?,?,?,?,?,?,?,?,?)"),
                            {rid.value(), subject, etfr_iso(captured), etfr_text_or_null(eff.toString(Qt::ISODate)),
                             eff.isValid() ? QStringLiteral("prior_completed_session_v1")
                                           : QStringLiteral("prior_completed_session_v1:outside_calendar"),
                             aum, etfr_opt(f.value(QStringLiteral("navPrice"))),
                             etfr_opt(f.value(QStringLiteral("regularMarketPreviousClose"))),
                             etfr_opt(f.value(QStringLiteral("sharesOutstanding"))), etfr_text_or_null(market_time),
                             QString::fromUtf8(QJsonDocument(f).toJson(QJsonDocument::Compact))});
                        if (ins.is_err())
                            return Result<void>::err(ins.error());
                    }
                    if (auto hr = etfr_record_holdings_read(rid.value(), subject, it); hr.is_err())
                        return hr;
                    for (auto& row : hrows)
                        row.prepend(rid.value());
                    for (auto& row : wrows)
                        row.prepend(rid.value());
                    auto hw = db().execute_many(
                        QStringLiteral("INSERT INTO etf_research_holdings (retrieval_id, symbol, rank, "
                                       "holding_symbol, holding_name, weight) VALUES (?,?,?,?,?,?)"),
                        hrows);
                    if (hw.is_err())
                        return Result<void>::err(hw.error());
                    auto ww = db().execute_many(
                        QStringLiteral("INSERT INTO etf_research_sector_weights (retrieval_id, symbol, "
                                       "sector_key, weight) VALUES (?,?,?,?)"),
                        wrows);
                    if (ww.is_err())
                        return Result<void>::err(ww.error());
                    if (quote) {
                        ++st.rows_inserted; // every capture is a new observation of an undated snapshot
                        if (eff.toString(Qt::ISODate) > latest_eff)
                            latest_eff = eff.toString(Qt::ISODate);
                    }
                } else if (stage == QLatin1String("yahoo_fundamentals")) {
                    const QString captured = it.value(QStringLiteral("captured_at")).toString();
                    auto rid =
                        etfr_insert_retrieval(run_id, stage, subject, req_at, captured, status, detail,
                                              so.value(QStringLiteral("sha256")).toString(), 1, QString(), QString());
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    auto ins =
                        db().execute(QStringLiteral("INSERT INTO etf_research_fundamentals (retrieval_id, symbol, "
                                                    "captured_at, fields_json) VALUES (?,?,?,?)"),
                                     {rid.value(), subject, captured,
                                      QString::fromUtf8(QJsonDocument(it.value(QStringLiteral("fields")).toObject())
                                                            .toJson(QJsonDocument::Compact))});
                    if (ins.is_err())
                        return Result<void>::err(ins.error());
                    ++st.rows_inserted;
                } else if (stage == QLatin1String("fred")) {
                    // A row without a valid date or a numeric value is left out alone
                    // (never read as 0); the source's missing points are already apart.
                    const QJsonArray rows = it.value(QStringLiteral("rows")).toArray();
                    QVector<QPair<QString, QVariant>> pts;
                    int malformed = 0;
                    for (const auto& rv : rows) {
                        const QJsonArray r = rv.toArray();
                        if (r.size() >= 2 && QDate::fromString(r.at(0).toString(), Qt::ISODate).isValid() &&
                            r.at(1).isDouble())
                            pts.append({r.at(0).toString(), QVariant(r.at(1).toDouble())});
                        else
                            ++malformed;
                    }
                    int repeated = 0, conflicting = 0;
                    pts = etfr_unique_points(pts, &repeated, &conflicting);
                    left_out(malformed, QStringLiteral("malformed row(s)"));
                    noted(repeated, QStringLiteral("identical repeated observation(s) kept once"), false);
                    left_out(conflicting, QStringLiteral("date(s) with conflicting observations"));
                    if (pts.isEmpty()) {
                        if (auto f = fail_item(QStringLiteral("no well-formed observation in the response"));
                            f.is_err())
                            return Result<void>::err(f.error());
                        return Result<void>::ok();
                    }
                    auto rid =
                        etfr_insert_retrieval(run_id, stage, subject, req_at, ret_at, status,
                                              QStringLiteral("missing_points=%1%2")
                                                  .arg(it.value(QStringLiteral("missing_points")).toInt())
                                                  .arg(detail.isEmpty() ? QString() : QStringLiteral("; ") + detail),
                                              it.value(QStringLiteral("response_sha256")).toString(), pts.size(),
                                              pts.first().first, pts.last().first);
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    auto w = etfr_write_macro(QStringLiteral("fred"), subject, QString(), pts, rid.value(), ret_at);
                    if (w.is_err())
                        return Result<void>::err(w.error());
                    st.rows_inserted += w.value().inserted;
                    st.rows_revised += w.value().revised;
                    st.rows_confirmed += w.value().confirmed;
                    auto cov =
                        db().execute(QStringLiteral("INSERT INTO etf_research_macro_coverage (retrieval_id, source, "
                                                    "series_id, area, first_date, last_date, points, retrieved_at) "
                                                    "VALUES (?,'fred',?,'',?,?,?,?)"),
                                     {rid.value(), subject, pts.first().first, pts.last().first,
                                      static_cast<int>(pts.size()), ret_at});
                    if (cov.is_err())
                        return Result<void>::err(cov.error());
                    if (pts.last().first > latest_eff)
                        latest_eff = pts.last().first;
                } else if (stage == QLatin1String("cftc")) {
                    // One weekly series per field of the market: open interest and the
                    // non-commercial long/short positions (a missing cell is no observation).
                    const QJsonArray rows = it.value(QStringLiteral("rows")).toArray();
                    const char* fields[] = {"open_interest", "non_commercial_long", "non_commercial_short"};
                    // Each field stands on its own: a market whose open interest is
                    // missing keeps its positions. A row without a valid date is left
                    // out, counted.
                    QVector<QPair<QString, QVariant>> pts[3];
                    int malformed = 0;
                    for (const auto& rv : rows) {
                        const QJsonArray r = rv.toArray();
                        if (r.size() < 4 || !QDate::fromString(r.at(0).toString(), Qt::ISODate).isValid()) {
                            ++malformed;
                            continue;
                        }
                        for (int f = 0; f < 3; ++f)
                            if (r.at(f + 1).isDouble())
                                pts[f].append({r.at(0).toString(), QVariant(r.at(f + 1).toDouble())});
                    }
                    int repeated = 0, conflicting = 0;
                    for (auto& series : pts) {
                        series = etfr_unique_points(series, &repeated, &conflicting);
                        std::sort(series.begin(), series.end(),
                                  [](const auto& a, const auto& b) { return a.first < b.first; });
                    }
                    left_out(malformed, QStringLiteral("undated row(s)"));
                    noted(repeated, QStringLiteral("identical repeated value(s) kept once"), false);
                    left_out(conflicting, QStringLiteral("report date(s) with conflicting values, per field"));
                    QString first_report, last_report;
                    for (int f = 0; f < 3; ++f) {
                        if (pts[f].isEmpty())
                            continue;
                        if (first_report.isEmpty() || pts[f].first().first < first_report)
                            first_report = pts[f].first().first;
                        if (pts[f].last().first > last_report)
                            last_report = pts[f].last().first;
                    }
                    if (last_report.isEmpty()) {
                        if (auto f = fail_item(QStringLiteral("no reported position in the response")); f.is_err())
                            return Result<void>::err(f.error());
                        return Result<void>::ok();
                    }
                    auto rid = etfr_insert_retrieval(
                        run_id, stage, subject, req_at, ret_at, status,
                        QStringLiteral("cftc_tool_status=%1 %2")
                            .arg(it.value(QStringLiteral("source_status")).toString(), detail)
                            .trimmed(),
                        QString(), static_cast<int>(rows.size()) - malformed, first_report, last_report);
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    for (int f = 0; f < 3; ++f) {
                        if (pts[f].isEmpty())
                            continue;
                        const QString area = QLatin1String(fields[f]);
                        auto w = etfr_write_macro(QStringLiteral("cftc"), subject, area, pts[f], rid.value(), ret_at);
                        if (w.is_err())
                            return Result<void>::err(w.error());
                        st.rows_inserted += w.value().inserted;
                        st.rows_revised += w.value().revised;
                        st.rows_confirmed += w.value().confirmed;
                        auto cov = db().execute(
                            QStringLiteral(
                                "INSERT INTO etf_research_macro_coverage (retrieval_id, source, series_id, "
                                "area, first_date, last_date, points, retrieved_at) VALUES (?,'cftc',?,?,?,?,?,?)"),
                            {rid.value(), subject, area, pts[f].first().first, pts[f].last().first,
                             static_cast<int>(pts[f].size()), ret_at});
                        if (cov.is_err())
                            return Result<void>::err(cov.error());
                    }
                    if (last_report > latest_eff)
                        latest_eff = last_report;
                } else if (stage == QLatin1String("world_bank")) {
                    const QJsonArray rows = it.value(QStringLiteral("rows")).toArray();
                    // A row without an area or a year is left out alone, counted; a null
                    // value stays the source's null (missing, never 0).
                    QHash<QString, QVector<QPair<QString, QVariant>>> by_area;
                    int malformed = 0;
                    for (const auto& rv : rows) {
                        const QJsonArray r = rv.toArray();
                        if (r.size() < 3 || r.at(0).toString().isEmpty() || !r.at(1).isDouble() ||
                            r.at(1).toInt() < 1000) {
                            ++malformed;
                            continue;
                        }
                        by_area[r.at(0).toString()].append(
                            {QStringLiteral("%1-12-31").arg(r.at(1).toInt()), etfr_opt(r.at(2))});
                    }
                    int repeated = 0, conflicting = 0;
                    for (auto a = by_area.begin(); a != by_area.end(); ++a)
                        a.value() = etfr_unique_points(a.value(), &repeated, &conflicting);
                    left_out(malformed, QStringLiteral("malformed row(s)"));
                    noted(repeated, QStringLiteral("identical repeated value(s) kept once"), false);
                    left_out(conflicting, QStringLiteral("year(s) with conflicting values"));
                    auto rid = etfr_insert_retrieval(
                        run_id, stage, subject, req_at, ret_at, status,
                        QStringLiteral("absent:%1%2")
                            .arg([&] {
                                QStringList a;
                                for (const auto& v : it.value(QStringLiteral("countries_absent")).toArray())
                                    a.append(v.toString());
                                return a.join(QLatin1Char(','));
                            }())
                            .arg(detail.isEmpty() ? QString() : QStringLiteral("; ") + detail),
                        QString(), rows.size() - malformed, QString(), QString());
                    if (rid.is_err())
                        return Result<void>::err(rid.error());
                    QStringList areas = by_area.keys();
                    std::sort(areas.begin(), areas.end());
                    for (const QString& area : areas) {
                        auto& pts = by_area[area];
                        std::sort(pts.begin(), pts.end(),
                                  [](const auto& a, const auto& b) { return a.first < b.first; });
                        auto w =
                            etfr_write_macro(QStringLiteral("world_bank"), subject, area, pts, rid.value(), ret_at);
                        if (w.is_err())
                            return Result<void>::err(w.error());
                        st.rows_inserted += w.value().inserted;
                        st.rows_revised += w.value().revised;
                        st.rows_confirmed += w.value().confirmed;
                        auto cov = db().execute(
                            QStringLiteral(
                                "INSERT INTO etf_research_macro_coverage (retrieval_id, source, series_id, area, "
                                "first_date, last_date, points, retrieved_at, source_updated) "
                                "VALUES (?,'world_bank',?,?,?,?,?,?,?)"),
                            {rid.value(), subject, area, pts.first().first, pts.last().first,
                             static_cast<int>(pts.size()), ret_at,
                             etfr_text(it.value(QStringLiteral("source_last_updated")).toString())});
                        if (cov.is_err())
                            return Result<void>::err(cov.error());
                    }
                    const QString upd = it.value(QStringLiteral("source_last_updated")).toString();
                    if (upd > latest_eff)
                        latest_eff = upd;
                }
                return Result<void>::ok();
            }();
            if (item.is_ok()) {
                if (auto rel = db().execute(QStringLiteral("RELEASE etfr_item")); rel.is_err())
                    return fail(rel.error());
                continue;
            }
            if (auto rb = db().execute(QStringLiteral("ROLLBACK TO etfr_item")); rb.is_err())
                return fail(rb.error());
            if (auto rel = db().execute(QStringLiteral("RELEASE etfr_item")); rel.is_err())
                return fail(rel.error());
            st = st_before;
            latest_eff = latest_before;
            partly.resize(partly_before);
            auto rid = etfr_insert_retrieval(run_id, stage, subject, req_at, QString(), QStringLiteral("FAILED"),
                                             QStringLiteral("not stored: %1").arg(QString::fromStdString(item.error())),
                                             QString(), 0, QString(), QString());
            if (rid.is_err())
                return fail(rid.error());
            st.failed_subjects.append(subject);
        }
        st.latest_effective = latest_eff;
        SourceStageStatus done = etfr_finish_status(st);
        if (!partly.isEmpty()) {
            if (done.status == QLatin1String("UPDATED") || done.status == QLatin1String("UNCHANGED") ||
                done.status == QLatin1String("STALE"))
                done.status = QStringLiteral("PARTIAL");
            done.detail += QStringLiteral(" Partly usable: %1.").arg(partly.join(QStringLiteral("; ")));
        }
        if (so.value(QStringLiteral("skipped_fresh")).toInt() > 0) {
            if (done.items_requested == 0)
                done.status = QStringLiteral("UNCHANGED"); // everything reused, nothing was due
            else if (done.status == QLatin1String("FAILED"))
                done.status = QStringLiteral("PARTIAL"); // the reused items are current; only the fetched failed
        }
        out.append(done);
    }
    auto c = db().commit();
    if (c.is_err())
        return fail(c.error());
    return R::ok(out);
}

Result<QJsonObject> EtfResearchRepository::last_run(const QDateTime& known_at) {
    auto q = db().execute(QStringLiteral("SELECT run_id, trigger, started_at, finished_at, status, script_version, "
                                         "summary_json FROM etf_research_runs WHERE finished_at IS NOT NULL AND "
                                         "finished_at <= ? ORDER BY finished_at DESC, rowid DESC LIMIT 1"),
                          {etfr_iso(known_at)});
    if (q.is_err())
        return Result<QJsonObject>::err(q.error());
    QJsonObject o;
    if (q.value().next()) {
        o.insert(QStringLiteral("run_id"), q.value().value(0).toString());
        o.insert(QStringLiteral("trigger"), q.value().value(1).toString());
        o.insert(QStringLiteral("started_at"), q.value().value(2).toString());
        o.insert(QStringLiteral("finished_at"), q.value().value(3).toString());
        o.insert(QStringLiteral("status"), q.value().value(4).toString());
        o.insert(QStringLiteral("script_version"), q.value().value(5).toString());
        o.insert(QStringLiteral("summary"), QJsonDocument::fromJson(q.value().value(6).toString().toUtf8()).object());
    }
    return Result<QJsonObject>::ok(o);
}

Result<ResearchInputs> EtfResearchRepository::load_inputs(const ResearchUniverse& universe, const QDateTime& as_of,
                                                          const QDateTime& known_at) {
    using R = Result<ResearchInputs>;
    ResearchInputs in;
    in.universe = universe;
    in.as_of = as_of.toUTC();
    in.known_at = known_at.toUTC();
    const QString k = etfr_iso(in.known_at);
    const QString as_of_date = in.as_of.date().toString(Qt::ISODate);
    const QDate completed_us = completed_us_session(in.as_of);

    // ── Bars: the latest coverage window per symbol at known_at ─────────────
    struct Window {
        QString first, last, retrieved_at;
    };
    QHash<QString, Window> windows;
    {
        auto q = db().execute(QStringLiteral("SELECT symbol, first_session, last_session, retrieved_at FROM "
                                             "etf_research_bar_coverage WHERE retrieved_at <= ? "
                                             "ORDER BY symbol, retrieved_at, retrieval_id"),
                              {k});
        if (q.is_err())
            return R::err(q.error());
        while (q.value().next())
            windows.insert(q.value().value(0).toString(), {q.value().value(1).toString(), q.value().value(2).toString(),
                                                           q.value().value(3).toString()});
    }
    {
        // Universe instruments keep their whole stored history (the regime models
        // use it); constituents and curated stocks feed returns up to three months
        // and short RRG trails, so only their last 400 days are read.
        QStringList marks;
        QVariantList params{k, as_of_date, in.as_of.date().addDays(-400).toString(Qt::ISODate)};
        for (const auto& inst : universe.instruments) {
            marks << QStringLiteral("?");
            params << inst.symbol;
        }
        auto q = db().execute(QStringLiteral("SELECT symbol, session_date, revision, close, volume, dividend, "
                                             "capital_gain, split_ratio, first_seen_at FROM etf_research_bars "
                                             "WHERE first_seen_at <= ? AND session_date <= ? AND (session_date >= ? "
                                             "OR symbol IN (%1)) ORDER BY symbol, session_date, revision")
                                  .arg(marks.join(QLatin1Char(','))),
                              params);
        if (q.is_err())
            return R::err(q.error());
        auto& s = q.value();
        while (s.next()) {
            const QString sym = s.value(0).toString();
            const auto w = windows.constFind(sym);
            if (w == windows.constEnd())
                continue;
            const QString date = s.value(1).toString();
            if (date < w->first || date > w->last || s.value(8).toString() > w->retrieved_at)
                continue;
            if (!bar_finished_by(sym, QDate::fromString(date, Qt::ISODate), in.as_of, completed_us))
                continue; // the session had not closed at as_of, whatever known_at holds
            BarSeries& b = in.bars[sym];
            b.symbol = sym;
            DailyBar bar;
            bar.date = QDate::fromString(date, Qt::ISODate);
            bar.close = s.value(3).toDouble();
            if (!s.value(4).isNull())
                bar.volume = s.value(4).toDouble();
            bar.dividend = s.value(5).toDouble();
            bar.capital_gain = s.value(6).toDouble();
            bar.split = s.value(7).toDouble();
            bar.revised = s.value(2).toInt() > 1;
            if (!b.bars.isEmpty() && b.bars.last().date == bar.date)
                b.bars.last() = bar; // a later revision of the same session replaces it
            else
                b.bars.append(bar);
            b.any_revised = b.any_revised || bar.revised;
        }
    }

    // ── Fund snapshots ───────────────────────────────────────────────────────
    {
        auto q =
            db().execute(QStringLiteral("SELECT retrieval_id, symbol, captured_at, effective_session, "
                                        "effective_rule, total_assets, nav, previous_close, shares_outstanding, "
                                        "market_time, fields_json FROM etf_research_fund_snapshots "
                                        "WHERE captured_at <= ? AND captured_at <= ? ORDER BY symbol, captured_at"),
                         {k, etfr_iso(in.as_of)});
        if (q.is_err())
            return R::err(q.error());
        auto& s = q.value();
        auto opt = [](const QVariant& v) {
            return v.isNull() ? std::optional<double>() : std::optional<double>(v.toDouble());
        };
        while (s.next()) {
            FundCapture c;
            c.retrieval_id = s.value(0).toLongLong();
            c.captured_at = etfr_parse_time(s.value(2).toString());
            c.effective_session = QDate::fromString(s.value(3).toString(), Qt::ISODate);
            c.effective_rule = s.value(4).toString();
            c.total_assets = opt(s.value(5));
            c.nav = opt(s.value(6));
            c.previous_close = opt(s.value(7));
            c.shares_outstanding = opt(s.value(8));
            c.market_time = etfr_parse_time(s.value(9).toString());
            c.fields = QJsonDocument::fromJson(s.value(10).toString().toUtf8()).object();
            const QString sym = s.value(1).toString();
            in.funds[sym].append(c);
            if (in.bars.contains(sym) && in.bars[sym].currency.isEmpty())
                in.bars[sym].currency = c.fields.value(QStringLiteral("currency")).toString();
        }
    }
    // ── Latest holdings read per fund, as of the frame ───────────────────────
    // The recorded outcome (v055) of each fund's latest holdings read: a FAILED
    // read is named, so the HOLDINGS view does not explain it as "none
    // published". A quote failure is not a holdings failure. Retrievals stored
    // before v055 have no outcome and are never guessed from their text. A
    // failed item has no retrieved_at; its request time places it.
    {
        auto q =
            db().execute(QStringLiteral("SELECT h.symbol, h.status, h.detail FROM etf_research_holdings_reads h JOIN "
                                        "etf_research_retrievals r ON r.retrieval_id = h.retrieval_id WHERE "
                                        "COALESCE(r.retrieved_at, r.requested_at) <= ? AND "
                                        "COALESCE(r.retrieved_at, r.requested_at) <= ? "
                                        "ORDER BY h.symbol, COALESCE(r.retrieved_at, r.requested_at), r.retrieval_id"),
                         {k, etfr_iso(in.as_of)});
        if (q.is_err())
            return R::err(q.error());
        QHash<QString, QString> latest;
        while (q.value().next()) {
            const bool failed = q.value().value(1).toString() == QLatin1String("FAILED");
            const QString why = q.value().value(2).toString();
            latest.insert(q.value().value(0).toString(),
                          failed ? (why.isEmpty() ? QStringLiteral("the holdings read failed") : why) : QString());
        }
        for (auto it = latest.constBegin(); it != latest.constEnd(); ++it)
            if (!it.value().isEmpty())
                in.holdings_read_failed.insert(it.key(), it.value());
    }
    // ── Holdings (every capture; the latest with rows is the current one) ───
    // The capture time is the retrieval's: holdings captured while the quote
    // summary failed have no fund snapshot, and they are still read.
    {
        auto q =
            db().execute(QStringLiteral("SELECT h.retrieval_id, h.symbol, h.rank, h.holding_symbol, h.holding_name, "
                                        "h.weight, f.retrieved_at FROM etf_research_holdings h JOIN "
                                        "etf_research_retrievals f ON f.retrieval_id = h.retrieval_id WHERE "
                                        "f.retrieved_at <= ? AND f.retrieved_at <= ? "
                                        "ORDER BY h.symbol, f.retrieved_at, h.retrieval_id, h.rank"),
                         {k, etfr_iso(in.as_of)});
        if (q.is_err())
            return R::err(q.error());
        auto& s = q.value();
        QHash<QString, qint64> current_rid;
        while (s.next()) {
            const QString sym = s.value(1).toString();
            const qint64 rid = s.value(0).toLongLong();
            auto& hist = in.holdings_history[sym];
            if (current_rid.value(sym, -1) != rid) {
                HoldingsCapture hc;
                hc.captured_at = etfr_parse_time(s.value(6).toString());
                hist.append(hc);
                current_rid[sym] = rid;
            }
            Holding h;
            h.rank = s.value(2).toInt();
            h.symbol = s.value(3).toString();
            h.name = s.value(4).toString();
            if (!s.value(5).isNull())
                h.weight = s.value(5).toDouble();
            hist.last().holdings.append(h);
        }
        auto w = db().execute(QStringLiteral("SELECT w.symbol, w.sector_key, w.weight, f.retrieved_at FROM "
                                             "etf_research_sector_weights w JOIN etf_research_retrievals f ON "
                                             "f.retrieval_id = w.retrieval_id WHERE f.retrieved_at <= ? "
                                             "AND f.retrieved_at <= ? ORDER BY w.symbol, f.retrieved_at"),
                              {k, etfr_iso(in.as_of)});
        if (w.is_err())
            return R::err(w.error());
        QHash<QString, QPair<QString, QHash<QString, double>>> latest_w;
        while (w.value().next()) {
            const QString sym = w.value().value(0).toString();
            const QString at = w.value().value(3).toString();
            auto& e = latest_w[sym];
            if (e.first != at) {
                e.first = at;
                e.second.clear();
            }
            e.second.insert(w.value().value(1).toString(), w.value().value(2).toDouble());
        }
        for (auto it = in.holdings_history.begin(); it != in.holdings_history.end(); ++it)
            in.holdings.insert(it.key(), it.value().last());
        for (auto it = latest_w.begin(); it != latest_w.end(); ++it) {
            auto& h = in.holdings[it.key()];
            if (!h.captured_at.isValid())
                h.captured_at = etfr_parse_time(it.value().first);
            h.sector_weights = it.value().second;
        }
    }
    // ── Fundamentals (latest per constituent) ────────────────────────────────
    {
        // A capture is a snapshot of its own moment: it must be known by known_at
        // AND taken by as_of (a later capture never describes an earlier frame).
        auto q =
            db().execute(QStringLiteral("SELECT symbol, captured_at, fields_json FROM etf_research_fundamentals "
                                        "WHERE captured_at <= ? AND captured_at <= ? ORDER BY symbol, captured_at"),
                         {k, etfr_iso(in.as_of)});
        if (q.is_err())
            return R::err(q.error());
        while (q.value().next()) {
            Fundamentals f;
            f.captured_at = etfr_parse_time(q.value().value(1).toString());
            f.fields = QJsonDocument::fromJson(q.value().value(2).toString().toUtf8()).object();
            in.fundamentals.insert(q.value().value(0).toString(), f);
        }
    }
    // ── Macro: latest coverage window per (source, series, area) ────────────
    {
        QHash<QString, Window> mw;
        QHash<QString, QString> updated;
        auto q = db().execute(QStringLiteral("SELECT source, series_id, area, first_date, last_date, retrieved_at, "
                                             "source_updated FROM etf_research_macro_coverage WHERE retrieved_at <= ? "
                                             "ORDER BY source, series_id, area, retrieved_at, retrieval_id"),
                              {k});
        if (q.is_err())
            return R::err(q.error());
        while (q.value().next()) {
            const QString key = q.value().value(0).toString() + QLatin1Char('|') + q.value().value(1).toString() +
                                QLatin1Char('|') + q.value().value(2).toString();
            mw.insert(key,
                      {q.value().value(3).toString(), q.value().value(4).toString(), q.value().value(5).toString()});
            updated.insert(key, q.value().value(6).toString());
        }
        auto p = db().execute(QStringLiteral("SELECT source, series_id, area, obs_date, revision, value, first_seen_at "
                                             "FROM etf_research_macro WHERE first_seen_at <= ? AND obs_date <= ? "
                                             "ORDER BY source, series_id, area, obs_date, revision"),
                              {k, as_of_date});
        if (p.is_err())
            return R::err(p.error());
        QHash<QString, MacroSeries> all;
        while (p.value().next()) {
            const QString source = p.value().value(0).toString();
            const QString series = p.value().value(1).toString();
            const QString area = p.value().value(2).toString();
            const QString key = source + QLatin1Char('|') + series + QLatin1Char('|') + area;
            const auto w = mw.constFind(key);
            if (w == mw.constEnd())
                continue;
            const QString date = p.value().value(3).toString();
            if (date < w->first || date > w->last || p.value().value(6).toString() > w->retrieved_at)
                continue;
            if (macro_available_from(source, series, QDate::fromString(date, Qt::ISODate)) > in.as_of.toUTC().date())
                continue; // not yet published at as_of (assumed lag)
            MacroSeries& m = all[key];
            m.source = source;
            m.id = series;
            m.area = area;
            m.source_updated = updated.value(key);
            const QDate d = QDate::fromString(date, Qt::ISODate);
            if (!m.points.isEmpty() && m.points.last().date == d)
                m.points.removeLast(); // a later revision replaces it (and may now be a null)
            if (p.value().value(5).isNull())
                continue; // the source reported no value: missing, never zero
            m.points.append({d, p.value().value(5).toDouble(), p.value().value(4).toInt() > 1});
        }
        for (auto it = all.begin(); it != all.end(); ++it) {
            if (it->source == QLatin1String("fred"))
                in.fred.insert(it->id, it.value());
            else if (it->source == QLatin1String("cftc"))
                in.cftc[it->id].insert(it->area, it.value());
            else
                in.world_bank[it->id].insert(it->area, it.value());
        }
    }
    // ── Last completed manual refresh ────────────────────────────────────────
    auto lr = last_run(std::min(in.known_at, in.as_of)); // a later run is not part of an earlier frame
    if (lr.is_ok() && !lr.value().isEmpty()) {
        in.last_refresh_run_id = lr.value().value(QStringLiteral("run_id")).toString();
        in.last_refresh_finished = etfr_parse_time(lr.value().value(QStringLiteral("finished_at")).toString());
        for (const auto& v :
             lr.value().value(QStringLiteral("summary")).toObject().value(QStringLiteral("stages")).toArray())
            in.last_refresh.append(etfr_status_from_json(v.toObject()));
    }
    return R::ok(in);
}

Result<QHash<QString, etf_research_store::HistoryCoverage>> EtfResearchRepository::history_coverage() {
    using R = Result<QHash<QString, etf_research_store::HistoryCoverage>>;
    QHash<QString, etf_research_store::HistoryCoverage> out;
    auto w = db().execute(QStringLiteral("SELECT symbol, first_session, last_session FROM etf_research_bar_coverage "
                                         "ORDER BY symbol, retrieved_at, retrieval_id"));
    if (w.is_err())
        return R::err(w.error());
    while (w.value().next()) {
        auto& c = out[w.value().value(0).toString()];
        c.window_first = QDate::fromString(w.value().value(1).toString(), Qt::ISODate);
        c.window_last = QDate::fromString(w.value().value(2).toString(), Qt::ISODate);
    }
    auto e = db().execute(QStringLiteral("SELECT symbol, MIN(session_date) FROM etf_research_bars GROUP BY symbol"));
    if (e.is_err())
        return R::err(e.error());
    while (e.value().next()) {
        const auto it = out.find(e.value().value(0).toString());
        if (it != out.end())
            it->earliest_stored = QDate::fromString(e.value().value(1).toString(), Qt::ISODate);
    }
    return R::ok(out);
}

Result<QHash<QString, QDateTime>> EtfResearchRepository::latest_fundamentals_capture() {
    QHash<QString, QDateTime> out;
    auto q =
        db().execute(QStringLiteral("SELECT symbol, MAX(captured_at) FROM etf_research_fundamentals GROUP BY symbol"));
    if (q.is_err())
        return Result<QHash<QString, QDateTime>>::err(q.error());
    while (q.value().next())
        out.insert(q.value().value(0).toString(), etfr_parse_time(q.value().value(1).toString()));
    return Result<QHash<QString, QDateTime>>::ok(out);
}

Result<QDate> EtfResearchRepository::latest_sec_report_period(qint64 entity_id) {
    auto q =
        db().execute(QStringLiteral("SELECT MAX(rep_pd_date) FROM etf_sec_filings WHERE entity_id = ?"), {entity_id});
    if (q.is_err())
        return Result<QDate>::err(q.error());
    QDate d;
    if (q.value().next() && !q.value().value(0).isNull())
        d = QDate::fromString(q.value().value(0).toString().left(10), Qt::ISODate);
    return Result<QDate>::ok(d);
}

Result<QJsonObject> EtfResearchRepository::table_counts() {
    QJsonObject o;
    for (const char* t : {"etf_research_runs", "etf_research_retrievals", "etf_research_bars",
                          "etf_research_bar_coverage", "etf_research_fund_snapshots", "etf_research_holdings",
                          "etf_research_holdings_reads", "etf_research_sector_weights", "etf_research_fundamentals",
                          "etf_research_macro", "etf_research_macro_coverage"}) {
        auto q = db().execute(QStringLiteral("SELECT COUNT(*) FROM %1").arg(QLatin1String(t)));
        if (q.is_err())
            return Result<QJsonObject>::err(q.error());
        q.value().next();
        o.insert(QLatin1String(t), q.value().value(0).toLongLong());
    }
    return Result<QJsonObject>::ok(o);
}

Result<QJsonObject> EtfResearchRepository::export_all() {
    QJsonObject out;
    const struct {
        const char* table;
        const char* order;
    } tables[] = {{"etf_research_runs", "run_id"},
                  {"etf_research_retrievals", "retrieval_id"},
                  {"etf_research_bars", "symbol, session_date, revision"},
                  {"etf_research_bar_coverage", "retrieval_id"},
                  {"etf_research_fund_snapshots", "retrieval_id"},
                  {"etf_research_holdings", "retrieval_id, rank"},
                  {"etf_research_holdings_reads", "retrieval_id"},
                  {"etf_research_sector_weights", "retrieval_id, sector_key"},
                  {"etf_research_fundamentals", "retrieval_id"},
                  {"etf_research_macro", "source, series_id, area, obs_date, revision"},
                  {"etf_research_macro_coverage", "retrieval_id, area"}};
    for (const auto& t : tables) {
        auto q = db().execute(
            QStringLiteral("SELECT * FROM %1 ORDER BY %2").arg(QLatin1String(t.table), QLatin1String(t.order)));
        if (q.is_err())
            return Result<QJsonObject>::err(q.error());
        QJsonArray rows;
        auto& s = q.value();
        while (s.next()) {
            QJsonArray r;
            const QSqlRecord rec = s.record();
            for (int i = 0; i < rec.count(); ++i) {
                const QVariant v = s.value(i);
                if (v.isNull())
                    r.append(QJsonValue(QJsonValue::Null));
                else if (v.typeId() == QMetaType::Double)
                    r.append(v.toDouble());
                else if (v.typeId() == QMetaType::LongLong || v.typeId() == QMetaType::Int)
                    r.append(v.toLongLong());
                else
                    r.append(v.toString());
            }
            rows.append(r);
        }
        out.insert(QLatin1String(t.table), rows);
    }
    return Result<QJsonObject>::ok(out);
}

} // namespace fincept
