#pragma once
// EtfDataRepository — persistence of the ETF Capital Flows data foundation
// (Batch B; schema in migration v052_etf_data_foundation).
//
// Writes are append-only for source values. A later retrieval that delivers
// the same value WITH THE SAME MEANING (kind, units, basis, period and, for
// SEC, acceptance time) confirms the stored vintage (last_seen_at,
// seen_count). Anything else is a NEW vintage and the earlier one stays exactly
// as it was, except under one SEC accession, where a filed document cannot
// change and the difference is refused; an SEC amendment is a new vintage of
// its own filing. Nothing here deletes or rewrites a stored value, its timing
// or its provenance.
//
// Callers own transactions: an ingestion unit (one filing, one bar response)
// is written between Database::begin_transaction() and commit(), so a failure
// part-way leaves nothing of that unit behind.
//
// Main-thread use, like the other repositories: Database::instance() hands a
// per-thread connection to any other caller.

#include "services/etf/EtfDataModel.h"
#include "services/etf/EtfReadModel.h"
#include "services/etf/EtfSessionCalendar.h"
#include "storage/repositories/BaseRepository.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace fincept {

namespace etf_store {

using services::etf::AcquisitionMode;
using services::etf::FieldValue;
using services::etf::LinkRelationship;
using services::etf::MarketSessionDay;
using services::etf::MeasurementKind;
using services::etf::QualityState;
using services::etf::RetrievalStatus;
using services::etf::SourceType;
using services::etf::StoredObservation;
using services::etf::SubjectType;

/// UTC ISO-8601 with milliseconds, the only instant format stored.
QString iso_utc(const QDateTime& t);
/// Parse a stored instant; invalid for NULL or anything else.
QDateTime parse_iso_utc(const QString& s);

struct RetrievalRecord {
    QString run_id;
    SourceType source_type = SourceType::SecNport;
    AcquisitionMode acquisition_mode = AcquisitionMode::RegulatoryApi;
    QString endpoint;
    QString request_ref;
    QString subject_ref;
    QDateTime requested_at;
    QDateTime retrieved_at; ///< invalid: stored NULL (no response)
    RetrievalStatus status = RetrievalStatus::SourceError;
    QString detail_code;
    QString detail;
    std::optional<int> http_status;
    QString response_sha256; ///< empty: stored NULL
    std::optional<qint64> response_bytes;
    QString interpretation;
    QString route_policy;
    QString calendar_version;
    QString runtime_identity;
};

struct IssueRecord {
    std::optional<SubjectType> subject_type;
    qint64 subject_id = 0;
    QDate effective_date; ///< invalid: stored NULL
    QualityState state = QualityState::SourceError;
    QString code;
    QString detail;
};

struct ReportingEntityFacts {
    QString cik;       ///< 10 digits
    QString series_id; ///< empty for a registrant that reports without a series
    QString registrant_name;
    QString series_name;
    QString reg_file_number;
    QString registrant_lei;
    QString series_lei;
    QDateTime source_accepted_at; ///< SEC acceptance time of the filing that reported these attributes
};

struct SecFilingFacts {
    QString accession;
    QString filer_cik;
    QString form;
    QDate filing_date;
    QString report_date;
    QDateTime accepted_at;
    QString primary_document;
    qint64 entity_id = 0;
    QString amends_accession;
    QDate rep_pd_date;
    QString rep_pd_end;
    QString is_final_filing;
    bool returns_block_present = false;
    QString class_ids; ///< sorted, comma-separated
    QString document_sha256;
};

/// An accession seen again with DIFFERENT bytes is not an outcome but an
/// error of upsert_sec_filing: a filed document does not change, so nothing of
/// such a delivery may be written (see upsert_sec_filing).
enum class FilingOutcome {
    Inserted,  ///< first time this accession was seen
    Confirmed, ///< seen again with the same document bytes
};

struct ListedInstrumentFacts {
    qint64 con_id = 0;
    QString symbol;
    QString security_type;
    QString exchange;
    QString primary_exchange;
    QString currency;
    QString stock_type; ///< IBKR contract-details classification; must be "ETF"
};

struct ObservationInput {
    SubjectType subject_type = SubjectType::ReportingEntity;
    qint64 subject_id = 0;
    MeasurementKind kind = MeasurementKind::RegulatoryReportedFlow;
    QString measure;
    QString units;
    QString basis;
    SourceType source_type = SourceType::SecNport;
    AcquisitionMode acquisition_mode = AcquisitionMode::RegulatoryApi;
    QString source_document; ///< SEC accession; empty for IBKR
    qint64 filing_id = 0;    ///< SEC only
    bool amended_filing = false;
    QDate effective_date;
    QDate period_start;
    QDate period_end;
    QDate report_period;
    QDateTime accepted_at; ///< SEC only
    FieldValue value;
    qint64 retrieval_id = 0;
    QDateTime seen_at;           ///< this retrieval's time: first_seen_at of a new vintage
    QDateTime observation_start; ///< when MarketLab began observing the subject from this source
};

enum class ObservationOutcome {
    InsertedOriginal,       ///< first vintage of the key (NPORT-P or first IBKR bar)
    InsertedAmendment,      ///< a new NPORT-P/A vintage; earlier vintages untouched
    InsertedRevision,       ///< IBKR re-delivered a different value or meaning; the earlier vintage kept
    Confirmed,              ///< same value and meaning from a later retrieval: last_seen_at / seen_count updated
    AlreadyRecorded,        ///< this very retrieval already recorded this value (exact replay): nothing changed
    RefusedDocumentChanged, ///< an SEC accession re-delivered a different value or meaning: refused, nothing written
};

const char* observation_outcome_id(ObservationOutcome o);

} // namespace etf_store

class EtfDataRepository : public BaseRepository<services::etf::StoredObservation> {
  public:
    static EtfDataRepository& instance();

    // ── Retrievals and issues ────────────────────────────────────────────────
    Result<qint64> record_retrieval(const etf_store::RetrievalRecord& r);
    Result<void> record_issue(qint64 retrieval_id, const etf_store::IssueRecord& issue);

    // ── Identity ─────────────────────────────────────────────────────────────
    /// Identity is (cik, series_id). Names and LEIs are attributes of the
    /// filing that reported them: the entity keeps those of its newest filing
    /// by SEC acceptance time, so an older filing ingested later never
    /// overwrites them. last_seen_at is the latest sighting. Returns the id.
    Result<qint64> upsert_reporting_entity(const etf_store::ReportingEntityFacts& f, const QDateTime& seen_at);
    /// Identity is the IBKR conId. The ticker is an attribute; every ticker
    /// seen for the conId is kept in etf_instrument_symbols. Only an instrument
    /// IBKR classifies as stockType ETF is accepted.
    Result<qint64> upsert_listed_instrument(const etf_store::ListedInstrumentFacts& f, const QDateTime& seen_at);
    /// Declare a listed instrument <-> reporting entity link. Refused when the
    /// relationship contradicts the class structure the stored N-PORT filings
    /// report for the entity (for example "sole_class_of_series" for a series
    /// whose latest filing reports two classes).
    Result<qint64> declare_link(qint64 instrument_id, qint64 entity_id, const QString& class_id,
                                services::etf::LinkRelationship relationship, const QString& basis,
                                const QDateTime& declared_at);
    /// The relationship of the instrument's link to an N-PORT reporting entity,
    /// or nullopt when none is stored. When several links exist the most
    /// recently declared one is returned.
    Result<std::optional<services::etf::LinkRelationship>> nport_link_relationship(qint64 instrument_id);

    // ── SEC filings ──────────────────────────────────────────────────────────
    /// Store a filing, or add a sighting when its accession is stored with the
    /// same document bytes. An accession stored with OTHER bytes is refused
    /// with an error and nothing is written, so the caller's transaction for
    /// that delivery cannot commit: a filed document does not change, and the
    /// stored filing, its entity attributes and its values stay as they were.
    Result<std::pair<qint64, etf_store::FilingOutcome>>
    upsert_sec_filing(const etf_store::SecFilingFacts& f, qint64 retrieval_id, const QDateTime& seen_at);
    /// The document SHA-256 stored for an accession, or nullopt when the
    /// accession is not stored.
    Result<std::optional<QString>> stored_filing_sha256(const QString& accession);

    // ── Observation start ────────────────────────────────────────────────────
    /// The stored observation start for (source, subject); stores `candidate`
    /// first when none exists yet. Never moves an existing start.
    Result<QDateTime> observation_start(services::etf::SourceType source, services::etf::SubjectType subject_type,
                                        qint64 subject_id, const QDateTime& candidate, qint64 retrieval_id);

    // ── Session calendar ─────────────────────────────────────────────────────
    /// Store the calendar days used by a bar retrieval (sessions and holidays).
    /// Existing (calendar, version, date) rows are kept as they are.
    Result<void> upsert_sessions(const QVector<services::etf::MarketSessionDay>& days);

    // ── Observations ─────────────────────────────────────────────────────────
    Result<etf_store::ObservationOutcome> record_observation(const etf_store::ObservationInput& in);

    // ── Reads ────────────────────────────────────────────────────────────────
    /// Every vintage of one key, in storage order.
    Result<QVector<services::etf::StoredObservation>> vintages(services::etf::SubjectType subject_type,
                                                               qint64 subject_id, const QString& measure,
                                                               const QDate& effective_date,
                                                               services::etf::SourceType source);
    /// Every vintage stored for a subject, ordered by measure, date, revision.
    Result<QVector<services::etf::StoredObservation>> subject_observations(services::etf::SubjectType subject_type,
                                                                           qint64 subject_id);
    Result<std::optional<qint64>> find_reporting_entity(const QString& cik10, const QString& series_id);
    Result<std::optional<qint64>> find_listed_instrument(qint64 con_id);

    /// Every ETF table, every row, in primary-key order, as JSON. Deterministic;
    /// NULL stays JSON null. Used to reload and compare state across restarts.
    Result<QJsonObject> export_all();

  private:
    EtfDataRepository() = default;
    static services::etf::StoredObservation map_observation(QSqlQuery& q);
};

} // namespace fincept
