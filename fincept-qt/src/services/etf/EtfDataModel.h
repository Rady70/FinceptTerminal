// src/services/etf/EtfDataModel.h
//
// ETF Capital Flows, Batch B: the stored vocabulary of the normalized ETF data
// foundation (control repository docs/ETF_FLOW_BATCH_B_IMPLEMENTATION.md).
//
// Every enumeration below is persisted in SQLite by its stable string id and is
// enforced there by a CHECK constraint (migration v052). The ids come from the
// Batch A2 qualification (ETF_FLOW_BATCH_A2_QUALIFICATION.md section 10). They
// are provenance, not display text, so they are never translated.
//
// Central rule: unknown is not zero. A value the source did not supply is
// Missing, a value the source supplied but that is not a number is
// Unparseable, and only a parsed number is Reported. None of the three is ever
// turned into another.
//
// Header-only over Qt Core so the vocabulary, and the rules that are built on
// it, can be unit-tested without the database, the network or Python.
#pragma once
#include <QRegularExpression>
#include <QString>

#include <cmath>
#include <optional>

namespace fincept::services::etf {

// ── Measurement class (A2 section 10) ────────────────────────────────────────
// Source facts and MarketLab calculations are different classes. Only the
// first five are ever stored as raw observations; the calculated and proxy
// kinds exist so a later batch can name them, and migration v052 refuses them
// in the raw observation table.
enum class MeasurementKind {
    RegulatoryReportedFlow,           ///< SEC N-PORT monthly sales / redemptions / reinvestments
    AccountingObservation,            ///< shares, NAV (issuer route: disabled)
    AumObservation,                   ///< net assets, always stored with its basis
    MarketBar,                        ///< IBKR completed-session daily bar (raw inputs only)
    Reference,                        ///< identity / provenance facts
    CalculatedCreationRedemptionFlow, ///< Batch C; never a stored source fact
    RotationProxy,                    ///< Batch C; never a stored source fact
};

inline const char* measurement_kind_id(MeasurementKind k) {
    switch (k) {
        case MeasurementKind::RegulatoryReportedFlow:
            return "regulatory_reported_flow";
        case MeasurementKind::AccountingObservation:
            return "accounting_observation";
        case MeasurementKind::AumObservation:
            return "aum_observation";
        case MeasurementKind::MarketBar:
            return "market_bar";
        case MeasurementKind::Reference:
            return "reference";
        case MeasurementKind::CalculatedCreationRedemptionFlow:
            return "calculated_creation_redemption_flow";
        case MeasurementKind::RotationProxy:
            return "rotation_proxy";
    }
    return "";
}

/// True for the kinds that may be stored as a raw source observation.
inline bool measurement_kind_is_source_fact(MeasurementKind k) {
    return k != MeasurementKind::CalculatedCreationRedemptionFlow && k != MeasurementKind::RotationProxy;
}

// ── Source type and acquisition mode (A2 section 10) ─────────────────────────
enum class SourceType {
    SecNport,        ///< Form N-PORT primary document (NPORT-P, NPORT-P/A)
    SecSubmissions,  ///< EDGAR submission metadata (submissions JSON, series filing index)
    SecPeriodicXbrl, ///< 10-Q/10-K XBRL facts: candidates only (D7), not ingested
    IssuerFile,      ///< issuer files: route disabled (D1-d)
    IbkrTwsReadonly, ///< the existing read-only IBKR TWS wrapper
};

inline const char* source_type_id(SourceType s) {
    switch (s) {
        case SourceType::SecNport:
            return "sec_nport";
        case SourceType::SecSubmissions:
            return "sec_submissions";
        case SourceType::SecPeriodicXbrl:
            return "sec_periodic_xbrl";
        case SourceType::IssuerFile:
            return "issuer_file";
        case SourceType::IbkrTwsReadonly:
            return "ibkr_tws_readonly";
    }
    return "";
}

enum class AcquisitionMode {
    RegulatoryApi,       ///< SEC EDGAR, keyless, declared User-Agent: enabled
    IbkrReadonlyWrapper, ///< existing MarketLab IBKR wrapper: enabled
    UserInitiatedImport, ///< defined, disabled per issuer until a permission basis exists (D1-d)
    IssuerAutomated,     ///< disabled, not permitted
};

inline const char* acquisition_mode_id(AcquisitionMode m) {
    switch (m) {
        case AcquisitionMode::RegulatoryApi:
            return "regulatory_api";
        case AcquisitionMode::IbkrReadonlyWrapper:
            return "ibkr_readonly_wrapper";
        case AcquisitionMode::UserInitiatedImport:
            return "user_initiated_import";
        case AcquisitionMode::IssuerAutomated:
            return "issuer_automated";
    }
    return "";
}

// ── Timing (A2 sections 5.1 and 5.2) ─────────────────────────────────────────
enum class HistoryType {
    ForwardObserved,  ///< first seen by MarketLab after its observation start
    RegulatoryFiling, ///< SEC filing accepted before MarketLab's observation start
    MarketBackfill,   ///< IBKR bar of a session that closed before the observation start
    RevisedBackfill,  ///< issuer history obtained after the fact (route disabled)
};

inline const char* history_type_id(HistoryType h) {
    switch (h) {
        case HistoryType::ForwardObserved:
            return "forward_observed";
        case HistoryType::RegulatoryFiling:
            return "regulatory_filing";
        case HistoryType::MarketBackfill:
            return "market_backfill";
        case HistoryType::RevisedBackfill:
            return "revised_backfill";
    }
    return "";
}

/// Replaces a yes/no point-in-time flag (A2 review finding 3).
enum class PointInTimeStatus {
    Observed,             ///< recorded by MarketLab; available_from never precedes first_seen_at
    ConservativeRule,     ///< available_from from an exact source time by a named MarketLab rule
    HistoricalAssumption, ///< available_from rests on a market-structure assumption (IBKR backfill)
    NotPointInTime,       ///< no availability time can be assigned
};

inline const char* point_in_time_status_id(PointInTimeStatus p) {
    switch (p) {
        case PointInTimeStatus::Observed:
            return "observed";
        case PointInTimeStatus::ConservativeRule:
            return "conservative_rule";
        case PointInTimeStatus::HistoricalAssumption:
            return "historical_assumption";
        case PointInTimeStatus::NotPointInTime:
            return "not_point_in_time";
    }
    return "";
}

/// How `available_from` was derived. Every value except None names either the
/// recorded first sighting or a versioned rule/assumption from A2 section 5.
enum class AvailabilityBasis {
    RecordedFirstSeen,                   ///< available_from = first_seen_at
    RecordedFirstSeenAndIbkrNextSession, ///< later of first_seen_at and ibkr_next_session_v1
    SecNextSessionAfterAcceptance,       ///< sec_next_session_after_acceptance_v1
    SessionCloseAssumption,              ///< IBKR backfill: public at the session close
    None,                                ///< not computable (outside calendar coverage)
};

inline const char* availability_basis_id(AvailabilityBasis b) {
    switch (b) {
        case AvailabilityBasis::RecordedFirstSeen:
            return "recorded_first_seen";
        case AvailabilityBasis::RecordedFirstSeenAndIbkrNextSession:
            return "recorded_first_seen_and_ibkr_next_session_v1";
        case AvailabilityBasis::SecNextSessionAfterAcceptance:
            return "sec_next_session_after_acceptance_v1";
        case AvailabilityBasis::SessionCloseAssumption:
            return "session_close_assumption";
        case AvailabilityBasis::None:
            return "none";
    }
    return "";
}

// ── Revision state (A2 section 10) ───────────────────────────────────────────
enum class RevisionState {
    Original,      ///< first vintage of an IBKR key, or an NPORT-P filing
    Revised,       ///< the same source re-delivered a different value
    AmendedFiling, ///< an NPORT-P/A: a separate filing with its own acceptance time
    SplitRestated, ///< issuer split restatement (issuer route disabled; never produced)
};

inline const char* revision_state_id(RevisionState r) {
    switch (r) {
        case RevisionState::Original:
            return "original";
        case RevisionState::Revised:
            return "revised";
        case RevisionState::AmendedFiling:
            return "amended_filing";
        case RevisionState::SplitRestated:
            return "split_restated";
    }
    return "";
}

// ── Value state ──────────────────────────────────────────────────────────────
enum class ValueState {
    Reported,    ///< the source supplied a number
    Missing,     ///< the source did not supply the field at all
    Unparseable, ///< the source supplied something that is not a number (raw text kept)
};

inline const char* value_state_id(ValueState v) {
    switch (v) {
        case ValueState::Reported:
            return "reported";
        case ValueState::Missing:
            return "missing";
        case ValueState::Unparseable:
            return "unparseable";
    }
    return "";
}

// ── Quality and operational states (strategy section 8 + A2 section 10) ──────
// One vocabulary for every level. Each table stores the subset that applies to
// it (CHECK-constrained in v052); the read model derives CONFIRMED / REVISED /
// MISSING for a selected vintage.
enum class QualityState {
    Confirmed,
    Calculated,
    Proxy,
    Missing,
    Stale,
    Revised,
    RouteDisabled,
    NotApplicable,
    InProgressSession,
    NotEntitled,
    SourceError,
    ReconciliationException,
};

inline const char* quality_state_id(QualityState q) {
    switch (q) {
        case QualityState::Confirmed:
            return "CONFIRMED";
        case QualityState::Calculated:
            return "CALCULATED";
        case QualityState::Proxy:
            return "PROXY";
        case QualityState::Missing:
            return "MISSING";
        case QualityState::Stale:
            return "STALE";
        case QualityState::Revised:
            return "REVISED";
        case QualityState::RouteDisabled:
            return "ROUTE_DISABLED";
        case QualityState::NotApplicable:
            return "NOT_APPLICABLE";
        case QualityState::InProgressSession:
            return "IN_PROGRESS_SESSION";
        case QualityState::NotEntitled:
            return "NOT_ENTITLED";
        case QualityState::SourceError:
            return "SOURCE_ERROR";
        case QualityState::ReconciliationException:
            return "RECONCILIATION_EXCEPTION";
    }
    return "";
}

/// The outcome of one retrieval (one request, or one refused request).
enum class RetrievalStatus {
    Ok,            ///< the response was usable
    Stale,         ///< usable history that ends before the last completed session, or the wrapper's own stale rule
    SourceError,   ///< transport failure, HTTP error, malformed response, identity mismatch, IBKR error
    NotEntitled,   ///< IBKR entitlement block
    RouteDisabled, ///< refused before any request: the acquisition route is disabled
    NotConfigured, ///< refused before any request: no declared SEC User-Agent / no IBKR configuration
};

inline const char* retrieval_status_id(RetrievalStatus s) {
    switch (s) {
        case RetrievalStatus::Ok:
            return "OK";
        case RetrievalStatus::Stale:
            return "STALE";
        case RetrievalStatus::SourceError:
            return "SOURCE_ERROR";
        case RetrievalStatus::NotEntitled:
            return "NOT_ENTITLED";
        case RetrievalStatus::RouteDisabled:
            return "ROUTE_DISABLED";
        case RetrievalStatus::NotConfigured:
            return "NOT_CONFIGURED";
    }
    return "";
}

// ── Identity ─────────────────────────────────────────────────────────────────
/// What a stored observation describes. SEC regulatory facts describe the
/// entity that reports them (a registrant, or a series of one); market bars
/// describe the listed instrument IBKR trades. They are never the same row.
enum class SubjectType { ReportingEntity, ListedInstrument };

inline const char* subject_type_id(SubjectType s) {
    return s == SubjectType::ReportingEntity ? "reporting_entity" : "listed_instrument";
}

/// How a listed instrument relates to the SEC entity whose flows are filed.
/// The relationship decides whether a series-level N-PORT flow is also the
/// listed ETF's flow: it is for a unit investment trust that reports without a
/// series (SPY) and for a series with one share class (IVV, QQQ today); it is
/// not for an ETF share class of a multi-class series (Vanguard).
enum class LinkRelationship {
    RegistrantIsInstrument,  ///< the registrant reports without a series and is this one listed ETF
    SoleClassOfSeries,       ///< the series reports exactly one share class, this ETF
    ClassOfMultiClassSeries, ///< the ETF is one class of a multi-class series
};

inline const char* link_relationship_id(LinkRelationship r) {
    switch (r) {
        case LinkRelationship::RegistrantIsInstrument:
            return "registrant_is_instrument";
        case LinkRelationship::SoleClassOfSeries:
            return "sole_class_of_series";
        case LinkRelationship::ClassOfMultiClassSeries:
            return "class_of_multi_class_series";
    }
    return "";
}

inline std::optional<LinkRelationship> link_relationship_from_id(const QString& id) {
    for (auto r : {LinkRelationship::RegistrantIsInstrument, LinkRelationship::SoleClassOfSeries,
                   LinkRelationship::ClassOfMultiClassSeries}) {
        if (id == QLatin1String(link_relationship_id(r)))
            return r;
    }
    return std::nullopt;
}

// ── A value as the source delivered it ──────────────────────────────────────
struct FieldValue {
    ValueState state = ValueState::Missing;
    double value = 0.0; ///< meaningful only when state == Reported
    QString raw;        ///< the source token exactly as received (empty when Missing)

    bool reported() const { return state == ValueState::Reported; }

    static FieldValue missing() { return {}; }

    static FieldValue reported_value(double v, const QString& raw_text = QString()) {
        FieldValue f;
        f.state = ValueState::Reported;
        f.value = v;
        f.raw = raw_text;
        return f;
    }

    static FieldValue unparseable(const QString& raw_text) {
        FieldValue f;
        f.state = ValueState::Unparseable;
        f.raw = raw_text;
        return f;
    }

    /// Two values are the same observation when their state and number agree.
    /// The raw token is provenance and is not compared: "100.0" and "100.00"
    /// are one value, and two different unparseable tokens are both unknown.
    bool same_value(const FieldValue& other) const {
        if (state != other.state)
            return false;
        return state != ValueState::Reported || value == other.value;
    }
};

/// Parse a decimal token as the SEC writes it ("121393713302.60000000",
/// "-0.96", "1.5E+3"). `present == false` means the element or attribute was
/// absent (Missing). A present token that is empty or not a finite decimal is
/// Unparseable and keeps its raw text; it never becomes zero.
inline FieldValue parse_decimal_field(const QString& token, bool present) {
    if (!present)
        return FieldValue::missing();
    static const QRegularExpression kDecimal(QStringLiteral("^[+-]?(\\d+(\\.\\d*)?|\\.\\d+)([eE][+-]?\\d+)?$"));
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty() || !kDecimal.match(trimmed).hasMatch())
        return FieldValue::unparseable(token);
    bool ok = false;
    const double v = trimmed.toDouble(&ok);
    if (!ok || !std::isfinite(v))
        return FieldValue::unparseable(token);
    return FieldValue::reported_value(v, token);
}

} // namespace fincept::services::etf
