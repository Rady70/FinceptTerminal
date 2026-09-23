// src/services/economics/CftcInterpretationModel.h
//
// Batch 4A: the UI-independent deterministic COT descriptive interpretation
// engine governed by docs/CFTC_DESCRIPTIVE_INTERPRETATION_PLAN.md in
// Rady70/Market_Lab.
//
// The engine converts official CFTC observations and optional qualified price
// observations into predefined composable descriptive states describing what
// the reported positioning is doing: current exposure, historical relative
// position, exact 1/4/13-report gross and net flows, sustained repositioning,
// extreme persistence/exit/unwind, Open Interest context, market-level
// concentration and contemporaneous price-versus-positioning alignment or
// divergence.
//
// It deliberately produces no BUY/HOLD/SELL, no bullish/bearish score, no
// predictive confidence, no expected return, no trading recommendation and no
// AI/LLM output. It is plain deterministic software: no network, no broker
// authority, no widgets, and no dependency on the historical Batch 2/3
// directional BUY/HOLD/SELL experiment. That experiment's executable
// implementation was removed from current main by the Batch 2/3 cleanup batch
// and remains preserved in Git history and the Rady70/Market_Lab control
// evidence.
//
// Truthfulness rules carried over from the finalized metric foundation:
//   * the input is fail-closed on provenance: the report basis must be named,
//     the source family code must match the declared family, every observation
//     must carry the family's participant slot count, and the non-empty CFTC
//     contract-market code must be uniform across the history (the display
//     market name may legitimately change and stays descriptive metadata);
//   * missing is not zero and unavailable is not neutral;
//   * a missing participant leg never becomes a fabricated net;
//   * normalized flows require a present, positive prior Open Interest;
//   * an exact 1/4/13-report horizon requires an unbroken weekly report
//     sequence, so a gap makes the horizon unavailable instead of relabelling
//     a longer elapsed interval;
//   * the 156-report percentile and move-materiality references are strictly
//     trailing: the current observation never contributes to the distribution
//     that judges it;
//   * missing or unsuitable price data leaves the price relationship
//     unavailable, never flat;
//   * Legacy, Disaggregated and TFF histories and participant identities are
//     never mixed, and TFF is never reconstructed into a Commercial/Speculator
//     split.
//
// Participant display labels come from the finalized Batch 1 participant table
// and are translated there; the engine's locale-independent identity is the
// stable `participant_key`, and no state or reason depends on a label string.
#pragma once

#include "services/economics/CftcMetricModel.h"

#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <optional>

namespace fincept::services {

// ── Rule-set identity and versioned configuration ───────────────────────────

/// The versioned descriptive rule-set identity for the default v1
/// configuration. Any change to a rule, threshold or reference window requires
/// a new version string; a caller-supplied configuration that differs from the
/// v1 defaults is reported under the distinct custom identity below.
inline QString cftc_interpretation_rule_set_version() {
    return QStringLiteral("cftc-descriptive-interpretation-v1");
}

/// The truthful identity for interpretations produced with a configuration
/// that differs from the v1 defaults. It is never presented as plain v1.
inline QString cftc_interpretation_custom_rule_set_version() {
    return QStringLiteral("cftc-descriptive-interpretation-v1-custom");
}

/// The versioned heuristic configuration. Every numeric cutoff here is an
/// ENGINE_HEURISTIC per the governing plan, not a CFTC definition. Callers
/// normally use the defaults; tests pin exact boundaries by lowering
/// history_window while keeping the default percentile thresholds.
///
/// The low-side bands are stored as their own literals (0.10 / 0.025 / 0.25)
/// rather than derived as 1 - high, so the boundary comparisons are exact
/// decimal values instead of floating-point subtraction results.
struct CftcInterpretationConfig {
    int history_window = 156;                     // HISTORY_WINDOW
    double extreme_percentile = 0.90;             // EXTREME_PERCENTILE
    double low_extreme_percentile = 0.10;         // HISTORICALLY_LOW_NET / CROWDED_SHORT band
    double severe_extreme_percentile = 0.975;     // SEVERE_EXTREME_PERCENTILE
    double low_severe_extreme_percentile = 0.025; // SEVERE_SHORT_EXTREME band
    double unwind_reentry_percentile = 0.75;      // UNWIND_REENTRY_PERCENTILE
    double low_unwind_reentry_percentile = 0.25;  // UNWINDING_LOW_EXTREME re-entry
    int persistent_extreme_reports = 3;           // PERSISTENT_EXTREME_REPORTS
    double material_move_percentile = 0.75;       // MATERIAL_MOVE_PERCENTILE
    double large_move_percentile = 0.90;          // LARGE_MOVE_PERCENTILE
    /// The flow, Open Interest and price horizons. Only the plan's supported
    /// report horizons (1, 4 and 13) are interpreted; other values are dropped
    /// from the effective configuration. Sustained repositioning has its own
    /// fixed 4- and 13-report taxonomy (plan section 8.4) and is always
    /// evaluated for those two horizons regardless of this list.
    QVector<int> horizons_reports = {1, 4, 13};
    /// The market concentration field the concentration states classify, as
    /// retained on CftcObservation. The standard CFTC CR4/CR8 fields are
    /// market/report properties; this selection never maps them onto a
    /// participant category.
    QString primary_concentration_field = QStringLiteral("concentration_gross_4_long");
};

/// The exact v1 default configuration.
inline CftcInterpretationConfig cftc_default_interpretation_config() {
    return {};
}

/// Whether a configuration carries the immutable v1 defaults. A result
/// produced under any other configuration is reported under
/// cftc_interpretation_custom_rule_set_version(), never as plain v1.
inline bool cftc_interpretation_config_is_v1(const CftcInterpretationConfig& config) {
    const CftcInterpretationConfig defaults;
    return config.history_window == defaults.history_window &&
           config.extreme_percentile == defaults.extreme_percentile &&
           config.low_extreme_percentile == defaults.low_extreme_percentile &&
           config.severe_extreme_percentile == defaults.severe_extreme_percentile &&
           config.low_severe_extreme_percentile == defaults.low_severe_extreme_percentile &&
           config.unwind_reentry_percentile == defaults.unwind_reentry_percentile &&
           config.low_unwind_reentry_percentile == defaults.low_unwind_reentry_percentile &&
           config.persistent_extreme_reports == defaults.persistent_extreme_reports &&
           config.material_move_percentile == defaults.material_move_percentile &&
           config.large_move_percentile == defaults.large_move_percentile &&
           config.horizons_reports == defaults.horizons_reports &&
           config.primary_concentration_field == defaults.primary_concentration_field;
}

/// The rule-set identity that truthfully describes a configuration.
inline QString cftc_interpretation_rule_set_version_for(const CftcInterpretationConfig& config) {
    return cftc_interpretation_config_is_v1(config) ? cftc_interpretation_rule_set_version()
                                                    : cftc_interpretation_custom_rule_set_version();
}

/// The only exact report horizons the descriptive taxonomy interprets.
inline bool cftc_is_supported_interpretation_horizon(int horizon_reports) {
    return horizon_reports == 1 || horizon_reports == 4 || horizon_reports == 13;
}

/// The effective v1 configuration: requested values with unsupported horizons
/// dropped and duplicates removed.
inline CftcInterpretationConfig cftc_effective_interpretation_config(const CftcInterpretationConfig& requested) {
    CftcInterpretationConfig effective = requested;
    QVector<int> horizons;
    for (int horizon : requested.horizons_reports) {
        if (cftc_is_supported_interpretation_horizon(horizon) && !horizons.contains(horizon))
            horizons.append(horizon);
    }
    effective.horizons_reports = horizons;
    return effective;
}

inline double cftc_unwind_low_reentry_percentile(const CftcInterpretationConfig& config) {
    return config.low_unwind_reentry_percentile;
}

inline double cftc_low_extreme_percentile(const CftcInterpretationConfig& config) {
    return config.low_extreme_percentile;
}

inline double cftc_low_severe_percentile(const CftcInterpretationConfig& config) {
    return config.low_severe_extreme_percentile;
}

/// Numerical tolerance for the reported-long-equals-reported-short NET_FLAT
/// rule. CFTC positions are whole contracts, so this only absorbs
/// representation noise.
inline constexpr double kCftcFlatPositionTolerance = 1e-9;

// ── Evidence basis and interpretation scope ────────────────────────────────

/// The kind of basis a rule or state has. The governing plan requires this
/// distinction so software-selected thresholds are never presented as CFTC
/// definitions.
enum class CftcEvidenceBasis {
    CftcDefined,
    DerivedIdentity,
    LiteratureSupportedMeasure,
    PractitionerConvention,
    EngineHeuristic,
};

inline QString cftc_evidence_basis_code(CftcEvidenceBasis basis) {
    switch (basis) {
        case CftcEvidenceBasis::CftcDefined:
            return QStringLiteral("cftc_defined");
        case CftcEvidenceBasis::DerivedIdentity:
            return QStringLiteral("derived_identity");
        case CftcEvidenceBasis::LiteratureSupportedMeasure:
            return QStringLiteral("literature_supported_measure");
        case CftcEvidenceBasis::PractitionerConvention:
            return QStringLiteral("practitioner_convention");
        case CftcEvidenceBasis::EngineHeuristic:
            return QStringLiteral("engine_heuristic");
    }
    return {};
}

/// What kind of statement the state makes. FORECAST, SIGNAL and
/// EXPECTED_RETURN are deliberately not members.
enum class CftcInterpretationScope {
    AccountingFact,
    HistoricalRelativeState,
    DescriptiveFlow,
    MarketStructure,
    ContemporaneousRelation,
};

inline QString cftc_interpretation_scope_code(CftcInterpretationScope scope) {
    switch (scope) {
        case CftcInterpretationScope::AccountingFact:
            return QStringLiteral("accounting_fact");
        case CftcInterpretationScope::HistoricalRelativeState:
            return QStringLiteral("historical_relative_state");
        case CftcInterpretationScope::DescriptiveFlow:
            return QStringLiteral("descriptive_flow");
        case CftcInterpretationScope::MarketStructure:
            return QStringLiteral("market_structure");
        case CftcInterpretationScope::ContemporaneousRelation:
            return QStringLiteral("contemporaneous_relation");
    }
    return {};
}

/// Why a state family could not be evaluated. `None` means the state was
/// established.
enum class CftcUnavailableReason {
    None,
    NoObservations,
    StaleCurrentReport,
    MissingParticipantLeg,
    MissingOpenInterest,
    NonPositiveOpenInterest,
    InsufficientHistory,
    BrokenReportSequence,
    MissingPriceContext,
    UnspecifiedPriceSource,
    PriceHistoryIncomplete,
    PriceContextStale,
    PriceContextUnusable,
    MissingConcentrationField,
    MissingContractIdentity,
    MixedContractIdentity,
    ParticipantCountMismatch,
    FamilyProvenanceMismatch,
    ReportBasisUnspecified,
};

inline QString cftc_unavailable_reason_code(CftcUnavailableReason reason) {
    switch (reason) {
        case CftcUnavailableReason::None:
            return QStringLiteral("none");
        case CftcUnavailableReason::NoObservations:
            return QStringLiteral("no_observations");
        case CftcUnavailableReason::StaleCurrentReport:
            return QStringLiteral("stale_current_report");
        case CftcUnavailableReason::MissingParticipantLeg:
            return QStringLiteral("missing_participant_leg");
        case CftcUnavailableReason::MissingOpenInterest:
            return QStringLiteral("missing_open_interest");
        case CftcUnavailableReason::NonPositiveOpenInterest:
            return QStringLiteral("non_positive_open_interest");
        case CftcUnavailableReason::InsufficientHistory:
            return QStringLiteral("insufficient_history");
        case CftcUnavailableReason::BrokenReportSequence:
            return QStringLiteral("broken_report_sequence");
        case CftcUnavailableReason::MissingPriceContext:
            return QStringLiteral("missing_price_context");
        case CftcUnavailableReason::UnspecifiedPriceSource:
            return QStringLiteral("unspecified_price_source");
        case CftcUnavailableReason::PriceHistoryIncomplete:
            return QStringLiteral("price_history_incomplete");
        case CftcUnavailableReason::PriceContextStale:
            return QStringLiteral("price_context_stale");
        case CftcUnavailableReason::PriceContextUnusable:
            return QStringLiteral("price_context_unusable");
        case CftcUnavailableReason::MissingConcentrationField:
            return QStringLiteral("missing_concentration_field");
        case CftcUnavailableReason::MissingContractIdentity:
            return QStringLiteral("missing_contract_identity");
        case CftcUnavailableReason::MixedContractIdentity:
            return QStringLiteral("mixed_contract_identity");
        case CftcUnavailableReason::ParticipantCountMismatch:
            return QStringLiteral("participant_count_mismatch");
        case CftcUnavailableReason::FamilyProvenanceMismatch:
            return QStringLiteral("family_provenance_mismatch");
        case CftcUnavailableReason::ReportBasisUnspecified:
            return QStringLiteral("report_basis_unspecified");
    }
    return {};
}

// ── Participant terminology semantics ──────────────────────────────────────

/// The category-specific terminology layer the later UI uses. It exists so
/// crowding wording is never applied to a category the governing plan keeps
/// neutral, and so TFF is never relabelled into a Commercial/Speculator split.
enum class CftcTerminologyClass {
    BroadNonCommercial,
    CommercialNeutral,
    ManagedMoney,
    ProducerMerchant,
    SwapDealer,
    OtherReportableNeutral,
    NonReportableNeutral,
    LeveragedFunds,
    AssetManagerInstitutional,
    DealerIntermediary,
    Unknown,
};

inline QString cftc_terminology_code(CftcTerminologyClass terminology) {
    switch (terminology) {
        case CftcTerminologyClass::BroadNonCommercial:
            return QStringLiteral("broad_non_commercial");
        case CftcTerminologyClass::CommercialNeutral:
            return QStringLiteral("commercial_neutral");
        case CftcTerminologyClass::ManagedMoney:
            return QStringLiteral("managed_money");
        case CftcTerminologyClass::ProducerMerchant:
            return QStringLiteral("producer_merchant");
        case CftcTerminologyClass::SwapDealer:
            return QStringLiteral("swap_dealer");
        case CftcTerminologyClass::OtherReportableNeutral:
            return QStringLiteral("other_reportable_neutral");
        case CftcTerminologyClass::NonReportableNeutral:
            return QStringLiteral("non_reportable_neutral");
        case CftcTerminologyClass::LeveragedFunds:
            return QStringLiteral("leveraged_funds");
        case CftcTerminologyClass::AssetManagerInstitutional:
            return QStringLiteral("asset_manager_institutional");
        case CftcTerminologyClass::DealerIntermediary:
            return QStringLiteral("dealer_intermediary");
        case CftcTerminologyClass::Unknown:
            return QStringLiteral("unknown");
    }
    return {};
}

/// Map one actual CFTC participant class to its neutral terminology. A key
/// that does not belong to the family is Unknown rather than being silently
/// reassigned to another family's category.
inline CftcTerminologyClass cftc_participant_terminology(CftcFamily family, const QString& participant_key) {
    if (family == CftcFamily::Legacy) {
        if (participant_key == QLatin1String("non_commercial"))
            return CftcTerminologyClass::BroadNonCommercial;
        if (participant_key == QLatin1String("commercial"))
            return CftcTerminologyClass::CommercialNeutral;
        if (participant_key == QLatin1String("non_reportable"))
            return CftcTerminologyClass::NonReportableNeutral;
        return CftcTerminologyClass::Unknown;
    }
    if (family == CftcFamily::Disaggregated) {
        if (participant_key == QLatin1String("managed_money"))
            return CftcTerminologyClass::ManagedMoney;
        if (participant_key == QLatin1String("producer_merchant"))
            return CftcTerminologyClass::ProducerMerchant;
        if (participant_key == QLatin1String("swap_dealer"))
            return CftcTerminologyClass::SwapDealer;
        if (participant_key == QLatin1String("other_reportable"))
            return CftcTerminologyClass::OtherReportableNeutral;
        if (participant_key == QLatin1String("non_reportable"))
            return CftcTerminologyClass::NonReportableNeutral;
        return CftcTerminologyClass::Unknown;
    }
    if (family == CftcFamily::Tff) {
        if (participant_key == QLatin1String("leveraged_funds"))
            return CftcTerminologyClass::LeveragedFunds;
        if (participant_key == QLatin1String("asset_manager"))
            return CftcTerminologyClass::AssetManagerInstitutional;
        if (participant_key == QLatin1String("dealer"))
            return CftcTerminologyClass::DealerIntermediary;
        if (participant_key == QLatin1String("other_reportable"))
            return CftcTerminologyClass::OtherReportableNeutral;
        if (participant_key == QLatin1String("non_reportable"))
            return CftcTerminologyClass::NonReportableNeutral;
        return CftcTerminologyClass::Unknown;
    }
    return CftcTerminologyClass::Unknown;
}

/// Only the broad non-commercial class, managed money and leveraged funds may
/// use "crowded long/short" wording. Everything else stays neutral, and
/// "crowded" never implies leverage, overbought/oversold, a squeeze or an
/// expected reversal.
inline bool cftc_crowding_terminology_allowed(CftcFamily family, const QString& participant_key) {
    const CftcTerminologyClass terminology = cftc_participant_terminology(family, participant_key);
    return terminology == CftcTerminologyClass::BroadNonCommercial ||
           terminology == CftcTerminologyClass::ManagedMoney || terminology == CftcTerminologyClass::LeveragedFunds;
}

// ── Structured result contract ──────────────────────────────────────────────

/// One actual numeric value used by a state. Absence is structural
/// (`has_value == false`), never a zero substituted for a missing reading.
struct CftcStateMetric {
    QString key;
    bool has_value = false;
    double value = 0.0;
};

inline CftcStateMetric cftc_state_metric(const QString& key, const std::optional<double>& value) {
    CftcStateMetric out;
    out.key = key;
    out.has_value = value.has_value();
    if (value)
        out.value = *value;
    return out;
}

/// One predefined descriptive state. Batch 4A emits only established states
/// (`available == true`); a state family that could not be evaluated appears in
/// the result's `unavailable` records instead. The availability/reason fields
/// stay on the state so a later presentation layer can consume either form.
struct CftcInterpretationState {
    QString state_id;
    QString participant_key;
    bool has_horizon = false;
    int horizon_reports = 0;
    CftcInterpretationScope scope = CftcInterpretationScope::AccountingFact;
    CftcEvidenceBasis threshold_basis = CftcEvidenceBasis::EngineHeuristic;
    bool available = true;
    CftcUnavailableReason reason = CftcUnavailableReason::None;
    QVector<CftcStateMetric> metrics;
    bool has_percentile = false;
    double percentile = 0.0;
    int percentile_reference_count = 0;
    bool has_move_rank = false;
    double move_rank = 0.0;
    int move_rank_reference_count = 0;
    bool move_large = false;
    /// For net-shift, sustained and price-relationship states: the gross-leg
    /// state ids that fired at the same horizon and describe the mechanism
    /// behind the net move. Empty when no gross leg was itself material.
    QStringList mechanism_state_ids;
};

/// The unavailable counterpart of a state family, with the same identity
/// fields and the explicit reason the family could not be evaluated. When a
/// single taxonomy state is the blocked one, `state_id` names it; otherwise the
/// record describes the family.
struct CftcUnavailableRecord {
    QString state_family;
    QString state_id;
    QString participant_key; // empty for market-level families
    bool has_horizon = false;
    int horizon_reports = 0;
    CftcInterpretationScope scope = CftcInterpretationScope::AccountingFact;
    CftcEvidenceBasis threshold_basis = CftcEvidenceBasis::EngineHeuristic;
    CftcUnavailableReason reason = CftcUnavailableReason::None;
};

inline CftcUnavailableRecord cftc_unavailable_record(const QString& state_family, const QString& participant_key,
                                                     bool has_horizon, int horizon_reports,
                                                     CftcInterpretationScope scope, CftcEvidenceBasis threshold_basis,
                                                     CftcUnavailableReason reason,
                                                     const QString& state_id = QString()) {
    CftcUnavailableRecord out;
    out.state_family = state_family;
    out.state_id = state_id;
    out.participant_key = participant_key;
    out.has_horizon = has_horizon;
    out.horizon_reports = horizon_reports;
    out.scope = scope;
    out.threshold_basis = threshold_basis;
    out.reason = reason;
    return out;
}

/// The raw normalized flow measurements for one participant and one report
/// horizon. Every configured horizon emits one reading regardless of
/// materiality, so a valid below-threshold observation stays inspectable
/// instead of being replaced by an absent value. The readings are measurements,
/// not conclusions: the states remain the only classification. `evaluated` is
/// false when the horizon itself could not be formed, and the values are then
/// absent rather than zeroed.
struct CftcHorizonFlowReading {
    int horizon_reports = 0;
    bool evaluated = false;
    CftcUnavailableReason reason = CftcUnavailableReason::None;
    bool has_long_flow = false;
    double long_flow = 0.0;
    bool has_short_flow = false;
    double short_flow = 0.0;
    bool has_net_flow = false;
    double net_flow = 0.0;
    bool has_long_rank = false;
    double long_rank = 0.0;
    bool has_short_rank = false;
    double short_rank = 0.0;
    bool has_net_rank = false;
    double net_rank = 0.0;
    int net_rank_reference_count = 0;
};

/// One participant's descriptive interpretation: its neutral terminology and
/// the established states. A participant the latest report did not carry keeps
/// explicit availability flags instead of zeros.
struct CftcParticipantInterpretation {
    QString participant_key;
    QString label;
    CftcTerminologyClass terminology = CftcTerminologyClass::Unknown;
    QString terminology_code;
    /// Category caveats the presentation layer must carry when it uses
    /// crowding wording: broad-category for Legacy Non-Commercial, and the
    /// "not all outright speculation" caveat for TFF Leveraged Funds.
    QString terminology_caveat_code;
    bool crowding_terminology_allowed = false;
    bool net_available = false;
    double net_position = 0.0;
    bool has_net_pct_oi = false;
    double net_pct_oi = 0.0;
    bool historical_percentile_available = false;
    double percentile = 0.0;
    int percentile_reference_count = 0;
    QVector<CftcInterpretationState> states;
    /// Raw per-horizon flow measurements emitted for every configured horizon,
    /// including horizons whose move stayed below the materiality threshold.
    QVector<CftcHorizonFlowReading> flow_readings;
};

/// Price-versus-positioning assessment for one participant and one horizon.
/// The relationship states are contemporaneous descriptions only; divergence
/// never claims a reversal. The scope/basis fields mirror
/// CftcInterpretationState so the presentation layer can consume both record
/// kinds uniformly.
struct CftcPricePositionAssessment {
    QString participant_key;
    int horizon_reports = 0;
    CftcInterpretationScope scope = CftcInterpretationScope::ContemporaneousRelation;
    CftcEvidenceBasis threshold_basis = CftcEvidenceBasis::EngineHeuristic;
    bool evaluated = false;
    CftcUnavailableReason reason = CftcUnavailableReason::None;
    bool has_price_move = false;
    double price_move = 0.0;
    QDate price_anchor_date;
    QDate price_latest_date;
    bool has_price_move_rank = false;
    double price_move_rank = 0.0;
    int price_reference_count = 0;
    bool price_material = false;
    bool price_move_large = false;
    bool has_positioning_move = false;
    double positioning_move = 0.0;
    bool has_positioning_move_rank = false;
    double positioning_move_rank = 0.0;
    int positioning_reference_count = 0;
    bool positioning_material = false;
    bool positioning_move_large = false;
    QString state_id; // empty when both sides were evaluated but no state holds
    bool has_state = false;
    QStringList mechanism_state_ids;
};

/// Market-level concentration assessment for one retained CFTC concentration
/// field. Concentration is a market/report property and is never assigned to
/// a participant category.
struct CftcConcentrationAssessment {
    QString field_key;
    bool has_current = false;
    double current = 0.0;
    bool has_change = false;
    double change = 0.0;
    /// Why the weekly change could not be formed: the previous report is not a
    /// weekly neighbour, the previous report does not carry the field, or the
    /// history is too short.
    CftcUnavailableReason change_unavailable_reason = CftcUnavailableReason::None;
    bool has_percentile = false;
    double percentile = 0.0;
    int percentile_reference_count = 0;
    bool has_change_rank = false;
    double change_rank = 0.0;
    int change_reference_count = 0;
};

struct CftcInterpretationInput {
    CftcFamily family = CftcFamily::Legacy;
    /// The exact family code the observation rows were parsed with
    /// ("legacy" | "disaggregated" | "tff"). Required: a mismatch with the
    /// declared family, or an unknown/empty code, fails closed because
    /// Disaggregated and TFF share the same five participant slots and cannot
    /// be told apart from the vectors alone.
    QString observations_family_code;
    QVector<CftcObservation> observations; // ascending official report order
    /// The official report basis the rows came from: "futures_only" or
    /// "futures_and_options_combined". Required and validated; it has no
    /// silent default because a mixed basis would change every normalized
    /// metric.
    QString report_basis_code;
    QDate as_of;                    // optional explicit current report
    QVector<CftcPricePoint> prices; // ascending closes; empty = no price context
    QString price_source;           // explicit provider/proxy description
    bool price_continuous_proxy = false;
    bool price_spot_index = false;
    CftcInterpretationConfig config;
};

/// The market-level Open Interest measurement for one report horizon. Emitted
/// for every configured horizon regardless of materiality, with the same
/// measurement-not-conclusion split as the participant flow readings.
struct CftcOpenInterestReading {
    int horizon_reports = 0;
    bool evaluated = false;
    CftcUnavailableReason reason = CftcUnavailableReason::None;
    bool has_oi_change = false;
    double oi_change = 0.0;
    bool has_rank = false;
    double rank = 0.0;
    int rank_reference_count = 0;
    bool material = false;
    bool move_large = false;
};

struct CftcInterpretationResult {
    QString rule_set_version;
    bool config_is_v1 = false;
    CftcFamily family = CftcFamily::Legacy;
    QString family_code;
    bool futures_only = false;
    QString report_basis; // futures_only | futures_and_options_combined
    QString market;
    QString contract_code;
    QString units;
    QDate report_date;
    bool report_date_available = false;
    bool current_report_stale = false;
    QDate latest_observation_date; // newest report the supplied history actually carried
    bool open_interest_available = false;
    double open_interest = 0.0;
    bool price_requested = false;
    QString price_source;
    bool price_continuous_proxy = false;
    bool price_spot_index = false;
    CftcInterpretationConfig config;
    QVector<CftcParticipantInterpretation> participants;
    QVector<CftcConcentrationAssessment> concentration;
    QVector<CftcInterpretationState> market_context;
    /// Raw market-level Open Interest measurements per horizon, emitted
    /// independently of whether a material state fired.
    QVector<CftcOpenInterestReading> open_interest_readings;
    /// Price relationship availability and reasons live here, one assessment
    /// per participant and horizon; the price layer is deliberately not
    /// duplicated into unavailable[] because each assessment already carries
    /// its own scope, materiality flags and explicit reason.
    QVector<CftcPricePositionAssessment> price_context;
    QVector<CftcUnavailableRecord> unavailable;
};

// ── Report continuity ───────────────────────────────────────────────────────

/// A true weekly CFTC interval: the same tolerance the finalized metric
/// foundation uses for a weekly neighbour.
inline bool cftc_weekly_neighbour(const QDate& earlier, const QDate& later) {
    if (!earlier.isValid() || !later.isValid() || earlier >= later)
        return false;
    return earlier.daysTo(later) <= kCftcWeeklyGapDays;
}

/// Whether every consecutive pair of reports in [first, last] is a weekly
/// neighbour. A missing report or a stale anchor inside the window makes the
/// window unavailable.
inline bool cftc_report_sequence_continuous(const QVector<CftcObservation>& observations, int first, int last) {
    if (first < 0 || last < 0 || first >= observations.size() || last >= observations.size() || first > last)
        return false;
    for (int i = first + 1; i <= last; ++i) {
        if (!cftc_weekly_neighbour(observations[i - 1].date, observations[i].date))
            return false;
    }
    return true;
}

// ── Strictly trailing percentile and materiality ranks ──────────────────────

/// Percentile of a value against a strictly trailing reference, with the
/// plan's midpoint-tie rule:
///   (count(reference < current) + 0.5 * count(reference == current)) / count
/// `current_present` says the series carried the report; `available` says the
/// full configured reference existed before it.
struct CftcTrailingPercentile {
    bool current_present = false;
    bool available = false;
    int reference_count = 0;
    double percentile = 0.0;
};

inline CftcTrailingPercentile cftc_trailing_percentile_at(const QVector<CftcDatedValue>& series,
                                                          const QDate& current_date, int window) {
    CftcTrailingPercentile out;
    if (window < 1 || !current_date.isValid())
        return out;
    double current = 0.0;
    bool found = false;
    QVector<double> reference;
    reference.reserve(series.size());
    for (const auto& point : series) {
        if (point.date < current_date) {
            reference.append(point.value);
        } else if (point.date == current_date) {
            current = point.value;
            found = true;
            break;
        } else {
            break; // ascending series; points after the current report are not reference
        }
    }
    if (!found)
        return out;
    out.current_present = true;
    if (reference.size() > window)
        reference = reference.mid(reference.size() - window);
    out.reference_count = reference.size();
    if (out.reference_count < window)
        return out;
    int below = 0;
    int equal = 0;
    for (double value : reference) {
        if (value < current)
            ++below;
        else if (value == current)
            ++equal;
    }
    out.available = true;
    out.percentile =
        (static_cast<double>(below) + 0.5 * static_cast<double>(equal)) / static_cast<double>(out.reference_count);
    return out;
}

/// Percentile of a value against its complete strictly trailing reference,
/// with the same midpoint-tie rule. This is the plan's "own full trailing
/// reference" form used by the market concentration state: no history window
/// truncates it. `minimum_reference` guards against a percentile computed from
/// a single reading; unavailable below the Batch 1 trailing minimum.
inline CftcTrailingPercentile cftc_full_trailing_percentile_at(const QVector<CftcDatedValue>& series,
                                                               const QDate& current_date,
                                                               int minimum_reference = kCftcTrailingMinObservations) {
    CftcTrailingPercentile out;
    if (!current_date.isValid() || minimum_reference < 1)
        return out;
    double current = 0.0;
    bool found = false;
    QVector<double> reference;
    reference.reserve(series.size());
    for (const auto& point : series) {
        if (point.date < current_date) {
            reference.append(point.value);
        } else if (point.date == current_date) {
            current = point.value;
            found = true;
            break;
        } else {
            break;
        }
    }
    if (!found)
        return out;
    out.current_present = true;
    out.reference_count = reference.size();
    if (out.reference_count < minimum_reference)
        return out;
    int below = 0;
    int equal = 0;
    for (double value : reference) {
        if (value < current)
            ++below;
        else if (value == current)
            ++equal;
    }
    out.available = true;
    out.percentile =
        (static_cast<double>(below) + 0.5 * static_cast<double>(equal)) / static_cast<double>(out.reference_count);
    return out;
}

/// Rank of the current absolute move against the strictly trailing absolute
/// moves, with the same midpoint-tie rule. The current move is identified by
/// its report date and never enters its own reference.
struct CftcTrailingMoveRank {
    bool has_current = false;
    bool available = false;
    int reference_count = 0;
    double rank = 0.0;
};

inline CftcTrailingMoveRank cftc_trailing_move_rank(const QVector<CftcDatedValue>& moves, const QDate& current_date,
                                                    int window) {
    CftcTrailingMoveRank out;
    if (window < 1 || !current_date.isValid())
        return out;
    double current = 0.0;
    bool found = false;
    QVector<double> reference;
    reference.reserve(moves.size());
    for (const auto& point : moves) {
        if (point.date < current_date) {
            reference.append(std::fabs(point.value));
        } else if (point.date == current_date) {
            current = std::fabs(point.value);
            found = true;
            break;
        } else {
            break;
        }
    }
    if (!found)
        return out;
    out.has_current = true;
    if (reference.size() > window)
        reference = reference.mid(reference.size() - window);
    out.reference_count = reference.size();
    if (out.reference_count < window)
        return out;
    int below = 0;
    int equal = 0;
    for (double value : reference) {
        if (value < current)
            ++below;
        else if (value == current)
            ++equal;
    }
    out.available = true;
    out.rank =
        (static_cast<double>(below) + 0.5 * static_cast<double>(equal)) / static_cast<double>(out.reference_count);
    return out;
}

// ── Exact report-horizon flows ──────────────────────────────────────────────

/// Exact report-count flows for one participant and horizon, measured between
/// report `current_index` and report `current_index - horizon_reports`.
/// Normalized legs divide by the anchor report's Open Interest (the prior-OI
/// denominator), and every normalized value is unavailable unless the
/// relevant Open Interest is present and positive.
struct CftcReportFlow {
    int horizon_reports = 0;
    bool anchored = false;   // both endpoint reports exist
    bool continuous = false; // every step between them is a weekly neighbour
    QDate anchor_date;
    QDate current_date;
    bool has_long_flow = false;
    double long_flow = 0.0;
    bool has_short_flow = false;
    double short_flow = 0.0;
    bool has_net_flow = false;
    double net_flow = 0.0;
    bool has_net_share_change = false;
    double net_share_change = 0.0;
    bool has_oi_change = false;
    double oi_change = 0.0;
};

inline CftcReportFlow cftc_report_flow(const QVector<CftcObservation>& observations, int participant_index,
                                       int horizon_reports, int current_index) {
    CftcReportFlow out;
    out.horizon_reports = horizon_reports;
    if (horizon_reports < 1 || current_index < 0 || current_index >= observations.size())
        return out;
    const int anchor_index = current_index - horizon_reports;
    if (anchor_index < 0)
        return out;
    out.anchored = true;
    out.anchor_date = observations[anchor_index].date;
    out.current_date = observations[current_index].date;
    if (!cftc_report_sequence_continuous(observations, anchor_index, current_index))
        return out;
    out.continuous = true;

    const CftcObservation& from = observations[anchor_index];
    const CftcObservation& to = observations[current_index];
    const bool prior_oi_positive = from.open_interest.has_value() && *from.open_interest > 0.0;

    if (participant_index >= 0) {
        const bool long_from = participant_index < from.longs.size() && from.longs[participant_index].has_value();
        const bool long_to = participant_index < to.longs.size() && to.longs[participant_index].has_value();
        const bool short_from = participant_index < from.shorts.size() && from.shorts[participant_index].has_value();
        const bool short_to = participant_index < to.shorts.size() && to.shorts[participant_index].has_value();
        const std::optional<double> net_from = cftc_participant_net(from, participant_index);
        const std::optional<double> net_to = cftc_participant_net(to, participant_index);

        if (prior_oi_positive && long_from && long_to) {
            out.has_long_flow = true;
            out.long_flow =
                100.0 * (*to.longs[participant_index] - *from.longs[participant_index]) / *from.open_interest;
        }
        if (prior_oi_positive && short_from && short_to) {
            out.has_short_flow = true;
            out.short_flow =
                100.0 * (*to.shorts[participant_index] - *from.shorts[participant_index]) / *from.open_interest;
        }
        if (prior_oi_positive && net_from && net_to) {
            out.has_net_flow = true;
            out.net_flow = 100.0 * (*net_to - *net_from) / *from.open_interest;
        }
        const CftcPositionMetrics metrics_from = cftc_position_metrics(from, participant_index);
        const CftcPositionMetrics metrics_to = cftc_position_metrics(to, participant_index);
        if (metrics_from.has_net_pct_oi && metrics_to.has_net_pct_oi) {
            out.has_net_share_change = true;
            out.net_share_change = metrics_to.net_pct_oi - metrics_from.net_pct_oi;
        }
    }

    if (from.open_interest.has_value() && *from.open_interest > 0.0 && to.open_interest.has_value() &&
        *to.open_interest > 0.0) {
        out.has_oi_change = true;
        out.oi_change = 100.0 * (*to.open_interest / *from.open_interest - 1.0);
    }
    return out;
}

/// The comparable flow metrics a move series / materiality rank can be built
/// from. NetShareChange ranks the change in Net %OI between the two reports.
enum class CftcFlowMetric { Long, Short, Net, NetShareChange, OpenInterest };

inline std::optional<double> cftc_flow_metric_value(const CftcReportFlow& flow, CftcFlowMetric metric) {
    switch (metric) {
        case CftcFlowMetric::Long:
            return flow.has_long_flow ? std::optional<double>(flow.long_flow) : std::nullopt;
        case CftcFlowMetric::Short:
            return flow.has_short_flow ? std::optional<double>(flow.short_flow) : std::nullopt;
        case CftcFlowMetric::Net:
            return flow.has_net_flow ? std::optional<double>(flow.net_flow) : std::nullopt;
        case CftcFlowMetric::NetShareChange:
            return flow.has_net_share_change ? std::optional<double>(flow.net_share_change) : std::nullopt;
        case CftcFlowMetric::OpenInterest:
            return flow.has_oi_change ? std::optional<double>(flow.oi_change) : std::nullopt;
    }
    return std::nullopt;
}

/// Every valid exact-horizon move in the supplied (already current-truncated)
/// history, one point per report that had a computable move. Reports with a
/// missing leg, unusable Open Interest or a broken sequence contribute no
/// point, never a zero.
inline QVector<CftcDatedValue> cftc_flow_move_series(const QVector<CftcObservation>& observations,
                                                     int participant_index, CftcFlowMetric metric,
                                                     int horizon_reports) {
    QVector<CftcDatedValue> out;
    for (int i = horizon_reports; i < observations.size(); ++i) {
        const CftcReportFlow flow = cftc_report_flow(observations, participant_index, horizon_reports, i);
        const std::optional<double> value = cftc_flow_metric_value(flow, metric);
        if (value)
            out.append({observations[i].date, observations[i].date_label, *value});
    }
    return out;
}

// ── Price context ───────────────────────────────────────────────────────────

/// The close used for one official report date: the most recent observation at
/// or before it, only when that observation is itself contemporaneous (within
/// the weekly tolerance) and carries a usable positive close. The reason names
/// the exact unavailability: no observation at or before the report, a
/// non-positive/unusable close, or a close too old to describe this report.
struct CftcReportPrice {
    bool available = false;
    double close = 0.0;
    QDate date;
    CftcUnavailableReason reason = CftcUnavailableReason::None;
};

inline CftcReportPrice cftc_report_price(const QVector<CftcPricePoint>& prices, const QDate& report_date) {
    CftcReportPrice out;
    if (!report_date.isValid()) {
        out.reason = CftcUnavailableReason::PriceHistoryIncomplete;
        return out;
    }
    const CftcPricePoint* found = nullptr;
    for (const auto& point : prices) {
        if (point.date <= report_date)
            found = &point;
        else
            break;
    }
    if (!found) {
        out.reason = CftcUnavailableReason::PriceHistoryIncomplete;
        return out;
    }
    if (!(found->close > 0.0) || !std::isfinite(found->close)) {
        out.reason = CftcUnavailableReason::PriceContextUnusable;
        return out;
    }
    if (found->date.daysTo(report_date) > kCftcWeeklyGapDays) {
        out.reason = CftcUnavailableReason::PriceContextStale;
        return out;
    }
    out.available = true;
    out.close = found->close;
    out.date = found->date;
    return out;
}

/// Comparable report-date-to-report-date price moves over the same continuous
/// report windows the positioning moves use.
inline QVector<CftcDatedValue> cftc_price_move_series(const QVector<CftcPricePoint>& prices,
                                                      const QVector<CftcObservation>& observations,
                                                      int horizon_reports) {
    QVector<CftcDatedValue> out;
    for (int i = horizon_reports; i < observations.size(); ++i) {
        if (!cftc_report_sequence_continuous(observations, i - horizon_reports, i))
            continue;
        const CftcReportPrice latest = cftc_report_price(prices, observations[i].date);
        const CftcReportPrice anchor = cftc_report_price(prices, observations[i - horizon_reports].date);
        if (!latest.available || !anchor.available)
            continue;
        out.append({observations[i].date, observations[i].date_label, latest.close - anchor.close});
    }
    return out;
}

// ── Market concentration fields ─────────────────────────────────────────────

inline QStringList cftc_concentration_field_keys() {
    return {QStringLiteral("concentration_gross_4_long"), QStringLiteral("concentration_gross_4_short"),
            QStringLiteral("concentration_net_4_long"),   QStringLiteral("concentration_net_4_short"),
            QStringLiteral("concentration_gross_8_long"), QStringLiteral("concentration_gross_8_short"),
            QStringLiteral("concentration_net_8_long"),   QStringLiteral("concentration_net_8_short")};
}

inline std::optional<double> cftc_concentration_value(const CftcObservation& observation, const QString& field_key) {
    if (field_key == QLatin1String("concentration_gross_4_long"))
        return observation.concentration_gross_4_long;
    if (field_key == QLatin1String("concentration_gross_4_short"))
        return observation.concentration_gross_4_short;
    if (field_key == QLatin1String("concentration_gross_8_long"))
        return observation.concentration_gross_8_long;
    if (field_key == QLatin1String("concentration_gross_8_short"))
        return observation.concentration_gross_8_short;
    if (field_key == QLatin1String("concentration_net_4_long"))
        return observation.concentration_net_4_long;
    if (field_key == QLatin1String("concentration_net_4_short"))
        return observation.concentration_net_4_short;
    if (field_key == QLatin1String("concentration_net_8_long"))
        return observation.concentration_net_8_long;
    if (field_key == QLatin1String("concentration_net_8_short"))
        return observation.concentration_net_8_short;
    return std::nullopt;
}

// ── Versioned rule catalog ──────────────────────────────────────────────────

struct CftcInterpretationRule {
    QString id;
    CftcInterpretationScope scope = CftcInterpretationScope::AccountingFact;
    CftcEvidenceBasis basis = CftcEvidenceBasis::EngineHeuristic;
    QString condition; // short formula, not UI prose
};

/// The versioned rule catalog for the v1 baseline. It records which part of
/// the interpretation is a CFTC fact, an identity, a literature measure, a
/// practitioner convention or a versioned engine heuristic. It describes the
/// default v1 configuration; when a caller overrides CftcInterpretationConfig
/// the result is reported under cftc_interpretation_custom_rule_set_version()
/// and this catalog no longer describes the effective thresholds.
inline QVector<CftcInterpretationRule> cftc_interpretation_rule_catalog() {
    return {
        {QStringLiteral("PARTICIPANT_POSITION_LEGS"), CftcInterpretationScope::AccountingFact,
         CftcEvidenceBasis::CftcDefined,
         QStringLiteral("reported long and short positions per CFTC participant class")},
        {QStringLiteral("NET_POSITION"), CftcInterpretationScope::AccountingFact, CftcEvidenceBasis::DerivedIdentity,
         QStringLiteral("net = long - short")},
        {QStringLiteral("NET_PCT_OI"), CftcInterpretationScope::AccountingFact,
         CftcEvidenceBasis::LiteratureSupportedMeasure,
         QStringLiteral("net_pct_oi = 100 * (long - short) / open_interest")},
        {QStringLiteral("CROWDED_TERMINOLOGY"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::PractitionerConvention,
         QStringLiteral("'crowded' wording only for broad non-commercial, managed money and leveraged funds")},
        {QStringLiteral("HISTORICAL_PERCENTILE"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("midpoint-tie percentile of net_pct_oi against the previous 156 valid reports")},
        {QStringLiteral("HISTORICALLY_HIGH_NET"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("percentile >= 0.90")},
        {QStringLiteral("HISTORICALLY_LOW_NET"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("percentile <= 0.10")},
        {QStringLiteral("CROWDED_LONG"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("net long and percentile >= 0.90 where crowding wording is allowed")},
        {QStringLiteral("CROWDED_SHORT"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("net short and percentile <= 0.10 where crowding wording is allowed")},
        {QStringLiteral("SEVERE_LONG_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("net long and percentile >= 0.975")},
        {QStringLiteral("SEVERE_SHORT_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("net short and percentile <= 0.025")},
        {QStringLiteral("GROSS_FLOW"), CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("1/4/13-report long/short flows material at the 0.75 move rank")},
        {QStringLiteral("NET_SHIFT"), CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("1/4/13-report net flow material at the 0.75 move rank")},
        {QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::DerivedIdentity,
         QStringLiteral("raw net flow and Net %OI change have opposite signs and neither is zero")},
        {QStringLiteral("SUSTAINED_REPOSITIONING"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("material net flow plus 3 of 4 (or 9 of 13) one-report net flows same sign")},
        {QStringLiteral("EXTREME_TRANSITION"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("extreme persistence/exit/unwind bands over consecutive weekly reports")},
        {QStringLiteral("OI_CONTEXT"), CftcInterpretationScope::MarketStructure, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("Open Interest change material at the 0.75 move rank")},
        {QStringLiteral("CONCENTRATION"), CftcInterpretationScope::MarketStructure, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("selected market CR concentration percentile >= 0.90 or rising move rank >= 0.75")},
        {QStringLiteral("PRICE_POSITION_RELATION"), CftcInterpretationScope::ContemporaneousRelation,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral(
             "price and net positioning moves both material at the same report window; divergence explicit")},
        {QStringLiteral("NET_FLAT"), CftcInterpretationScope::AccountingFact, CftcEvidenceBasis::DerivedIdentity,
         QStringLiteral("reported long equals reported short")},
        {QStringLiteral("NET_LONG"), CftcInterpretationScope::AccountingFact, CftcEvidenceBasis::DerivedIdentity,
         QStringLiteral("reported net contracts > 0")},
        {QStringLiteral("NET_SHORT"), CftcInterpretationScope::AccountingFact, CftcEvidenceBasis::DerivedIdentity,
         QStringLiteral("reported net contracts < 0")},
        {QStringLiteral("LONG_ACCUMULATION"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("1/4/13-report long flow > 0 and material")},
        {QStringLiteral("LONG_LIQUIDATION"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("1/4/13-report long flow < 0 and material")},
        {QStringLiteral("SHORT_BUILDING"), CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("1/4/13-report short flow > 0 and material")},
        {QStringLiteral("SHORT_COVERING"), CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("1/4/13-report short flow < 0 and material")},
        {QStringLiteral("NET_LONGWARD_SHIFT"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("1/4/13-report net flow > 0 and material")},
        {QStringLiteral("NET_SHORTWARD_SHIFT"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("1/4/13-report net flow < 0 and material")},
        {QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("material positive 4-report net flow and >= 3 of the last 4 one-report flows positive")},
        {QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_4R"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("material negative 4-report net flow and >= 3 of the last 4 one-report flows negative")},
        {QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("material positive 13-report net flow and >= 9 of the last 13 one-report flows positive")},
        {QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_13R"), CftcInterpretationScope::DescriptiveFlow,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("material negative 13-report net flow and >= 9 of the last 13 one-report flows negative")},
        {QStringLiteral("PERSISTENT_HIGH_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("percentile >= 0.90 for 3 consecutive reports")},
        {QStringLiteral("PERSISTENT_LOW_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("percentile <= 0.10 for 3 consecutive reports")},
        {QStringLiteral("EXITED_HIGH_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("previous percentile >= 0.90, current < 0.90 and net_pct_oi fell")},
        {QStringLiteral("EXITED_LOW_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("previous percentile <= 0.10, current > 0.10 and net_pct_oi rose")},
        {QStringLiteral("UNWINDING_HIGH_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("prior persistent high, current percentile <= 0.75 and net_pct_oi fell")},
        {QStringLiteral("UNWINDING_LOW_EXTREME"), CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("prior persistent low, current percentile >= 0.25 and net_pct_oi rose")},
        {QStringLiteral("OI_EXPANSION"), CftcInterpretationScope::MarketStructure, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("Open Interest change > 0 and material")},
        {QStringLiteral("OI_CONTRACTION"), CftcInterpretationScope::MarketStructure, CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("Open Interest change < 0 and material")},
        {QStringLiteral("HIGH_MARKET_CONCENTRATION"), CftcInterpretationScope::MarketStructure,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("selected market CR concentration percentile >= 0.90 of its trailing reference")},
        {QStringLiteral("CONCENTRATION_RISING"), CftcInterpretationScope::MarketStructure,
         CftcEvidenceBasis::EngineHeuristic,
         QStringLiteral("market concentration change > 0 and absolute move rank >= 0.75")},
        {QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), CftcInterpretationScope::ContemporaneousRelation,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("material price rise and material longward net shift")},
        {QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"), CftcInterpretationScope::ContemporaneousRelation,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("material price fall and material shortward net shift")},
        {QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"), CftcInterpretationScope::ContemporaneousRelation,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("material price rise and material shortward net shift")},
        {QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"), CftcInterpretationScope::ContemporaneousRelation,
         CftcEvidenceBasis::EngineHeuristic, QStringLiteral("material price fall and material longward net shift")},
    };
}

/// The complete Batch 4A state taxonomy in stable order. Every id is either
/// emitted by cftc_interpret or recorded as unavailable, and every id appears
/// in the versioned rule catalog.
inline QStringList cftc_interpretation_state_ids() {
    return {
        QStringLiteral("NET_LONG"),
        QStringLiteral("NET_SHORT"),
        QStringLiteral("NET_FLAT"),
        QStringLiteral("HISTORICALLY_HIGH_NET"),
        QStringLiteral("HISTORICALLY_LOW_NET"),
        QStringLiteral("CROWDED_LONG"),
        QStringLiteral("CROWDED_SHORT"),
        QStringLiteral("SEVERE_LONG_EXTREME"),
        QStringLiteral("SEVERE_SHORT_EXTREME"),
        QStringLiteral("LONG_ACCUMULATION"),
        QStringLiteral("LONG_LIQUIDATION"),
        QStringLiteral("SHORT_BUILDING"),
        QStringLiteral("SHORT_COVERING"),
        QStringLiteral("NET_LONGWARD_SHIFT"),
        QStringLiteral("NET_SHORTWARD_SHIFT"),
        QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"),
        QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R"),
        QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_4R"),
        QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R"),
        QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_13R"),
        QStringLiteral("PERSISTENT_HIGH_EXTREME"),
        QStringLiteral("PERSISTENT_LOW_EXTREME"),
        QStringLiteral("EXITED_HIGH_EXTREME"),
        QStringLiteral("EXITED_LOW_EXTREME"),
        QStringLiteral("UNWINDING_HIGH_EXTREME"),
        QStringLiteral("UNWINDING_LOW_EXTREME"),
        QStringLiteral("OI_EXPANSION"),
        QStringLiteral("OI_CONTRACTION"),
        QStringLiteral("HIGH_MARKET_CONCENTRATION"),
        QStringLiteral("CONCENTRATION_RISING"),
        QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"),
        QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"),
        QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
        QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"),
    };
}

/// True when a state id belongs to the market-level concentration taxonomy and
/// must never be attached to a participant category.
inline bool cftc_is_market_concentration_state(const QString& state_id) {
    return state_id == QLatin1String("HIGH_MARKET_CONCENTRATION") || state_id == QLatin1String("CONCENTRATION_RISING");
}

// ── Participant interpretation ──────────────────────────────────────────────

namespace detail {

inline std::optional<double> cftc_optional(bool has_value, double value) {
    return has_value ? std::optional<double>(value) : std::nullopt;
}

/// Explicit reason why a participant's current normalized reading is absent.
inline CftcUnavailableReason cftc_position_reason(const CftcObservation& observation, int participant_index,
                                                  const std::optional<double>& net) {
    if (!net)
        return CftcUnavailableReason::MissingParticipantLeg;
    if (!observation.open_interest.has_value())
        return CftcUnavailableReason::MissingOpenInterest;
    if (*observation.open_interest <= 0.0)
        return CftcUnavailableReason::NonPositiveOpenInterest;
    if (participant_index < 0 || participant_index >= observation.longs.size() ||
        participant_index >= observation.shorts.size())
        return CftcUnavailableReason::MissingParticipantLeg;
    return CftcUnavailableReason::MissingParticipantLeg;
}

/// Whether the anchor report's Open Interest is usable as the prior-OI
/// denominator for an exact horizon.
inline bool cftc_anchor_oi_positive(const QVector<CftcObservation>& history, int anchor_index) {
    return anchor_index >= 0 && anchor_index < history.size() && history[anchor_index].open_interest.has_value() &&
           *history[anchor_index].open_interest > 0.0;
}

} // namespace detail

/// Interpret one participant class of one family over an already
/// current-truncated history. Appends any price assessments and unavailable
/// records to the caller's result containers.
inline CftcParticipantInterpretation
cftc_interpret_participant(const QVector<CftcObservation>& history, int participant_index,
                           const CftcParticipant& participant, CftcFamily family,
                           const CftcInterpretationConfig& config, const QVector<CftcPricePoint>& prices,
                           bool price_source_specified, QVector<CftcPricePositionAssessment>& price_context,
                           QVector<CftcUnavailableRecord>& unavailable) {
    CftcParticipantInterpretation out;
    out.participant_key = participant.key;
    out.label = participant.label;
    out.terminology = cftc_participant_terminology(family, participant.key);
    out.terminology_code = cftc_terminology_code(out.terminology);
    if (out.terminology == CftcTerminologyClass::BroadNonCommercial)
        out.terminology_caveat_code = QStringLiteral("broad_category_caveat");
    else if (out.terminology == CftcTerminologyClass::LeveragedFunds)
        out.terminology_caveat_code = QStringLiteral("not_all_outright_speculation_caveat");
    out.crowding_terminology_allowed = cftc_crowding_terminology_allowed(family, participant.key);
    if (history.isEmpty())
        return out;

    const int current_index = history.size() - 1;
    const CftcObservation& current = history.last();
    const QDate current_date = current.date;

    const std::optional<double> net = cftc_participant_net(current, participant_index);
    const CftcPositionMetrics position = cftc_position_metrics(current, participant_index);
    out.net_available = net.has_value();
    if (net)
        out.net_position = *net;
    out.has_net_pct_oi = position.has_net_pct_oi;
    if (position.has_net_pct_oi)
        out.net_pct_oi = position.net_pct_oi;

    auto base_metrics = [&]() {
        QVector<CftcStateMetric> metrics;
        metrics.append(
            cftc_state_metric(QStringLiteral("long_leg"), detail::cftc_optional(position.has_long, position.long_leg)));
        metrics.append(cftc_state_metric(QStringLiteral("short_leg"),
                                         detail::cftc_optional(position.has_short, position.short_leg)));
        metrics.append(cftc_state_metric(QStringLiteral("net_position"), net));
        metrics.append(cftc_state_metric(QStringLiteral("open_interest"), current.open_interest));
        metrics.append(cftc_state_metric(QStringLiteral("net_pct_oi"),
                                         detail::cftc_optional(position.has_net_pct_oi, position.net_pct_oi)));
        return metrics;
    };

    // Current exposure. The state uses the reported net contracts
    // (long - short), which is a CFTC accounting fact; its sign equals
    // net_pct_oi whenever Open Interest is positive and present. When Open
    // Interest is missing or non-positive the normalized metric stays absent
    // while the raw-net exposure state remains available and truthful.
    if (net) {
        CftcInterpretationState state;
        state.participant_key = participant.key;
        state.scope = CftcInterpretationScope::AccountingFact;
        state.threshold_basis = CftcEvidenceBasis::DerivedIdentity;
        if (*net > kCftcFlatPositionTolerance)
            state.state_id = QStringLiteral("NET_LONG");
        else if (*net < -kCftcFlatPositionTolerance)
            state.state_id = QStringLiteral("NET_SHORT");
        else
            state.state_id = QStringLiteral("NET_FLAT");
        state.metrics = base_metrics();
        out.states.append(state);
    } else {
        unavailable.append(cftc_unavailable_record(
            QStringLiteral("NET_EXPOSURE"), participant.key, false, 0, CftcInterpretationScope::AccountingFact,
            CftcEvidenceBasis::DerivedIdentity, CftcUnavailableReason::MissingParticipantLeg));
    }

    // Historical relative position against the previous history_window valid
    // Net %OI reports, with the current observation excluded.
    const QVector<CftcDatedValue> net_pct_series =
        cftc_metric_series(history, participant_index, CftcMetricKind::NetPctOi);
    const CftcTrailingPercentile percentile =
        cftc_trailing_percentile_at(net_pct_series, current_date, config.history_window);
    out.historical_percentile_available = percentile.available;
    if (percentile.available) {
        out.percentile = percentile.percentile;
        out.percentile_reference_count = percentile.reference_count;
        const double low_extreme = cftc_low_extreme_percentile(config);
        const double low_severe = cftc_low_severe_percentile(config);
        auto add_historical_state = [&](const QString& state_id) {
            CftcInterpretationState state;
            state.state_id = state_id;
            state.participant_key = participant.key;
            state.scope = CftcInterpretationScope::HistoricalRelativeState;
            state.threshold_basis = CftcEvidenceBasis::EngineHeuristic;
            state.metrics = base_metrics();
            state.has_percentile = true;
            state.percentile = percentile.percentile;
            state.percentile_reference_count = percentile.reference_count;
            out.states.append(state);
        };
        if (percentile.percentile >= config.extreme_percentile)
            add_historical_state(QStringLiteral("HISTORICALLY_HIGH_NET"));
        if (percentile.percentile <= low_extreme)
            add_historical_state(QStringLiteral("HISTORICALLY_LOW_NET"));
        if (percentile.percentile >= config.severe_extreme_percentile && net && *net > 0.0)
            add_historical_state(QStringLiteral("SEVERE_LONG_EXTREME"));
        if (percentile.percentile <= low_severe && net && *net < 0.0)
            add_historical_state(QStringLiteral("SEVERE_SHORT_EXTREME"));
        if (out.crowding_terminology_allowed && percentile.percentile >= config.extreme_percentile && net && *net > 0.0)
            add_historical_state(QStringLiteral("CROWDED_LONG"));
        if (out.crowding_terminology_allowed && percentile.percentile <= low_extreme && net && *net < 0.0)
            add_historical_state(QStringLiteral("CROWDED_SHORT"));
    } else {
        const CftcUnavailableReason reason = percentile.current_present
                                                 ? CftcUnavailableReason::InsufficientHistory
                                                 : detail::cftc_position_reason(current, participant_index, net);
        unavailable.append(cftc_unavailable_record(QStringLiteral("HISTORICAL_RELATIVE_STATE"), participant.key, false,
                                                   0, CftcInterpretationScope::HistoricalRelativeState,
                                                   CftcEvidenceBasis::EngineHeuristic, reason));
    }

    // Exact 1/4/13-report flows.
    struct HorizonEvaluation {
        int horizon = 0;
        CftcReportFlow flow;
        CftcTrailingMoveRank long_rank;
        CftcTrailingMoveRank short_rank;
        CftcTrailingMoveRank net_rank;
        CftcUnavailableReason horizon_reason = CftcUnavailableReason::None;
        QStringList gross_fired;
        CftcUnavailableReason net_reason = CftcUnavailableReason::None;
        CftcUnavailableReason share_reason = CftcUnavailableReason::None;
        CftcTrailingMoveRank price_rank;
    };
    QVector<HorizonEvaluation> horizons;
    horizons.reserve(config.horizons_reports.size());
    for (int horizon : config.horizons_reports) {
        HorizonEvaluation evaluation;
        evaluation.horizon = horizon;
        evaluation.flow = cftc_report_flow(history, participant_index, horizon, current_index);
        if (current_index - horizon < 0)
            evaluation.horizon_reason = CftcUnavailableReason::InsufficientHistory;
        else if (!evaluation.flow.continuous)
            evaluation.horizon_reason = CftcUnavailableReason::BrokenReportSequence;
        evaluation.long_rank =
            cftc_trailing_move_rank(cftc_flow_move_series(history, participant_index, CftcFlowMetric::Long, horizon),
                                    current_date, config.history_window);
        evaluation.short_rank =
            cftc_trailing_move_rank(cftc_flow_move_series(history, participant_index, CftcFlowMetric::Short, horizon),
                                    current_date, config.history_window);
        evaluation.net_rank =
            cftc_trailing_move_rank(cftc_flow_move_series(history, participant_index, CftcFlowMetric::Net, horizon),
                                    current_date, config.history_window);
        horizons.append(evaluation);
    }

    // Emit the raw per-horizon measurements before any state classification so
    // a valid below-threshold move stays inspectable.
    out.flow_readings.reserve(horizons.size());
    for (const HorizonEvaluation& evaluation : horizons) {
        CftcHorizonFlowReading reading;
        reading.horizon_reports = evaluation.horizon;
        reading.evaluated = evaluation.horizon_reason == CftcUnavailableReason::None;
        reading.reason = evaluation.horizon_reason;
        reading.has_long_flow = evaluation.flow.has_long_flow;
        reading.long_flow = evaluation.flow.long_flow;
        reading.has_short_flow = evaluation.flow.has_short_flow;
        reading.short_flow = evaluation.flow.short_flow;
        reading.has_net_flow = evaluation.flow.has_net_flow;
        reading.net_flow = evaluation.flow.net_flow;
        reading.has_long_rank = evaluation.long_rank.available;
        reading.long_rank = evaluation.long_rank.rank;
        reading.has_short_rank = evaluation.short_rank.available;
        reading.short_rank = evaluation.short_rank.rank;
        reading.has_net_rank = evaluation.net_rank.available;
        reading.net_rank = evaluation.net_rank.rank;
        reading.net_rank_reference_count = evaluation.net_rank.reference_count;
        out.flow_readings.append(reading);
    }

    auto base_flow_state = [&](const QString& state_id, CftcInterpretationScope scope, CftcEvidenceBasis basis) {
        CftcInterpretationState state;
        state.state_id = state_id;
        state.participant_key = participant.key;
        state.scope = scope;
        state.threshold_basis = basis;
        state.metrics = base_metrics();
        return state;
    };

    for (HorizonEvaluation& evaluation : horizons) {
        const int horizon = evaluation.horizon;
        const CftcReportFlow& flow = evaluation.flow;
        const int anchor_index = current_index - horizon;
        const bool anchor_oi_positive = detail::cftc_anchor_oi_positive(history, anchor_index);
        const CftcUnavailableReason anchor_oi_reason =
            anchor_index >= 0 && history[anchor_index].open_interest.has_value()
                ? CftcUnavailableReason::NonPositiveOpenInterest
                : CftcUnavailableReason::MissingOpenInterest;
        auto flow_missing_reason = [&]() {
            if (anchor_index < 0)
                return CftcUnavailableReason::InsufficientHistory;
            if (!anchor_oi_positive)
                return anchor_oi_reason;
            return CftcUnavailableReason::MissingParticipantLeg;
        };
        auto current_share_reason = [&]() {
            if (!anchor_oi_positive)
                return anchor_oi_reason;
            if (!current.open_interest.has_value())
                return CftcUnavailableReason::MissingOpenInterest;
            if (*current.open_interest <= 0.0)
                return CftcUnavailableReason::NonPositiveOpenInterest;
            return CftcUnavailableReason::MissingParticipantLeg;
        };

        // Gross-long / gross-short states stay separate, and each leg is
        // evaluated from its own flow and its own strictly trailing reference:
        // a missing short leg or a short reference must not suppress a valid
        // long-leg state (or the other way round).
        if (evaluation.horizon_reason != CftcUnavailableReason::None) {
            evaluation.net_reason = evaluation.horizon_reason;
            evaluation.share_reason = evaluation.horizon_reason;
            unavailable.append(cftc_unavailable_record(QStringLiteral("GROSS_FLOW"), participant.key, true, horizon,
                                                       CftcInterpretationScope::DescriptiveFlow,
                                                       CftcEvidenceBasis::EngineHeuristic, evaluation.horizon_reason));
        } else {
            if (!flow.has_long_flow || (!evaluation.long_rank.available && flow.long_flow != 0.0)) {
                const CftcUnavailableReason reason =
                    flow.has_long_flow ? CftcUnavailableReason::InsufficientHistory : flow_missing_reason();
                const QString state_id = !flow.has_long_flow
                                             ? QString()
                                             : (flow.long_flow > 0.0 ? QStringLiteral("LONG_ACCUMULATION")
                                                                     : QStringLiteral("LONG_LIQUIDATION"));
                unavailable.append(cftc_unavailable_record(QStringLiteral("GROSS_FLOW"), participant.key, true, horizon,
                                                           CftcInterpretationScope::DescriptiveFlow,
                                                           CftcEvidenceBasis::EngineHeuristic, reason, state_id));
            }
            if (!flow.has_short_flow || (!evaluation.short_rank.available && flow.short_flow != 0.0)) {
                const CftcUnavailableReason reason =
                    flow.has_short_flow ? CftcUnavailableReason::InsufficientHistory : flow_missing_reason();
                const QString state_id =
                    !flow.has_short_flow
                        ? QString()
                        : (flow.short_flow > 0.0 ? QStringLiteral("SHORT_BUILDING") : QStringLiteral("SHORT_COVERING"));
                unavailable.append(cftc_unavailable_record(QStringLiteral("GROSS_FLOW"), participant.key, true, horizon,
                                                           CftcInterpretationScope::DescriptiveFlow,
                                                           CftcEvidenceBasis::EngineHeuristic, reason, state_id));
            }
            if (!flow.has_net_flow)
                evaluation.net_reason = flow_missing_reason();
            else if (!evaluation.net_rank.available && flow.net_flow != 0.0)
                evaluation.net_reason = CftcUnavailableReason::InsufficientHistory;
            if (!flow.has_net_flow)
                evaluation.share_reason = flow_missing_reason();
            else if (!flow.has_net_share_change)
                evaluation.share_reason = current_share_reason();
        }

        auto add_gross = [&](const QString& state_id, double flow_value, const CftcTrailingMoveRank& rank) {
            CftcInterpretationState state =
                base_flow_state(state_id, CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic);
            state.has_horizon = true;
            state.horizon_reports = horizon;
            state.metrics.append(cftc_state_metric(QStringLiteral("move_value"), flow_value));
            state.has_move_rank = true;
            state.move_rank = rank.rank;
            state.move_rank_reference_count = rank.reference_count;
            state.move_large = rank.rank >= config.large_move_percentile;
            out.states.append(state);
            evaluation.gross_fired.append(state_id);
        };
        if (flow.has_long_flow && evaluation.long_rank.available) {
            if (flow.long_flow > 0.0 && evaluation.long_rank.rank >= config.material_move_percentile)
                add_gross(QStringLiteral("LONG_ACCUMULATION"), flow.long_flow, evaluation.long_rank);
            else if (flow.long_flow < 0.0 && evaluation.long_rank.rank >= config.material_move_percentile)
                add_gross(QStringLiteral("LONG_LIQUIDATION"), flow.long_flow, evaluation.long_rank);
        }
        if (flow.has_short_flow && evaluation.short_rank.available) {
            if (flow.short_flow > 0.0 && evaluation.short_rank.rank >= config.material_move_percentile)
                add_gross(QStringLiteral("SHORT_BUILDING"), flow.short_flow, evaluation.short_rank);
            else if (flow.short_flow < 0.0 && evaluation.short_rank.rank >= config.material_move_percentile)
                add_gross(QStringLiteral("SHORT_COVERING"), flow.short_flow, evaluation.short_rank);
        }

        if (evaluation.net_reason == CftcUnavailableReason::None) {
            const bool longward = flow.net_flow > 0.0;
            const bool shortward = flow.net_flow < 0.0;
            const bool material = evaluation.net_rank.rank >= config.material_move_percentile;
            if (material && (longward || shortward)) {
                CftcInterpretationState state = base_flow_state(
                    longward ? QStringLiteral("NET_LONGWARD_SHIFT") : QStringLiteral("NET_SHORTWARD_SHIFT"),
                    CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic);
                state.has_horizon = true;
                state.horizon_reports = horizon;
                state.metrics.append(cftc_state_metric(QStringLiteral("net_flow"), flow.net_flow));
                state.has_move_rank = true;
                state.move_rank = evaluation.net_rank.rank;
                state.move_rank_reference_count = evaluation.net_rank.reference_count;
                state.move_large = evaluation.net_rank.rank >= config.large_move_percentile;
                const QStringList candidates =
                    longward ? QStringList{QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")}
                             : QStringList{QStringLiteral("LONG_LIQUIDATION"), QStringLiteral("SHORT_BUILDING")};
                for (const QString& candidate : candidates) {
                    if (evaluation.gross_fired.contains(candidate))
                        state.mechanism_state_ids.append(candidate);
                }
                out.states.append(state);
            }
        } else {
            unavailable.append(cftc_unavailable_record(QStringLiteral("NET_SHIFT"), participant.key, true, horizon,
                                                       CftcInterpretationScope::DescriptiveFlow,
                                                       CftcEvidenceBasis::EngineHeuristic, evaluation.net_reason));
        }

        if (evaluation.share_reason == CftcUnavailableReason::None) {
            // The plan defines NET_SHARE_RAW_DISAGREEMENT without a materiality
            // gate; it is an accounting observation, and the actual net flow and
            // share change are exposed as metrics so a presentation layer can
            // judge whether to narrate it.
            const bool raw_and_share_opposed = (flow.net_flow > 0.0 && flow.net_share_change < 0.0) ||
                                               (flow.net_flow < 0.0 && flow.net_share_change > 0.0);
            const bool neither_zero = flow.net_flow != 0.0 && flow.net_share_change != 0.0;
            if (raw_and_share_opposed && neither_zero) {
                CftcInterpretationState state =
                    base_flow_state(QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"),
                                    CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::DerivedIdentity);
                state.has_horizon = true;
                state.horizon_reports = horizon;
                state.metrics.append(cftc_state_metric(QStringLiteral("net_flow"), flow.net_flow));
                state.metrics.append(cftc_state_metric(QStringLiteral("net_share_change"), flow.net_share_change));
                out.states.append(state);
            }
        } else {
            unavailable.append(cftc_unavailable_record(QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), participant.key,
                                                       true, horizon, CftcInterpretationScope::DescriptiveFlow,
                                                       CftcEvidenceBasis::DerivedIdentity, evaluation.share_reason));
        }

        // Price-versus-positioning assessment for this participant/horizon.
        CftcPricePositionAssessment assessment;
        assessment.participant_key = participant.key;
        assessment.horizon_reports = horizon;
        // The positioning side is pure CFTC data: populate it whenever the
        // flow and its materiality reference exist, independently of price
        // availability, so a missing price context can never suppress a valid
        // CFTC positioning measurement from the presentation.
        if (evaluation.horizon_reason == CftcUnavailableReason::None && flow.has_net_flow &&
            evaluation.net_rank.available) {
            assessment.has_positioning_move = true;
            assessment.positioning_move = flow.net_flow;
            assessment.has_positioning_move_rank = true;
            assessment.positioning_move_rank = evaluation.net_rank.rank;
            assessment.positioning_reference_count = evaluation.net_rank.reference_count;
            assessment.positioning_material =
                evaluation.net_rank.rank >= config.material_move_percentile && flow.net_flow != 0.0;
            assessment.positioning_move_large = evaluation.net_rank.rank >= config.large_move_percentile;
        }
        if (prices.isEmpty()) {
            assessment.reason = CftcUnavailableReason::MissingPriceContext;
        } else if (!price_source_specified) {
            assessment.reason = CftcUnavailableReason::UnspecifiedPriceSource;
        } else if (evaluation.horizon_reason != CftcUnavailableReason::None) {
            assessment.reason = evaluation.horizon_reason;
        } else if (!flow.has_net_flow) {
            assessment.reason = flow_missing_reason();
        } else if (!evaluation.net_rank.available) {
            assessment.reason = CftcUnavailableReason::InsufficientHistory;
        } else {
            const CftcReportPrice latest = cftc_report_price(prices, current_date);
            const CftcReportPrice anchor = cftc_report_price(prices, history[anchor_index].date);
            if (!latest.available || !anchor.available) {
                assessment.reason = !latest.available ? latest.reason : anchor.reason;
            } else {
                assessment.has_price_move = true;
                assessment.price_move = latest.close - anchor.close;
                assessment.price_anchor_date = anchor.date;
                assessment.price_latest_date = latest.date;
                const QVector<CftcDatedValue> price_moves = cftc_price_move_series(prices, history, horizon);
                evaluation.price_rank = cftc_trailing_move_rank(price_moves, current_date, config.history_window);
                if (!evaluation.price_rank.available) {
                    assessment.reason = evaluation.price_rank.has_current
                                            ? CftcUnavailableReason::InsufficientHistory
                                            : CftcUnavailableReason::PriceHistoryIncomplete;
                } else {
                    assessment.has_price_move_rank = true;
                    assessment.price_move_rank = evaluation.price_rank.rank;
                    assessment.price_reference_count = evaluation.price_rank.reference_count;
                    assessment.price_material =
                        evaluation.price_rank.rank >= config.material_move_percentile && assessment.price_move != 0.0;
                    assessment.price_move_large = evaluation.price_rank.rank >= config.large_move_percentile;
                    assessment.evaluated = true;
                    if (assessment.price_material && assessment.positioning_material) {
                        const bool price_up = assessment.price_move > 0.0;
                        const bool positioning_longward = assessment.positioning_move > 0.0;
                        if (price_up && positioning_longward)
                            assessment.state_id = QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP");
                        else if (!price_up && !positioning_longward)
                            assessment.state_id = QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN");
                        else if (price_up && !positioning_longward)
                            assessment.state_id = QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE");
                        else
                            assessment.state_id = QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE");
                        assessment.has_state = true;
                        const QStringList candidates =
                            positioning_longward
                                ? QStringList{QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")}
                                : QStringList{QStringLiteral("LONG_LIQUIDATION"), QStringLiteral("SHORT_BUILDING")};
                        for (const QString& candidate : candidates) {
                            if (evaluation.gross_fired.contains(candidate))
                                assessment.mechanism_state_ids.append(candidate);
                        }
                    }
                }
            }
        }
        price_context.append(assessment);
    }

    // Sustained repositioning: a material net move confirmed by the majority
    // of the one-report net steps inside the same window.
    auto horizon_net_evaluation = [&](int horizon, CftcReportFlow& flow, CftcTrailingMoveRank& rank,
                                      CftcUnavailableReason& reason) {
        flow = cftc_report_flow(history, participant_index, horizon, current_index);
        if (current_index - horizon < 0) {
            reason = CftcUnavailableReason::InsufficientHistory;
            return false;
        }
        if (!flow.continuous) {
            reason = CftcUnavailableReason::BrokenReportSequence;
            return false;
        }
        rank = cftc_trailing_move_rank(cftc_flow_move_series(history, participant_index, CftcFlowMetric::Net, horizon),
                                       current_date, config.history_window);
        const bool anchor_oi_positive = detail::cftc_anchor_oi_positive(history, current_index - horizon);
        if (!flow.has_net_flow) {
            if (anchor_oi_positive)
                reason = CftcUnavailableReason::MissingParticipantLeg;
            else if (history[current_index - horizon].open_interest.has_value())
                reason = CftcUnavailableReason::NonPositiveOpenInterest;
            else
                reason = CftcUnavailableReason::MissingOpenInterest;
            return false;
        }
        if (!rank.available) {
            reason = CftcUnavailableReason::InsufficientHistory;
            return false;
        }
        return true;
    };
    auto one_report_net_steps = [&](int count, QVector<double>& steps, CftcUnavailableReason& reason) {
        steps.clear();
        for (int k = count - 1; k >= 0; --k) {
            const int index = current_index - k;
            if (index < 1) {
                reason = CftcUnavailableReason::InsufficientHistory;
                return false;
            }
            if (!cftc_weekly_neighbour(history[index - 1].date, history[index].date)) {
                reason = CftcUnavailableReason::BrokenReportSequence;
                return false;
            }
            const CftcReportFlow step = cftc_report_flow(history, participant_index, 1, index);
            if (!step.has_net_flow) {
                const bool anchor_oi_positive = detail::cftc_anchor_oi_positive(history, index - 1);
                if (anchor_oi_positive)
                    reason = CftcUnavailableReason::MissingParticipantLeg;
                else if (history[index - 1].open_interest.has_value())
                    reason = CftcUnavailableReason::NonPositiveOpenInterest;
                else
                    reason = CftcUnavailableReason::MissingOpenInterest;
                return false;
            }
            steps.append(step.net_flow);
        }
        return true;
    };
    for (int horizon : {4, 13}) {
        CftcReportFlow flow;
        CftcTrailingMoveRank rank;
        CftcUnavailableReason reason = CftcUnavailableReason::None;
        if (!horizon_net_evaluation(horizon, flow, rank, reason)) {
            if (reason != CftcUnavailableReason::None) {
                unavailable.append(cftc_unavailable_record(QStringLiteral("SUSTAINED_REPOSITIONING"), participant.key,
                                                           true, horizon, CftcInterpretationScope::DescriptiveFlow,
                                                           CftcEvidenceBasis::EngineHeuristic, reason));
            }
            continue;
        }
        const bool material = rank.rank >= config.material_move_percentile;
        if (!material || flow.net_flow == 0.0)
            continue;
        QVector<double> steps;
        if (!one_report_net_steps(horizon, steps, reason)) {
            unavailable.append(cftc_unavailable_record(QStringLiteral("SUSTAINED_REPOSITIONING"), participant.key, true,
                                                       horizon, CftcInterpretationScope::DescriptiveFlow,
                                                       CftcEvidenceBasis::EngineHeuristic, reason));
            continue;
        }
        int positives = 0;
        int negatives = 0;
        for (double step : steps) {
            if (step > 0.0)
                ++positives;
            else if (step < 0.0)
                ++negatives;
        }
        const int required = horizon == 4 ? 3 : 9;
        QString sustained_id;
        int confirming_steps = 0;
        if (flow.net_flow > 0.0 && positives >= required) {
            sustained_id = horizon == 4 ? QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R")
                                        : QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R");
            confirming_steps = positives;
        } else if (flow.net_flow < 0.0 && negatives >= required) {
            sustained_id = horizon == 4 ? QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_4R")
                                        : QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_13R");
            confirming_steps = negatives;
        } else {
            continue;
        }
        CftcInterpretationState state =
            base_flow_state(sustained_id, CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic);
        state.has_horizon = true;
        state.horizon_reports = horizon;
        state.metrics.append(cftc_state_metric(QStringLiteral("net_flow"), flow.net_flow));
        state.metrics.append(
            cftc_state_metric(QStringLiteral("confirming_steps"), static_cast<double>(confirming_steps)));
        state.has_move_rank = true;
        state.move_rank = rank.rank;
        state.move_rank_reference_count = rank.reference_count;
        state.move_large = rank.rank >= config.large_move_percentile;
        out.states.append(state);
    }

    // Extreme persistence, exit and unwind. Exact reading of the governing
    // plan:
    //   * PERSISTENT_* needs `persistent_extreme_reports` consecutive weekly
    //     reports inside the band, each percentile measured against its own
    //     strictly trailing reference;
    //   * EXITED_* compares the previous report's percentile with the current
    //     one and requires the Net %OI level to move in the exit direction;
    //   * UNWINDING_* additionally requires the immediately preceding report
    //     to have carried the persistent-extreme state (the "prior persistent
    //     state" reading) and the current percentile to re-enter at or past
    //     the re-entry band.
    // An exit or unwind is a positioning transition, never a price forecast.
    const int persistence = std::max(1, config.persistent_extreme_reports);
    auto percentile_at_offset = [&](int offset) {
        const int index = current_index - offset;
        if (index < 0 || index >= history.size())
            return CftcTrailingPercentile{};
        return cftc_trailing_percentile_at(net_pct_series, history[index].date, config.history_window);
    };
    auto adjacent_at = [&](int offset) {
        const int index = current_index - offset;
        return index >= 1 && cftc_weekly_neighbour(history[index - 1].date, history[index].date);
    };
    auto position_reason_at = [&](int offset) {
        const int index = current_index - offset;
        if (index < 0 || index >= history.size())
            return CftcUnavailableReason::InsufficientHistory;
        return detail::cftc_position_reason(history[index], participant_index,
                                            cftc_participant_net(history[index], participant_index));
    };
    // A run of `persistence` consecutive weekly reports at the requested band,
    // starting `start_offset` reports before the current report. When a report
    // in the run is not evaluable, `missing_reason` distinguishes an absent
    // normalized reading from an insufficient reference window.
    auto persistent_run = [&](int start_offset, bool high, CftcUnavailableReason& missing_reason) {
        missing_reason = CftcUnavailableReason::None;
        for (int k = 0; k < persistence; ++k) {
            const int offset = start_offset + k;
            const CftcTrailingPercentile point = percentile_at_offset(offset);
            if (!point.available) {
                missing_reason =
                    point.current_present ? CftcUnavailableReason::InsufficientHistory : position_reason_at(offset);
                return false;
            }
            if (high ? point.percentile < config.extreme_percentile
                     : point.percentile > cftc_low_extreme_percentile(config))
                return false;
            // The step between offsets (offset-1) and offset is adjacent_at(offset-1).
            // A broken weekly sequence makes the run unevaluable, not false.
            if (k > 0 && !adjacent_at(offset - 1)) {
                missing_reason = CftcUnavailableReason::BrokenReportSequence;
                return false;
            }
        }
        return true;
    };
    const CftcTrailingPercentile p0 = percentile_at_offset(0);
    auto history_net_value = [&](int offset) -> std::optional<double> {
        const int index = current_index - offset;
        if (index < 0 || index >= history.size())
            return std::nullopt;
        for (const auto& point : net_pct_series) {
            if (point.date == history[index].date)
                return point.value;
        }
        return std::nullopt;
    };
    auto extreme_state = [&](const QString& state_id, const CftcTrailingPercentile& point) {
        CftcInterpretationState state = base_flow_state(state_id, CftcInterpretationScope::HistoricalRelativeState,
                                                        CftcEvidenceBasis::EngineHeuristic);
        state.has_percentile = true;
        state.percentile = point.percentile;
        state.percentile_reference_count = point.reference_count;
        return state;
    };
    if (!p0.available) {
        const CftcUnavailableReason reason = p0.current_present
                                                 ? CftcUnavailableReason::InsufficientHistory
                                                 : detail::cftc_position_reason(current, participant_index, net);
        unavailable.append(cftc_unavailable_record(QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                                                   CftcInterpretationScope::HistoricalRelativeState,
                                                   CftcEvidenceBasis::EngineHeuristic, reason));
    } else {
        const std::optional<double> net_current = history_net_value(0);
        const std::optional<double> net_previous = history_net_value(1);
        const bool adjacent_now = adjacent_at(0);
        const bool net_fell = net_current && net_previous && *net_current < *net_previous && adjacent_now;
        const bool net_rose = net_current && net_previous && *net_current > *net_previous && adjacent_now;

        CftcUnavailableReason high_missing = CftcUnavailableReason::None;
        CftcUnavailableReason low_missing = CftcUnavailableReason::None;
        const bool persistent_high = persistent_run(0, true, high_missing);
        const bool persistent_low = persistent_run(0, false, low_missing);
        if (persistent_high)
            out.states.append(extreme_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), p0));
        else if (high_missing != CftcUnavailableReason::None)
            unavailable.append(cftc_unavailable_record(QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                                                       CftcInterpretationScope::HistoricalRelativeState,
                                                       CftcEvidenceBasis::EngineHeuristic, high_missing,
                                                       QStringLiteral("PERSISTENT_HIGH_EXTREME")));
        if (persistent_low)
            out.states.append(extreme_state(QStringLiteral("PERSISTENT_LOW_EXTREME"), p0));
        else if (low_missing != CftcUnavailableReason::None)
            unavailable.append(cftc_unavailable_record(QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                                                       CftcInterpretationScope::HistoricalRelativeState,
                                                       CftcEvidenceBasis::EngineHeuristic, low_missing,
                                                       QStringLiteral("PERSISTENT_LOW_EXTREME")));

        const CftcTrailingPercentile p1 = percentile_at_offset(1);
        if (!p1.available) {
            const CftcUnavailableReason reason =
                p1.current_present ? CftcUnavailableReason::InsufficientHistory : position_reason_at(1);
            unavailable.append(cftc_unavailable_record(QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                                                       CftcInterpretationScope::HistoricalRelativeState,
                                                       CftcEvidenceBasis::EngineHeuristic, reason,
                                                       QStringLiteral("EXITED_HIGH_EXTREME")));
            unavailable.append(cftc_unavailable_record(QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                                                       CftcInterpretationScope::HistoricalRelativeState,
                                                       CftcEvidenceBasis::EngineHeuristic, reason,
                                                       QStringLiteral("EXITED_LOW_EXTREME")));
        } else {
            const double low_extreme = cftc_low_extreme_percentile(config);
            const bool high_exit_band =
                p1.percentile >= config.extreme_percentile && p0.percentile < config.extreme_percentile;
            const bool low_exit_band = p1.percentile <= low_extreme && p0.percentile > low_extreme;
            if (!adjacent_now) {
                // A gap between the previous and the current report makes the
                // transition unevaluable, not false.
                if (high_exit_band)
                    unavailable.append(cftc_unavailable_record(
                        QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                        CftcInterpretationScope::HistoricalRelativeState, CftcEvidenceBasis::EngineHeuristic,
                        CftcUnavailableReason::BrokenReportSequence, QStringLiteral("EXITED_HIGH_EXTREME")));
                if (low_exit_band)
                    unavailable.append(cftc_unavailable_record(
                        QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                        CftcInterpretationScope::HistoricalRelativeState, CftcEvidenceBasis::EngineHeuristic,
                        CftcUnavailableReason::BrokenReportSequence, QStringLiteral("EXITED_LOW_EXTREME")));
            } else {
                if (high_exit_band && net_fell)
                    out.states.append(extreme_state(QStringLiteral("EXITED_HIGH_EXTREME"), p0));
                if (low_exit_band && net_rose)
                    out.states.append(extreme_state(QStringLiteral("EXITED_LOW_EXTREME"), p0));
            }
        }
        if (p0.percentile <= config.unwind_reentry_percentile) {
            CftcUnavailableReason prior_missing = CftcUnavailableReason::None;
            const bool prior_persistent_high = persistent_run(1, true, prior_missing);
            if (prior_persistent_high) {
                if (!adjacent_now)
                    unavailable.append(cftc_unavailable_record(
                        QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                        CftcInterpretationScope::HistoricalRelativeState, CftcEvidenceBasis::EngineHeuristic,
                        CftcUnavailableReason::BrokenReportSequence, QStringLiteral("UNWINDING_HIGH_EXTREME")));
                else if (net_fell)
                    out.states.append(extreme_state(QStringLiteral("UNWINDING_HIGH_EXTREME"), p0));
            } else if (prior_missing != CftcUnavailableReason::None) {
                unavailable.append(cftc_unavailable_record(QStringLiteral("EXTREME_TRANSITION"), participant.key, false,
                                                           0, CftcInterpretationScope::HistoricalRelativeState,
                                                           CftcEvidenceBasis::EngineHeuristic, prior_missing,
                                                           QStringLiteral("UNWINDING_HIGH_EXTREME")));
            }
        }
        if (p0.percentile >= cftc_unwind_low_reentry_percentile(config)) {
            CftcUnavailableReason prior_missing = CftcUnavailableReason::None;
            const bool prior_persistent_low = persistent_run(1, false, prior_missing);
            if (prior_persistent_low) {
                if (!adjacent_now)
                    unavailable.append(cftc_unavailable_record(
                        QStringLiteral("EXTREME_TRANSITION"), participant.key, false, 0,
                        CftcInterpretationScope::HistoricalRelativeState, CftcEvidenceBasis::EngineHeuristic,
                        CftcUnavailableReason::BrokenReportSequence, QStringLiteral("UNWINDING_LOW_EXTREME")));
                else if (net_rose)
                    out.states.append(extreme_state(QStringLiteral("UNWINDING_LOW_EXTREME"), p0));
            } else if (prior_missing != CftcUnavailableReason::None) {
                unavailable.append(cftc_unavailable_record(QStringLiteral("EXTREME_TRANSITION"), participant.key, false,
                                                           0, CftcInterpretationScope::HistoricalRelativeState,
                                                           CftcEvidenceBasis::EngineHeuristic, prior_missing,
                                                           QStringLiteral("UNWINDING_LOW_EXTREME")));
            }
        }
    }

    return out;
}

// ── Market context: Open Interest and concentration ─────────────────────────

inline void cftc_interpret_open_interest(const QVector<CftcObservation>& history,
                                         const CftcInterpretationConfig& config, CftcInterpretationResult& result) {
    const int current_index = history.size() - 1;
    const QDate current_date = history.last().date;
    for (int horizon : config.horizons_reports) {
        const CftcReportFlow flow = cftc_report_flow(history, -1, horizon, current_index);
        const int anchor_index = current_index - horizon;
        CftcUnavailableReason reason = CftcUnavailableReason::None;
        if (anchor_index < 0)
            reason = CftcUnavailableReason::InsufficientHistory;
        else if (!flow.continuous)
            reason = CftcUnavailableReason::BrokenReportSequence;
        else if (!history.last().open_interest.has_value())
            reason = CftcUnavailableReason::MissingOpenInterest;
        else if (*history.last().open_interest <= 0.0)
            reason = CftcUnavailableReason::NonPositiveOpenInterest;
        else if (!history[anchor_index].open_interest.has_value())
            reason = CftcUnavailableReason::MissingOpenInterest;
        else if (*history[anchor_index].open_interest <= 0.0)
            reason = CftcUnavailableReason::NonPositiveOpenInterest;
        const CftcTrailingMoveRank rank =
            cftc_trailing_move_rank(cftc_flow_move_series(history, -1, CftcFlowMetric::OpenInterest, horizon),
                                    current_date, config.history_window);
        if (reason == CftcUnavailableReason::None && !rank.available)
            reason = CftcUnavailableReason::InsufficientHistory;
        CftcOpenInterestReading reading;
        reading.horizon_reports = horizon;
        reading.evaluated = reason == CftcUnavailableReason::None;
        reading.reason = reason;
        if (reading.evaluated) {
            reading.has_oi_change = true;
            reading.oi_change = flow.oi_change;
            reading.has_rank = true;
            reading.rank = rank.rank;
            reading.rank_reference_count = rank.reference_count;
            reading.material = flow.oi_change != 0.0 && rank.rank >= config.material_move_percentile;
            reading.move_large = rank.rank >= config.large_move_percentile;
        }
        result.open_interest_readings.append(reading);
        if (reason != CftcUnavailableReason::None) {
            result.unavailable.append(cftc_unavailable_record(QStringLiteral("OI_CONTEXT"), QString(), true, horizon,
                                                              CftcInterpretationScope::MarketStructure,
                                                              CftcEvidenceBasis::EngineHeuristic, reason));
            continue;
        }
        if (flow.oi_change != 0.0 && rank.rank >= config.material_move_percentile) {
            CftcInterpretationState state;
            state.state_id = flow.oi_change > 0.0 ? QStringLiteral("OI_EXPANSION") : QStringLiteral("OI_CONTRACTION");
            state.has_horizon = true;
            state.horizon_reports = horizon;
            state.scope = CftcInterpretationScope::MarketStructure;
            state.threshold_basis = CftcEvidenceBasis::EngineHeuristic;
            state.metrics.append(cftc_state_metric(QStringLiteral("open_interest"), history.last().open_interest));
            state.metrics.append(
                cftc_state_metric(QStringLiteral("anchor_open_interest"), history[anchor_index].open_interest));
            state.metrics.append(cftc_state_metric(QStringLiteral("oi_change"), flow.oi_change));
            state.has_move_rank = true;
            state.move_rank = rank.rank;
            state.move_rank_reference_count = rank.reference_count;
            state.move_large = rank.rank >= config.large_move_percentile;
            result.market_context.append(state);
        }
    }
}

inline void cftc_interpret_concentration(const QVector<CftcObservation>& history,
                                         const CftcInterpretationConfig& config, CftcInterpretationResult& result) {
    const int current_index = history.size() - 1;
    const QDate current_date = history.last().date;
    const bool adjacent_previous =
        current_index >= 1 && cftc_weekly_neighbour(history[current_index - 1].date, history[current_index].date);

    for (const QString& field : cftc_concentration_field_keys()) {
        CftcConcentrationAssessment assessment;
        assessment.field_key = field;
        QVector<CftcDatedValue> series;
        for (const auto& observation : history) {
            const std::optional<double> value = cftc_concentration_value(observation, field);
            if (value)
                series.append({observation.date, observation.date_label, *value});
        }
        const std::optional<double> current_value = cftc_concentration_value(history.last(), field);
        assessment.has_current = current_value.has_value();
        if (current_value)
            assessment.current = *current_value;
        // The plan's HIGH_MARKET_CONCENTRATION uses the field's own full
        // trailing reference, not the Net %OI history window.
        const CftcTrailingPercentile percentile = cftc_full_trailing_percentile_at(series, current_date);
        assessment.has_percentile = percentile.available;
        if (percentile.available) {
            assessment.percentile = percentile.percentile;
            assessment.percentile_reference_count = percentile.reference_count;
        }
        if (current_value) {
            if (current_index < 1)
                assessment.change_unavailable_reason = CftcUnavailableReason::InsufficientHistory;
            else if (!adjacent_previous)
                assessment.change_unavailable_reason = CftcUnavailableReason::BrokenReportSequence;
            else if (!cftc_concentration_value(history[current_index - 1], field).has_value())
                assessment.change_unavailable_reason = CftcUnavailableReason::MissingConcentrationField;
        }
        if (current_value && adjacent_previous) {
            const std::optional<double> previous_value = cftc_concentration_value(history[current_index - 1], field);
            if (previous_value) {
                assessment.has_change = true;
                assessment.change = *current_value - *previous_value;
                QVector<CftcDatedValue> change_series;
                for (int i = 1; i < history.size(); ++i) {
                    if (!cftc_weekly_neighbour(history[i - 1].date, history[i].date))
                        continue;
                    const std::optional<double> before = cftc_concentration_value(history[i - 1], field);
                    const std::optional<double> after = cftc_concentration_value(history[i], field);
                    if (before && after)
                        change_series.append({history[i].date, history[i].date_label, *after - *before});
                }
                const CftcTrailingMoveRank rank =
                    cftc_trailing_move_rank(change_series, current_date, config.history_window);
                assessment.has_change_rank = rank.available;
                if (rank.available) {
                    assessment.change_rank = rank.rank;
                    assessment.change_reference_count = rank.reference_count;
                }
            }
        }
        result.concentration.append(assessment);
    }

    const CftcConcentrationAssessment* primary = nullptr;
    for (const auto& assessment : result.concentration) {
        if (assessment.field_key == config.primary_concentration_field) {
            primary = &assessment;
            break;
        }
    }
    if (!primary) {
        result.unavailable.append(cftc_unavailable_record(
            QStringLiteral("CONCENTRATION"), QString(), false, 0, CftcInterpretationScope::MarketStructure,
            CftcEvidenceBasis::EngineHeuristic, CftcUnavailableReason::MissingConcentrationField));
        return;
    }
    if (!primary->has_current) {
        result.unavailable.append(cftc_unavailable_record(
            QStringLiteral("CONCENTRATION"), QString(), false, 0, CftcInterpretationScope::MarketStructure,
            CftcEvidenceBasis::EngineHeuristic, CftcUnavailableReason::MissingConcentrationField));
        return;
    }
    // Each concentration state reports its own availability: a percentile-
    // qualified field with an unevaluable change must not simultaneously read
    // as an unavailable concentration family.
    if (!primary->has_percentile) {
        result.unavailable.append(cftc_unavailable_record(
            QStringLiteral("CONCENTRATION"), QString(), false, 0, CftcInterpretationScope::MarketStructure,
            CftcEvidenceBasis::EngineHeuristic, CftcUnavailableReason::InsufficientHistory,
            QStringLiteral("HIGH_MARKET_CONCENTRATION")));
    } else if (primary->percentile >= config.extreme_percentile) {
        CftcInterpretationState state;
        state.state_id = QStringLiteral("HIGH_MARKET_CONCENTRATION");
        state.scope = CftcInterpretationScope::MarketStructure;
        state.threshold_basis = CftcEvidenceBasis::EngineHeuristic;
        state.metrics.append(
            cftc_state_metric(primary->field_key, detail::cftc_optional(primary->has_current, primary->current)));
        state.has_percentile = true;
        state.percentile = primary->percentile;
        state.percentile_reference_count = primary->percentile_reference_count;
        result.market_context.append(state);
    }
    if (!primary->has_change) {
        const CftcUnavailableReason reason = primary->change_unavailable_reason != CftcUnavailableReason::None
                                                 ? primary->change_unavailable_reason
                                                 : (current_index >= 1 ? CftcUnavailableReason::BrokenReportSequence
                                                                       : CftcUnavailableReason::InsufficientHistory);
        result.unavailable.append(cftc_unavailable_record(
            QStringLiteral("CONCENTRATION"), QString(), false, 0, CftcInterpretationScope::MarketStructure,
            CftcEvidenceBasis::EngineHeuristic, reason, QStringLiteral("CONCENTRATION_RISING")));
    } else if (!primary->has_change_rank) {
        result.unavailable.append(cftc_unavailable_record(
            QStringLiteral("CONCENTRATION"), QString(), false, 0, CftcInterpretationScope::MarketStructure,
            CftcEvidenceBasis::EngineHeuristic, CftcUnavailableReason::InsufficientHistory,
            QStringLiteral("CONCENTRATION_RISING")));
    } else if (primary->change > 0.0 && primary->change_rank >= config.material_move_percentile) {
        CftcInterpretationState state;
        state.state_id = QStringLiteral("CONCENTRATION_RISING");
        state.scope = CftcInterpretationScope::MarketStructure;
        state.threshold_basis = CftcEvidenceBasis::EngineHeuristic;
        state.metrics.append(
            cftc_state_metric(primary->field_key, detail::cftc_optional(primary->has_current, primary->current)));
        state.metrics.append(cftc_state_metric(primary->field_key + QStringLiteral("_change"),
                                               detail::cftc_optional(primary->has_change, primary->change)));
        state.has_move_rank = true;
        state.move_rank = primary->change_rank;
        state.move_rank_reference_count = primary->change_reference_count;
        state.move_large = primary->change_rank >= config.large_move_percentile;
        result.market_context.append(state);
    }
}

// ── Top-level interpretation entry point ────────────────────────────────────

/// The unavailable counterpart of a whole report that cannot be interpreted
/// (no observations, or an as-of report the supplied history does not reach).
inline void cftc_append_report_unavailable(CftcInterpretationResult& result,
                                           const QVector<CftcParticipant>& participants, CftcUnavailableReason reason) {
    for (const auto& participant : participants) {
        CftcParticipantInterpretation interpretation;
        interpretation.participant_key = participant.key;
        interpretation.label = participant.label;
        interpretation.terminology = cftc_participant_terminology(result.family, participant.key);
        interpretation.terminology_code = cftc_terminology_code(interpretation.terminology);
        if (interpretation.terminology == CftcTerminologyClass::BroadNonCommercial)
            interpretation.terminology_caveat_code = QStringLiteral("broad_category_caveat");
        else if (interpretation.terminology == CftcTerminologyClass::LeveragedFunds)
            interpretation.terminology_caveat_code = QStringLiteral("not_all_outright_speculation_caveat");
        interpretation.crowding_terminology_allowed = cftc_crowding_terminology_allowed(result.family, participant.key);
        result.participants.append(interpretation);
    }
    struct FamilyScope {
        const char* family;
        CftcInterpretationScope scope;
        CftcEvidenceBasis basis;
    };
    const FamilyScope families[] = {
        {"NET_EXPOSURE", CftcInterpretationScope::AccountingFact, CftcEvidenceBasis::DerivedIdentity},
        {"HISTORICAL_RELATIVE_STATE", CftcInterpretationScope::HistoricalRelativeState,
         CftcEvidenceBasis::EngineHeuristic},
        {"GROSS_FLOW", CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic},
        {"NET_SHIFT", CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic},
        {"NET_SHARE_RAW_DISAGREEMENT", CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::DerivedIdentity},
        {"SUSTAINED_REPOSITIONING", CftcInterpretationScope::DescriptiveFlow, CftcEvidenceBasis::EngineHeuristic},
        {"EXTREME_TRANSITION", CftcInterpretationScope::HistoricalRelativeState, CftcEvidenceBasis::EngineHeuristic},
        {"OI_CONTEXT", CftcInterpretationScope::MarketStructure, CftcEvidenceBasis::EngineHeuristic},
        {"CONCENTRATION", CftcInterpretationScope::MarketStructure, CftcEvidenceBasis::EngineHeuristic},
        {"PRICE_POSITION_RELATION", CftcInterpretationScope::ContemporaneousRelation,
         CftcEvidenceBasis::EngineHeuristic},
    };
    for (const FamilyScope& family : families) {
        result.unavailable.append(cftc_unavailable_record(QString::fromLatin1(family.family), QString(), false, 0,
                                                          family.scope, family.basis, reason));
    }
}

/// The deterministic Batch 4A entry point. Pure function of its input: no
/// clock, no randomness, no network, no widget state.
/// Fail-closed provenance validation of the supplied history. Returns None
/// only when the declared report basis, source family and participant identity
/// are internally consistent with every observation. The engine never
/// reinterprets one family's participant slots as another's, never mixes
/// contracts and never guesses a silent report basis. The stable market
/// identity is the CFTC contract-market code; the display market name is
/// descriptive metadata and may legitimately change between reports (CFTC
/// documents contract name changes while the code stays fixed).
inline CftcUnavailableReason cftc_validate_provenance(const CftcInterpretationInput& input) {
    const QString basis = input.report_basis_code.trimmed().toLower();
    if (basis != QLatin1String("futures_only") && basis != QLatin1String("futures_and_options_combined"))
        return CftcUnavailableReason::ReportBasisUnspecified;
    const QString family_code = input.observations_family_code.trimmed().toLower();
    const bool known_family = family_code == QLatin1String("legacy") || family_code == QLatin1String("disaggregated") ||
                              family_code == QLatin1String("disagg") || family_code == QLatin1String("tff") ||
                              family_code == QLatin1String("financial");
    if (!known_family || cftc_family_from_code(family_code) != input.family)
        return CftcUnavailableReason::FamilyProvenanceMismatch;
    const int expected_slots = cftc_family_participants(input.family).size();
    const QString contract = input.observations.first().contract_code.trimmed();
    for (const auto& observation : input.observations) {
        if (observation.longs.size() != expected_slots || observation.shorts.size() != expected_slots)
            return CftcUnavailableReason::ParticipantCountMismatch;
        const QString row_contract = observation.contract_code.trimmed();
        if (row_contract.isEmpty())
            return CftcUnavailableReason::MissingContractIdentity;
        if (row_contract != contract)
            return CftcUnavailableReason::MixedContractIdentity;
    }
    return CftcUnavailableReason::None;
}

/// The deterministic Batch 4A entry point. Pure function of its input: no
/// clock, no randomness, no network, no widget state.
inline CftcInterpretationResult cftc_interpret(const CftcInterpretationInput& input) {
    CftcInterpretationResult result;
    // Only the plan's supported report horizons and first-seen values run, and
    // the result carries the truthful identity of the effective configuration.
    const CftcInterpretationConfig effective = cftc_effective_interpretation_config(input.config);
    result.config = effective;
    result.config_is_v1 = cftc_interpretation_config_is_v1(effective);
    result.rule_set_version = cftc_interpretation_rule_set_version_for(effective);
    result.family = input.family;
    result.family_code = cftc_family_code(input.family);
    const QString requested_basis = input.report_basis_code.trimmed().toLower();
    if (requested_basis == QLatin1String("futures_only") ||
        requested_basis == QLatin1String("futures_and_options_combined")) {
        result.report_basis = requested_basis;
        result.futures_only = requested_basis == QLatin1String("futures_only");
    }
    result.price_requested = !input.prices.isEmpty();
    result.price_source = input.price_source;
    result.price_continuous_proxy = input.price_continuous_proxy;
    result.price_spot_index = input.price_spot_index;
    const QVector<CftcParticipant> participants = cftc_family_participants(input.family);
    if (!input.observations.isEmpty())
        result.latest_observation_date = input.observations.last().date;

    if (input.observations.isEmpty()) {
        result.report_date = input.as_of;
        cftc_append_report_unavailable(result, participants, CftcUnavailableReason::NoObservations);
        return result;
    }

    const CftcUnavailableReason provenance = cftc_validate_provenance(input);
    if (provenance != CftcUnavailableReason::None) {
        result.report_date = input.as_of.isValid() ? input.as_of : input.observations.last().date;
        cftc_append_report_unavailable(result, participants, provenance);
        return result;
    }

    int current_index = -1;
    bool stale = false;
    if (input.as_of.isValid()) {
        for (int i = 0; i < input.observations.size(); ++i) {
            if (input.observations[i].date <= input.as_of)
                current_index = i;
            else
                break;
        }
        if (current_index >= 0 && input.observations[current_index].date != input.as_of)
            stale = true;
    } else {
        current_index = input.observations.size() - 1;
    }

    if (current_index < 0) {
        result.report_date = input.as_of;
        result.current_report_stale = true;
        cftc_append_report_unavailable(result, participants, CftcUnavailableReason::StaleCurrentReport);
        return result;
    }

    const QVector<CftcObservation> history = input.observations.mid(0, current_index + 1);
    const CftcObservation& current = history.last();
    result.report_date = input.as_of.isValid() ? input.as_of : current.date;
    result.current_report_stale = stale;
    result.report_date_available = !stale;
    result.market = current.market;
    result.contract_code = current.contract_code;
    result.units = current.units;
    result.open_interest_available = current.open_interest.has_value();
    if (current.open_interest)
        result.open_interest = *current.open_interest;

    if (stale) {
        cftc_append_report_unavailable(result, participants, CftcUnavailableReason::StaleCurrentReport);
        return result;
    }

    const bool price_source_specified = !input.price_source.trimmed().isEmpty();
    for (int i = 0; i < participants.size(); ++i) {
        result.participants.append(cftc_interpret_participant(history, i, participants[i], input.family, effective,
                                                              input.prices, price_source_specified,
                                                              result.price_context, result.unavailable));
    }
    cftc_interpret_open_interest(history, effective, result);
    cftc_interpret_concentration(history, effective, result);
    return result;
}

} // namespace fincept::services
