#include "storage/repositories/EtfDataRepository.h"

#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfTiming.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSqlRecord>
#include <QStringList>
#include <QVariant>

#include <algorithm>

namespace fincept {

namespace etf_store {

QString iso_utc(const QDateTime& t) {
    if (!t.isValid())
        return {};
    return t.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'"));
}

QDateTime parse_iso_utc(const QString& s) {
    if (s.isEmpty())
        return {};
    const QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    return dt.isValid() ? dt.toUTC() : QDateTime();
}

const char* observation_outcome_id(ObservationOutcome o) {
    switch (o) {
        case ObservationOutcome::InsertedOriginal:
            return "inserted_original";
        case ObservationOutcome::InsertedAmendment:
            return "inserted_amendment";
        case ObservationOutcome::InsertedRevision:
            return "inserted_revision";
        case ObservationOutcome::Confirmed:
            return "confirmed";
        case ObservationOutcome::AlreadyRecorded:
            return "already_recorded";
        case ObservationOutcome::RefusedDocumentChanged:
            return "refused_document_changed";
    }
    return "";
}

} // namespace etf_store

namespace {

using namespace services::etf;

/// A TEXT value that must never become SQL NULL. A null QString binds as NULL
/// in the SQLite driver (sqlite3_bind_text16 with a null pointer), which would
/// break the NOT NULL DEFAULT '' columns and make `series_id = ?` lookups miss
/// a registrant-level entity. An empty, non-null string binds as ''.
QVariant etf_text(const QString& s) {
    return s.isNull() ? QVariant(QString::fromUtf8("")) : QVariant(s);
}

QVariant etf_text_or_null(const QString& s) {
    return s.isEmpty() ? QVariant() : QVariant(s);
}

QVariant etf_date_or_null(const QDate& d) {
    return d.isValid() ? QVariant(d.toString(Qt::ISODate)) : QVariant();
}

QVariant etf_time_or_null(const QDateTime& t) {
    return t.isValid() ? QVariant(etf_store::iso_utc(t)) : QVariant();
}

QDate etf_parse_date(const QVariant& v) {
    return v.isNull() ? QDate() : QDate::fromString(v.toString(), Qt::ISODate);
}

// The etf_observations columns, in the order map_observation() reads them.
const QString& etf_observation_columns() {
    static const QString kColumns = QStringLiteral(
        "observation_id, subject_type, subject_id, measurement_kind, measure, units, basis, source_type, "
        "acquisition_mode, source_document, filing_id, effective_date, period_start, period_end, report_period, "
        "accepted_at, value, value_state, raw_text, source_revision, revision_state, history_type, "
        "point_in_time_status, availability_basis, available_from, first_seen_at, last_seen_at, "
        "first_retrieval_id, last_retrieval_id, seen_count");
    return kColumns;
}

QJsonValue etf_json_value(const QVariant& v) {
    if (v.isNull() || !v.isValid())
        return QJsonValue(QJsonValue::Null);
    switch (v.typeId()) {
        case QMetaType::Int:
        case QMetaType::LongLong:
        case QMetaType::UInt:
        case QMetaType::ULongLong:
            return QJsonValue(v.toLongLong());
        case QMetaType::Double:
            return QJsonValue(v.toDouble());
        default:
            return QJsonValue(v.toString());
    }
}

} // namespace

EtfDataRepository& EtfDataRepository::instance() {
    static EtfDataRepository s;
    return s;
}

StoredObservation EtfDataRepository::map_observation(QSqlQuery& q) {
    StoredObservation o;
    o.observation_id = q.value(0).toLongLong();
    o.subject_type = q.value(1).toString();
    o.subject_id = q.value(2).toLongLong();
    o.measurement_kind = q.value(3).toString();
    o.measure = q.value(4).toString();
    o.units = q.value(5).toString();
    o.basis = q.value(6).toString();
    o.source_type = q.value(7).toString();
    o.acquisition_mode = q.value(8).toString();
    o.source_document = q.value(9).toString();
    o.filing_id = q.value(10).isNull() ? 0 : q.value(10).toLongLong();
    o.effective_date = etf_parse_date(q.value(11));
    o.period_start = etf_parse_date(q.value(12));
    o.period_end = etf_parse_date(q.value(13));
    o.report_period = etf_parse_date(q.value(14));
    o.accepted_at = etf_store::parse_iso_utc(q.value(15).toString());
    const QString state = q.value(17).toString();
    const QString raw = q.value(18).isNull() ? QString() : q.value(18).toString();
    if (state == QLatin1String(value_state_id(ValueState::Reported)) && !q.value(16).isNull())
        o.value = FieldValue::reported_value(q.value(16).toDouble(), raw);
    else if (state == QLatin1String(value_state_id(ValueState::Unparseable)))
        o.value = FieldValue::unparseable(raw);
    else
        o.value = FieldValue::missing();
    o.source_revision = q.value(19).toInt();
    o.revision_state = q.value(20).toString();
    o.history_type = q.value(21).toString();
    o.point_in_time_status = q.value(22).toString();
    o.availability_basis = q.value(23).toString();
    o.available_from = etf_store::parse_iso_utc(q.value(24).toString());
    o.first_seen_at = etf_store::parse_iso_utc(q.value(25).toString());
    o.last_seen_at = etf_store::parse_iso_utc(q.value(26).toString());
    o.first_retrieval_id = q.value(27).toLongLong();
    o.last_retrieval_id = q.value(28).toLongLong();
    o.seen_count = q.value(29).toInt();
    return o;
}

// ── Retrievals and issues ────────────────────────────────────────────────────

Result<qint64> EtfDataRepository::record_retrieval(const etf_store::RetrievalRecord& r) {
    if (!r.requested_at.isValid())
        return Result<qint64>::err("retrieval has no request time");
    return exec_insert(
        "INSERT INTO etf_retrievals (run_id, source_type, acquisition_mode, endpoint, request_ref, subject_ref, "
        "requested_at, retrieved_at, status, detail_code, detail, http_status, response_sha256, response_bytes, "
        "interpretation, route_policy, calendar_version, runtime_identity) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {etf_text(r.run_id), QLatin1String(source_type_id(r.source_type)),
         QLatin1String(acquisition_mode_id(r.acquisition_mode)), etf_text(r.endpoint), etf_text(r.request_ref),
         etf_text(r.subject_ref), etf_store::iso_utc(r.requested_at), etf_time_or_null(r.retrieved_at),
         QLatin1String(retrieval_status_id(r.status)), etf_text(r.detail_code), etf_text(r.detail),
         r.http_status ? QVariant(*r.http_status) : QVariant(), etf_text_or_null(r.response_sha256),
         r.response_bytes ? QVariant(*r.response_bytes) : QVariant(), etf_text(r.interpretation),
         etf_text(r.route_policy), etf_text(r.calendar_version), etf_text(r.runtime_identity)});
}

Result<void> EtfDataRepository::record_issue(qint64 retrieval_id, const etf_store::IssueRecord& issue) {
    return exec_write(
        "INSERT INTO etf_retrieval_issues (retrieval_id, subject_type, subject_id, effective_date, "
        "state, code, detail) VALUES (?, ?, ?, ?, ?, ?, ?)",
        {retrieval_id,
         issue.subject_type ? QVariant(QLatin1String(subject_type_id(*issue.subject_type))) : etf_text(QString()),
         issue.subject_type ? QVariant(issue.subject_id) : QVariant(), etf_date_or_null(issue.effective_date),
         QLatin1String(quality_state_id(issue.state)), etf_text(issue.code), etf_text(issue.detail)});
}

// ── Identity ─────────────────────────────────────────────────────────────────

Result<qint64> EtfDataRepository::upsert_reporting_entity(const etf_store::ReportingEntityFacts& f,
                                                          const QDateTime& seen_at) {
    if (f.cik.size() != 10)
        return Result<qint64>::err("reporting entity CIK is not 10 digits");
    const QString seen = etf_store::iso_utc(seen_at);
    auto found = db().execute("SELECT entity_id, last_seen_at FROM etf_reporting_entities WHERE cik = ? AND "
                              "series_id = ?",
                              {etf_text(f.cik), etf_text(f.series_id)});
    if (found.is_err())
        return Result<qint64>::err(found.error());
    auto& q = found.value();
    if (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        // Names and LEIs are attributes: keep the most recently seen values, so
        // an older filing processed later does not overwrite newer ones.
        if (seen >= q.value(1).toString()) {
            auto upd = exec_write("UPDATE etf_reporting_entities SET registrant_name = ?, series_name = ?, "
                                  "reg_file_number = ?, registrant_lei = ?, series_lei = ?, last_seen_at = ? "
                                  "WHERE entity_id = ?",
                                  {etf_text(f.registrant_name), etf_text(f.series_name), etf_text(f.reg_file_number),
                                   etf_text(f.registrant_lei), etf_text(f.series_lei), seen, id});
            if (upd.is_err())
                return Result<qint64>::err(upd.error());
        }
        return Result<qint64>::ok(id);
    }
    return exec_insert("INSERT INTO etf_reporting_entities (cik, series_id, reporting_level, registrant_name, "
                       "series_name, reg_file_number, registrant_lei, series_lei, first_seen_at, last_seen_at) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                       {etf_text(f.cik), etf_text(f.series_id),
                        f.series_id.isEmpty() ? QStringLiteral("registrant") : QStringLiteral("series"),
                        etf_text(f.registrant_name), etf_text(f.series_name), etf_text(f.reg_file_number),
                        etf_text(f.registrant_lei), etf_text(f.series_lei), seen, seen});
}

Result<qint64> EtfDataRepository::upsert_listed_instrument(const etf_store::ListedInstrumentFacts& f,
                                                           const QDateTime& seen_at) {
    if (f.con_id <= 0)
        return Result<qint64>::err("listed instrument has no positive IBKR conId");
    const QString seen = etf_store::iso_utc(seen_at);
    qint64 id = 0;
    auto found = db().execute("SELECT instrument_id, last_seen_at FROM etf_listed_instruments WHERE ibkr_con_id = ?",
                              {f.con_id});
    if (found.is_err())
        return Result<qint64>::err(found.error());
    auto& q = found.value();
    if (q.next()) {
        id = q.value(0).toLongLong();
        if (seen >= q.value(1).toString()) {
            auto upd = exec_write("UPDATE etf_listed_instruments SET symbol = ?, security_type = ?, exchange = ?, "
                                  "primary_exchange = ?, currency = ?, last_seen_at = ? WHERE instrument_id = ?",
                                  {etf_text(f.symbol), etf_text(f.security_type), etf_text(f.exchange),
                                   etf_text(f.primary_exchange), etf_text(f.currency), seen, id});
            if (upd.is_err())
                return Result<qint64>::err(upd.error());
        }
    } else {
        auto ins =
            exec_insert("INSERT INTO etf_listed_instruments (ibkr_con_id, symbol, security_type, exchange, "
                        "primary_exchange, currency, first_seen_at, last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                        {f.con_id, etf_text(f.symbol), etf_text(f.security_type), etf_text(f.exchange),
                         etf_text(f.primary_exchange), etf_text(f.currency), seen, seen});
        if (ins.is_err())
            return ins;
        id = ins.value();
    }
    // The ticker is an attribute with a history of its own, never the identity.
    auto sym = exec_write("INSERT OR IGNORE INTO etf_instrument_symbols (instrument_id, symbol, first_seen_at, "
                          "last_seen_at) VALUES (?, ?, ?, ?)",
                          {id, etf_text(f.symbol), seen, seen});
    if (sym.is_err())
        return Result<qint64>::err(sym.error());
    auto touch = exec_write("UPDATE etf_instrument_symbols SET last_seen_at = ? WHERE instrument_id = ? AND symbol = ? "
                            "AND last_seen_at < ?",
                            {seen, id, etf_text(f.symbol), seen});
    if (touch.is_err())
        return Result<qint64>::err(touch.error());
    return Result<qint64>::ok(id);
}

Result<qint64> EtfDataRepository::declare_link(qint64 instrument_id, qint64 entity_id, const QString& class_id,
                                               LinkRelationship relationship, const QString& basis,
                                               const QDateTime& declared_at) {
    if (basis.trimmed().isEmpty())
        return Result<qint64>::err("an identity link needs a stated basis");
    auto ent = db().execute("SELECT series_id FROM etf_reporting_entities WHERE entity_id = ?", {entity_id});
    if (ent.is_err())
        return Result<qint64>::err(ent.error());
    if (!ent.value().next())
        return Result<qint64>::err("unknown reporting entity");
    const QString series_id = ent.value().value(0).toString();
    auto ins_check = db().execute("SELECT 1 FROM etf_listed_instruments WHERE instrument_id = ?", {instrument_id});
    if (ins_check.is_err())
        return Result<qint64>::err(ins_check.error());
    if (!ins_check.value().next())
        return Result<qint64>::err("unknown listed instrument");

    if (relationship == LinkRelationship::RegistrantIsInstrument && !series_id.isEmpty())
        return Result<qint64>::err("registrant_is_instrument needs a registrant that reports without a series");
    if (relationship != LinkRelationship::RegistrantIsInstrument && series_id.isEmpty())
        return Result<qint64>::err("a class relationship needs a series-level reporting entity");

    // Check the declared relationship against the class structure the newest
    // stored filing reports (monthlyTotReturn class ids). With no filing stored
    // the declaration is accepted as declared.
    auto latest = db().execute("SELECT returns_block_present, class_ids FROM etf_sec_filings WHERE entity_id = ? "
                               "ORDER BY accepted_at DESC, filing_id DESC LIMIT 1",
                               {entity_id});
    if (latest.is_err())
        return Result<qint64>::err(latest.error());
    if (latest.value().next() && latest.value().value(0).toInt() == 1) {
        const QStringList classes = latest.value().value(1).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
        if (relationship == LinkRelationship::SoleClassOfSeries && classes.size() != 1)
            return Result<qint64>::err(QStringLiteral("sole_class_of_series contradicts the latest filing, which "
                                                      "reports %1 share classes")
                                           .arg(classes.size())
                                           .toStdString());
        if (relationship == LinkRelationship::ClassOfMultiClassSeries && classes.size() < 2)
            return Result<qint64>::err("class_of_multi_class_series contradicts the latest filing, which reports "
                                       "fewer than two share classes");
        if (!class_id.isEmpty() && !classes.contains(class_id))
            return Result<qint64>::err("the class id is not among the classes the latest filing reports");
    }

    auto existing = db().execute("SELECT link_id, relationship FROM etf_identity_links WHERE instrument_id = ? AND "
                                 "entity_id = ? AND class_id = ?",
                                 {instrument_id, entity_id, etf_text(class_id)});
    if (existing.is_err())
        return Result<qint64>::err(existing.error());
    if (existing.value().next()) {
        if (existing.value().value(1).toString() != QLatin1String(link_relationship_id(relationship)))
            return Result<qint64>::err("a different relationship is already declared for this link; links are "
                                       "never rewritten silently");
        return Result<qint64>::ok(existing.value().value(0).toLongLong());
    }
    return exec_insert("INSERT INTO etf_identity_links (instrument_id, entity_id, class_id, relationship, link_basis, "
                       "declared_at) VALUES (?, ?, ?, ?, ?, ?)",
                       {instrument_id, entity_id, etf_text(class_id), QLatin1String(link_relationship_id(relationship)),
                        etf_text(basis), etf_store::iso_utc(declared_at)});
}

Result<std::optional<LinkRelationship>> EtfDataRepository::nport_link_relationship(qint64 instrument_id) {
    auto r = db().execute("SELECT relationship FROM etf_identity_links WHERE instrument_id = ? "
                          "ORDER BY declared_at DESC, link_id DESC LIMIT 1",
                          {instrument_id});
    if (r.is_err())
        return Result<std::optional<LinkRelationship>>::err(r.error());
    if (!r.value().next())
        return Result<std::optional<LinkRelationship>>::ok(std::nullopt);
    return Result<std::optional<LinkRelationship>>::ok(link_relationship_from_id(r.value().value(0).toString()));
}

// ── SEC filings ──────────────────────────────────────────────────────────────

Result<std::pair<qint64, etf_store::FilingOutcome>>
EtfDataRepository::upsert_sec_filing(const etf_store::SecFilingFacts& f, qint64 retrieval_id,
                                     const QDateTime& seen_at) {
    using R = Result<std::pair<qint64, etf_store::FilingOutcome>>;
    if (!f.accepted_at.isValid())
        return R::err("filing has no acceptance time");
    const QString seen = etf_store::iso_utc(seen_at);
    auto found = db().execute("SELECT filing_id, document_sha256 FROM etf_sec_filings WHERE accession = ?",
                              {etf_text(f.accession)});
    if (found.is_err())
        return R::err(found.error());
    auto& q = found.value();
    if (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        if (q.value(1).toString() != f.document_sha256)
            return R::ok({id, etf_store::FilingOutcome::DocumentChanged});
        auto upd = exec_write("UPDATE etf_sec_filings SET last_seen_at = ?, last_retrieval_id = ? WHERE filing_id = ? "
                              "AND last_retrieval_id < ?",
                              {seen, retrieval_id, id, retrieval_id});
        if (upd.is_err())
            return R::err(upd.error());
        return R::ok({id, etf_store::FilingOutcome::Confirmed});
    }
    auto ins = exec_insert(
        "INSERT INTO etf_sec_filings (accession, filer_cik, form, filing_date, report_date, accepted_at, "
        "primary_document, entity_id, amends_accession, rep_pd_date, rep_pd_end, is_final_filing, "
        "returns_block_present, class_ids, document_sha256, first_retrieval_id, last_retrieval_id, first_seen_at, "
        "last_seen_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        {etf_text(f.accession), etf_text(f.filer_cik), etf_text(f.form), etf_text(f.filing_date.toString(Qt::ISODate)),
         etf_text(f.report_date), etf_store::iso_utc(f.accepted_at), etf_text(f.primary_document), f.entity_id,
         etf_text(f.amends_accession), etf_text(f.rep_pd_date.toString(Qt::ISODate)), etf_text(f.rep_pd_end),
         etf_text(f.is_final_filing), f.returns_block_present ? 1 : 0, etf_text(f.class_ids),
         etf_text(f.document_sha256), retrieval_id, retrieval_id, seen, seen});
    if (ins.is_err())
        return R::err(ins.error());
    return R::ok({ins.value(), etf_store::FilingOutcome::Inserted});
}

// ── Observation start ────────────────────────────────────────────────────────

Result<QDateTime> EtfDataRepository::observation_start(SourceType source, SubjectType subject_type, qint64 subject_id,
                                                       const QDateTime& candidate, qint64 retrieval_id) {
    auto found =
        db().execute("SELECT observation_start FROM etf_observation_starts WHERE source_type = ? AND "
                     "subject_type = ? AND subject_id = ?",
                     {QLatin1String(source_type_id(source)), QLatin1String(subject_type_id(subject_type)), subject_id});
    if (found.is_err())
        return Result<QDateTime>::err(found.error());
    if (found.value().next())
        return Result<QDateTime>::ok(etf_store::parse_iso_utc(found.value().value(0).toString()));
    if (!candidate.isValid())
        return Result<QDateTime>::err("no observation start to record");
    auto ins = exec_write("INSERT INTO etf_observation_starts (source_type, subject_type, subject_id, "
                          "observation_start, retrieval_id) VALUES (?, ?, ?, ?, ?)",
                          {QLatin1String(source_type_id(source)), QLatin1String(subject_type_id(subject_type)),
                           subject_id, etf_store::iso_utc(candidate), retrieval_id});
    if (ins.is_err())
        return Result<QDateTime>::err(ins.error());
    return Result<QDateTime>::ok(etf_store::parse_iso_utc(etf_store::iso_utc(candidate)));
}

// ── Session calendar ─────────────────────────────────────────────────────────

Result<void> EtfDataRepository::upsert_sessions(const QVector<MarketSessionDay>& days) {
    QVector<QVariantList> rows;
    for (const MarketSessionDay& d : days) {
        if (d.type != SessionDayType::Regular && d.type != SessionDayType::EarlyClose &&
            d.type != SessionDayType::Holiday)
            continue;
        const bool session = d.is_session();
        rows.append({QLatin1String(kUsEquityCalendarId), QLatin1String(kUsEquityCalendarVersion),
                     d.date.toString(Qt::ISODate), QLatin1String(session_day_type_id(d.type)),
                     session ? QVariant(d.open_local.toString(QStringLiteral("HH:mm"))) : QVariant(),
                     session ? QVariant(d.close_local.toString(QStringLiteral("HH:mm"))) : QVariant(),
                     session ? QVariant(etf_store::iso_utc(d.open_utc)) : QVariant(),
                     session ? QVariant(etf_store::iso_utc(d.close_utc)) : QVariant(), d.is_early_close() ? 1 : 0});
    }
    return db().execute_many("INSERT OR IGNORE INTO etf_market_sessions (calendar_id, calendar_version, "
                             "session_date, day_type, open_local, close_local, open_utc, close_utc, is_early_close) "
                             "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                             rows);
}

// ── Observations ─────────────────────────────────────────────────────────────

Result<etf_store::ObservationOutcome> EtfDataRepository::record_observation(const etf_store::ObservationInput& in) {
    using R = Result<etf_store::ObservationOutcome>;
    using etf_store::ObservationOutcome;

    // Defence in depth: the ingestors never call this for a disabled route, and
    // v052's CHECK constraints refuse such rows as well.
    if (!acquisition_route(in.source_type, in.acquisition_mode).enabled)
        return R::err("acquisition route disabled");
    if (!measurement_kind_is_source_fact(in.kind))
        return R::err("calculated and proxy measurements are not source observations");
    if (in.retrieval_id <= 0 || !in.seen_at.isValid() || !in.effective_date.isValid() || in.measure.isEmpty())
        return R::err("observation lacks its retrieval, sighting time, effective date or measure");
    const bool sec = in.source_type == SourceType::SecNport;
    if (sec && (in.source_document.isEmpty() || in.filing_id <= 0 || !in.accepted_at.isValid()))
        return R::err("an SEC observation needs its accession, filing and acceptance time");
    if (!sec && !in.source_document.isEmpty())
        return R::err("an IBKR observation has no source document");

    auto existing_r = vintages(in.subject_type, in.subject_id, in.measure, in.effective_date, in.source_type);
    if (existing_r.is_err())
        return R::err(existing_r.error());
    const QVector<StoredObservation>& existing = existing_r.value();

    // The vintages of the same source document: the same accession for SEC,
    // every vintage for IBKR (it has no document identity).
    const StoredObservation* latest_same = nullptr;
    qint64 same_doc_max_retrieval = 0;
    int max_revision = 0;
    for (const StoredObservation& v : existing) {
        max_revision = std::max(max_revision, v.source_revision);
        if (v.source_document != in.source_document)
            continue;
        same_doc_max_retrieval = std::max(same_doc_max_retrieval, v.last_retrieval_id);
        if (!latest_same || v.source_revision > latest_same->source_revision)
            latest_same = &v;
    }

    if (latest_same) {
        // A retrieval that is not newer than the last one that recorded this
        // document carries no new information: an exact replay, or an older
        // retrieval re-applied. Deterministically a no-op.
        if (in.retrieval_id <= same_doc_max_retrieval)
            return R::ok(ObservationOutcome::AlreadyRecorded);
        if (latest_same->value.same_value(in.value)) {
            auto upd = exec_write("UPDATE etf_observations SET last_seen_at = ?, last_retrieval_id = ?, "
                                  "seen_count = seen_count + 1 WHERE observation_id = ?",
                                  {etf_store::iso_utc(in.seen_at), in.retrieval_id, latest_same->observation_id});
            if (upd.is_err())
                return R::err(upd.error());
            return R::ok(ObservationOutcome::Confirmed);
        }
        // A filed SEC document does not change. A different value under the
        // same accession is an anomaly: refused, and the caller records it.
        if (sec)
            return R::ok(ObservationOutcome::RefusedDocumentChanged);
    }

    const bool first_vintage = existing.isEmpty();
    TimingAssessment timing;
    RevisionState revision_state = RevisionState::Original;
    ObservationOutcome outcome = ObservationOutcome::InsertedOriginal;
    if (sec) {
        timing = classify_sec_vintage(in.accepted_at, in.observation_start, in.seen_at);
        if (in.amended_filing) {
            revision_state = RevisionState::AmendedFiling;
            outcome = ObservationOutcome::InsertedAmendment;
        }
    } else {
        timing = classify_ibkr_bar_vintage(in.effective_date, in.observation_start, in.seen_at, first_vintage);
        if (!first_vintage) {
            revision_state = RevisionState::Revised;
            outcome = ObservationOutcome::InsertedRevision;
        }
    }

    const QString seen = etf_store::iso_utc(in.seen_at);
    auto ins = exec_write(
        QStringLiteral("INSERT INTO etf_observations (subject_type, subject_id, measurement_kind, measure, units, "
                       "basis, source_type, acquisition_mode, source_document, filing_id, effective_date, "
                       "period_start, period_end, report_period, accepted_at, value, value_state, raw_text, "
                       "source_revision, revision_state, history_type, point_in_time_status, availability_basis, "
                       "available_from, first_seen_at, last_seen_at, first_retrieval_id, last_retrieval_id, "
                       "seen_count) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                       "?, ?, ?, ?, 1)"),
        {QLatin1String(subject_type_id(in.subject_type)),
         in.subject_id,
         QLatin1String(measurement_kind_id(in.kind)),
         etf_text(in.measure),
         etf_text(in.units),
         etf_text(in.basis),
         QLatin1String(source_type_id(in.source_type)),
         QLatin1String(acquisition_mode_id(in.acquisition_mode)),
         etf_text(in.source_document),
         sec ? QVariant(in.filing_id) : QVariant(),
         in.effective_date.toString(Qt::ISODate),
         etf_date_or_null(in.period_start),
         etf_date_or_null(in.period_end),
         etf_date_or_null(in.report_period),
         etf_time_or_null(in.accepted_at),
         in.value.reported() ? QVariant(in.value.value) : QVariant(),
         QLatin1String(value_state_id(in.value.state)),
         in.value.state == ValueState::Missing ? QVariant() : etf_text_or_null(in.value.raw),
         max_revision + 1,
         QLatin1String(revision_state_id(revision_state)),
         QLatin1String(history_type_id(timing.history_type)),
         QLatin1String(point_in_time_status_id(timing.point_in_time_status)),
         QLatin1String(availability_basis_id(timing.availability_basis)),
         etf_time_or_null(timing.available_from),
         seen,
         seen,
         in.retrieval_id,
         in.retrieval_id});
    if (ins.is_err())
        return R::err(ins.error());
    return R::ok(outcome);
}

// ── Reads ────────────────────────────────────────────────────────────────────

Result<QVector<StoredObservation>> EtfDataRepository::vintages(SubjectType subject_type, qint64 subject_id,
                                                               const QString& measure, const QDate& effective_date,
                                                               SourceType source) {
    return query_list(QStringLiteral("SELECT %1 FROM etf_observations WHERE subject_type = ? AND subject_id = ? AND "
                                     "measure = ? AND effective_date = ? AND source_type = ? ORDER BY source_revision")
                          .arg(etf_observation_columns()),
                      {QLatin1String(subject_type_id(subject_type)), subject_id, etf_text(measure),
                       effective_date.toString(Qt::ISODate), QLatin1String(source_type_id(source))},
                      &EtfDataRepository::map_observation);
}

Result<QVector<StoredObservation>> EtfDataRepository::subject_observations(SubjectType subject_type,
                                                                           qint64 subject_id) {
    return query_list(QStringLiteral("SELECT %1 FROM etf_observations WHERE subject_type = ? AND subject_id = ? "
                                     "ORDER BY measure, effective_date, source_revision")
                          .arg(etf_observation_columns()),
                      {QLatin1String(subject_type_id(subject_type)), subject_id}, &EtfDataRepository::map_observation);
}

Result<std::optional<qint64>> EtfDataRepository::find_reporting_entity(const QString& cik10, const QString& series_id) {
    auto r = db().execute("SELECT entity_id FROM etf_reporting_entities WHERE cik = ? AND series_id = ?",
                          {etf_text(cik10), etf_text(series_id)});
    if (r.is_err())
        return Result<std::optional<qint64>>::err(r.error());
    if (!r.value().next())
        return Result<std::optional<qint64>>::ok(std::nullopt);
    return Result<std::optional<qint64>>::ok(r.value().value(0).toLongLong());
}

Result<std::optional<qint64>> EtfDataRepository::find_listed_instrument(qint64 con_id) {
    auto r = db().execute("SELECT instrument_id FROM etf_listed_instruments WHERE ibkr_con_id = ?", {con_id});
    if (r.is_err())
        return Result<std::optional<qint64>>::err(r.error());
    if (!r.value().next())
        return Result<std::optional<qint64>>::ok(std::nullopt);
    return Result<std::optional<qint64>>::ok(r.value().value(0).toLongLong());
}

Result<QJsonObject> EtfDataRepository::export_all() {
    struct TableOrder {
        const char* table;
        const char* order_by;
    };
    static const TableOrder kTables[] = {
        {"etf_retrievals", "retrieval_id"},
        {"etf_retrieval_issues", "issue_id"},
        {"etf_reporting_entities", "entity_id"},
        {"etf_sec_filings", "filing_id"},
        {"etf_listed_instruments", "instrument_id"},
        {"etf_instrument_symbols", "instrument_id, symbol"},
        {"etf_identity_links", "link_id"},
        {"etf_observation_starts", "source_type, subject_type, subject_id"},
        {"etf_market_sessions", "calendar_id, calendar_version, session_date"},
        {"etf_observations", "observation_id"},
    };
    QJsonObject out;
    for (const TableOrder& t : kTables) {
        auto r = db().execute(
            QStringLiteral("SELECT * FROM %1 ORDER BY %2").arg(QLatin1String(t.table), QLatin1String(t.order_by)));
        if (r.is_err())
            return Result<QJsonObject>::err(std::string(t.table) + ": " + r.error());
        auto& q = r.value();
        QJsonArray rows;
        while (q.next()) {
            const QSqlRecord rec = q.record();
            QJsonObject row;
            for (int i = 0; i < rec.count(); ++i)
                row.insert(rec.fieldName(i), etf_json_value(q.value(i)));
            rows.append(row);
        }
        out.insert(QLatin1String(t.table), rows);
    }
    return Result<QJsonObject>::ok(out);
}

} // namespace fincept
