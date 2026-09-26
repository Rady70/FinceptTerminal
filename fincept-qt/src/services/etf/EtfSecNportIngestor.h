// src/services/etf/EtfSecNportIngestor.h
//
// SEC N-PORT ingestion for the ETF data foundation (ETF Capital Flows Batch
// B). One run ingests the newest N-PORT filings of one SEC reporting target:
//
//   * a series (CIK + series id): its filings are listed by the EDGAR series
//     filing index, because a trust such as iShares Trust files for hundreds
//     of series under one CIK;
//   * a registrant that reports without a series (CIK only), such as the SPY
//     unit investment trust: its filings are listed by its submissions JSON.
//
// Acceptance times always come from the registrant's submissions JSON
// (`acceptanceDateTime`, exact, UTC), fetching an older page only when a
// listed filing is older than the recent window. Each primary document is
// parsed and checked against the requested identity before anything is
// stored, and each is written in its own transaction.
//
// A run is OK only when every filing it selected was stored as delivered. A
// filing that could not be fetched, parsed or identified, or a value refused
// because a filed document changed, makes the run a SOURCE_ERROR, while the
// filings that were stored stay stored. A storage failure ends the run at
// once, as a SOURCE_ERROR storage_error.
//
// Respectful access: one request at a time, a minimum interval between
// requests (the production configuration clamps it to at least 150 ms, well
// inside the SEC's 10 requests per second), a declared User-Agent with an
// administrative contact, at most 40 filings and a bounded number of pages per
// run, and the run stops at the first HTTP 403 or 429.
//
// The HTTP transport and the clock are injected: production passes the shared
// HttpClient and the wall clock; tests pass fixtures and a fixed clock.
#pragma once
#include "services/etf/EtfDataModel.h"
#include "services/etf/SecEdgarParse.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace fincept::services::etf {

inline constexpr const char* kSecNportInterpretation = "sec_nport_primary_doc_v1";
inline constexpr const char* kSecSubmissionsInterpretation = "sec_submissions_json_v1";
inline constexpr const char* kSecSeriesIndexInterpretation = "sec_series_filing_index_atom_v1";
inline constexpr int kSecMaxFilingsPerRun = 40;
inline constexpr int kSecMaxOlderPagesPerRun = 4;

struct SecHttpResponse {
    bool transport_ok = false; ///< a response arrived (any HTTP status)
    int http_status = 0;
    QByteArray body;
    QString error; ///< transport error text when !transport_ok
};

using SecHttpGet =
    std::function<void(const QString& url, const QByteArray& user_agent, std::function<void(const SecHttpResponse&)>)>;

struct SecNportRequest {
    QString cik;       ///< the registrant's CIK (any zero padding)
    QString series_id; ///< optional: S + 9 digits
    int max_filings = 4;
    QDate report_period_from; ///< optional filter on the listed report date
    QDate report_period_to;
};

struct SecNportRunSummary {
    QString run_id;
    RetrievalStatus status = RetrievalStatus::SourceError;
    QString detail_code;
    QString detail;
    int retrievals = 0;
    int filings_selected = 0;
    int filings_stored = 0;
    int filings_skipped = 0;
    int observations_inserted = 0;
    int observations_amended = 0;
    int observations_confirmed = 0;
    int observations_already_recorded = 0;
    int observations_refused = 0;
    int issues = 0;
    QStringList accessions;
    QVector<qint64> entity_ids;

    QJsonObject to_json() const;
};

class EtfSecNportIngestor : public QObject {
    Q_OBJECT
  public:
    using Clock = std::function<QDateTime()>;
    using Done = std::function<void(const SecNportRunSummary&)>;

    EtfSecNportIngestor(SecHttpGet http, QString user_agent, int min_interval_ms, Clock clock,
                        QObject* parent = nullptr);

    /// Start one run. `done` is called exactly once, on the event loop.
    void run(const SecNportRequest& request, Done done);

  private:
    using Handler =
        std::function<void(const SecHttpResponse&, const QDateTime& requested_at, const QDateTime& retrieved_at)>;

    void fetch(const QString& url, Handler handler);
    qint64 record_response(SourceType source, const QString& endpoint, const QString& url,
                           const SecHttpResponse& response, const QDateTime& requested_at,
                           const QDateTime& retrieved_at, RetrievalStatus status, const QString& detail_code,
                           const QString& detail, const QString& interpretation);
    qint64 record_refusal(RetrievalStatus status, const QString& code, const QString& detail);
    void record_issue(qint64 retrieval_id, QualityState state, const QString& code, const QString& detail,
                      qint64 entity_id = 0, const QDate& effective_date = QDate());
    /// SOURCE_ERROR for a response that is not a usable 200; empty otherwise.
    static QString http_problem(const SecHttpResponse& r);
    static bool is_throttle(const SecHttpResponse& r);

    void start_series_index();
    void start_submissions();
    void on_submissions(const SecSubmissions& subs);
    void select_next_candidate();
    void fetch_page_then(const SecOlderPage& page, std::function<void()> next);
    void fetch_next_document();
    /// Store one filing in one transaction. False when anything could not be
    /// stored: nothing of the filing is then left behind, `error` says why, and
    /// the caller ends the run as a storage error.
    bool persist_document(const SecFilingRef& ref, const NportDocument& doc, const QByteArray& body,
                          qint64 retrieval_id, const QDateTime& seen_at, QString* error);
    void finish(RetrievalStatus status, const QString& code, const QString& detail);

    SecHttpGet http_;
    QString user_agent_;
    int min_interval_ms_ = 500;
    Clock clock_;
    Done done_;
    SecNportRequest request_;
    QString cik10_;
    SecNportRunSummary summary_;
    QElapsedTimer pacer_; ///< real time since the last request started (the injected clock only dates records)
    QDateTime run_started_at_;
    qint64 first_retrieval_id_ = 0;
    bool finished_ = false;

    // Discovery state.
    QVector<SecIndexEntry> index_entries_;
    int index_cursor_ = 0;
    QHash<QString, SecFilingRef> refs_by_accession_;
    QVector<SecOlderPage> older_pages_;
    QSet<QString> fetched_pages_;
    int pages_fetched_ = 0;
    QVector<SecFilingRef> selected_;
    int document_cursor_ = 0;
};

} // namespace fincept::services::etf
