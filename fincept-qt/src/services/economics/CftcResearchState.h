// src/services/economics/CftcResearchState.h
//
// Batch 2 of the CFTC research engine: the bounded, CFTC-specific evidence
// evaluator and the provisional v0 COT Research State (BUY | HOLD | SELL).
//
// This layer is a research interpretation, not an execution signal. The v0
// rules below are hypotheses for Batch 3 historical validation; nothing here
// claims profitability, predictive power, or qualification.
//
// Design (CFTC_RESEARCH_ENGINE_PLAN.md sections 3, 6, 7, 8):
//
//   * Direction comes from two independently evaluated horizons — 4W tactical
//     and 13W swing. Each emits one family state (Bullish | Neutral | Bearish |
//     Unavailable) from a coherent combination of its sub-signals; a family's
//     metrics are never counted as separate votes. Price/COT, Open Interest and
//     participant confirmation are separate evidence families (plan section 6),
//     so the tactical/swing states stay positioning-only and cannot double-count
//     their own confirmation.
//   * A 26W regime reading is exposed as context and never votes.
//   * Historical context (2Y/5Y trailing normalization) classifies crowding and
//     can only become directional when a recent 4W reversal exists, and even
//     then it never satisfies the independence requirement because it derives
//     from the same positioning series. A crowded position that is still
//     strengthening is not a contrarian signal. Disagreeing percentile / COT
//     Index / z-score views become a Mixed context, never a code-order pick.
//   * BUY or SELL requires agreement from at least two independent evidence
//     families, at least one of which is a directional-core horizon (4W/13W).
//   * Percentile, COT Index and z-score are one historical-context family, not
//     three votes. ΔNet and ΔNet %OI are one positioning-change signal per
//     horizon, not two votes. A moving-average reading whose window spans a
//     missing report is marked gapped and cannot vote as an ordinary
//     continuous-window signal.
//   * Unavailable evidence is omitted, never scored as zero or neutral. A stale
//     speculative series is not replaced by the prior report.
//   * Confidence is a coverage/agreement measure under the v0 rules, not a
//     probability. Missing families, core disagreement and visible conflicts
//     lower it, and a conflict is penalized once per family so several
//     correlated items cannot multiply the penalty. Participant context-only
//     classes (commercial, producer/merchant, swap dealers, dealers) do not
//     count as participant confirmation coverage.
//
// Trader-count and concentration fields are retained by Batch 1 but are exposed
// here only as secondary context: their interpretation is not established
// enough for v0 direction or confidence. The engine never uses them to vote.
//
// The engine is deterministic, header-only over Qt Core, performs no network
// access, holds no broker/execution authority and can replay historical inputs.
// It produces no HTML, widget text or colors.
#pragma once

#include "services/economics/CftcMetricModel.h"

#include <QDate>
#include <QString>
#include <QVector>

#include <cmath>
#include <optional>

namespace fincept::services {

/// The exact rule-set identity every result carries. Batch 3 replay results must
/// be tied to this version; changing any rule, threshold or aggregation below
/// requires a new version string.
inline QString cftc_research_rule_set_version() {
    return QStringLiteral("cftc-cot-state-v0");
}

// ── Evidence vocabulary ─────────────────────────────────────────────────────

/// The bounded evidence families of the plan (section 6). Only the families
/// whose semantics are justified by the finalized Batch 1 metrics are
/// implemented; each group carries `counts_for_independence` so a reviewer can
/// see which groups may satisfy the BUY/SELL independence requirement.
enum class CftcEvidenceFamily {
    Tactical4W,
    Swing13W,
    Regime26W,
    PriceCot,
    OpenInterest,
    Participant,
    HistoricalContext,
    DataQuality,
};

inline QString cftc_evidence_family_code(CftcEvidenceFamily family) {
    switch (family) {
        case CftcEvidenceFamily::Tactical4W:
            return QStringLiteral("tactical_4w");
        case CftcEvidenceFamily::Swing13W:
            return QStringLiteral("swing_13w");
        case CftcEvidenceFamily::Regime26W:
            return QStringLiteral("regime_26w");
        case CftcEvidenceFamily::PriceCot:
            return QStringLiteral("price_cot");
        case CftcEvidenceFamily::OpenInterest:
            return QStringLiteral("open_interest");
        case CftcEvidenceFamily::Participant:
            return QStringLiteral("participant");
        case CftcEvidenceFamily::HistoricalContext:
            return QStringLiteral("historical_context");
        case CftcEvidenceFamily::DataQuality:
            return QStringLiteral("data_quality");
    }
    return {};
}

enum class CftcEvidenceDirection { Unavailable, Neutral, Bullish, Bearish };

inline QString cftc_evidence_direction_code(CftcEvidenceDirection direction) {
    switch (direction) {
        case CftcEvidenceDirection::Neutral:
            return QStringLiteral("neutral");
        case CftcEvidenceDirection::Bullish:
            return QStringLiteral("bullish");
        case CftcEvidenceDirection::Bearish:
            return QStringLiteral("bearish");
        case CftcEvidenceDirection::Unavailable:
            break;
    }
    return QStringLiteral("unavailable");
}

enum class CftcEvidenceStrength { None, Weak, Moderate, Strong };

inline QString cftc_evidence_strength_code(CftcEvidenceStrength strength) {
    switch (strength) {
        case CftcEvidenceStrength::Weak:
            return QStringLiteral("weak");
        case CftcEvidenceStrength::Moderate:
            return QStringLiteral("moderate");
        case CftcEvidenceStrength::Strong:
            return QStringLiteral("strong");
        case CftcEvidenceStrength::None:
            break;
    }
    return QStringLiteral("none");
}

enum class CftcTacticalState { Unavailable, Neutral, Bullish, Bearish };

inline QString cftc_tactical_state_code(CftcTacticalState state) {
    switch (state) {
        case CftcTacticalState::Neutral:
            return QStringLiteral("neutral");
        case CftcTacticalState::Bullish:
            return QStringLiteral("bullish");
        case CftcTacticalState::Bearish:
            return QStringLiteral("bearish");
        case CftcTacticalState::Unavailable:
            break;
    }
    return QStringLiteral("unavailable");
}

enum class CftcHistoricalContext { Unavailable, Neutral, CrowdedLong, CrowdedShort, Mixed };

inline QString cftc_historical_context_code(CftcHistoricalContext context) {
    switch (context) {
        case CftcHistoricalContext::Neutral:
            return QStringLiteral("neutral");
        case CftcHistoricalContext::CrowdedLong:
            return QStringLiteral("crowded_long");
        case CftcHistoricalContext::CrowdedShort:
            return QStringLiteral("crowded_short");
        case CftcHistoricalContext::Mixed:
            return QStringLiteral("mixed");
        case CftcHistoricalContext::Unavailable:
            break;
    }
    return QStringLiteral("unavailable");
}

enum class CftcResearchState { Buy, Hold, Sell };

inline QString cftc_research_state_code(CftcResearchState state) {
    switch (state) {
        case CftcResearchState::Buy:
            return QStringLiteral("buy");
        case CftcResearchState::Sell:
            return QStringLiteral("sell");
        case CftcResearchState::Hold:
            break;
    }
    return QStringLiteral("hold");
}

enum class CftcResearchConfidence { Low, Medium, High };

inline QString cftc_research_confidence_code(CftcResearchConfidence confidence) {
    switch (confidence) {
        case CftcResearchConfidence::Medium:
            return QStringLiteral("medium");
        case CftcResearchConfidence::High:
            return QStringLiteral("high");
        case CftcResearchConfidence::Low:
            break;
    }
    return QStringLiteral("low");
}

// ── Structured rule definitions ─────────────────────────────────────────────

/// The structured definition of one v0 rule. Every emitted evidence item names
/// one of these stable ids, so "what condition fired, on which horizon, from
/// which inputs" is answerable from the result alone. Conditions are stated as
/// formulas over Batch 1 primitives; the conventional thresholds used here are
/// provisional hypotheses for Batch 3, not optimized parameters.
struct CftcResearchRule {
    QString id;
    QString family;  // stable family code
    QString horizon; // "1W", "4W", "13W", "26W", "2Y", "current"
    QString required_inputs;
    QString condition;
    QString interpretation;
    QString version = cftc_research_rule_set_version();
};

inline const QVector<CftcResearchRule>& cftc_research_rules() {
    static const QVector<CftcResearchRule> rules = {
        {QStringLiteral("RDQ-REPORT-CURRENT"), QStringLiteral("data_quality"), QStringLiteral("current"),
         QStringLiteral("speculative Net series; official as-of report"),
         QStringLiteral("series last report == as-of report; a series ending earlier is stale"),
         QStringLiteral("the state is only constructed from the current official report")},
        {QStringLiteral("RDQ-HISTORY-COVERAGE"), QStringLiteral("data_quality"), QStringLiteral("26W-5Y"),
         QStringLiteral("strictly trailing 26W/52W/2Y/5Y reference windows"),
         QStringLiteral("reference_covered and reference_count per window"),
         QStringLiteral("records which historical references exist; incomplete history is not normal positioning")},
        {QStringLiteral("RDQ-CONCENTRATION-CONTEXT"), QStringLiteral("data_quality"), QStringLiteral("current"),
         QStringLiteral("traders_* and concentration_* provider fields"),
         QStringLiteral("fields present on the as-of report"),
         QStringLiteral("secondary context only; not used for direction or confidence in v0")},

        {QStringLiteral("R4W-NET-CHANGE"), QStringLiteral("tactical_4w"), QStringLiteral("4W"),
         QStringLiteral("ΔNet(4W), ΔNet %OI(4W), ΔLong(4W), ΔShort(4W)"),
         QStringLiteral("combined sign of ΔNet and ΔNet %OI over the shared 4W anchor"),
         QStringLiteral("recent speculative accumulation (bullish) or distribution (bearish)")},
        {QStringLiteral("R4W-TREND"), QStringLiteral("tactical_4w"), QStringLiteral("4W"),
         QStringLiteral("Net minus 4-report mean; 4-report mean slope"),
         QStringLiteral("combined sign of the position-vs-mean reading and the mean slope"),
         QStringLiteral("whether the 4W positioning move sits above/below its recent trend")},
        {QStringLiteral("R4W-PERSISTENCE"), QStringLiteral("tactical_4w"), QStringLiteral("1W x N"),
         QStringLiteral("consecutive same-direction weekly Net steps"),
         QStringLiteral("persistence direction and run length >= 2"),
         QStringLiteral("a single noisy week cannot carry the tactical state")},
        {QStringLiteral("R4W-EXTREME"), QStringLiteral("tactical_4w"), QStringLiteral("2Y+4W"),
         QStringLiteral("2Y trailing min/max thresholds; extreme state; ΔNet(4W)"),
         QStringLiteral(
             "previous report outside the 2Y range and current back inside while ΔNet opposes the extreme side"),
         QStringLiteral(
             "return-from-extreme combined with recent positioning behavior; an extreme alone is not a trade")},

        {QStringLiteral("R13W-NET-CHANGE"), QStringLiteral("swing_13w"), QStringLiteral("13W"),
         QStringLiteral("ΔNet(13W), ΔNet %OI(13W), ΔLong(13W), ΔShort(13W)"),
         QStringLiteral("combined sign of ΔNet and ΔNet %OI over the shared 13W anchor"),
         QStringLiteral("sustained speculative accumulation (bullish) or distribution (bearish)")},
        {QStringLiteral("R13W-TREND"), QStringLiteral("swing_13w"), QStringLiteral("13W"),
         QStringLiteral("Net minus 13-report mean; 13-report mean slope"),
         QStringLiteral("combined sign of the position-vs-mean reading and the mean slope"),
         QStringLiteral("whether the 13W move sits above/below its swing trend")},
        {QStringLiteral("R13W-SUSTAINED-PERSISTENCE"), QStringLiteral("swing_13w"), QStringLiteral("13W"),
         QStringLiteral("weekly persistence run start date; 13W anchor report date"),
         QStringLiteral("run direction unchanged since at or before the 13W anchor and run length >= 2"),
         QStringLiteral("distinguishes sustained accumulation/distribution from a shorter tactical run")},
        {QStringLiteral("R13W-EXTREME"), QStringLiteral("swing_13w"), QStringLiteral("2Y+13W"),
         QStringLiteral("2Y trailing min/max thresholds; extreme state; ΔNet(13W)"),
         QStringLiteral(
             "previous report outside the 2Y range and current back inside while ΔNet opposes the extreme side"),
         QStringLiteral("return-from-extreme at the swing horizon; requires recent behavior")},

        {QStringLiteral("R26W-NET-CHANGE"), QStringLiteral("regime_26w"), QStringLiteral("26W"),
         QStringLiteral("ΔNet(26W), ΔNet %OI(26W)"),
         QStringLiteral("combined sign of ΔNet and ΔNet %OI over the shared 26W anchor"),
         QStringLiteral("context only: never votes, never satisfies independence, never changes confidence in v0")},
        {QStringLiteral("R26W-TREND"), QStringLiteral("regime_26w"), QStringLiteral("26W"),
         QStringLiteral("Net minus 26-report mean; 26-report mean slope"),
         QStringLiteral("combined sign of the position-vs-mean reading and the mean slope"),
         QStringLiteral("context only: never votes, never satisfies independence, never changes confidence in v0")},

        {QStringLiteral("RPC-4W-RELATIONSHIP"), QStringLiteral("price_cot"), QStringLiteral("4W"),
         QStringLiteral("qualified price change (28d); ΔNet(4W)"),
         QStringLiteral("same-direction = alignment; opposite = divergence; missing price = unavailable"),
         QStringLiteral("confirmation or divergence; divergence is not an automatic reversal")},
        {QStringLiteral("RPC-13W-RELATIONSHIP"), QStringLiteral("price_cot"), QStringLiteral("13W"),
         QStringLiteral("qualified price change (91d); ΔNet(13W)"),
         QStringLiteral("same-direction = alignment; opposite = divergence; missing price = unavailable"),
         QStringLiteral("confirmation or divergence over the swing horizon")},

        {QStringLiteral("ROI-4W-RELATIONSHIP"), QStringLiteral("open_interest"), QStringLiteral("4W"),
         QStringLiteral("ΔOpenInterest(4W); ΔNet(4W)"),
         QStringLiteral("OI expansion in the Net direction confirms; OI contraction is caution, never a reversal"),
         QStringLiteral("participation confirmation/context only, never a dominant signal")},
        {QStringLiteral("ROI-13W-RELATIONSHIP"), QStringLiteral("open_interest"), QStringLiteral("13W"),
         QStringLiteral("ΔOpenInterest(13W); ΔNet(13W)"),
         QStringLiteral("OI expansion in the Net direction confirms; OI contraction is caution, never a reversal"),
         QStringLiteral("participation confirmation/context over the swing horizon")},

        {QStringLiteral("RPART-LEG-NON-REPORTABLE"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("legacy Non-Reportable 13W Net change; speculative 13W direction"),
         QStringLiteral("same direction confirms; opposite direction conflicts"),
         QStringLiteral("corroboration from the legacy family's other non-hedging class")},
        {QStringLiteral("RPART-LEG-COMMERCIAL"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("legacy Commercial 13W Net change"), QStringLiteral("context only"),
         QStringLiteral("a hedging class is exposed separately and is never directional in v0")},

        {QStringLiteral("RPART-DIS-OTHER-REPORTABLE"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("disaggregated Other Reportables 13W Net change; Managed Money 13W direction"),
         QStringLiteral("same direction confirms; opposite direction conflicts"),
         QStringLiteral("corroboration from the disaggregated family's other non-hedging class")},
        {QStringLiteral("RPART-DIS-PRODUCER-MERCHANT"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("disaggregated Producer/Merchant/Processor/User 13W Net change"),
         QStringLiteral("context only; never collapsed with Swap Dealers"),
         QStringLiteral("kept separately identifiable, not a generic commercial vote")},
        {QStringLiteral("RPART-DIS-SWAP-DEALER"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("disaggregated Swap Dealers 13W Net change"),
         QStringLiteral("context only; never collapsed with Producer/Merchant"),
         QStringLiteral("kept separately identifiable, not a generic commercial vote")},

        {QStringLiteral("RPART-TFF-ASSET-MANAGER"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("TFF Asset Manager/Institutional 13W Net change; Leveraged Funds 13W direction"),
         QStringLiteral("same direction confirms; opposite direction conflicts"),
         QStringLiteral("corroboration from a named non-dealer reportable class")},
        {QStringLiteral("RPART-TFF-OTHER-REPORTABLE"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("TFF Other Reportables 13W Net change; Leveraged Funds 13W direction"),
         QStringLiteral("same direction confirms; opposite direction conflicts"),
         QStringLiteral("separate corroboration from the other named non-dealer class")},
        {QStringLiteral("RPART-TFF-DEALER"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("TFF Dealer/Intermediary 13W Net change"), QStringLiteral("context only"),
         QStringLiteral("TFF has no Commercial/Speculator split and none is invented")},
        {QStringLiteral("RPART-TFF-NON-REPORTABLE"), QStringLiteral("participant"), QStringLiteral("13W"),
         QStringLiteral("TFF Non-Reportable 13W Net change"), QStringLiteral("context only"),
         QStringLiteral("small-trader class exposed separately, never directional in v0")},

        {QStringLiteral("RHIST-CROWDING"), QStringLiteral("historical_context"), QStringLiteral("2Y/5Y"),
         QStringLiteral("strictly trailing 2Y percentile/COT Index/z-score; 5Y percentile"),
         QStringLiteral("crowded long: percentile >= 90 or COT Index >= 90 or z >= 1.5; "
                        "crowded short: percentile <= 10 or COT Index <= 10 or z <= -1.5; else neutral"),
         QStringLiteral("classifies how unusual current positioning is; context, not direction")},
        {QStringLiteral("RHIST-EXTREME-STATE"), QStringLiteral("historical_context"), QStringLiteral("2Y"),
         QStringLiteral("2Y trailing min/max; extreme state streaks and prior extremes"),
         QStringLiteral("reports at/above or at/below the trailing range, persistence and exits"),
         QStringLiteral("an extreme or an extreme exit is not automatically a contrarian trade")},
        {QStringLiteral("RHIST-REVERSAL"), QStringLiteral("historical_context"), QStringLiteral("2Y+4W"),
         QStringLiteral("crowding classification; 4W tactical state; extreme exit state"),
         QStringLiteral("crowded long with 4W Bearish, or crowded short with 4W Bullish"),
         QStringLiteral("reinforces a recent reversal; the same positioning series is not independent, so this rule "
                        "never satisfies the BUY/SELL independence requirement")},
        {QStringLiteral("RHIST-CROWDED-CONTINUATION"), QStringLiteral("historical_context"), QStringLiteral("2Y+4W"),
         QStringLiteral("crowding classification; 4W tactical state"),
         QStringLiteral("crowded positioning whose recent 4W move is not a reversal"),
         QStringLiteral(
             "a historically crowded position that is still strengthening is not a SELL (or BUY) by itself")},
        {QStringLiteral("RHIST-EXTREME-EXIT-NO-REVERSAL"), QStringLiteral("historical_context"),
         QStringLiteral("2Y+4W"), QStringLiteral("extreme state exit next to the current report; 4W tactical state"),
         QStringLiteral("an adjacent extreme exit without a matching 4W reversal"),
         QStringLiteral("the exit is exposed as context, not converted into direction")},

        // Aggregation and confidence rules. These do not emit evidence items;
        // they are versioned metadata so Batch 3 replay can bind the final
        // state and confidence to the exact rules that produced them.
        {QStringLiteral("RSTATE-HORIZON-AGGREGATION"), QStringLiteral("aggregation"), QStringLiteral("4W/13W/26W"),
         QStringLiteral("one family's position-change, trend, persistence and extreme-return signals"),
         QStringLiteral("score = sum of signed signal weights (position-change ±2/±1, trend ±2/±1, persistence ±1, "
                        "extreme-return ±2); Bullish iff score >= 2 and no opposing sub-signal; Bearish symmetric; "
                        "otherwise Neutral; strength weak/moderate/strong at |score| 2/3/5"),
         QStringLiteral("one coherent direction per horizon family; correlated sub-metrics are combined, not voted "
                        "separately")},
        {QStringLiteral("RSTATE-INDEPENDENCE-GATE"), QStringLiteral("aggregation"), QStringLiteral("4W+13W"),
         QStringLiteral("available independent family directions; core opposition state"),
         QStringLiteral("BUY/SELL iff the 4W/13W core is not opposed, at least two independent families agree, and "
                        "at least one of them is a core horizon; otherwise HOLD"),
         QStringLiteral("prevents one family or several correlated metrics from carrying a directional state; "
                        "historical context and the 26W regime never satisfy this gate")},
        {QStringLiteral("RSTATE-CONFIDENCE"), QStringLiteral("confidence"), QStringLiteral("result"),
         QStringLiteral("family coverage, core agreement, family-level conflicts and core opposition"),
         QStringLiteral("score = coverage (five independent families + historical availability) + 1 coherent core "
                        "- 2 core opposition - 1 per conflicted or opposing family - 1 more for core opposition "
                        "(core opposition therefore removes 3 points in total and blocks High); High iff score >= 7 "
                        "and zero conflicts; Medium iff score >= 3; Low otherwise or when data is unavailable or a "
                        "directional tendency failed the independence gate"),
         QStringLiteral("coverage/agreement measure under v0, not a probability; correlated items inside one family "
                        "cost one penalty")},
    };
    return rules;
}

// ── Structured evidence ─────────────────────────────────────────────────────

/// One exact metric behind an evidence item. `has_value == false` records that
/// the metric was unavailable; it is never presented as a zero.
struct CftcEvidenceMetric {
    QString key;
    bool has_value = false;
    double value = 0.0;
};

/// One evaluated rule instance. `explanation` states the actual relationship
/// with the real values and horizon, e.g. "Price declined 5.00 over 13W while
/// Non-Commercial Net increased 12.00". `conflicted` marks evidence that argues
/// against the rest of the evaluation rather than a clean directional read.
struct CftcEvidenceItem {
    QString rule_id;
    CftcEvidenceFamily family = CftcEvidenceFamily::DataQuality;
    QString horizon;
    CftcEvidenceDirection direction = CftcEvidenceDirection::Unavailable;
    CftcEvidenceStrength strength = CftcEvidenceStrength::None;
    bool available = false;
    bool conflicted = false;
    QString explanation;
    QVector<CftcEvidenceMetric> metrics;
};

/// One evidence family's result. `direction` is the family's single vote (if
/// `counts_for_independence`), not a count of its items. `counts_for_confidence`
/// says the family's availability is part of the confidence coverage; historical
/// context counts there but never for the direction independence requirement.
struct CftcEvidenceGroup {
    CftcEvidenceFamily family = CftcEvidenceFamily::DataQuality;
    CftcEvidenceDirection direction = CftcEvidenceDirection::Unavailable;
    CftcEvidenceStrength strength = CftcEvidenceStrength::None;
    bool available = false;
    bool counts_for_independence = false;
    bool counts_for_confidence = false;
    QVector<CftcEvidenceItem> items;
};

// ── Engine input and result ─────────────────────────────────────────────────

/// Everything the engine may use. It performs no acquisition: observations come
/// from the finalized Batch 1 CFTC path and prices from the existing qualified
/// price path. `effective_date` is the caller's authoritative
/// publication/effective date when one is truthfully known; it is never
/// synthesized here.
struct CftcResearchInput {
    CftcFamily family = CftcFamily::Legacy;
    QVector<CftcObservation> observations; // ascending official reports
    QDate as_of;                           // official as-of report; invalid -> newest returned
    QDate effective_date;                  // publication/effective date if truthfully known
    QVector<CftcPricePoint> prices;        // ascending qualified closes; empty -> price unavailable
    QString price_source;                  // e.g. "continuous front-month proxy"
    bool price_continuous_proxy = false;   // semantics remain external metadata
    bool price_spot_index = false;
    int trailing_min_reference = kCftcTrailingMinObservations;
};

/// Exact Batch 1 primitive readings behind the evidence, exposed for replay and
/// audit. Presence flags stay false when the underlying statistic is undefined;
/// nothing here substitutes zero for missing data.
struct CftcStateReadings {
    bool data_available = false;
    bool data_stale = false;
    CftcPositionChanges changes_4w;
    CftcPositionChanges changes_13w;
    CftcPositionChanges changes_26w;
    CftcMetricReading net_minus_ma_4w;
    CftcMetricReading ma_slope_4w;
    CftcMetricReading net_minus_ma_13w;
    CftcMetricReading ma_slope_13w;
    CftcMetricReading net_minus_ma_26w;
    CftcMetricReading ma_slope_26w;
    CftcPositioningPersistence persistence;
    CftcTrailingStats stats_26w;
    CftcTrailingStats stats_52w;
    CftcTrailingStats stats_2y;
    CftcTrailingStats stats_5y;
    bool extreme_thresholds_available = false;
    CftcExtremeState extreme;
    CftcHorizonChange open_interest_change_4w;
    CftcHorizonChange open_interest_change_13w;
    bool price_change_4w_available = false;
    double price_change_4w = 0.0;
    bool price_change_13w_available = false;
    double price_change_13w = 0.0;
    bool price_series_supplied = false;
    bool price_series_fresh = false;
    QDate latest_price_date;
};

/// The deterministic provisional research state. UI-independent: no colors, no
/// markup, no widget text. Supporting/conflicting/unavailable lists expose the
/// items behind the state relative to the selected direction (or the strongest
/// provisional tendency when the state is HOLD).
struct CftcResearchResult {
    QString rule_set_version;
    CftcFamily family = CftcFamily::Legacy;
    QString family_code;
    QString primary_speculative_key;
    QString primary_speculative_label;
    QDate report_date;
    QDate latest_available_report_date;

    bool data_available = false;
    bool data_stale = false;
    bool report_missing = false;
    bool report_predates_history = false;

    bool effective_date_known = false;
    QDate effective_date;

    bool price_used = false;
    QString price_source;
    bool price_continuous_proxy = false;
    bool price_spot_index = false;

    CftcResearchState state = CftcResearchState::Hold;
    CftcResearchConfidence confidence = CftcResearchConfidence::Low;
    CftcTacticalState tactical_4w = CftcTacticalState::Unavailable;
    CftcTacticalState swing_13w = CftcTacticalState::Unavailable;
    CftcTacticalState regime_26w = CftcTacticalState::Unavailable;
    CftcHistoricalContext historical_context = CftcHistoricalContext::Unavailable;

    CftcStateReadings readings;
    QVector<CftcEvidenceGroup> groups;
    QVector<CftcEvidenceItem> supporting;
    QVector<CftcEvidenceItem> conflicting;
    QVector<CftcEvidenceItem> unavailable;
};

// ── Internal signal combination (one signal per underlying move) ────────────

/// The result of combining correlated measurements of the same underlying
/// positioning information into one signal. `weight` is the signal's bounded
/// contribution inside its family; `conflicted` marks measurements that
/// disagree with each other (for example ΔNet rising while ΔNet %OI falls).
struct CftcSignal {
    CftcEvidenceDirection direction = CftcEvidenceDirection::Unavailable;
    int weight = 0;
    bool available = false;
    bool conflicted = false;
    bool gapped = false; // a contributing reading's window spans a missing report

    int sign() const {
        if (direction == CftcEvidenceDirection::Bullish)
            return 1;
        if (direction == CftcEvidenceDirection::Bearish)
            return -1;
        return 0;
    }
};

inline CftcSignal cftc_combine_change_pair(const CftcHorizonChange& net, const CftcHorizonChange& net_pct_oi) {
    CftcSignal out;
    const bool has_net = net.has_value;
    const bool has_pct = net_pct_oi.has_value;
    if (!has_net && !has_pct)
        return out;
    out.available = true;
    const CftcDirection net_dir = has_net ? cftc_direction(net.value) : CftcDirection::Unavailable;
    const CftcDirection pct_dir = has_pct ? cftc_direction(net_pct_oi.value) : CftcDirection::Unavailable;
    const auto as_evidence = [](CftcDirection direction) {
        if (direction == CftcDirection::Up)
            return CftcEvidenceDirection::Bullish;
        if (direction == CftcDirection::Down)
            return CftcEvidenceDirection::Bearish;
        return CftcEvidenceDirection::Neutral;
    };
    if (has_net && has_pct) {
        if (net_dir == pct_dir) {
            out.direction = as_evidence(net_dir);
            out.weight = out.direction == CftcEvidenceDirection::Neutral ? 0 : 2;
            return out;
        }
        if (net_dir == CftcDirection::Flat || pct_dir == CftcDirection::Flat) {
            const CftcDirection moving = net_dir == CftcDirection::Flat ? pct_dir : net_dir;
            out.direction = as_evidence(moving);
            out.weight = out.direction == CftcEvidenceDirection::Neutral ? 0 : 1;
            return out;
        }
        out.direction = CftcEvidenceDirection::Neutral;
        out.weight = 0;
        out.conflicted = true;
        return out;
    }
    out.direction = as_evidence(has_net ? net_dir : pct_dir);
    out.weight = out.direction == CftcEvidenceDirection::Neutral ? 0 : 1;
    return out;
}

inline CftcSignal cftc_combine_reading_pair(const CftcMetricReading& position_vs_mean, const CftcMetricReading& slope) {
    CftcSignal out;
    // A reading whose window spans a missing report is not an ordinary
    // continuous-window reading: it is excluded from the vote. If both readings
    // are gapped, the trend signal itself is unavailable rather than neutral.
    const bool position_gapped = position_vs_mean.has_value && position_vs_mean.gapped;
    const bool slope_gapped = slope.has_value && slope.gapped;
    out.gapped = position_gapped || slope_gapped;
    const bool has_position = position_vs_mean.has_value && !position_gapped;
    const bool has_slope = slope.has_value && !slope_gapped;
    if (!has_position && !has_slope)
        return out;
    out.available = true;
    const CftcDirection position_dir =
        has_position ? cftc_direction(position_vs_mean.value) : CftcDirection::Unavailable;
    const CftcDirection slope_dir = has_slope ? cftc_direction(slope.value) : CftcDirection::Unavailable;
    const auto as_evidence = [](CftcDirection direction) {
        if (direction == CftcDirection::Up)
            return CftcEvidenceDirection::Bullish;
        if (direction == CftcDirection::Down)
            return CftcEvidenceDirection::Bearish;
        return CftcEvidenceDirection::Neutral;
    };
    if (has_position && has_slope) {
        if (position_dir == slope_dir) {
            out.direction = as_evidence(position_dir);
            out.weight = out.direction == CftcEvidenceDirection::Neutral ? 0 : 2;
            return out;
        }
        if (position_dir == CftcDirection::Flat || slope_dir == CftcDirection::Flat) {
            const CftcDirection moving = position_dir == CftcDirection::Flat ? slope_dir : position_dir;
            out.direction = as_evidence(moving);
            out.weight = out.direction == CftcEvidenceDirection::Neutral ? 0 : 1;
            return out;
        }
        out.direction = CftcEvidenceDirection::Neutral;
        out.weight = 0;
        out.conflicted = true;
        return out;
    }
    out.direction = as_evidence(has_position ? position_dir : slope_dir);
    out.weight = out.direction == CftcEvidenceDirection::Neutral ? 0 : 1;
    return out;
}

/// Weekly-run persistence signal for the 4W horizon: the latest run must be at
/// least `min_changes` same-direction weekly steps, so one noisy week cannot
/// carry a family state.
inline CftcSignal cftc_weekly_persistence_signal(const CftcPositioningPersistence& persistence, int min_changes) {
    CftcSignal out;
    if (!persistence.has_value)
        return out;
    out.available = true;
    if (persistence.changes < min_changes)
        return out;
    if (persistence.direction == CftcDirection::Up) {
        out.direction = CftcEvidenceDirection::Bullish;
        out.weight = 1;
    } else if (persistence.direction == CftcDirection::Down) {
        out.direction = CftcEvidenceDirection::Bearish;
        out.weight = 1;
    }
    return out;
}

/// Sustained-run persistence for the 13W horizon: the current same-direction
/// weekly run must already have started at or before the 13W anchor report, so
/// it describes accumulation/distribution over the swing window rather than
/// repeating the tactical run.
inline CftcSignal cftc_sustained_persistence_signal(const CftcPositioningPersistence& persistence, int min_changes,
                                                    const CftcHorizonAnchor& anchor) {
    CftcSignal out = cftc_weekly_persistence_signal(persistence, min_changes);
    if (!out.available || out.weight == 0)
        return out;
    if (!anchor.has_value || !anchor.anchor_date.isValid() || !persistence.since_date.isValid() ||
        persistence.since_date > anchor.anchor_date) {
        out.direction = CftcEvidenceDirection::Neutral;
        out.weight = 0;
    }
    return out;
}

/// Return-from-extreme signal, only when the previous report left a strictly
/// trailing 2Y extreme and the horizon's recent Net change moves away from that
/// extreme. An extreme that persists, or an exit without a matching recent
/// move, produces no directional signal.
inline CftcSignal cftc_extreme_return_signal(const CftcExtremeState& extreme, const CftcHorizonChange& net_change) {
    CftcSignal out;
    if (!extreme.has_value)
        return out;
    out.available = true;
    const CftcDirection recent = net_change.has_value ? cftc_direction(net_change.value) : CftcDirection::Unavailable;
    if (extreme.left_upper && recent == CftcDirection::Down) {
        out.direction = CftcEvidenceDirection::Bearish;
        out.weight = 2;
    } else if (extreme.left_lower && recent == CftcDirection::Up) {
        out.direction = CftcEvidenceDirection::Bullish;
        out.weight = 2;
    }
    return out;
}

// ── Horizon assessment ──────────────────────────────────────────────────────

/// One horizon's coherent tactical interpretation. The bounded internal score
/// combines the horizon's signals (position change, trend, persistence, extreme
/// return); it is a within-family coherence measure, and the family still emits
/// exactly one direction.
struct CftcHorizonAssessment {
    CftcTacticalState state = CftcTacticalState::Unavailable;
    CftcEvidenceStrength strength = CftcEvidenceStrength::None;
    int score = 0;
    CftcSignal position;
    CftcSignal trend;
    CftcSignal persistence;
    CftcSignal extreme;
};

inline CftcHorizonAssessment cftc_assess_horizon(const CftcSignal& position, const CftcSignal& trend,
                                                 const CftcSignal& persistence, const CftcSignal& extreme) {
    CftcHorizonAssessment out;
    out.position = position;
    out.trend = trend;
    out.persistence = persistence;
    out.extreme = extreme;
    const bool any = position.available || trend.available || persistence.weight > 0 || extreme.weight > 0;
    if (!any)
        return out;
    out.score = position.sign() * position.weight + trend.sign() * trend.weight +
                persistence.sign() * persistence.weight + extreme.sign() * extreme.weight;
    const bool no_opposition =
        position.sign() >= 0 && trend.sign() >= 0 && persistence.sign() >= 0 && extreme.sign() >= 0;
    const bool no_bullish = position.sign() <= 0 && trend.sign() <= 0 && persistence.sign() <= 0 && extreme.sign() <= 0;
    if (out.score >= 2 && no_opposition) {
        out.state = CftcTacticalState::Bullish;
    } else if (out.score <= -2 && no_bullish) {
        out.state = CftcTacticalState::Bearish;
    } else {
        out.state = CftcTacticalState::Neutral;
    }
    const int magnitude = std::abs(out.score);
    if (magnitude >= 5)
        out.strength = CftcEvidenceStrength::Strong;
    else if (magnitude >= 3)
        out.strength = CftcEvidenceStrength::Moderate;
    else if (magnitude >= 2)
        out.strength = CftcEvidenceStrength::Weak;
    return out;
}

// ── Formatting helpers (plain, locale-independent text) ─────────────────────

inline QString cftc_format_evidence_value(double value) {
    return QString::number(value, 'f', 2);
}

inline QString cftc_format_evidence_count(double value) {
    return QString::number(value, 'f', 0);
}

inline CftcEvidenceMetric cftc_evidence_metric(const QString& key, const std::optional<double>& value) {
    CftcEvidenceMetric out;
    out.key = key;
    out.has_value = value.has_value();
    if (value)
        out.value = *value;
    return out;
}

inline CftcEvidenceMetric cftc_evidence_metric(const QString& key, bool present, double value) {
    CftcEvidenceMetric out;
    out.key = key;
    out.has_value = present;
    out.value = value;
    return out;
}

inline QString cftc_change_direction_word(const CftcHorizonChange& change) {
    if (!change.has_value)
        return QStringLiteral("was unavailable");
    if (change.value > 0.0)
        return QStringLiteral("increased");
    if (change.value < 0.0)
        return QStringLiteral("decreased");
    return QStringLiteral("was unchanged");
}

inline CftcEvidenceStrength cftc_signal_strength(const CftcSignal& signal) {
    if (!signal.available || signal.weight == 0)
        return CftcEvidenceStrength::None;
    if (signal.conflicted)
        return CftcEvidenceStrength::None;
    return signal.weight >= 2 ? CftcEvidenceStrength::Moderate : CftcEvidenceStrength::Weak;
}

inline CftcEvidenceItem cftc_position_change_item(const QString& rule_id, CftcEvidenceFamily family,
                                                  const QString& horizon, const CftcSignal& signal,
                                                  const CftcPositionChanges& changes, const QString& participant_label,
                                                  bool metric_at_report, bool metric_at_anchor) {
    CftcEvidenceItem item;
    item.rule_id = rule_id;
    item.family = family;
    item.horizon = horizon;
    item.available = signal.available;
    item.direction = signal.direction;
    item.strength = cftc_signal_strength(signal);
    item.conflicted = signal.conflicted;
    item.metrics = {
        cftc_evidence_metric(QStringLiteral("net_change"),
                             changes.net.has_value ? std::optional<double>(changes.net.value) : std::nullopt),
        cftc_evidence_metric(QStringLiteral("net_pct_oi_change"), changes.net_pct_oi.has_value
                                                                      ? std::optional<double>(changes.net_pct_oi.value)
                                                                      : std::nullopt),
        cftc_evidence_metric(QStringLiteral("long_change"),
                             changes.long_leg.has_value ? std::optional<double>(changes.long_leg.value) : std::nullopt),
        cftc_evidence_metric(QStringLiteral("short_change"), changes.short_leg.has_value
                                                                 ? std::optional<double>(changes.short_leg.value)
                                                                 : std::nullopt),
    };
    if (!signal.available) {
        QString reason;
        if (changes.net.stale)
            reason = QStringLiteral("the Net series does not end at the requested as-of report");
        else if (!metric_at_report)
            reason = QStringLiteral("the Net value is missing at the official as-of report");
        else if (changes.net.has_anchor && !metric_at_anchor)
            reason = QStringLiteral("the Net value is missing at the shared horizon anchor report");
        else if (changes.net.has_anchor && !changes.net.has_value)
            reason = QStringLiteral("the nearest report anchor falls outside the horizon tolerance");
        else
            reason = QStringLiteral("the history does not reach back to the horizon anchor");
        item.explanation = QStringLiteral("%1 %2 Net change unavailable: %3.").arg(horizon, participant_label, reason);
        return item;
    }
    const QString net_text =
        changes.net.has_value ? cftc_format_evidence_count(changes.net.value) : QStringLiteral("unavailable");
    const QString pct_text = changes.net_pct_oi.has_value ? cftc_format_evidence_value(changes.net_pct_oi.value)
                                                          : QStringLiteral("unavailable");
    const QString long_text =
        changes.long_leg.has_value ? cftc_format_evidence_count(changes.long_leg.value) : QStringLiteral("unavailable");
    const QString short_text = changes.short_leg.has_value ? cftc_format_evidence_count(changes.short_leg.value)
                                                           : QStringLiteral("unavailable");
    QString headline;
    if (signal.conflicted) {
        headline = QStringLiteral("ambiguous");
    } else if (signal.sign() > 0) {
        headline = QStringLiteral("bullish");
    } else if (signal.sign() < 0) {
        headline = QStringLiteral("bearish");
    } else {
        headline = QStringLiteral("neutral");
    }
    item.explanation = QStringLiteral("%1 %2 Net change %3 (Net %OI change %4); gross long change %5, gross "
                                      "short change %6 — %7.")
                           .arg(horizon, participant_label, net_text, pct_text, long_text, short_text, headline);
    if (changes.long_leg.has_value && changes.short_leg.has_value) {
        if (changes.long_leg.value > 0.0 && changes.short_leg.value <= 0.0)
            item.explanation += QStringLiteral(" The move is driven by long additions.");
        else if (changes.short_leg.value < 0.0 && changes.long_leg.value >= 0.0)
            item.explanation += QStringLiteral(" The move is driven by short reductions.");
        else if (changes.long_leg.value < 0.0 && changes.short_leg.value >= 0.0)
            item.explanation += QStringLiteral(" The move is driven by long liquidation.");
        else if (changes.short_leg.value > 0.0 && changes.long_leg.value <= 0.0)
            item.explanation += QStringLiteral(" The move is driven by new shorts.");
    }
    return item;
}

inline CftcEvidenceItem cftc_trend_item(const QString& rule_id, CftcEvidenceFamily family, const QString& horizon,
                                        const CftcSignal& signal, const CftcMetricReading& position_vs_mean,
                                        const CftcMetricReading& slope, int window_reports,
                                        bool series_reaches_report) {
    CftcEvidenceItem item;
    item.rule_id = rule_id;
    item.family = family;
    item.horizon = horizon;
    item.available = signal.available;
    item.direction = signal.direction;
    item.strength = cftc_signal_strength(signal);
    item.conflicted = signal.conflicted;
    item.metrics = {
        cftc_evidence_metric(QStringLiteral("net_minus_mean"),
                             position_vs_mean.has_value ? std::optional<double>(position_vs_mean.value) : std::nullopt),
        cftc_evidence_metric(QStringLiteral("mean_slope"),
                             slope.has_value ? std::optional<double>(slope.value) : std::nullopt),
        cftc_evidence_metric(QStringLiteral("window_gapped"), signal.gapped, signal.gapped ? 1.0 : 0.0),
    };
    if (!signal.available) {
        if (signal.gapped) {
            item.explanation =
                QStringLiteral("%1 Net versus the %2-report causal mean and mean slope unavailable: the window spans "
                               "a missing report, so the readings are not ordinary continuous-window signals.")
                    .arg(horizon)
                    .arg(window_reports);
        } else {
            item.explanation = series_reaches_report
                                   ? QStringLiteral("%1 Net versus the %2-report causal mean and mean slope "
                                                    "unavailable (insufficient history at the official report).")
                                         .arg(horizon)
                                         .arg(window_reports)
                                   : QStringLiteral("%1 Net versus the %2-report causal mean and mean slope "
                                                    "unavailable: the Net series does not reach the official as-of "
                                                    "report.")
                                         .arg(horizon)
                                         .arg(window_reports);
        }
        return item;
    }
    const QString position_text =
        position_vs_mean.has_value ? cftc_format_evidence_count(position_vs_mean.value) : QStringLiteral("unavailable");
    const QString slope_text =
        slope.has_value ? cftc_format_evidence_count(slope.value) : QStringLiteral("unavailable");
    QString headline;
    if (signal.conflicted)
        headline = QStringLiteral("conflicting");
    else if (signal.sign() > 0)
        headline = QStringLiteral("above a rising trend");
    else if (signal.sign() < 0)
        headline = QStringLiteral("below a falling trend");
    else
        headline = QStringLiteral("no clear trend position");
    item.explanation = QStringLiteral("%1 Net minus its %2-report mean is %3; mean slope is %4 — %5.")
                           .arg(horizon)
                           .arg(window_reports)
                           .arg(position_text, slope_text, headline);
    if (signal.gapped)
        item.explanation += QStringLiteral(" A window spans a missing report, so only the complete-window readings "
                                           "contributed to this signal.");
    return item;
}

inline CftcEvidenceItem cftc_persistence_item(const QString& rule_id, CftcEvidenceFamily family, const QString& horizon,
                                              const CftcSignal& signal, const CftcPositioningPersistence& persistence,
                                              bool sustained) {
    CftcEvidenceItem item;
    item.rule_id = rule_id;
    item.family = family;
    item.horizon = horizon;
    item.available = signal.available;
    item.direction = !signal.available ? CftcEvidenceDirection::Unavailable
                                       : (signal.weight > 0 ? signal.direction : CftcEvidenceDirection::Neutral);
    item.strength = cftc_signal_strength(signal);
    item.metrics = {
        cftc_evidence_metric(QStringLiteral("persistence_changes"), persistence.has_value,
                             static_cast<double>(persistence.changes)),
    };
    if (!signal.available) {
        item.explanation = QStringLiteral("%1 persistence unavailable: no weekly-neighbour Net step anchored at the "
                                          "official report.")
                               .arg(horizon);
        return item;
    }
    if (signal.weight == 0) {
        if (persistence.direction == CftcDirection::Flat) {
            item.explanation = QStringLiteral("%1 weekly Net run is flat over %2 step(s); no persistence direction.")
                                   .arg(horizon)
                                   .arg(persistence.changes);
        } else {
            item.explanation =
                QStringLiteral("%1 weekly Net run is %2 same-direction step(s) since %3; below the persistence "
                               "threshold.")
                    .arg(horizon)
                    .arg(persistence.changes)
                    .arg(persistence.since_date.toString(Qt::ISODate));
        }
        if (sustained && !signal.conflicted && persistence.direction != CftcDirection::Flat)
            item.explanation += QStringLiteral(" The run does not span the 13W anchor, so it is not treated as "
                                               "sustained accumulation/distribution.");
        return item;
    }
    item.explanation =
        QStringLiteral("%1 weekly Net run is %2 consecutive %3 steps since %4 — %5.")
            .arg(horizon)
            .arg(persistence.changes)
            .arg(persistence.direction == CftcDirection::Up ? QStringLiteral("up") : QStringLiteral("down"),
                 persistence.since_date.toString(Qt::ISODate),
                 sustained ? QStringLiteral("sustained across the swing window") : QStringLiteral("a tactical run"));
    return item;
}

inline CftcEvidenceItem cftc_extreme_item(const QString& rule_id, CftcEvidenceFamily family, const QString& horizon,
                                          const CftcSignal& signal, const CftcExtremeState& extreme,
                                          bool thresholds_available, bool reference_zero_variance,
                                          const CftcHorizonChange& net_change) {
    CftcEvidenceItem item;
    item.rule_id = rule_id;
    item.family = family;
    item.horizon = horizon;
    item.available = thresholds_available && extreme.has_value;
    // An evaluable extreme state that did not fire is an available neutral
    // observation, not unavailable evidence.
    item.direction = !item.available ? CftcEvidenceDirection::Unavailable
                                     : (signal.weight > 0 ? signal.direction : CftcEvidenceDirection::Neutral);
    item.strength = cftc_signal_strength(signal);
    item.conflicted = false;
    item.metrics = {
        cftc_evidence_metric(QStringLiteral("weeks_at_upper"), extreme.has_value,
                             static_cast<double>(extreme.weeks_at_upper)),
        cftc_evidence_metric(QStringLiteral("weeks_at_lower"), extreme.has_value,
                             static_cast<double>(extreme.weeks_at_lower)),
        cftc_evidence_metric(QStringLiteral("at_upper"), extreme.has_value, extreme.at_upper ? 1.0 : 0.0),
        cftc_evidence_metric(QStringLiteral("at_lower"), extreme.has_value, extreme.at_lower ? 1.0 : 0.0),
        cftc_evidence_metric(QStringLiteral("left_upper"), extreme.has_value, extreme.left_upper ? 1.0 : 0.0),
        cftc_evidence_metric(QStringLiteral("left_lower"), extreme.has_value, extreme.left_lower ? 1.0 : 0.0),
        cftc_evidence_metric(QStringLiteral("away_from_upper"), extreme.has_value, extreme.away_from_upper ? 1.0 : 0.0),
        cftc_evidence_metric(QStringLiteral("away_from_lower"), extreme.has_value, extreme.away_from_lower ? 1.0 : 0.0),
    };
    if (!item.available) {
        item.explanation =
            reference_zero_variance
                ? QStringLiteral("%1 return-from-extreme unavailable: the strictly trailing 2Y reference has zero "
                                 "variance, so it defines no extreme band.")
                      .arg(horizon)
                : QStringLiteral("%1 return-from-extreme unavailable: the strictly trailing 2Y reference is "
                                 "incomplete.")
                      .arg(horizon);
        return item;
    }
    if (signal.weight > 0) {
        item.explanation = QStringLiteral("Previous report was %1 the 2Y trailing extreme and the current report "
                                          "returned inside while %2 Net %3 by %4 (anchor %5) — a "
                                          "return-from-extreme reversal.")
                               .arg(extreme.left_upper ? QStringLiteral("at/above") : QStringLiteral("at/below"),
                                    horizon, cftc_change_direction_word(net_change),
                                    net_change.has_value ? cftc_format_evidence_count(qAbs(net_change.value))
                                                         : QStringLiteral("unavailable"),
                                    net_change.anchor_date.isValid() ? net_change.anchor_date.toString(Qt::ISODate)
                                                                     : QStringLiteral("unknown"));
        return item;
    }
    if (extreme.at_upper || extreme.at_lower) {
        item.explanation =
            QStringLiteral("Current Net is %1 the 2Y trailing range and has persisted %2 week(s); an extreme alone "
                           "is not a contrarian rule.")
                .arg(extreme.at_upper ? QStringLiteral("at/above") : QStringLiteral("at/below"))
                .arg(extreme.at_upper ? extreme.weeks_at_upper : extreme.weeks_at_lower);
        return item;
    }
    if (extreme.left_upper || extreme.left_lower) {
        item.explanation = QStringLiteral("The previous report left the %1 2Y extreme but the current %2 Net move "
                                          "does not match a reversal; exposed as context only.")
                               .arg(extreme.left_upper ? QStringLiteral("upper") : QStringLiteral("lower"), horizon);
        return item;
    }
    if (extreme.has_prior_upper || extreme.has_prior_lower) {
        const bool prior_upper = extreme.has_prior_upper;
        item.explanation = QStringLiteral("The current Net is inside the trailing 2Y range while a prior %1 extreme "
                                          "exists at %2 (%3 from it); moving away from a prior extreme is context "
                                          "only unless a recent reversal accompanies it.")
                               .arg(prior_upper ? QStringLiteral("upper") : QStringLiteral("lower"),
                                    prior_upper ? extreme.prior_upper_date.toString(Qt::ISODate)
                                                : extreme.prior_lower_date.toString(Qt::ISODate),
                                    prior_upper ? cftc_format_evidence_value(extreme.distance_from_upper)
                                                : cftc_format_evidence_value(extreme.distance_from_lower));
        return item;
    }
    item.explanation = QStringLiteral("Current Net is inside the strictly trailing 2Y range; no return-from-extreme "
                                      "condition.");
    return item;
}

/// One family's single vote from its items: available non-neutral items must
/// agree, otherwise the family is mixed (Neutral). This is the mechanism that
/// prevents several correlated metrics inside one family from becoming several
/// directional votes.
inline CftcEvidenceDirection cftc_combine_item_directions(const QVector<CftcEvidenceItem>& items, bool& any_available) {
    bool bullish = false;
    bool bearish = false;
    any_available = false;
    for (const auto& item : items) {
        if (!item.available)
            continue;
        any_available = true;
        if (item.direction == CftcEvidenceDirection::Bullish)
            bullish = true;
        else if (item.direction == CftcEvidenceDirection::Bearish)
            bearish = true;
    }
    if (!any_available)
        return CftcEvidenceDirection::Unavailable;
    if (bullish && bearish)
        return CftcEvidenceDirection::Neutral;
    if (bullish)
        return CftcEvidenceDirection::Bullish;
    if (bearish)
        return CftcEvidenceDirection::Bearish;
    return CftcEvidenceDirection::Neutral;
}

inline CftcEvidenceStrength cftc_combine_item_strength(const QVector<CftcEvidenceItem>& items) {
    CftcEvidenceStrength strength = CftcEvidenceStrength::None;
    for (const auto& item : items) {
        if (static_cast<int>(item.strength) > static_cast<int>(strength))
            strength = item.strength;
    }
    return strength;
}

inline void cftc_finalize_group(CftcEvidenceGroup& group, bool counts_for_independence) {
    bool any_available = false;
    group.direction = cftc_combine_item_directions(group.items, any_available);
    group.strength = cftc_combine_item_strength(group.items);
    group.available = any_available;
    group.counts_for_independence = counts_for_independence;
    group.counts_for_confidence = counts_for_independence;
}

inline CftcEvidenceDirection cftc_tactical_direction(CftcTacticalState state) {
    switch (state) {
        case CftcTacticalState::Bullish:
            return CftcEvidenceDirection::Bullish;
        case CftcTacticalState::Bearish:
            return CftcEvidenceDirection::Bearish;
        case CftcTacticalState::Neutral:
            return CftcEvidenceDirection::Neutral;
        case CftcTacticalState::Unavailable:
            break;
    }
    return CftcEvidenceDirection::Unavailable;
}

// ── Engine ──────────────────────────────────────────────────────────────────

inline CftcResearchResult cftc_evaluate_research_state(const CftcResearchInput& input) {
    CftcResearchResult result;
    result.rule_set_version = cftc_research_rule_set_version();
    result.family = input.family;
    result.family_code = cftc_family_code(input.family);
    result.price_source = input.price_source;
    result.price_continuous_proxy = input.price_continuous_proxy;
    result.price_spot_index = input.price_spot_index;
    result.effective_date_known = input.effective_date.isValid();
    result.effective_date = input.effective_date;

    const QVector<CftcParticipant> participants = cftc_family_participants(input.family);
    const int spec_index = cftc_speculative_index(participants);
    if (spec_index >= 0) {
        result.primary_speculative_key = participants[spec_index].key;
        result.primary_speculative_label = participants[spec_index].label;
    }
    const QString spec_label =
        result.primary_speculative_label.isEmpty() ? QStringLiteral("speculative") : result.primary_speculative_label;

    const QDate report_date = cftc_official_as_of(input.observations, input.as_of);
    result.report_date = report_date;
    result.latest_available_report_date = input.observations.isEmpty() ? QDate() : input.observations.last().date;
    result.report_missing = input.observations.isEmpty();

    const QVector<CftcDatedValue> net_series =
        spec_index >= 0 ? cftc_metric_series(input.observations, spec_index, CftcMetricKind::Net)
                        : QVector<CftcDatedValue>{};
    const QVector<CftcDatedValue> oi_series = cftc_open_interest_series(input.observations);
    const auto series_has_date = [](const QVector<CftcDatedValue>& series, const QDate& date) {
        if (!date.isValid())
            return false;
        for (const auto& point : series) {
            if (point.date == date)
                return true;
        }
        return false;
    };
    result.data_available = report_date.isValid() && !net_series.isEmpty() && net_series.last().date == report_date;
    result.data_stale = report_date.isValid() && !result.data_available && !net_series.isEmpty() &&
                        net_series.last().date < report_date;
    result.report_predates_history =
        report_date.isValid() && !input.observations.isEmpty() && input.observations.last().date > report_date;
    result.readings.data_available = result.data_available;
    result.readings.data_stale = result.data_stale;

    CftcStateReadings& readings = result.readings;
    readings.changes_4w = cftc_position_changes(input.observations, spec_index, CftcHorizon::FourWeeks, report_date);
    readings.changes_13w =
        cftc_position_changes(input.observations, spec_index, CftcHorizon::ThirteenWeeks, report_date);
    readings.changes_26w =
        cftc_position_changes(input.observations, spec_index, CftcHorizon::TwentySixWeeks, report_date);
    readings.net_minus_ma_4w = cftc_net_minus_moving_average(net_series, 4, report_date);
    readings.ma_slope_4w = cftc_moving_average_slope(net_series, 4, report_date);
    readings.net_minus_ma_13w = cftc_net_minus_moving_average(net_series, 13, report_date);
    readings.ma_slope_13w = cftc_moving_average_slope(net_series, 13, report_date);
    readings.net_minus_ma_26w = cftc_net_minus_moving_average(net_series, 26, report_date);
    readings.ma_slope_26w = cftc_moving_average_slope(net_series, 26, report_date);
    readings.persistence = cftc_positioning_persistence(net_series, report_date);
    readings.stats_26w =
        cftc_trailing_stats(net_series, CftcTrailingWindow::Weeks26, report_date, input.trailing_min_reference);
    readings.stats_52w =
        cftc_trailing_stats(net_series, CftcTrailingWindow::Weeks52, report_date, input.trailing_min_reference);
    readings.stats_2y =
        cftc_trailing_stats(net_series, CftcTrailingWindow::Years2, report_date, input.trailing_min_reference);
    readings.stats_5y =
        cftc_trailing_stats(net_series, CftcTrailingWindow::Years5, report_date, input.trailing_min_reference);
    const bool extreme_available =
        readings.stats_2y.has_min && readings.stats_2y.has_max && !readings.stats_2y.zero_variance;
    readings.extreme_thresholds_available = extreme_available;
    if (extreme_available) {
        readings.extreme =
            cftc_extreme_state(net_series, readings.stats_2y.max_value, readings.stats_2y.min_value, report_date);
    }
    readings.open_interest_change_4w =
        cftc_open_interest_change(input.observations, CftcHorizon::FourWeeks, report_date);
    readings.open_interest_change_13w =
        cftc_open_interest_change(input.observations, CftcHorizon::ThirteenWeeks, report_date);
    // Price observations are current only when the latest close at or before
    // the official report is within the weekly tolerance. A series that stops
    // long before the report is stale for this comparison and must not supply
    // a current-looking price change.
    readings.price_series_supplied = !input.prices.isEmpty();
    for (const auto& point : input.prices) {
        if (point.date <= report_date)
            readings.latest_price_date = point.date;
        else
            break;
    }
    readings.price_series_fresh =
        report_date.isValid() && readings.latest_price_date.isValid() &&
        static_cast<int>(readings.latest_price_date.daysTo(report_date)) <= kCftcWeeklyGapDays;
    if (readings.price_series_fresh) {
        if (const auto change = cftc_price_change_since_days(input.prices, 28, report_date)) {
            readings.price_change_4w_available = true;
            readings.price_change_4w = *change;
        }
        if (const auto change = cftc_price_change_since_days(input.prices, 91, report_date)) {
            readings.price_change_13w_available = true;
            readings.price_change_13w = *change;
        }
    }
    result.price_used = readings.price_change_4w_available || readings.price_change_13w_available;

    // ── Core horizon signals ────────────────────────────────────────────────
    const CftcSignal position_4w = cftc_combine_change_pair(readings.changes_4w.net, readings.changes_4w.net_pct_oi);
    const CftcSignal trend_4w = cftc_combine_reading_pair(readings.net_minus_ma_4w, readings.ma_slope_4w);
    const CftcSignal persistence_4w = cftc_weekly_persistence_signal(readings.persistence, 2);
    const CftcSignal extreme_4w =
        extreme_available ? cftc_extreme_return_signal(readings.extreme, readings.changes_4w.net) : CftcSignal{};
    const CftcHorizonAssessment horizon_4w = cftc_assess_horizon(position_4w, trend_4w, persistence_4w, extreme_4w);

    const CftcSignal position_13w = cftc_combine_change_pair(readings.changes_13w.net, readings.changes_13w.net_pct_oi);
    const CftcSignal trend_13w = cftc_combine_reading_pair(readings.net_minus_ma_13w, readings.ma_slope_13w);
    const CftcHorizonAnchor anchor_13w =
        cftc_horizon_anchor(input.observations, CftcHorizon::ThirteenWeeks, report_date);
    const CftcSignal persistence_13w = cftc_sustained_persistence_signal(readings.persistence, 2, anchor_13w);
    const CftcSignal extreme_13w =
        extreme_available ? cftc_extreme_return_signal(readings.extreme, readings.changes_13w.net) : CftcSignal{};
    const CftcHorizonAssessment horizon_13w =
        cftc_assess_horizon(position_13w, trend_13w, persistence_13w, extreme_13w);

    result.tactical_4w = horizon_4w.state;
    result.swing_13w = horizon_13w.state;

    // 26W regime: context only, never counted toward direction or independence.
    const CftcSignal position_26w = cftc_combine_change_pair(readings.changes_26w.net, readings.changes_26w.net_pct_oi);
    const CftcSignal trend_26w = cftc_combine_reading_pair(readings.net_minus_ma_26w, readings.ma_slope_26w);
    const CftcSignal persistence_26w; // not used at 26W
    const CftcSignal extreme_26w;     // not used at 26W
    const CftcHorizonAssessment horizon_26w =
        cftc_assess_horizon(position_26w, trend_26w, persistence_26w, extreme_26w);
    result.regime_26w = horizon_26w.state;

    // ── Group: 4W tactical ──────────────────────────────────────────────────
    CftcEvidenceGroup tactical_4w;
    tactical_4w.family = CftcEvidenceFamily::Tactical4W;
    tactical_4w.items = {
        cftc_position_change_item(QStringLiteral("R4W-NET-CHANGE"), CftcEvidenceFamily::Tactical4W,
                                  QStringLiteral("4W"), position_4w, readings.changes_4w, spec_label,
                                  result.data_available,
                                  series_has_date(net_series, readings.changes_4w.net.anchor_date)),
        cftc_trend_item(QStringLiteral("R4W-TREND"), CftcEvidenceFamily::Tactical4W, QStringLiteral("4W"), trend_4w,
                        readings.net_minus_ma_4w, readings.ma_slope_4w, 4, result.data_available),
        cftc_persistence_item(QStringLiteral("R4W-PERSISTENCE"), CftcEvidenceFamily::Tactical4W, QStringLiteral("4W"),
                              persistence_4w, readings.persistence, false),
        cftc_extreme_item(QStringLiteral("R4W-EXTREME"), CftcEvidenceFamily::Tactical4W, QStringLiteral("4W"),
                          extreme_4w, readings.extreme, extreme_available, readings.stats_2y.zero_variance,
                          readings.changes_4w.net),
    };
    cftc_finalize_group(tactical_4w, true);
    tactical_4w.direction = cftc_tactical_direction(horizon_4w.state);

    // ── Group: 13W swing ────────────────────────────────────────────────────
    CftcEvidenceGroup swing_13w;
    swing_13w.family = CftcEvidenceFamily::Swing13W;
    swing_13w.items = {
        cftc_position_change_item(QStringLiteral("R13W-NET-CHANGE"), CftcEvidenceFamily::Swing13W,
                                  QStringLiteral("13W"), position_13w, readings.changes_13w, spec_label,
                                  result.data_available,
                                  series_has_date(net_series, readings.changes_13w.net.anchor_date)),
        cftc_trend_item(QStringLiteral("R13W-TREND"), CftcEvidenceFamily::Swing13W, QStringLiteral("13W"), trend_13w,
                        readings.net_minus_ma_13w, readings.ma_slope_13w, 13, result.data_available),
        cftc_persistence_item(QStringLiteral("R13W-SUSTAINED-PERSISTENCE"), CftcEvidenceFamily::Swing13W,
                              QStringLiteral("13W"), persistence_13w, readings.persistence, true),
        cftc_extreme_item(QStringLiteral("R13W-EXTREME"), CftcEvidenceFamily::Swing13W, QStringLiteral("13W"),
                          extreme_13w, readings.extreme, extreme_available, readings.stats_2y.zero_variance,
                          readings.changes_13w.net),
    };
    cftc_finalize_group(swing_13w, true);
    swing_13w.direction = cftc_tactical_direction(horizon_13w.state);

    // ── Group: 26W regime (context only) ────────────────────────────────────
    CftcEvidenceGroup regime_26w;
    regime_26w.family = CftcEvidenceFamily::Regime26W;
    regime_26w.items = {
        cftc_position_change_item(QStringLiteral("R26W-NET-CHANGE"), CftcEvidenceFamily::Regime26W,
                                  QStringLiteral("26W"), position_26w, readings.changes_26w, spec_label,
                                  result.data_available,
                                  series_has_date(net_series, readings.changes_26w.net.anchor_date)),
        cftc_trend_item(QStringLiteral("R26W-TREND"), CftcEvidenceFamily::Regime26W, QStringLiteral("26W"), trend_26w,
                        readings.net_minus_ma_26w, readings.ma_slope_26w, 26, result.data_available),
    };
    cftc_finalize_group(regime_26w, false);
    regime_26w.direction = cftc_tactical_direction(horizon_26w.state);

    // ── Group: historical context ───────────────────────────────────────────
    CftcHistoricalContext context = CftcHistoricalContext::Unavailable;
    CftcEvidenceGroup historical;
    historical.family = CftcEvidenceFamily::HistoricalContext;
    {
        const CftcTrailingStats& stats_2y = readings.stats_2y;
        // A zero-variance reference has no distribution to rank against: Batch 1
        // deliberately keeps its percentile defined by its tie rule, but a
        // perfectly flat 2Y window must not be classified as crowded.
        const bool crowding_available =
            stats_2y.reference_covered && stats_2y.has_percentile && !stats_2y.zero_variance;
        if (crowding_available) {
            const bool crowded_long = (stats_2y.has_percentile && stats_2y.percentile >= 90.0) ||
                                      (stats_2y.has_cot_index && stats_2y.cot_index >= 90.0) ||
                                      (stats_2y.has_zscore && stats_2y.zscore >= 1.5);
            const bool crowded_short = (stats_2y.has_percentile && stats_2y.percentile <= 10.0) ||
                                       (stats_2y.has_cot_index && stats_2y.cot_index <= 10.0) ||
                                       (stats_2y.has_zscore && stats_2y.zscore <= -1.5);
            // Percentile, COT Index and z-score are related views of one
            // distribution and can disagree on a skewed reference (for example
            // a high percentile inside a wide range gives a low COT Index).
            // The disagreement is exposed as a mixed context, never silently
            // resolved in favor of whichever condition is tested first.
            const bool crowding_mixed = crowded_long && crowded_short;
            context = crowding_mixed ? CftcHistoricalContext::Mixed
                                     : (crowded_long ? CftcHistoricalContext::CrowdedLong
                                                     : (crowded_short ? CftcHistoricalContext::CrowdedShort
                                                                      : CftcHistoricalContext::Neutral));

            CftcEvidenceItem crowding;
            crowding.rule_id = QStringLiteral("RHIST-CROWDING");
            crowding.family = CftcEvidenceFamily::HistoricalContext;
            crowding.horizon = QStringLiteral("2Y");
            crowding.direction = CftcEvidenceDirection::Neutral;
            crowding.available = true;
            crowding.conflicted = crowding_mixed;
            crowding.metrics = {
                cftc_evidence_metric(QStringLiteral("percentile_2y"), stats_2y.has_percentile, stats_2y.percentile),
                cftc_evidence_metric(QStringLiteral("cot_index_2y"), stats_2y.has_cot_index, stats_2y.cot_index),
                cftc_evidence_metric(QStringLiteral("zscore_2y"), stats_2y.has_zscore, stats_2y.zscore),
                cftc_evidence_metric(QStringLiteral("percentile_5y"), readings.stats_5y.has_percentile,
                                     readings.stats_5y.percentile),
            };
            crowding.explanation =
                crowding_mixed
                    ? QStringLiteral("Strictly trailing 2Y normalization disagrees with itself: percentile %1, COT "
                                     "Index %2, z-score %3 describe both a long and a short extreme at once, so no "
                                     "crowding direction is applied.")
                          .arg(stats_2y.has_percentile ? cftc_format_evidence_value(stats_2y.percentile)
                                                       : QStringLiteral("unavailable"),
                               stats_2y.has_cot_index ? cftc_format_evidence_value(stats_2y.cot_index)
                                                      : QStringLiteral("unavailable"),
                               stats_2y.has_zscore ? cftc_format_evidence_value(stats_2y.zscore)
                                                   : QStringLiteral("unavailable"))
                    : QStringLiteral("Strictly trailing 2Y positioning percentile %1, COT Index %2, z-score %3 — %4. "
                                     "Historical context modifies interpretation and confidence but does not set "
                                     "direction by itself.")
                          .arg(stats_2y.has_percentile ? cftc_format_evidence_value(stats_2y.percentile)
                                                       : QStringLiteral("unavailable"),
                               stats_2y.has_cot_index ? cftc_format_evidence_value(stats_2y.cot_index)
                                                      : QStringLiteral("unavailable"),
                               stats_2y.has_zscore ? cftc_format_evidence_value(stats_2y.zscore)
                                                   : QStringLiteral("unavailable"),
                               cftc_historical_context_code(context));
            historical.items.append(crowding);
        } else {
            CftcEvidenceItem crowding;
            crowding.rule_id = QStringLiteral("RHIST-CROWDING");
            crowding.family = CftcEvidenceFamily::HistoricalContext;
            crowding.horizon = QStringLiteral("2Y");
            crowding.direction = CftcEvidenceDirection::Unavailable;
            crowding.explanation =
                stats_2y.zero_variance
                    ? QStringLiteral("Historical crowding unavailable: the strictly trailing 2Y reference has zero "
                                     "variance, so no distribution exists to rank the current reading against. A "
                                     "flat reference is not normal or crowded positioning.")
                    : QStringLiteral("Historical crowding unavailable: the strictly trailing 2Y reference is not "
                                     "covered or too sparse. Insufficient history is not normal positioning.");
            crowding.metrics = {
                cftc_evidence_metric(QStringLiteral("reference_count_52w"), readings.stats_52w.reference_covered,
                                     static_cast<double>(readings.stats_52w.reference_count)),
                cftc_evidence_metric(QStringLiteral("reference_count_2y"), stats_2y.reference_covered,
                                     static_cast<double>(stats_2y.reference_count)),
            };
            historical.items.append(crowding);
        }

        CftcEvidenceItem extreme_context;
        extreme_context.rule_id = QStringLiteral("RHIST-EXTREME-STATE");
        extreme_context.family = CftcEvidenceFamily::HistoricalContext;
        extreme_context.horizon = QStringLiteral("2Y");
        extreme_context.direction = CftcEvidenceDirection::Neutral;
        extreme_context.available = extreme_available && readings.extreme.has_value;
        if (extreme_context.available) {
            extreme_context.metrics = {
                cftc_evidence_metric(QStringLiteral("weeks_at_upper"), true,
                                     static_cast<double>(readings.extreme.weeks_at_upper)),
                cftc_evidence_metric(QStringLiteral("weeks_at_lower"), true,
                                     static_cast<double>(readings.extreme.weeks_at_lower)),
                cftc_evidence_metric(QStringLiteral("remained_at_upper"), true,
                                     readings.extreme.remained_at_upper ? 1.0 : 0.0),
                cftc_evidence_metric(QStringLiteral("remained_at_lower"), true,
                                     readings.extreme.remained_at_lower ? 1.0 : 0.0),
            };
            extreme_context.explanation =
                QStringLiteral("Extreme state at the official report: %1 week(s) at/above and %2 week(s) at/below "
                               "the trailing 2Y band; %3.")
                    .arg(readings.extreme.weeks_at_upper)
                    .arg(readings.extreme.weeks_at_lower)
                    .arg(readings.extreme.remained_at_upper || readings.extreme.remained_at_lower
                             ? QStringLiteral("persistent extreme without an exit")
                             : QStringLiteral("no extended extreme persistence"));
        } else {
            extreme_context.explanation =
                readings.stats_2y.zero_variance
                    ? QStringLiteral("Extreme state unavailable: the strictly trailing 2Y reference has zero "
                                     "variance, so it defines no extreme band.")
                    : QStringLiteral("Extreme state unavailable: the strictly trailing 2Y reference is incomplete.");
        }
        historical.items.append(extreme_context);

        if (context == CftcHistoricalContext::CrowdedLong || context == CftcHistoricalContext::CrowdedShort) {
            const bool reversal =
                (context == CftcHistoricalContext::CrowdedLong && horizon_4w.state == CftcTacticalState::Bearish) ||
                (context == CftcHistoricalContext::CrowdedShort && horizon_4w.state == CftcTacticalState::Bullish);
            if (reversal) {
                CftcEvidenceItem item;
                item.rule_id = QStringLiteral("RHIST-REVERSAL");
                item.family = CftcEvidenceFamily::HistoricalContext;
                item.horizon = QStringLiteral("2Y+4W");
                item.direction = context == CftcHistoricalContext::CrowdedLong ? CftcEvidenceDirection::Bearish
                                                                               : CftcEvidenceDirection::Bullish;
                const bool exited = context == CftcHistoricalContext::CrowdedLong ? readings.extreme.left_upper
                                                                                  : readings.extreme.left_lower;
                item.strength = exited ? CftcEvidenceStrength::Strong : CftcEvidenceStrength::Moderate;
                item.available = true;
                item.metrics = {
                    cftc_evidence_metric(QStringLiteral("net_change_4w"),
                                         readings.changes_4w.net.has_value
                                             ? std::optional<double>(readings.changes_4w.net.value)
                                             : std::nullopt),
                    cftc_evidence_metric(QStringLiteral("percentile_2y"), stats_2y.has_percentile, stats_2y.percentile),
                };
                item.explanation =
                    QStringLiteral("Historically %1 positioning met a genuine recent reversal: 4W Net %2 by %3 "
                                   "(anchor %4) and 4W tactical state %5%6.")
                        .arg(cftc_historical_context_code(context), cftc_change_direction_word(readings.changes_4w.net),
                             readings.changes_4w.net.has_value
                                 ? cftc_format_evidence_count(qAbs(readings.changes_4w.net.value))
                                 : QStringLiteral("unavailable"),
                             readings.changes_4w.net.anchor_date.isValid()
                                 ? readings.changes_4w.net.anchor_date.toString(Qt::ISODate)
                                 : QStringLiteral("unknown"),
                             cftc_tactical_state_code(horizon_4w.state),
                             exited ? QStringLiteral("; the previous report also left the trailing 2Y extreme")
                                    : QString());
                historical.items.append(item);
            } else if (horizon_4w.state != CftcTacticalState::Unavailable) {
                CftcEvidenceItem item;
                item.rule_id = QStringLiteral("RHIST-CROWDED-CONTINUATION");
                item.family = CftcEvidenceFamily::HistoricalContext;
                item.horizon = QStringLiteral("2Y+4W");
                item.direction = CftcEvidenceDirection::Neutral;
                item.available = true;
                item.metrics = {
                    cftc_evidence_metric(QStringLiteral("percentile_2y"), stats_2y.has_percentile, stats_2y.percentile),
                    cftc_evidence_metric(QStringLiteral("net_change_4w"),
                                         readings.changes_4w.net.has_value
                                             ? std::optional<double>(readings.changes_4w.net.value)
                                             : std::nullopt),
                };
                item.explanation =
                    QStringLiteral("Historically %1 positioning with recent 4W state %2: crowding is context only in "
                                   "v0 and does not by itself create BUY or SELL.")
                        .arg(cftc_historical_context_code(context), cftc_tactical_state_code(horizon_4w.state));
                historical.items.append(item);
            }
        } else if ((readings.extreme.left_upper || readings.extreme.left_lower) &&
                   horizon_4w.state != CftcTacticalState::Unavailable) {
            CftcEvidenceItem item;
            item.rule_id = QStringLiteral("RHIST-EXTREME-EXIT-NO-REVERSAL");
            item.family = CftcEvidenceFamily::HistoricalContext;
            item.horizon = QStringLiteral("2Y+4W");
            item.direction = CftcEvidenceDirection::Neutral;
            item.available = true;
            item.explanation =
                QStringLiteral("The previous report left a trailing 2Y extreme without a matching 4W reversal; "
                               "exposed as context, not direction.");
            historical.items.append(item);
        }
    }
    cftc_finalize_group(historical, false);
    // Historical context is not independent of the core positioning series, so
    // it never satisfies the direction independence requirement. Its
    // availability and agreement still count as evidence quality/coverage.
    historical.counts_for_confidence = true;
    result.historical_context = context;

    // ── Group: price / COT ──────────────────────────────────────────────────
    CftcEvidenceGroup price_cot;
    price_cot.family = CftcEvidenceFamily::PriceCot;
    {
        const auto relationship_item = [&](const QString& rule_id, const QString& horizon, bool has_price,
                                           double price_change, const CftcHorizonChange& net_change) {
            CftcEvidenceItem item;
            item.rule_id = rule_id;
            item.family = CftcEvidenceFamily::PriceCot;
            item.horizon = horizon;
            item.metrics = {
                cftc_evidence_metric(QStringLiteral("price_change"), has_price, price_change),
                cftc_evidence_metric(QStringLiteral("net_change"),
                                     net_change.has_value ? std::optional<double>(net_change.value) : std::nullopt),
            };
            if (!has_price) {
                if (!readings.price_series_supplied) {
                    item.explanation =
                        QStringLiteral("Price/COT relationship unavailable over %1: no qualified price observation "
                                       "was supplied. An absent price is not a flat price.")
                            .arg(horizon);
                } else if (!readings.latest_price_date.isValid()) {
                    item.explanation =
                        QStringLiteral("Price/COT relationship unavailable over %1: the supplied price series has "
                                       "no close at or before the %2 report.")
                            .arg(horizon,
                                 report_date.isValid() ? report_date.toString(Qt::ISODate) : QStringLiteral("unknown"));
                } else if (!readings.price_series_fresh) {
                    item.explanation =
                        QStringLiteral("Price/COT relationship unavailable over %1: the supplied price series stops "
                                       "at %2, outside the weekly freshness tolerance before the %3 report. A stale "
                                       "price is not a current price.")
                            .arg(horizon, readings.latest_price_date.toString(Qt::ISODate),
                                 report_date.isValid() ? report_date.toString(Qt::ISODate) : QStringLiteral("unknown"));
                } else {
                    item.explanation =
                        QStringLiteral("Price/COT relationship unavailable over %1: the supplied price series does "
                                       "not reach back to the horizon anchor (latest close %2).")
                            .arg(horizon, readings.latest_price_date.toString(Qt::ISODate));
                }
                return item;
            }
            if (!net_change.has_value) {
                item.explanation = QStringLiteral("Price/COT relationship unavailable over %1: the %2 Net change is "
                                                  "unavailable.")
                                       .arg(horizon, spec_label);
                return item;
            }
            item.available = true;
            const CftcDirection price_dir = cftc_direction(price_change);
            const CftcDirection net_dir = cftc_direction(net_change.value);
            if (cftc_directions_aligned(price_dir, net_dir) && price_dir != CftcDirection::Flat) {
                item.direction =
                    price_dir == CftcDirection::Up ? CftcEvidenceDirection::Bullish : CftcEvidenceDirection::Bearish;
                item.strength = CftcEvidenceStrength::Moderate;
                item.explanation =
                    QStringLiteral("Price %1 by %2 over %3 while %4 Net %5 by %6 — aligned "
                                   "confirmation.")
                        .arg(price_dir == CftcDirection::Up ? QStringLiteral("rose") : QStringLiteral("fell"),
                             cftc_format_evidence_value(qAbs(price_change)), horizon, spec_label,
                             cftc_change_direction_word(net_change),
                             cftc_format_evidence_count(qAbs(net_change.value)));
                return item;
            }
            if (cftc_directions_opposed(price_dir, net_dir)) {
                item.direction = CftcEvidenceDirection::Neutral;
                item.conflicted = true;
                item.explanation =
                    QStringLiteral("Price %1 by %2 over %3 while %4 Net %5 by %6 — divergence. The v0 "
                                   "rule records the opposition and does not treat it as an automatic "
                                   "reversal.")
                        .arg(price_dir == CftcDirection::Up ? QStringLiteral("rose") : QStringLiteral("fell"),
                             cftc_format_evidence_value(qAbs(price_change)), horizon, spec_label,
                             net_dir == CftcDirection::Up ? QStringLiteral("rose") : QStringLiteral("fell"),
                             cftc_format_evidence_count(qAbs(net_change.value)));
                return item;
            }
            item.direction = CftcEvidenceDirection::Neutral;
            item.explanation = QStringLiteral("Price change %1 and %2 Net change %3 over %4 include a flat side; no "
                                              "directional relationship.")
                                   .arg(cftc_format_evidence_value(price_change), spec_label,
                                        cftc_format_evidence_count(net_change.value), horizon);
            return item;
        };
        price_cot.items.append(relationship_item(QStringLiteral("RPC-4W-RELATIONSHIP"), QStringLiteral("4W"),
                                                 readings.price_change_4w_available, readings.price_change_4w,
                                                 readings.changes_4w.net));
        price_cot.items.append(relationship_item(QStringLiteral("RPC-13W-RELATIONSHIP"), QStringLiteral("13W"),
                                                 readings.price_change_13w_available, readings.price_change_13w,
                                                 readings.changes_13w.net));
    }
    cftc_finalize_group(price_cot, true);

    // ── Group: open interest ────────────────────────────────────────────────
    CftcEvidenceGroup open_interest;
    open_interest.family = CftcEvidenceFamily::OpenInterest;
    {
        const auto relationship_item = [&](const QString& rule_id, const QString& horizon,
                                           const CftcHorizonChange& oi_change, const CftcHorizonChange& net_change) {
            CftcEvidenceItem item;
            item.rule_id = rule_id;
            item.family = CftcEvidenceFamily::OpenInterest;
            item.horizon = horizon;
            item.metrics = {
                cftc_evidence_metric(QStringLiteral("open_interest_change"),
                                     oi_change.has_value ? std::optional<double>(oi_change.value) : std::nullopt),
                cftc_evidence_metric(QStringLiteral("net_change"),
                                     net_change.has_value ? std::optional<double>(net_change.value) : std::nullopt),
            };
            if (!oi_change.has_value) {
                item.explanation = QStringLiteral("Open-interest relationship unavailable over %1: the open-interest "
                                                  "change is unavailable. A missing open interest is not zero.")
                                       .arg(horizon);
                return item;
            }
            if (!net_change.has_value) {
                item.explanation = QStringLiteral("Open-interest relationship unavailable over %1: the %2 Net change "
                                                  "is unavailable.")
                                       .arg(horizon, spec_label);
                return item;
            }
            item.available = true;
            const CftcDirection oi_dir = cftc_direction(oi_change.value);
            const CftcDirection net_dir = cftc_direction(net_change.value);
            if (oi_dir == CftcDirection::Up && net_dir != CftcDirection::Flat) {
                item.direction =
                    net_dir == CftcDirection::Up ? CftcEvidenceDirection::Bullish : CftcEvidenceDirection::Bearish;
                item.strength = CftcEvidenceStrength::Weak;
                item.explanation = QStringLiteral("Open interest expanded by %1 over %2 while %3 Net %4 by %5 — "
                                                  "participation confirmation.")
                                       .arg(cftc_format_evidence_count(qAbs(oi_change.value)), horizon, spec_label,
                                            cftc_change_direction_word(net_change),
                                            cftc_format_evidence_count(qAbs(net_change.value)));
                return item;
            }
            if (oi_dir == CftcDirection::Down && net_dir != CftcDirection::Flat) {
                item.direction = CftcEvidenceDirection::Neutral;
                item.conflicted = true;
                item.explanation = QStringLiteral("Open interest fell by %1 over %2 while %3 Net %4 by %5 — "
                                                  "consistent with reduced participation/liquidation. Falling open "
                                                  "interest does not reverse the state in v0.")
                                       .arg(cftc_format_evidence_count(qAbs(oi_change.value)), horizon, spec_label,
                                            cftc_change_direction_word(net_change),
                                            cftc_format_evidence_count(qAbs(net_change.value)));
                return item;
            }
            item.direction = CftcEvidenceDirection::Neutral;
            item.explanation = QStringLiteral("Open-interest change %1 over %2 is flat or has a flat Net side; no "
                                              "participation signal.")
                                   .arg(cftc_format_evidence_count(oi_change.value), horizon);
            return item;
        };
        open_interest.items.append(relationship_item(QStringLiteral("ROI-4W-RELATIONSHIP"), QStringLiteral("4W"),
                                                     readings.open_interest_change_4w, readings.changes_4w.net));
        open_interest.items.append(relationship_item(QStringLiteral("ROI-13W-RELATIONSHIP"), QStringLiteral("13W"),
                                                     readings.open_interest_change_13w, readings.changes_13w.net));
    }
    cftc_finalize_group(open_interest, true);

    // ── Group: participant confirmation ─────────────────────────────────────
    CftcEvidenceGroup participant;
    participant.family = CftcEvidenceFamily::Participant;
    {
        const auto participant_index = [&participants](const QString& key) {
            for (int i = 0; i < participants.size(); ++i) {
                if (participants[i].key == key)
                    return i;
            }
            return -1;
        };
        const auto class_change = [&](const QString& key) {
            const int index = participant_index(key);
            if (index < 0)
                return CftcPositionChanges{};
            return cftc_position_changes(input.observations, index, CftcHorizon::ThirteenWeeks, report_date);
        };
        const auto directional_item = [&](const QString& rule_id, const QString& key) {
            CftcEvidenceItem item;
            item.rule_id = rule_id;
            item.family = CftcEvidenceFamily::Participant;
            item.horizon = QStringLiteral("13W");
            const CftcPositionChanges changes = class_change(key);
            item.metrics = {
                cftc_evidence_metric(QStringLiteral("participant_net_change_13w"),
                                     changes.net.has_value ? std::optional<double>(changes.net.value) : std::nullopt),
                cftc_evidence_metric(QStringLiteral("speculative_net_change_13w"),
                                     readings.changes_13w.net.has_value
                                         ? std::optional<double>(readings.changes_13w.net.value)
                                         : std::nullopt),
            };
            if (!changes.net.has_value || !readings.changes_13w.net.has_value) {
                item.explanation = QStringLiteral("%1 13W Net change unavailable; participant confirmation cannot be "
                                                  "evaluated.")
                                       .arg(key);
                return item;
            }
            item.available = true;
            const CftcDirection class_dir = cftc_direction(changes.net.value);
            const CftcDirection spec_dir = cftc_direction(readings.changes_13w.net.value);
            if (class_dir == CftcDirection::Flat || spec_dir == CftcDirection::Flat) {
                item.direction = CftcEvidenceDirection::Neutral;
                item.explanation = QStringLiteral("%1 13W Net change %2 is flat against %3 Net change %4; no "
                                                  "confirmation or conflict.")
                                       .arg(key, cftc_format_evidence_count(changes.net.value), spec_label,
                                            cftc_format_evidence_count(readings.changes_13w.net.value));
                return item;
            }
            const bool same = class_dir == spec_dir;
            item.direction =
                class_dir == CftcDirection::Up ? CftcEvidenceDirection::Bullish : CftcEvidenceDirection::Bearish;
            item.strength = CftcEvidenceStrength::Weak;
            item.conflicted = !same;
            item.explanation = QStringLiteral("%1 13W Net change %2 vs %3 Net change %4 — %5.")
                                   .arg(key, cftc_format_evidence_count(changes.net.value), spec_label,
                                        cftc_format_evidence_count(readings.changes_13w.net.value),
                                        same ? QStringLiteral("same direction, confirming")
                                             : QStringLiteral("opposite direction, conflicting"));
            return item;
        };
        const auto context_item = [&](const QString& rule_id, const QString& key) {
            CftcEvidenceItem item;
            item.rule_id = rule_id;
            item.family = CftcEvidenceFamily::Participant;
            item.horizon = QStringLiteral("13W");
            const CftcPositionChanges changes = class_change(key);
            item.metrics = {
                cftc_evidence_metric(QStringLiteral("participant_net_change_13w"),
                                     changes.net.has_value ? std::optional<double>(changes.net.value) : std::nullopt),
            };
            if (!changes.net.has_value) {
                item.explanation = QStringLiteral("%1 13W Net change unavailable; context only.").arg(key);
                return item;
            }
            item.available = true;
            item.direction = CftcEvidenceDirection::Neutral;
            item.explanation = QStringLiteral("%1 13W Net change %2 — context only, not directional in v0.")
                                   .arg(key, cftc_format_evidence_count(changes.net.value));
            return item;
        };

        bool confirmation_available = false;
        const auto append_directional = [&](const QString& rule_id, const QString& key) {
            CftcEvidenceItem item = directional_item(rule_id, key);
            confirmation_available = confirmation_available || item.available;
            participant.items.append(item);
        };
        switch (input.family) {
            case CftcFamily::Legacy:
                append_directional(QStringLiteral("RPART-LEG-NON-REPORTABLE"), QStringLiteral("non_reportable"));
                participant.items.append(
                    context_item(QStringLiteral("RPART-LEG-COMMERCIAL"), QStringLiteral("commercial")));
                break;
            case CftcFamily::Disaggregated:
                append_directional(QStringLiteral("RPART-DIS-OTHER-REPORTABLE"), QStringLiteral("other_reportable"));
                participant.items.append(
                    context_item(QStringLiteral("RPART-DIS-PRODUCER-MERCHANT"), QStringLiteral("producer_merchant")));
                participant.items.append(
                    context_item(QStringLiteral("RPART-DIS-SWAP-DEALER"), QStringLiteral("swap_dealer")));
                break;
            case CftcFamily::Tff:
                append_directional(QStringLiteral("RPART-TFF-ASSET-MANAGER"), QStringLiteral("asset_manager"));
                append_directional(QStringLiteral("RPART-TFF-OTHER-REPORTABLE"), QStringLiteral("other_reportable"));
                participant.items.append(context_item(QStringLiteral("RPART-TFF-DEALER"), QStringLiteral("dealer")));
                participant.items.append(
                    context_item(QStringLiteral("RPART-TFF-NON-REPORTABLE"), QStringLiteral("non_reportable")));
                break;
        }
        cftc_finalize_group(participant, true);
        // Context-only classes are inspectable but are not confirmation: the
        // family counts toward confidence coverage only when at least one
        // actual confirmation item was evaluated.
        participant.counts_for_confidence = confirmation_available;
    }

    // ── Group: data quality / freshness ─────────────────────────────────────
    CftcEvidenceGroup data_quality;
    data_quality.family = CftcEvidenceFamily::DataQuality;
    {
        CftcEvidenceItem current;
        current.rule_id = QStringLiteral("RDQ-REPORT-CURRENT");
        current.family = CftcEvidenceFamily::DataQuality;
        current.horizon = QStringLiteral("current");
        current.direction = CftcEvidenceDirection::Neutral;
        current.available = true;
        if (result.data_available) {
            current.explanation = QStringLiteral("The official %1 report is the current report; no prior report was "
                                                 "substituted.")
                                      .arg(report_date.toString(Qt::ISODate));
        } else if (result.report_missing) {
            current.available = false;
            current.direction = CftcEvidenceDirection::Unavailable;
            current.explanation = QStringLiteral("No official CFTC observations were supplied; the research state is "
                                                 "not constructed.");
        } else if (result.report_predates_history) {
            current.available = false;
            current.direction = CftcEvidenceDirection::Unavailable;
            current.explanation =
                QStringLiteral("The requested as-of report %1 predates the newest returned report "
                               "%2; supply the history truncated at the as-of report.")
                    .arg(report_date.toString(Qt::ISODate), result.latest_available_report_date.toString(Qt::ISODate));
        } else if (result.data_stale) {
            current.available = false;
            current.direction = CftcEvidenceDirection::Unavailable;
            current.explanation =
                QStringLiteral("The %1 series ends at %2 before the %3 as-of report; the prior "
                               "report is not used as the current state.")
                    .arg(spec_label, net_series.last().date.toString(Qt::ISODate), report_date.toString(Qt::ISODate));
        } else {
            current.available = false;
            current.direction = CftcEvidenceDirection::Unavailable;
            current.explanation = QStringLiteral("The %1 report at %2 does not carry a usable Net position; the "
                                                 "prior report is not used as the current state.")
                                      .arg(spec_label, report_date.toString(Qt::ISODate));
        }
        data_quality.items.append(current);

        CftcEvidenceItem coverage;
        coverage.rule_id = QStringLiteral("RDQ-HISTORY-COVERAGE");
        coverage.family = CftcEvidenceFamily::DataQuality;
        coverage.horizon = QStringLiteral("26W-5Y");
        coverage.direction = CftcEvidenceDirection::Neutral;
        coverage.available = true;
        coverage.metrics = {
            cftc_evidence_metric(QStringLiteral("reference_count_26w"), readings.stats_26w.reference_covered,
                                 static_cast<double>(readings.stats_26w.reference_count)),
            cftc_evidence_metric(QStringLiteral("reference_count_52w"), readings.stats_52w.reference_covered,
                                 static_cast<double>(readings.stats_52w.reference_count)),
            cftc_evidence_metric(QStringLiteral("reference_count_2y"), readings.stats_2y.reference_covered,
                                 static_cast<double>(readings.stats_2y.reference_count)),
            cftc_evidence_metric(QStringLiteral("reference_count_5y"), readings.stats_5y.reference_covered,
                                 static_cast<double>(readings.stats_5y.reference_count)),
        };
        coverage.explanation =
            QStringLiteral("Trailing reference coverage: 26W %1, 52W %2, 2Y %3, 5Y %4. Incomplete history is "
                           "unavailable context, not normal positioning.")
                .arg(readings.stats_26w.reference_covered ? QStringLiteral("covered") : QStringLiteral("not covered"),
                     readings.stats_52w.reference_covered ? QStringLiteral("covered") : QStringLiteral("not covered"),
                     readings.stats_2y.reference_covered ? QStringLiteral("covered") : QStringLiteral("not covered"),
                     readings.stats_5y.reference_covered ? QStringLiteral("covered") : QStringLiteral("not covered"));
        data_quality.items.append(coverage);

        const CftcObservation* latest_obs = nullptr;
        for (const auto& obs : input.observations) {
            if (obs.date == report_date)
                latest_obs = &obs;
        }
        if (latest_obs &&
            (latest_obs->traders_total || latest_obs->traders_reportable_long || latest_obs->traders_reportable_short ||
             latest_obs->concentration_gross_4_long || latest_obs->concentration_gross_4_short ||
             latest_obs->concentration_gross_8_long || latest_obs->concentration_gross_8_short ||
             latest_obs->concentration_net_4_long || latest_obs->concentration_net_4_short ||
             latest_obs->concentration_net_8_long || latest_obs->concentration_net_8_short)) {
            CftcEvidenceItem concentration;
            concentration.rule_id = QStringLiteral("RDQ-CONCENTRATION-CONTEXT");
            concentration.family = CftcEvidenceFamily::DataQuality;
            concentration.horizon = QStringLiteral("current");
            concentration.direction = CftcEvidenceDirection::Neutral;
            concentration.available = true;
            concentration.metrics = {
                cftc_evidence_metric(QStringLiteral("traders_total"), latest_obs->traders_total),
                cftc_evidence_metric(QStringLiteral("traders_reportable_long"), latest_obs->traders_reportable_long),
                cftc_evidence_metric(QStringLiteral("traders_reportable_short"), latest_obs->traders_reportable_short),
                cftc_evidence_metric(QStringLiteral("concentration_gross_4_long"),
                                     latest_obs->concentration_gross_4_long),
                cftc_evidence_metric(QStringLiteral("concentration_gross_4_short"),
                                     latest_obs->concentration_gross_4_short),
                cftc_evidence_metric(QStringLiteral("concentration_gross_8_long"),
                                     latest_obs->concentration_gross_8_long),
                cftc_evidence_metric(QStringLiteral("concentration_gross_8_short"),
                                     latest_obs->concentration_gross_8_short),
            };
            concentration.explanation =
                QStringLiteral("Trader-count and concentration fields are present on the current report and exposed "
                               "as secondary context only; their interpretation is not established enough for "
                               "direction or confidence in v0.");
            data_quality.items.append(concentration);
        }
    }
    cftc_finalize_group(data_quality, false);

    result.groups = {tactical_4w, swing_13w,     regime_26w,  historical,
                     price_cot,   open_interest, participant, data_quality};

    // ── Aggregation ─────────────────────────────────────────────────────────
    const CftcTacticalState tactical = horizon_4w.state;
    const CftcTacticalState swing = horizon_13w.state;
    const bool core_opposed = (tactical == CftcTacticalState::Bullish && swing == CftcTacticalState::Bearish) ||
                              (tactical == CftcTacticalState::Bearish && swing == CftcTacticalState::Bullish);
    CftcResearchState candidate = CftcResearchState::Hold;
    if (!core_opposed) {
        if (tactical == CftcTacticalState::Bullish || swing == CftcTacticalState::Bullish)
            candidate = CftcResearchState::Buy;
        else if (tactical == CftcTacticalState::Bearish || swing == CftcTacticalState::Bearish)
            candidate = CftcResearchState::Sell;
    }

    int support_count = 0;
    bool core_supports = false;
    for (const auto& group : result.groups) {
        if (!group.counts_for_independence || !group.available)
            continue;
        const bool supports =
            (candidate == CftcResearchState::Buy && group.direction == CftcEvidenceDirection::Bullish) ||
            (candidate == CftcResearchState::Sell && group.direction == CftcEvidenceDirection::Bearish);
        if (!supports)
            continue;
        ++support_count;
        if (group.family == CftcEvidenceFamily::Tactical4W || group.family == CftcEvidenceFamily::Swing13W)
            core_supports = true;
    }
    const bool directional_candidate = candidate != CftcResearchState::Hold;
    const bool independent_support_satisfied = directional_candidate && support_count >= 2 && core_supports;
    if (independent_support_satisfied)
        result.state = candidate;

    // Items relative to the selected state (or the provisional tendency when
    // the state is HOLD, so the lists still explain what the evidence favoured).
    // The 26W regime is context-only: it stays inspectable in its group but is
    // never listed as state support/conflict and never changes confidence.
    const CftcResearchState tendency = result.state != CftcResearchState::Hold ? result.state : candidate;
    for (const auto& group : result.groups) {
        for (const auto& item : group.items) {
            if (!item.available) {
                result.unavailable.append(item);
                continue;
            }
            if (group.family == CftcEvidenceFamily::Regime26W)
                continue;
            const bool directional =
                item.direction == CftcEvidenceDirection::Bullish || item.direction == CftcEvidenceDirection::Bearish;
            // A conflicted item is evidence against the evaluation, never a
            // supporting item, even when its direction happens to match the
            // provisional tendency.
            if (item.conflicted) {
                result.conflicting.append(item);
            } else if (tendency == CftcResearchState::Buy) {
                if (item.direction == CftcEvidenceDirection::Bullish)
                    result.supporting.append(item);
                else if (directional)
                    result.conflicting.append(item);
            } else if (tendency == CftcResearchState::Sell) {
                if (item.direction == CftcEvidenceDirection::Bearish)
                    result.supporting.append(item);
                else if (directional)
                    result.conflicting.append(item);
            } else {
                if (directional)
                    result.conflicting.append(item);
            }
        }
    }

    // ── Confidence (coverage/agreement, distinct from direction) ────────────
    // Coverage counts evidence families whose availability is evidence quality:
    // the independent directional/confirmation families plus historical
    // context. The 26W regime and data quality never change confidence.
    // Conflicts are counted at family level: several correlated items inside
    // one family (for example both price horizons diverging) cost one penalty,
    // not one per metric. Core opposition subtracts 2 and also counts as one
    // conflict, so it removes 3 points in total and blocks High. A directional
    // tendency that failed the independence gate is Low confidence.
    int coverage = 0;
    for (const auto& group : result.groups) {
        if (!group.counts_for_confidence)
            continue;
        if (group.available)
            ++coverage;
    }
    int conflicts = 0;
    for (const auto& group : result.groups) {
        if (!group.counts_for_confidence || !group.available)
            continue;
        bool family_conflicted = false;
        for (const auto& item : group.items) {
            if (item.available && item.conflicted) {
                family_conflicted = true;
                break;
            }
        }
        const bool family_opposes =
            (tendency == CftcResearchState::Buy && group.direction == CftcEvidenceDirection::Bearish) ||
            (tendency == CftcResearchState::Sell && group.direction == CftcEvidenceDirection::Bullish);
        if (family_conflicted || family_opposes)
            ++conflicts;
    }
    if (core_opposed)
        ++conflicts;
    int score = coverage;
    const bool directional = tactical == CftcTacticalState::Bullish || tactical == CftcTacticalState::Bearish;
    if (directional && tactical == swing)
        score += 1;
    if (core_opposed)
        score -= 2;
    score -= conflicts;
    if (!result.data_available) {
        result.confidence = CftcResearchConfidence::Low;
    } else if (directional_candidate && !independent_support_satisfied) {
        result.confidence = CftcResearchConfidence::Low;
    } else if (score >= 7 && conflicts == 0) {
        result.confidence = CftcResearchConfidence::High;
    } else if (score >= 3) {
        result.confidence = CftcResearchConfidence::Medium;
    } else {
        result.confidence = CftcResearchConfidence::Low;
    }
    return result;
}

} // namespace fincept::services
