#include "services/etf/EtfSecNportIngestor.h"

#include "core/logging/Logger.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace fincept::services::etf {

namespace {

constexpr const char* kSecIngestTag = "EtfSecIngest";

QString sec_sha256_hex(const QByteArray& body) {
    return QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
}

/// Net assets are a fiscal-quarter-end value when the report date is a whole
/// number of quarters from the fiscal year end (repPdEnd); otherwise the value
/// is still the report-date net assets, labelled as such.
QString nport_net_assets_basis(const NportDocument& doc) {
    const QDate fye = QDate::fromString(doc.rep_pd_end_raw, Qt::ISODate);
    if (fye.isValid() && doc.rep_pd_date.isValid() && nport_report_date_is_month_end(doc.rep_pd_date) &&
        nport_report_date_is_month_end(fye)) {
        const int months = (fye.year() - doc.rep_pd_date.year()) * 12 + (fye.month() - doc.rep_pd_date.month());
        if (months % 3 == 0)
            return QStringLiteral("regulatory_quarter_end_net_assets");
    }
    return QStringLiteral("regulatory_report_date_net_assets");
}

} // namespace

QJsonObject SecNportRunSummary::to_json() const {
    QJsonObject o;
    o["run_id"] = run_id;
    o["status"] = QLatin1String(retrieval_status_id(status));
    o["detail_code"] = detail_code;
    o["detail"] = detail;
    o["retrievals"] = retrievals;
    o["filings_selected"] = filings_selected;
    o["filings_stored"] = filings_stored;
    o["filings_skipped"] = filings_skipped;
    o["observations_inserted"] = observations_inserted;
    o["observations_amended"] = observations_amended;
    o["observations_confirmed"] = observations_confirmed;
    o["observations_already_recorded"] = observations_already_recorded;
    o["observations_refused"] = observations_refused;
    o["issues"] = issues;
    o["accessions"] = QJsonArray::fromStringList(accessions);
    QJsonArray ids;
    for (qint64 id : entity_ids)
        ids.append(id);
    o["entity_ids"] = ids;
    return o;
}

EtfSecNportIngestor::EtfSecNportIngestor(SecHttpGet http, QString user_agent, int min_interval_ms, Clock clock,
                                         QObject* parent)
    : QObject(parent),
      http_(std::move(http)),
      user_agent_(std::move(user_agent)),
      min_interval_ms_(std::max(0, min_interval_ms)),
      clock_(std::move(clock)) {}

void EtfSecNportIngestor::run(const SecNportRequest& request, Done done) {
    done_ = std::move(done);
    request_ = request;
    summary_ = SecNportRunSummary();
    summary_.run_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    cik10_ = sec_normalized_cik(request.cik);

    if (!acquisition_route(SourceType::SecNport, AcquisitionMode::RegulatoryApi).enabled) {
        const QString reason = acquisition_route(SourceType::SecNport, AcquisitionMode::RegulatoryApi).reason;
        record_refusal(RetrievalStatus::RouteDisabled, QStringLiteral("route_disabled"), reason);
        finish(RetrievalStatus::RouteDisabled, QStringLiteral("route_disabled"), reason);
        return;
    }
    if (!sec_user_agent_valid(user_agent_)) {
        const QString detail = QStringLiteral("SEC requests need a declared User-Agent: a name or organisation and "
                                              "an administrative contact e-mail (set it in sec_edgar.json or "
                                              "MARKETLAB_SEC_USER_AGENT; never commit it)");
        record_refusal(RetrievalStatus::NotConfigured, QStringLiteral("sec_user_agent_not_declared"), detail);
        finish(RetrievalStatus::NotConfigured, QStringLiteral("sec_user_agent_not_declared"), detail);
        return;
    }
    if (cik10_.isEmpty() || (!request.series_id.isEmpty() && !sec_valid_series_id(request.series_id)) ||
        request.max_filings < 1 || request.max_filings > kSecMaxFilingsPerRun) {
        // An invalid request never reaches the network and records nothing.
        finish(RetrievalStatus::SourceError, QStringLiteral("invalid_request"),
               QStringLiteral("a CIK of 1-10 digits, an optional series id S#########, and 1-%1 filings are "
                              "required")
                   .arg(kSecMaxFilingsPerRun));
        return;
    }
    if (!request.series_id.isEmpty())
        start_series_index();
    else
        start_submissions();
}

// ── Transport and recording ──────────────────────────────────────────────────

void EtfSecNportIngestor::fetch(const QString& url, Handler handler) {
    int delay = 0;
    if (pacer_.isValid()) {
        const qint64 elapsed = pacer_.elapsed();
        if (elapsed < min_interval_ms_)
            delay = static_cast<int>(min_interval_ms_ - elapsed);
    }
    QPointer<EtfSecNportIngestor> self(this);
    QTimer::singleShot(delay, this, [self, url, handler]() {
        if (!self || self->finished_)
            return;
        self->pacer_.start();
        const QDateTime requested_at = self->clock_();
        self->http_(url, self->user_agent_.toUtf8(), [self, handler, requested_at](const SecHttpResponse& r) {
            if (!self || self->finished_)
                return;
            handler(r, requested_at, self->clock_());
        });
    });
}

QString EtfSecNportIngestor::http_problem(const SecHttpResponse& r) {
    if (!r.transport_ok)
        return QStringLiteral("transport_error");
    if (r.http_status != 200)
        return QStringLiteral("http_%1").arg(r.http_status);
    return {};
}

bool EtfSecNportIngestor::is_throttle(const SecHttpResponse& r) {
    return r.transport_ok && (r.http_status == 403 || r.http_status == 429);
}

qint64 EtfSecNportIngestor::record_response(SourceType source, const QString& endpoint, const QString& url,
                                            const SecHttpResponse& response, const QDateTime& requested_at,
                                            const QDateTime& retrieved_at, RetrievalStatus status,
                                            const QString& detail_code, const QString& detail,
                                            const QString& interpretation) {
    etf_store::RetrievalRecord r;
    r.run_id = summary_.run_id;
    r.source_type = source;
    r.acquisition_mode = AcquisitionMode::RegulatoryApi;
    r.endpoint = endpoint;
    r.request_ref = url;
    r.subject_ref = request_.series_id.isEmpty() ? cik10_ : cik10_ + QLatin1Char('/') + request_.series_id;
    r.requested_at = requested_at;
    r.status = status;
    r.detail_code = detail_code;
    r.detail = detail;
    if (response.transport_ok) {
        r.retrieved_at = retrieved_at;
        r.http_status = response.http_status;
        r.response_sha256 = sec_sha256_hex(response.body);
        r.response_bytes = response.body.size();
    }
    r.interpretation = interpretation;
    r.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    r.calendar_version = QLatin1String(kUsEquityCalendarVersion);
    r.runtime_identity =
        QString::fromUtf8(QJsonDocument(QJsonObject{{"user_agent", sec_user_agent_redacted(user_agent_)}})
                              .toJson(QJsonDocument::Compact));
    auto id = EtfDataRepository::instance().record_retrieval(r);
    if (id.is_err()) {
        LOG_ERROR(kSecIngestTag, QString("could not record retrieval: %1").arg(QString::fromStdString(id.error())));
        return 0;
    }
    ++summary_.retrievals;
    if (first_retrieval_id_ == 0) {
        first_retrieval_id_ = id.value();
        run_started_at_ = requested_at;
    }
    return id.value();
}

qint64 EtfSecNportIngestor::record_refusal(RetrievalStatus status, const QString& code, const QString& detail) {
    etf_store::RetrievalRecord r;
    r.run_id = summary_.run_id;
    r.source_type = SourceType::SecNport;
    r.acquisition_mode = AcquisitionMode::RegulatoryApi;
    r.endpoint = QStringLiteral("refused_before_request");
    r.request_ref = QStringLiteral("none");
    r.subject_ref = request_.series_id.isEmpty() ? request_.cik : request_.cik + QLatin1Char('/') + request_.series_id;
    r.requested_at = clock_();
    r.status = status;
    r.detail_code = code;
    r.detail = detail;
    r.interpretation = QLatin1String(kSecNportInterpretation);
    r.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    auto id = EtfDataRepository::instance().record_retrieval(r);
    if (id.is_err())
        return 0;
    ++summary_.retrievals;
    return id.value();
}

void EtfSecNportIngestor::record_issue(qint64 retrieval_id, QualityState state, const QString& code,
                                       const QString& detail, qint64 entity_id, const QDate& effective_date) {
    if (retrieval_id <= 0)
        return;
    etf_store::IssueRecord issue;
    if (entity_id > 0) {
        issue.subject_type = SubjectType::ReportingEntity;
        issue.subject_id = entity_id;
    }
    issue.effective_date = effective_date;
    issue.state = state;
    issue.code = code;
    issue.detail = detail;
    if (EtfDataRepository::instance().record_issue(retrieval_id, issue).is_ok())
        ++summary_.issues;
}

// ── Discovery ────────────────────────────────────────────────────────────────

void EtfSecNportIngestor::start_series_index() {
    const QString url = sec_series_index_url(request_.series_id);
    fetch(url, [this, url](const SecHttpResponse& r, const QDateTime& requested_at, const QDateTime& retrieved_at) {
        const QString problem = http_problem(r);
        if (!problem.isEmpty()) {
            record_response(SourceType::SecSubmissions, QStringLiteral("series_filing_index"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, problem, r.error,
                            QLatin1String(kSecSeriesIndexInterpretation));
            finish(RetrievalStatus::SourceError, problem, QStringLiteral("the series filing index was unavailable"));
            return;
        }
        const SecSeriesIndex index = parse_sec_series_index(r.body);
        if (!index.ok) {
            const QString code = index.error.section(QLatin1Char(':'), 0, 0);
            record_response(SourceType::SecSubmissions, QStringLiteral("series_filing_index"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, code, index.error,
                            QLatin1String(kSecSeriesIndexInterpretation));
            finish(RetrievalStatus::SourceError, code, index.error);
            return;
        }
        if (index.company_cik != cik10_) {
            const QString detail = QStringLiteral("series %1 is filed by CIK %2, not the requested %3")
                                       .arg(request_.series_id, index.company_cik, cik10_);
            record_response(SourceType::SecSubmissions, QStringLiteral("series_filing_index"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, QStringLiteral("series_registrant_mismatch"),
                            detail, QLatin1String(kSecSeriesIndexInterpretation));
            finish(RetrievalStatus::SourceError, QStringLiteral("series_registrant_mismatch"), detail);
            return;
        }
        record_response(SourceType::SecSubmissions, QStringLiteral("series_filing_index"), url, r, requested_at,
                        retrieved_at, RetrievalStatus::Ok, QString(),
                        QStringLiteral("%1 index entries").arg(index.entries.size()),
                        QLatin1String(kSecSeriesIndexInterpretation));
        for (const SecIndexEntry& e : index.entries) {
            if (e.form == QLatin1String("NPORT-P") || e.form == QLatin1String("NPORT-P/A"))
                index_entries_.append(e);
        }
        start_submissions();
    });
}

void EtfSecNportIngestor::start_submissions() {
    const QString url = sec_submissions_url(cik10_);
    fetch(url, [this, url](const SecHttpResponse& r, const QDateTime& requested_at, const QDateTime& retrieved_at) {
        const QString problem = http_problem(r);
        if (!problem.isEmpty()) {
            record_response(SourceType::SecSubmissions, QStringLiteral("submissions_json"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, problem, r.error,
                            QLatin1String(kSecSubmissionsInterpretation));
            finish(RetrievalStatus::SourceError, problem,
                   QStringLiteral("the registrant submissions were unavailable"));
            return;
        }
        const SecSubmissions subs = parse_sec_submissions(r.body, /*older_page=*/false);
        if (!subs.ok || subs.cik != cik10_) {
            const QString code = subs.ok ? QStringLiteral("submissions_registrant_mismatch")
                                         : subs.error.section(QLatin1Char(':'), 0, 0);
            const QString detail =
                subs.ok ? QStringLiteral("submissions describe CIK %1, not %2").arg(subs.cik, cik10_) : subs.error;
            record_response(SourceType::SecSubmissions, QStringLiteral("submissions_json"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, code, detail,
                            QLatin1String(kSecSubmissionsInterpretation));
            finish(RetrievalStatus::SourceError, code, detail);
            return;
        }
        record_response(SourceType::SecSubmissions, QStringLiteral("submissions_json"), url, r, requested_at,
                        retrieved_at, RetrievalStatus::Ok, QString(),
                        QStringLiteral("%1 recent filings").arg(subs.filings.size()),
                        QLatin1String(kSecSubmissionsInterpretation));
        on_submissions(subs);
    });
}

void EtfSecNportIngestor::on_submissions(const SecSubmissions& subs) {
    for (const SecFilingRef& f : subs.filings)
        refs_by_accession_.insert(f.accession, f);
    older_pages_ = subs.older_pages;
    if (request_.series_id.isEmpty()) {
        // Registrant target: its own submissions list its N-PORT filings.
        for (const SecFilingRef& f : subs.filings) {
            if (f.form == QLatin1String("NPORT-P") || f.form == QLatin1String("NPORT-P/A"))
                index_entries_.append({f.accession, f.form, f.filing_date});
        }
    }
    std::stable_sort(index_entries_.begin(), index_entries_.end(),
                     [](const SecIndexEntry& a, const SecIndexEntry& b) { return a.filing_date > b.filing_date; });
    select_next_candidate();
}

void EtfSecNportIngestor::fetch_page_then(const SecOlderPage& page, std::function<void()> next) {
    fetched_pages_.insert(page.name);
    ++pages_fetched_;
    const QString url = sec_submissions_page_url(page.name);
    if (url.isEmpty()) {
        next();
        return;
    }
    fetch(url,
          [this, url, next](const SecHttpResponse& r, const QDateTime& requested_at, const QDateTime& retrieved_at) {
              const QString problem = http_problem(r);
              if (!problem.isEmpty()) {
                  record_response(SourceType::SecSubmissions, QStringLiteral("submissions_page"), url, r, requested_at,
                                  retrieved_at, RetrievalStatus::SourceError, problem, r.error,
                                  QLatin1String(kSecSubmissionsInterpretation));
                  if (is_throttle(r)) {
                      finish(RetrievalStatus::SourceError, problem, QStringLiteral("the SEC throttled the run"));
                      return;
                  }
                  next();
                  return;
              }
              const SecSubmissions page = parse_sec_submissions(r.body, /*older_page=*/true);
              record_response(SourceType::SecSubmissions, QStringLiteral("submissions_page"), url, r, requested_at,
                              retrieved_at, page.ok ? RetrievalStatus::Ok : RetrievalStatus::SourceError,
                              page.ok ? QString() : page.error.section(QLatin1Char(':'), 0, 0),
                              page.ok ? QStringLiteral("%1 filings").arg(page.filings.size()) : page.error,
                              QLatin1String(kSecSubmissionsInterpretation));
              if (page.ok) {
                  for (const SecFilingRef& f : page.filings) {
                      refs_by_accession_.insert(f.accession, f);
                      if (request_.series_id.isEmpty() &&
                          (f.form == QLatin1String("NPORT-P") || f.form == QLatin1String("NPORT-P/A"))) {
                          bool listed = false;
                          for (const SecIndexEntry& e : index_entries_)
                              listed = listed || e.accession == f.accession;
                          if (!listed)
                              index_entries_.append({f.accession, f.form, f.filing_date});
                      }
                  }
                  std::stable_sort(
                      index_entries_.begin() + index_cursor_, index_entries_.end(),
                      [](const SecIndexEntry& a, const SecIndexEntry& b) { return a.filing_date > b.filing_date; });
              }
              next();
          });
}

void EtfSecNportIngestor::select_next_candidate() {
    while (selected_.size() < request_.max_filings && index_cursor_ < index_entries_.size()) {
        const SecIndexEntry& entry = index_entries_[index_cursor_];
        if (!refs_by_accession_.contains(entry.accession)) {
            // The filing is older than the pages read so far: read the page
            // whose filing-date range covers it, once, within the page budget.
            for (const SecOlderPage& page : older_pages_) {
                if (fetched_pages_.contains(page.name) || pages_fetched_ >= kSecMaxOlderPagesPerRun)
                    continue;
                if (entry.filing_date.isValid() && page.filing_from.isValid() && page.filing_to.isValid() &&
                    entry.filing_date >= page.filing_from && entry.filing_date <= page.filing_to) {
                    fetch_page_then(page, [this]() { select_next_candidate(); });
                    return;
                }
            }
            ++index_cursor_;
            ++summary_.filings_skipped;
            record_issue(first_retrieval_id_, QualityState::SourceError, QStringLiteral("acceptance_time_not_found"),
                         QStringLiteral("%1 is listed but its acceptance time was not found in the submissions read")
                             .arg(entry.accession));
            continue;
        }
        const SecFilingRef ref = refs_by_accession_.value(entry.accession);
        ++index_cursor_;
        if (!ref.accepted_at.isValid()) {
            ++summary_.filings_skipped;
            record_issue(first_retrieval_id_, QualityState::SourceError, QStringLiteral("acceptance_time_unparseable"),
                         QStringLiteral("%1 acceptanceDateTime '%2'").arg(ref.accession, ref.accepted_at_raw));
            continue;
        }
        const QDate report = QDate::fromString(ref.report_date, Qt::ISODate);
        if ((request_.report_period_from.isValid() && (!report.isValid() || report < request_.report_period_from)) ||
            (request_.report_period_to.isValid() && (!report.isValid() || report > request_.report_period_to)))
            continue;
        selected_.append(ref);
    }
    // A registrant target lists its filings in its own submissions: when the
    // recent window holds fewer N-PORT filings than asked for, read the next
    // older page (newest first), within the page budget.
    if (request_.series_id.isEmpty() && selected_.size() < request_.max_filings &&
        index_cursor_ >= index_entries_.size() && pages_fetched_ < kSecMaxOlderPagesPerRun) {
        for (const SecOlderPage& page : older_pages_) {
            if (!fetched_pages_.contains(page.name)) {
                fetch_page_then(page, [this]() { select_next_candidate(); });
                return;
            }
        }
    }
    // Process in acceptance order, so an amendment is recorded after the filing
    // it amends whenever both are in the run.
    std::stable_sort(selected_.begin(), selected_.end(),
                     [](const SecFilingRef& a, const SecFilingRef& b) { return a.accepted_at < b.accepted_at; });
    summary_.filings_selected = selected_.size();
    fetch_next_document();
}

// ── Documents ────────────────────────────────────────────────────────────────

void EtfSecNportIngestor::fetch_next_document() {
    if (document_cursor_ >= selected_.size()) {
        finish(RetrievalStatus::Ok, selected_.isEmpty() ? QStringLiteral("no_nport_filings_selected") : QString(),
               QStringLiteral("%1 of %2 selected filings stored").arg(summary_.filings_stored).arg(selected_.size()));
        return;
    }
    const SecFilingRef ref = selected_[document_cursor_++];
    const QString url = sec_nport_primary_doc_url(cik10_, ref.accession);
    fetch(url, [this, url, ref](const SecHttpResponse& r, const QDateTime& requested_at,
                                const QDateTime& retrieved_at) {
        const QString problem = http_problem(r);
        if (!problem.isEmpty()) {
            record_response(SourceType::SecNport, QStringLiteral("nport_primary_doc"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, problem, r.error,
                            QLatin1String(kSecNportInterpretation));
            ++summary_.filings_skipped;
            if (is_throttle(r)) {
                finish(RetrievalStatus::SourceError, problem, QStringLiteral("the SEC throttled the run"));
                return;
            }
            fetch_next_document();
            return;
        }
        const NportDocument doc = parse_nport_primary_doc(r.body);
        if (!doc.ok) {
            record_response(SourceType::SecNport, QStringLiteral("nport_primary_doc"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, doc.error.section(QLatin1Char(':'), 0, 0),
                            doc.error, QLatin1String(kSecNportInterpretation));
            ++summary_.filings_skipped;
            fetch_next_document();
            return;
        }
        // Identity and form checks come before anything is stored.
        QString refusal;
        if (doc.submission_type != ref.form)
            refusal = QStringLiteral("form_mismatch: submissions list %1, the document says %2")
                          .arg(ref.form, doc.submission_type);
        else if (doc.reg_cik.isEmpty())
            refusal = QStringLiteral("registrant_cik_missing: the document names no regCik");
        else if (doc.reg_cik != cik10_)
            refusal = QStringLiteral("registrant_mismatch: the document names CIK %1, not %2").arg(doc.reg_cik, cik10_);
        else if (!request_.series_id.isEmpty() && doc.series_id != request_.series_id)
            refusal = QStringLiteral("series_mismatch: the document names series '%1', not %2")
                          .arg(doc.series_id, request_.series_id);
        else if (!doc.series_id.isEmpty() && !sec_valid_series_id(doc.series_id))
            refusal = QStringLiteral("series_id_invalid: '%1'").arg(doc.series_id);
        else if (!doc.rep_pd_date.isValid())
            refusal = QStringLiteral("report_period_invalid: repPdDate '%1'").arg(doc.rep_pd_date_raw);
        else if (!ref.report_date.isEmpty() && ref.report_date != doc.rep_pd_date.toString(Qt::ISODate))
            refusal = QStringLiteral("report_date_mismatch: submissions list %1, the document says %2")
                          .arg(ref.report_date, doc.rep_pd_date_raw);
        else if (doc.submission_type == QLatin1String("NPORT-P/A") && !doc.amended_accession.isEmpty() &&
                 !sec_valid_accession(doc.amended_accession))
            refusal = QStringLiteral("amended_accession_invalid: '%1'").arg(doc.amended_accession);
        if (!refusal.isEmpty()) {
            record_response(SourceType::SecNport, QStringLiteral("nport_primary_doc"), url, r, requested_at,
                            retrieved_at, RetrievalStatus::SourceError, refusal.section(QLatin1Char(':'), 0, 0),
                            refusal, QLatin1String(kSecNportInterpretation));
            ++summary_.filings_skipped;
            fetch_next_document();
            return;
        }
        const qint64 retrieval_id = record_response(SourceType::SecNport, QStringLiteral("nport_primary_doc"), url, r,
                                                    requested_at, retrieved_at, RetrievalStatus::Ok, QString(),
                                                    ref.accession, QLatin1String(kSecNportInterpretation));
        if (retrieval_id <= 0) {
            finish(RetrievalStatus::SourceError, QStringLiteral("storage_error"),
                   QStringLiteral("the retrieval could not be recorded"));
            return;
        }
        persist_document(ref, doc, r.body, retrieval_id, retrieved_at);
        fetch_next_document();
    });
}

void EtfSecNportIngestor::persist_document(const SecFilingRef& ref, const NportDocument& doc, const QByteArray& body,
                                           qint64 retrieval_id, const QDateTime& seen_at) {
    auto& repo = EtfDataRepository::instance();
    auto& db = Database::instance();
    auto begin = db.begin_transaction();
    if (begin.is_err()) {
        record_issue(retrieval_id, QualityState::SourceError, QStringLiteral("storage_error"),
                     QString::fromStdString(begin.error()));
        ++summary_.filings_skipped;
        return;
    }
    auto fail = [&](const std::string& what) {
        db.rollback();
        LOG_ERROR(kSecIngestTag, QString("could not store %1: %2").arg(ref.accession, QString::fromStdString(what)));
        record_issue(retrieval_id, QualityState::SourceError, QStringLiteral("storage_error"),
                     QString::fromStdString(what));
        ++summary_.filings_skipped;
    };

    etf_store::ReportingEntityFacts facts;
    facts.cik = doc.reg_cik;
    facts.series_id = doc.series_id;
    facts.registrant_name = doc.reg_name;
    facts.series_name = doc.series_name;
    facts.reg_file_number = doc.reg_file_number;
    facts.registrant_lei = doc.reg_lei;
    facts.series_lei = doc.series_lei;
    auto entity = repo.upsert_reporting_entity(facts, seen_at);
    if (entity.is_err())
        return fail(entity.error());
    const qint64 entity_id = entity.value();

    auto start = repo.observation_start(SourceType::SecNport, SubjectType::ReportingEntity, entity_id, run_started_at_,
                                        first_retrieval_id_);
    if (start.is_err())
        return fail(start.error());

    etf_store::SecFilingFacts filing;
    filing.accession = ref.accession;
    filing.filer_cik = cik10_;
    filing.form = ref.form;
    filing.filing_date = ref.filing_date;
    filing.report_date = ref.report_date;
    filing.accepted_at = ref.accepted_at;
    filing.primary_document = ref.primary_document;
    filing.entity_id = entity_id;
    filing.amends_accession = ref.form == QLatin1String("NPORT-P/A") ? doc.amended_accession : QString();
    filing.rep_pd_date = doc.rep_pd_date;
    filing.rep_pd_end = doc.rep_pd_end_raw;
    filing.is_final_filing = doc.is_final_filing;
    filing.returns_block_present = doc.returns_block_present;
    filing.class_ids = doc.return_class_ids.join(QLatin1Char(','));
    filing.document_sha256 = sec_sha256_hex(body);
    auto filing_r = repo.upsert_sec_filing(filing, retrieval_id, seen_at);
    if (filing_r.is_err())
        return fail(filing_r.error());
    const qint64 filing_id = filing_r.value().first;
    if (filing_r.value().second == etf_store::FilingOutcome::DocumentChanged)
        record_issue(
            retrieval_id, QualityState::SourceError, QStringLiteral("filing_document_changed"),
            QStringLiteral("%1 was re-delivered with different bytes; changed values are refused").arg(ref.accession),
            entity_id);

    auto record = [&](MeasurementKind kind, const QString& measure, const QString& basis, const QDate& effective,
                      const QDate& period_start, const QDate& period_end, const FieldValue& value) -> bool {
        etf_store::ObservationInput in;
        in.subject_type = SubjectType::ReportingEntity;
        in.subject_id = entity_id;
        in.kind = kind;
        in.measure = measure;
        in.units = QStringLiteral("USD");
        in.basis = basis;
        in.source_type = SourceType::SecNport;
        in.acquisition_mode = AcquisitionMode::RegulatoryApi;
        in.source_document = ref.accession;
        in.filing_id = filing_id;
        in.amended_filing = ref.form == QLatin1String("NPORT-P/A");
        in.effective_date = effective;
        in.period_start = period_start;
        in.period_end = period_end;
        in.report_period = doc.rep_pd_date;
        in.accepted_at = ref.accepted_at;
        in.value = value;
        in.retrieval_id = retrieval_id;
        in.seen_at = seen_at;
        in.observation_start = start.value();
        auto outcome = repo.record_observation(in);
        if (outcome.is_err()) {
            fail(outcome.error());
            return false;
        }
        switch (outcome.value()) {
            case etf_store::ObservationOutcome::InsertedOriginal:
            case etf_store::ObservationOutcome::InsertedRevision:
                ++summary_.observations_inserted;
                break;
            case etf_store::ObservationOutcome::InsertedAmendment:
                ++summary_.observations_amended;
                break;
            case etf_store::ObservationOutcome::Confirmed:
                ++summary_.observations_confirmed;
                break;
            case etf_store::ObservationOutcome::AlreadyRecorded:
                ++summary_.observations_already_recorded;
                break;
            case etf_store::ObservationOutcome::RefusedDocumentChanged:
                ++summary_.observations_refused;
                record_issue(retrieval_id, QualityState::SourceError, QStringLiteral("value_changed_in_filed_document"),
                             QStringLiteral("%1 %2 %3").arg(ref.accession, measure, effective.toString(Qt::ISODate)),
                             entity_id, effective);
                break;
        }
        return true;
    };

    // Net assets of the report date: the regulatory-period denominator Batch D
    // may use for a regulatory_coverage_estimate. Never a prior-day AUM.
    if (!record(MeasurementKind::AumObservation, QStringLiteral("nport_net_assets"), nport_net_assets_basis(doc),
                doc.rep_pd_date, QDate(), QDate(), doc.net_assets))
        return;

    // Monthly flows, per calendar month. Only defined for a month-end report
    // date; anything else is refused rather than mapped by guesswork.
    if (!nport_report_date_is_month_end(doc.rep_pd_date)) {
        record_issue(retrieval_id, QualityState::SourceError, QStringLiteral("report_date_not_month_end"),
                     QStringLiteral("repPdDate %1: monthly flows not mapped").arg(doc.rep_pd_date_raw), entity_id,
                     doc.rep_pd_date);
    } else {
        for (const NportFlowMonth& m : doc.months) {
            const auto bounds = nport_flow_month_bounds(doc.rep_pd_date, m.month_index);
            const std::pair<const char*, const FieldValue*> fields[] = {{"nport_sales", &m.sales},
                                                                        {"nport_redemption", &m.redemption},
                                                                        {"nport_reinvestment", &m.reinvestment}};
            for (const auto& [measure, value] : fields) {
                // An absent <monNFlow> element leaves all three values Missing
                // (FieldValue's default); it never becomes a zero flow.
                if (!record(MeasurementKind::RegulatoryReportedFlow, QLatin1String(measure),
                            QStringLiteral("nport_monthly_flow"), bounds.second, bounds.first, bounds.second,
                            m.element_present ? *value : FieldValue::missing()))
                    return;
            }
        }
    }

    auto commit = db.commit();
    if (commit.is_err())
        return fail(commit.error());
    ++summary_.filings_stored;
    summary_.accessions.append(ref.accession);
    if (!summary_.entity_ids.contains(entity_id))
        summary_.entity_ids.append(entity_id);
}

void EtfSecNportIngestor::finish(RetrievalStatus status, const QString& code, const QString& detail) {
    if (finished_)
        return;
    finished_ = true;
    summary_.status = status;
    summary_.detail_code = code;
    summary_.detail = detail;
    LOG_INFO(kSecIngestTag, QString("run %1 finished: %2 %3 (%4 retrievals, %5 filings stored)")
                                .arg(summary_.run_id, QLatin1String(retrieval_status_id(status)), code)
                                .arg(summary_.retrievals)
                                .arg(summary_.filings_stored));
    if (done_) {
        // Deliver on the event loop, never re-entrantly inside run().
        QPointer<EtfSecNportIngestor> self(this);
        QTimer::singleShot(0, this, [self]() {
            if (self && self->done_)
                self->done_(self->summary_);
        });
    }
}

} // namespace fincept::services::etf
