// src/screens/economics/panels/CftcInterpretationPresentation.h
//
// Batch 4B: the deterministic presentation/composition layer between the
// finalized Batch 4A CftcInterpretationModel and the CFTC Analysis widgets.
//
// This layer never recalculates an analytical state. The Batch 4A engine
// decides which states exist and with which horizons; this header only maps
// those already-emitted state ids and their emitted metric values into the
// predefined, participant-aware wording that the panel renders. The narrative
// precedence follows the governing plan:
//
//   1. historical positioning level / extreme
//   2. persistence, exit or unwind of the extreme
//   3. recent net repositioning
//   4. gross-leg mechanism
//   5. Open Interest context
//   6. price relationship / divergence
//
// The composer is header-only over Qt Core so the wording contract is
// unit-testable without linking the panel or the widget tree (the same reason
// CftcNetFormat.h exists; see the HARD RULE at the top of tests/CMakeLists.txt).
//
// It emits no BUY/HOLD/SELL, no bullish/bearish score, no predictive
// confidence, no expected return, no forecast and no trading recommendation.
//
// Wording contract after the 2026-09-24 audit corrections: a historically high
// or low Net %OI is phrased by the actual net direction; an unevaluable level,
// persistence or sustained check is stated with its reason instead of falling
// back to plain wording; "remains" needs an established persistent extreme;
// historical and materiality conclusions name their MarketLab thresholds;
// divergence attribution uses the legs' contribution shares; a price
// conclusion names its series, roll/spot caveat and closing sessions; and an
// out-of-date report is flagged in the headline and the first conclusion.
#pragma once

#include "screens/economics/panels/CftcNetFormat.h"
#include "screens/economics/panels/CftcWorkspaceContract.h"
#include "services/economics/CftcInterpretationModel.h"

#include <QCoreApplication>
#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>

namespace fincept::screens {

/// How one inspectable evidence item resolved. `NoMaterialState` is a
/// successfully evaluated dimension that produced no material Batch 4A state;
/// it is deliberately distinct from `Unavailable` (missing data, insufficient
/// history, a broken sequence, a missing price context, and so on) and from
/// `Pending` (price context still loading). `value` carries the concise reason
/// or the exact readout, never a zero or a neutral substitute.
enum class CftcEvidenceStatus { Available, NoMaterialState, Pending, Unavailable };

/// One inspectable piece of supporting evidence. `horizon_reports` is 4 or 13
/// for a horizon-scoped measure so the panel can render those as aligned
/// columns; 0 means a whole-report measure. A single-horizon view carries its
/// own selected horizon on the view and renders every row as label/value.
struct CftcEvidenceItem {
    QString label;
    QString value;
    CftcEvidenceStatus status = CftcEvidenceStatus::Unavailable;
    int horizon_reports = 0; // 0 (whole report) or the measure's report horizon (1, 4, 13)
};

/// The actual price context the caller holds. The prose composer, the evidence
/// area and the synchronized chart all consume this one state so they never
/// describe the same price situation differently.
enum class CftcPriceContextState { Pending, Unavailable, Ready };

/// The predefined display content for one Batch 4A result. The panel renders
/// this without deciding any wording itself. `horizon_reports` is 0 for the
/// combined multi-horizon view and the selected 1/4/13 report horizon for a
/// single-horizon view; the panel uses it to choose the evidence layout.
struct CftcInterpretationView {
    bool interpreted = false; ///< Batch 4A produced an interpretable report
    int horizon_reports = 0;  ///< 0 = combined view; else the selected 1/4/13 horizon
    QString headline;
    QStringList sentences;
    QVector<CftcEvidenceItem> evidence;
    QString context_text;
};

inline QString cftc_presentation_tr(const char* text) {
    return QCoreApplication::translate("CftcPresentation", text);
}

// ── State id / reason vocabulary ────────────────────────────────────────────

/// Short deterministic wording for one Batch 4A state id. Returns an empty
/// string for an id this layer does not own; no unknown id is ever rendered.
inline QString cftc_state_short_wording(const QString& state_id) {
    if (state_id == QLatin1String("NET_LONG"))
        return cftc_presentation_tr("net long");
    if (state_id == QLatin1String("NET_SHORT"))
        return cftc_presentation_tr("net short");
    if (state_id == QLatin1String("NET_FLAT"))
        return cftc_presentation_tr("net flat");
    // Sign-neutral on purpose: a high percentile of a net-short participant's
    // Net %OI means the smallest net short, never a large exposure.
    if (state_id == QLatin1String("HISTORICALLY_HIGH_NET"))
        return cftc_presentation_tr("Net %OI historically high");
    if (state_id == QLatin1String("HISTORICALLY_LOW_NET"))
        return cftc_presentation_tr("Net %OI historically low");
    if (state_id == QLatin1String("CROWDED_LONG"))
        return cftc_presentation_tr("historically crowded long");
    if (state_id == QLatin1String("CROWDED_SHORT"))
        return cftc_presentation_tr("historically crowded short");
    if (state_id == QLatin1String("SEVERE_LONG_EXTREME"))
        return cftc_presentation_tr("severe historical net-long extreme");
    if (state_id == QLatin1String("SEVERE_SHORT_EXTREME"))
        return cftc_presentation_tr("severe historical net-short extreme");
    if (state_id == QLatin1String("LONG_ACCUMULATION"))
        return cftc_presentation_tr("long accumulation");
    if (state_id == QLatin1String("LONG_LIQUIDATION"))
        return cftc_presentation_tr("long liquidation");
    if (state_id == QLatin1String("SHORT_BUILDING"))
        return cftc_presentation_tr("short building");
    if (state_id == QLatin1String("SHORT_COVERING"))
        return cftc_presentation_tr("short covering");
    if (state_id == QLatin1String("NET_LONGWARD_SHIFT"))
        return cftc_presentation_tr("longward shift");
    if (state_id == QLatin1String("NET_SHORTWARD_SHIFT"))
        return cftc_presentation_tr("shortward shift");
    if (state_id == QLatin1String("NET_SHARE_RAW_DISAGREEMENT"))
        return cftc_presentation_tr("raw net / Net %OI disagreement");
    if (state_id == QLatin1String("SUSTAINED_LONGWARD_REPOSITIONING_4R"))
        return cftc_presentation_tr("sustained longward repositioning (4 reports)");
    if (state_id == QLatin1String("SUSTAINED_SHORTWARD_REPOSITIONING_4R"))
        return cftc_presentation_tr("sustained shortward repositioning (4 reports)");
    if (state_id == QLatin1String("SUSTAINED_LONGWARD_REPOSITIONING_13R"))
        return cftc_presentation_tr("sustained longward repositioning (13 reports)");
    if (state_id == QLatin1String("SUSTAINED_SHORTWARD_REPOSITIONING_13R"))
        return cftc_presentation_tr("sustained shortward repositioning (13 reports)");
    if (state_id == QLatin1String("PERSISTENT_HIGH_EXTREME"))
        return cftc_presentation_tr("persistent high extreme");
    if (state_id == QLatin1String("PERSISTENT_LOW_EXTREME"))
        return cftc_presentation_tr("persistent low extreme");
    if (state_id == QLatin1String("EXITED_HIGH_EXTREME"))
        return cftc_presentation_tr("exit from high extreme");
    if (state_id == QLatin1String("EXITED_LOW_EXTREME"))
        return cftc_presentation_tr("exit from low extreme");
    if (state_id == QLatin1String("UNWINDING_HIGH_EXTREME"))
        return cftc_presentation_tr("moved back from a recent high extreme");
    if (state_id == QLatin1String("UNWINDING_LOW_EXTREME"))
        return cftc_presentation_tr("moved back from a recent low extreme");
    if (state_id == QLatin1String("OI_EXPANSION"))
        return cftc_presentation_tr("Open Interest expansion");
    if (state_id == QLatin1String("OI_CONTRACTION"))
        return cftc_presentation_tr("Open Interest contraction");
    if (state_id == QLatin1String("HIGH_MARKET_CONCENTRATION"))
        return cftc_presentation_tr("high market concentration");
    if (state_id == QLatin1String("CONCENTRATION_RISING"))
        return cftc_presentation_tr("rising market concentration");
    return {};
}

/// Deterministic wording for the four contemporaneous price relationship
/// states. "Divergence" is retained for the two opposing-direction states.
inline QString cftc_relationship_wording(const QString& state_id) {
    if (state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_UP"))
        return cftc_presentation_tr("Moving together up");
    if (state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_DOWN"))
        return cftc_presentation_tr("Moving together down");
    if (state_id == QLatin1String("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"))
        return cftc_presentation_tr("Divergence: price up / positioning down");
    if (state_id == QLatin1String("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"))
        return cftc_presentation_tr("Divergence: price down / positioning up");
    return {};
}

inline bool cftc_is_divergence_relationship(const QString& state_id) {
    return state_id == QLatin1String("PRICE_UP_POSITIONING_DOWN_DIVERGENCE") ||
           state_id == QLatin1String("PRICE_DOWN_POSITIONING_UP_DIVERGENCE");
}

/// Concise human reason for a Batch 4A unavailable code.
inline QString cftc_unavailable_reason_wording(services::CftcUnavailableReason reason) {
    using services::CftcUnavailableReason;
    switch (reason) {
        case CftcUnavailableReason::NoObservations:
            return cftc_presentation_tr("no official CFTC observations are available");
        case CftcUnavailableReason::StaleCurrentReport:
            return cftc_presentation_tr("the current report is stale");
        case CftcUnavailableReason::MissingParticipantLeg:
            return cftc_presentation_tr("the reported long or short leg is missing");
        case CftcUnavailableReason::MissingOpenInterest:
            return cftc_presentation_tr("Open Interest is missing");
        case CftcUnavailableReason::NonPositiveOpenInterest:
            return cftc_presentation_tr("Open Interest is zero or negative");
        case CftcUnavailableReason::InsufficientHistory:
            return cftc_presentation_tr("fewer than the 156 prior reports this reference requires are available");
        case CftcUnavailableReason::InsufficientHorizonHistory:
            return cftc_presentation_tr("the history does not reach back far enough for this comparison");
        case CftcUnavailableReason::InsufficientPriceHistory:
            return cftc_presentation_tr("fewer than 156 prior comparable price moves are available");
        case CftcUnavailableReason::InsufficientConcentrationHistory:
            return cftc_presentation_tr("fewer than %1 prior concentration readings are available")
                .arg(services::kCftcTrailingMinObservations);
        case CftcUnavailableReason::PriceSessionsNotDistinct:
            return cftc_presentation_tr(
                "both report dates resolve to the same price session, so there is no price move to describe");
        case CftcUnavailableReason::ReportBasisMismatch:
            return cftc_presentation_tr("the rows' published report basis does not match the requested basis");
        case CftcUnavailableReason::BrokenReportSequence:
            return cftc_presentation_tr("the weekly report sequence is broken");
        case CftcUnavailableReason::MissingPriceContext:
            return cftc_presentation_tr("no price observations were supplied");
        case CftcUnavailableReason::UnspecifiedPriceSource:
            return cftc_presentation_tr("the price source is unspecified");
        case CftcUnavailableReason::PriceHistoryIncomplete:
            return cftc_presentation_tr("price history does not reach the report date");
        case CftcUnavailableReason::PriceContextStale:
            return cftc_presentation_tr("the price close is too old to describe the report date");
        case CftcUnavailableReason::PriceContextUnusable:
            return cftc_presentation_tr("the price close is unusable");
        case CftcUnavailableReason::MissingConcentrationField:
            return cftc_presentation_tr("the concentration field is missing");
        case CftcUnavailableReason::MissingContractIdentity:
            return cftc_presentation_tr("the contract identity is missing");
        case CftcUnavailableReason::MixedContractIdentity:
            return cftc_presentation_tr("the history mixes contract identities");
        case CftcUnavailableReason::ParticipantCountMismatch:
            return cftc_presentation_tr("participant slots do not match the report family");
        case CftcUnavailableReason::FamilyProvenanceMismatch:
            return cftc_presentation_tr("the family provenance does not match");
        case CftcUnavailableReason::ReportBasisUnspecified:
            return cftc_presentation_tr("the report basis is unspecified");
        case CftcUnavailableReason::None:
            break;
    }
    return cftc_presentation_tr("the dimension is unavailable");
}

inline QString cftc_evidence_basis_wording(services::CftcEvidenceBasis basis) {
    return services::cftc_evidence_basis_code(basis);
}

/// Participant display name derived from the finalized terminology metadata,
/// not from the Batch 1 label or the speculative flag. The Legacy headline
/// class is the neutral "Non-Commercial" (the plan's broad non-commercial
/// category), never a speculator label; Other Reportables / Non-Reportables
/// stay neutral and TFF keeps its own classes.
inline QString cftc_participant_display_name(const services::CftcParticipantInterpretation& participant) {
    using services::CftcTerminologyClass;
    switch (participant.terminology) {
        case CftcTerminologyClass::BroadNonCommercial:
            return cftc_presentation_tr("Non-Commercial");
        case CftcTerminologyClass::CommercialNeutral:
            return cftc_presentation_tr("Commercial");
        case CftcTerminologyClass::ManagedMoney:
            return cftc_presentation_tr("Managed Money");
        case CftcTerminologyClass::ProducerMerchant:
            return cftc_presentation_tr("Producer/Merchant/Processor/User");
        case CftcTerminologyClass::SwapDealer:
            return cftc_presentation_tr("Swap Dealers");
        case CftcTerminologyClass::OtherReportableNeutral:
            return cftc_presentation_tr("Other Reportables");
        case CftcTerminologyClass::NonReportableNeutral:
            return cftc_presentation_tr("Non-Reportable");
        case CftcTerminologyClass::LeveragedFunds:
            return cftc_presentation_tr("Leveraged Funds");
        case CftcTerminologyClass::AssetManagerInstitutional:
            return cftc_presentation_tr("Asset Manager/Institutional");
        case CftcTerminologyClass::DealerIntermediary:
            return cftc_presentation_tr("Dealer/Intermediary");
        case CftcTerminologyClass::Unknown:
            break;
    }
    return participant.label;
}

/// The neutral display label for a metric-model participant (the R3 numerical
/// sections that predate Batch 4A). The Legacy broad non-commercial class is
/// rendered with its Batch 4A terminology name; the historical
/// "Non-Commercial (Speculators)" metric label is never surfaced. Other
/// classes keep their metric-model label unchanged.
inline QString cftc_metric_participant_display_name(services::CftcFamily family, const QString& participant_key,
                                                    const QString& fallback_label) {
    if (services::cftc_participant_terminology(family, participant_key) ==
        services::CftcTerminologyClass::BroadNonCommercial)
        return cftc_presentation_tr("Non-Commercial");
    return fallback_label;
}

/// The principal participant key defined by the finalized Batch 4A family and
/// terminology contract: Legacy Non-Commercial (BroadNonCommercial),
/// Disaggregated Managed Money (ManagedMoney), TFF Leveraged Funds
/// (LeveragedFunds). This never consults the generic `speculative` flag, and it
/// returns an empty string rather than a substituted participant when the
/// family's principal class cannot be resolved.
inline QString cftc_principal_participant_key(services::CftcFamily family) {
    using services::CftcTerminologyClass;
    const QVector<services::CftcParticipant> participants = services::cftc_family_participants(family);
    for (const auto& participant : participants) {
        const CftcTerminologyClass terminology = services::cftc_participant_terminology(family, participant.key);
        if (terminology == CftcTerminologyClass::BroadNonCommercial ||
            terminology == CftcTerminologyClass::ManagedMoney || terminology == CftcTerminologyClass::LeveragedFunds)
            return participant.key;
    }
    return {};
}

/// One concise terminology caveat sentence, emitted only for the categories the
/// frozen Batch 4A contract marks with a caveat code. Neutral categories carry
/// no invented caveat, and the sentence is used once per conclusion rather than
/// repeated in every sentence.
inline QString cftc_terminology_caveat_sentence(const services::CftcParticipantInterpretation& participant) {
    if (participant.terminology_caveat_code == QLatin1String("broad_category_caveat"))
        return cftc_presentation_tr("Non-Commercial is the CFTC's broad non-commercial category; the crowding "
                                    "description applies to aggregate reported positioning only.");
    if (participant.terminology_caveat_code == QLatin1String("not_all_outright_speculation_caveat"))
        return cftc_presentation_tr("Leveraged Funds is the CFTC's leveraged-funds category; not all positions in "
                                    "it are outright speculation.");
    return {};
}

// ── State indexing helpers (read-only) ──────────────────────────────────────

inline const services::CftcInterpretationState*
cftc_presentation_state(const QVector<services::CftcInterpretationState>& states, const QString& state_id,
                        int horizon = -1) {
    for (const auto& state : states) {
        if (state.state_id != state_id)
            continue;
        if (horizon >= 0 && (!state.has_horizon || state.horizon_reports != horizon))
            continue;
        return &state;
    }
    return nullptr;
}

/// Whether the level conclusion renders crowding wording (and therefore whether
/// the terminology caveat belongs in the composed conclusion). A co-emitted
/// severe extreme takes precedence over crowding, so the caveat is not emitted
/// for a conclusion that no longer says "crowded".
inline bool cftc_exposure_is_crowded(const services::CftcParticipantInterpretation& participant) {
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_LONG_EXTREME")) ||
        cftc_presentation_state(participant.states, QStringLiteral("SEVERE_SHORT_EXTREME")))
        return false;
    return participant.crowding_terminology_allowed &&
           (cftc_presentation_state(participant.states, QStringLiteral("CROWDED_LONG")) != nullptr ||
            cftc_presentation_state(participant.states, QStringLiteral("CROWDED_SHORT")) != nullptr);
}

inline const services::CftcStateMetric* cftc_presentation_metric(const QVector<services::CftcStateMetric>& metrics,
                                                                 const QString& key) {
    for (const auto& metric : metrics) {
        if (metric.key == key)
            return &metric;
    }
    return nullptr;
}

inline const services::CftcUnavailableRecord*
cftc_presentation_unavailable(const QVector<services::CftcUnavailableRecord>& records, const QString& state_family,
                              const QString& participant_key, int horizon = -1) {
    for (const auto& record : records) {
        if (record.state_family != state_family)
            continue;
        if (!participant_key.isEmpty() && record.participant_key != participant_key)
            continue;
        if (horizon >= 0 && (!record.has_horizon || record.horizon_reports != horizon))
            continue;
        return &record;
    }
    return nullptr;
}

inline const services::CftcPricePositionAssessment*
cftc_presentation_price(const services::CftcInterpretationResult& result, const QString& participant_key, int horizon) {
    for (const auto& assessment : result.price_context) {
        if (assessment.participant_key == participant_key && assessment.horizon_reports == horizon)
            return &assessment;
    }
    return nullptr;
}

/// The participant's raw flow measurement for one horizon, when the engine
/// emitted one (every configured horizon emits a reading, including
/// below-threshold horizons).
inline const services::CftcHorizonFlowReading*
cftc_presentation_flow_reading(const services::CftcParticipantInterpretation& participant, int horizon) {
    for (const auto& reading : participant.flow_readings) {
        if (reading.horizon_reports == horizon)
            return &reading;
    }
    return nullptr;
}

/// The market-level Open Interest measurement for one horizon.
inline const services::CftcOpenInterestReading*
cftc_presentation_oi_reading(const services::CftcInterpretationResult& result, int horizon) {
    for (const auto& reading : result.open_interest_readings) {
        if (reading.horizon_reports == horizon)
            return &reading;
    }
    return nullptr;
}

/// Marker appended to a measurement that was evaluated but stayed below the
/// materiality threshold, so the row keeps its number and still states why no
/// state was emitted.
inline QString cftc_below_threshold_suffix() {
    return cftc_presentation_tr(" (below threshold)");
}

/// Marker for a raw measurement whose materiality comparison could not be
/// performed at all (the strictly trailing reference was too short or
/// otherwise unavailable). The value is still a valid observation, but the row
/// must not claim a threshold comparison happened: it carries the engine's own
/// reason instead of "below threshold".
inline QString cftc_materiality_unavailable_suffix(const QString& reason) {
    return cftc_presentation_tr(" (materiality unavailable — %1)").arg(reason);
}

/// Unavailable reason for the secondary NET Δ cell. This is CFTC positioning
/// evidence only: the participant's NET_SHIFT record, then its horizon reading,
/// then nothing. A price assessment reason (which gives a missing price context
/// precedence) must never label a positioning cell.
inline QString cftc_net_delta_unavailable_reason(const services::CftcInterpretationResult& result,
                                                 const QString& participant_key, int horizon) {
    if (const auto* record =
            cftc_presentation_unavailable(result.unavailable, QStringLiteral("NET_SHIFT"), participant_key, horizon))
        return cftc_unavailable_reason_wording(record->reason);
    for (const auto& participant : result.participants) {
        if (participant.participant_key != participant_key)
            continue;
        for (const auto& reading : participant.flow_readings) {
            if (reading.horizon_reports == horizon && !reading.evaluated)
                return cftc_unavailable_reason_wording(reading.reason);
        }
    }
    return QString();
}

// ── Indexed horizons ────────────────────────────────────────────────────────

/// Read-only summary of the states Batch 4A already emitted for one participant
/// and one report horizon. This does not classify anything; it only indexes
/// emitted states and emitted unavailable records.
struct CftcHorizonFlowSummary {
    int horizon = 0;
    bool evaluated = true; ///< false when the horizon itself is unavailable
    QString unavailable_reason;
    bool material_net_move = false;
    int net_direction = 0; ///< -1 shortward, 0 none, +1 longward
    bool sustained = false;
    int sustained_steps = 0;    ///< weekly steps in the move's direction (from the engine metric)
    int sustained_required = 0; ///< steps the sustained rule requires (3 of 4 or 9 of 13)
    /// Concrete reason when the sustained check could not be evaluated at this
    /// horizon (for example a missing leg inside the window); empty otherwise.
    QString sustained_unavailable_reason;
    bool long_accumulation = false;
    bool long_liquidation = false;
    bool short_building = false;
    bool short_covering = false;
};

inline CftcHorizonFlowSummary cftc_summarize_horizon(const services::CftcParticipantInterpretation& participant,
                                                     const QVector<services::CftcUnavailableRecord>& unavailable,
                                                     int horizon) {
    CftcHorizonFlowSummary out;
    out.horizon = horizon;
    auto has = [&](const QString& state_id) {
        return cftc_presentation_state(participant.states, state_id, horizon) != nullptr;
    };
    out.long_accumulation = has(QStringLiteral("LONG_ACCUMULATION"));
    out.long_liquidation = has(QStringLiteral("LONG_LIQUIDATION"));
    out.short_building = has(QStringLiteral("SHORT_BUILDING"));
    out.short_covering = has(QStringLiteral("SHORT_COVERING"));
    if (has(QStringLiteral("NET_LONGWARD_SHIFT")))
        out.net_direction = 1;
    else if (has(QStringLiteral("NET_SHORTWARD_SHIFT")))
        out.net_direction = -1;
    out.material_net_move = out.net_direction != 0;
    // Sustained repositioning is only defined at the fixed four- and
    // thirteen-report horizons (plan section 8.4); the one-report reading must
    // never inherit a thirteen-report sustained state.
    if (horizon == 4 || horizon == 13) {
        const QString sustained_long = horizon == 4 ? QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R")
                                                    : QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R");
        const QString sustained_short = horizon == 4 ? QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_4R")
                                                     : QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_13R");
        out.sustained = has(sustained_long) || has(sustained_short);
        out.sustained_required = horizon == 4 ? 3 : 9;
        const services::CftcInterpretationState* sustained_state =
            cftc_presentation_state(participant.states, sustained_long, horizon);
        if (!sustained_state)
            sustained_state = cftc_presentation_state(participant.states, sustained_short, horizon);
        if (sustained_state) {
            if (const auto* steps =
                    cftc_presentation_metric(sustained_state->metrics, QStringLiteral("confirming_steps"));
                steps && steps->has_value)
                out.sustained_steps = static_cast<int>(steps->value);
        }
        if (const services::CftcUnavailableRecord* sustained_record = cftc_presentation_unavailable(
                unavailable, QStringLiteral("SUSTAINED_REPOSITIONING"), participant.participant_key, horizon))
            out.sustained_unavailable_reason = cftc_unavailable_reason_wording(sustained_record->reason);
    }
    const services::CftcUnavailableRecord* record =
        cftc_presentation_unavailable(unavailable, QStringLiteral("NET_SHIFT"), participant.participant_key, horizon);
    if (record) {
        out.evaluated = false;
        out.unavailable_reason = cftc_unavailable_reason_wording(record->reason);
    }
    return out;
}

// ── Phrases and sentences ───────────────────────────────────────────────────

/// The reported net direction: +1 net long, -1 net short, 0 flat, and
/// `known == false` when no exposure state was emitted.
struct CftcNetSign {
    bool known = false;
    int sign = 0;
};

inline CftcNetSign cftc_net_sign(const services::CftcParticipantInterpretation& participant) {
    CftcNetSign out;
    if (cftc_presentation_state(participant.states, QStringLiteral("NET_LONG"))) {
        out.known = true;
        out.sign = 1;
    } else if (cftc_presentation_state(participant.states, QStringLiteral("NET_SHORT"))) {
        out.known = true;
        out.sign = -1;
    } else if (cftc_presentation_state(participant.states, QStringLiteral("NET_FLAT"))) {
        out.known = true;
    }
    return out;
}

/// Family-appropriate exposure phrase. The severe historical extreme takes
/// precedence over ordinary crowding so a co-emitted severe state is never
/// silently downgraded. Crowding wording is used only when the engine emitted a
/// crowding state (which it does only where the Batch 4A semantics permit it)
/// and the participant carries the applicability flag. A historically high or
/// low Net %OI is phrased by the actual net direction: a high percentile of a
/// net-short participant is its smallest recent net short, never "high
/// exposure". When the historical comparison could not be evaluated the phrase
/// says so instead of silently falling back to the plain direction.
inline QString cftc_exposure_phrase(const services::CftcParticipantInterpretation& participant) {
    const bool crowded_long = participant.crowding_terminology_allowed &&
                              cftc_presentation_state(participant.states, QStringLiteral("CROWDED_LONG")) != nullptr;
    const bool crowded_short = participant.crowding_terminology_allowed &&
                               cftc_presentation_state(participant.states, QStringLiteral("CROWDED_SHORT")) != nullptr;
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_LONG_EXTREME")))
        return cftc_presentation_tr("historically severe net long");
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_SHORT_EXTREME")))
        return cftc_presentation_tr("historically severe net short");
    if (crowded_long)
        return cftc_presentation_tr("historically crowded long");
    if (crowded_short)
        return cftc_presentation_tr("historically crowded short");
    const CftcNetSign net = cftc_net_sign(participant);
    const bool high = cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_HIGH_NET")) != nullptr;
    const bool low = cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_LOW_NET")) != nullptr;
    if (high || low) {
        if (net.known && net.sign > 0)
            return high ? cftc_presentation_tr("historically large net long")
                        : cftc_presentation_tr("net long, historically small");
        if (net.known && net.sign < 0)
            return high ? cftc_presentation_tr("net short, historically small")
                        : cftc_presentation_tr("historically large net short");
        if (net.known)
            return high ? cftc_presentation_tr("net flat, Net %OI historically high")
                        : cftc_presentation_tr("net flat, Net %OI historically low");
        return high ? cftc_presentation_tr("Net %OI historically high")
                    : cftc_presentation_tr("Net %OI historically low");
    }
    QString direction;
    if (net.known)
        direction = net.sign > 0   ? cftc_presentation_tr("net long")
                    : net.sign < 0 ? cftc_presentation_tr("net short")
                                   : cftc_presentation_tr("net flat");
    else
        return cftc_presentation_tr("positioning unavailable");
    if (!participant.historical_percentile_available)
        return cftc_presentation_tr("%1 (historical level unavailable)").arg(direction);
    return direction;
}

/// Share-of-tail wording for a percentile threshold: 0.90 -> "10%",
/// 0.975 -> "2.5%".
inline QString cftc_tail_share_text(double tail_fraction) {
    return QString::number(tail_fraction * 100.0, 'g', 3) + QLatin1Char('%');
}

/// The inline heuristic qualifier carried by every historical-level
/// conclusion, so a MarketLab threshold is never presented as a CFTC fact:
/// "Net %OI in the top 10% of its previous 156 reports; MarketLab threshold".
inline QString cftc_level_threshold_note(const services::CftcInterpretationResult& result,
                                         const services::CftcParticipantInterpretation& participant, bool high,
                                         bool severe) {
    const services::CftcInterpretationConfig& config = result.config;
    const double tail = severe ? (high ? 1.0 - config.severe_extreme_percentile : config.low_severe_extreme_percentile)
                               : (high ? 1.0 - config.extreme_percentile : config.low_extreme_percentile);
    const int reference =
        participant.percentile_reference_count > 0 ? participant.percentile_reference_count : config.history_window;
    return high ? cftc_presentation_tr("Net %OI in the top %1 of its previous %2 reports; MarketLab threshold")
                      .arg(cftc_tail_share_text(tail))
                      .arg(reference)
                : cftc_presentation_tr("Net %OI in the bottom %1 of its previous %2 reports; MarketLab threshold")
                      .arg(cftc_tail_share_text(tail))
                      .arg(reference);
}

inline QString cftc_horizon_phrase(int horizon) {
    if (horizon == 1)
        return cftc_presentation_tr("one report");
    if (horizon == 4)
        return cftc_presentation_tr("four reports");
    if (horizon == 13)
        return cftc_presentation_tr("thirteen reports");
    return cftc_presentation_tr("%1 reports").arg(horizon);
}

inline QString cftc_direction_word(int direction) {
    return direction > 0 ? cftc_presentation_tr("longward") : cftc_presentation_tr("shortward");
}

/// The concrete reason the participant's historical comparison could not be
/// evaluated, from the engine's HISTORICAL_RELATIVE_STATE record.
inline QString cftc_historical_level_unavailable_reason(const services::CftcInterpretationResult& result,
                                                        const services::CftcParticipantInterpretation& participant) {
    if (const auto* record = cftc_presentation_unavailable(
            result.unavailable, QStringLiteral("HISTORICAL_RELATIVE_STATE"), participant.participant_key))
        return cftc_unavailable_reason_wording(record->reason);
    return cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
}

/// Historical-level sentence (precedence 1). The severe historical extreme is
/// checked before ordinary crowding so a co-emitted severe state keeps its
/// specificity. Every historical conclusion carries its MarketLab threshold
/// inline; "remains" is only used when the engine established persistence;
/// a high or low Net %OI is phrased by the actual net direction; and an
/// unavailable historical comparison is stated with its reason instead of
/// falling back to the plain direction as if nothing were missing.
inline QString cftc_level_sentence(const services::CftcInterpretationResult& result,
                                   const services::CftcParticipantInterpretation& participant,
                                   const QString& net_unavailable_reason) {
    const QString name = cftc_participant_display_name(participant);
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_LONG_EXTREME")))
        return cftc_presentation_tr("%1 sits at a severe historical net-long extreme (%2).")
            .arg(name, cftc_level_threshold_note(result, participant, true, true));
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_SHORT_EXTREME")))
        return cftc_presentation_tr("%1 sits at a severe historical net-short extreme (%2).")
            .arg(name, cftc_level_threshold_note(result, participant, false, true));
    const bool persistent_high =
        cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_HIGH_EXTREME")) != nullptr;
    const bool persistent_low =
        cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_LOW_EXTREME")) != nullptr;
    if (participant.crowding_terminology_allowed &&
        cftc_presentation_state(participant.states, QStringLiteral("CROWDED_LONG"))) {
        return (persistent_high ? cftc_presentation_tr("%1 remains unusually net long relative to its recent history "
                                                       "(%2).")
                                : cftc_presentation_tr("%1 is unusually net long relative to its recent history (%2)."))
            .arg(name, cftc_level_threshold_note(result, participant, true, false));
    }
    if (participant.crowding_terminology_allowed &&
        cftc_presentation_state(participant.states, QStringLiteral("CROWDED_SHORT"))) {
        return (persistent_low ? cftc_presentation_tr("%1 remains unusually net short relative to its recent history "
                                                      "(%2).")
                               : cftc_presentation_tr("%1 is unusually net short relative to its recent history (%2)."))
            .arg(name, cftc_level_threshold_note(result, participant, false, false));
    }
    const CftcNetSign net = cftc_net_sign(participant);
    const bool high = cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_HIGH_NET")) != nullptr;
    const bool low = cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_LOW_NET")) != nullptr;
    if (high || low) {
        const QString note = cftc_level_threshold_note(result, participant, high, false);
        if (net.known && net.sign > 0)
            return high ? cftc_presentation_tr("%1 net long exposure is unusually large relative to its recent history "
                                               "(%2).")
                              .arg(name, note)
                        : cftc_presentation_tr("%1 is net long, but the net long position is unusually small relative "
                                               "to its recent history (%2).")
                              .arg(name, note);
        if (net.known && net.sign < 0)
            return high ? cftc_presentation_tr("%1 is net short, but the net short position is unusually small "
                                               "relative to its recent history (%2).")
                              .arg(name, note)
                        : cftc_presentation_tr("%1 net short exposure is unusually large relative to its recent "
                                               "history (%2).")
                              .arg(name, note);
        return high ? cftc_presentation_tr("%1 reported long and short positions are equal; that Net %OI is "
                                           "historically high relative to its recent history (%2).")
                          .arg(name, note)
                    : cftc_presentation_tr("%1 reported long and short positions are equal; that Net %OI is "
                                           "historically low relative to its recent history (%2).")
                          .arg(name, note);
    }
    QString direction;
    if (net.known && net.sign > 0)
        direction = cftc_presentation_tr("%1 is net long in the latest report").arg(name);
    else if (net.known && net.sign < 0)
        direction = cftc_presentation_tr("%1 is net short in the latest report").arg(name);
    else if (net.known)
        direction =
            cftc_presentation_tr("%1 reported long and short positions are equal in the latest report").arg(name);
    else
        return cftc_presentation_tr("Current %1 net exposure is unavailable: %2.")
            .arg(name, net_unavailable_reason.isEmpty()
                           ? cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingParticipantLeg)
                           : net_unavailable_reason);
    if (!participant.historical_percentile_available)
        return cftc_presentation_tr("%1; its historical level could not be evaluated: %2.")
            .arg(direction, cftc_historical_level_unavailable_reason(result, participant));
    return direction + QLatin1Char('.');
}

/// Extreme persistence / exit / unwind sentence (precedence 2). When no
/// extreme-transition state was emitted but the engine could not evaluate one
/// (for example the persistence run reaches past the available history or
/// crosses a missing report), the sentence states that explicitly instead of
/// staying silent. Empty when there is nothing to report.
inline QString cftc_extreme_sentence(const services::CftcInterpretationResult& result,
                                     const services::CftcParticipantInterpretation& participant) {
    const services::CftcInterpretationConfig& config = result.config;
    if (cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_HIGH_EXTREME")))
        return cftc_presentation_tr("Net %OI has fallen back to or below the %1 percentile within %2 reports of a "
                                    "persistent high extreme (MarketLab threshold).")
            .arg(QString::number(config.unwind_reentry_percentile * 100.0, 'g', 3) + QStringLiteral("th"))
            .arg(config.unwind_lookback_reports);
    if (cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_LOW_EXTREME")))
        return cftc_presentation_tr("Net %OI has risen back to or above the %1 percentile within %2 reports of a "
                                    "persistent low extreme (MarketLab threshold).")
            .arg(QString::number(config.low_unwind_reentry_percentile * 100.0, 'g', 3) + QStringLiteral("th"))
            .arg(config.unwind_lookback_reports);
    if (cftc_presentation_state(participant.states, QStringLiteral("EXITED_HIGH_EXTREME")))
        return cftc_presentation_tr("Net %OI moved back below the high extreme band in the latest report.");
    if (cftc_presentation_state(participant.states, QStringLiteral("EXITED_LOW_EXTREME")))
        return cftc_presentation_tr("Net %OI moved back above the low extreme band in the latest report.");
    if (cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_HIGH_EXTREME")))
        return cftc_presentation_tr("The elevated reading has persisted for at least %1 consecutive reports.")
            .arg(config.persistent_extreme_reports);
    if (cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_LOW_EXTREME")))
        return cftc_presentation_tr("The depressed reading has persisted for at least %1 consecutive reports.")
            .arg(config.persistent_extreme_reports);

    // No transition state: say which check could not be performed.
    const bool in_high_band =
        cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_HIGH_NET")) != nullptr;
    const bool in_low_band =
        cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_LOW_NET")) != nullptr;
    const services::CftcUnavailableRecord* persistence_record = nullptr;
    const services::CftcUnavailableRecord* any_record = nullptr;
    for (const auto& record : result.unavailable) {
        if (record.state_family != QLatin1String("EXTREME_TRANSITION") ||
            record.participant_key != participant.participant_key)
            continue;
        if (!any_record)
            any_record = &record;
        if ((in_high_band && record.state_id == QLatin1String("PERSISTENT_HIGH_EXTREME")) ||
            (in_low_band && record.state_id == QLatin1String("PERSISTENT_LOW_EXTREME")))
            persistence_record = &record;
    }
    if (persistence_record)
        return cftc_presentation_tr("Whether the extreme reading has persisted for %1 consecutive reports could not be "
                                    "evaluated: %2.")
            .arg(QString::number(config.persistent_extreme_reports),
                 cftc_unavailable_reason_wording(persistence_record->reason));
    // The whole-family record (empty state id) repeats the historical-level
    // reason already stated by the level sentence.
    if (any_record && !any_record->state_id.isEmpty())
        return cftc_presentation_tr("The extreme persistence, exit and unwind checks could not be evaluated: %1.")
            .arg(cftc_unavailable_reason_wording(any_record->reason));
    return {};
}

/// The sustained-repositioning statement for one four- or thirteen-report
/// horizon with a material net move: the actual count of weekly steps in the
/// move's direction and the rule's requirement, or the concrete reason the
/// check could not be evaluated. Empty when neither applies.
inline QString cftc_sustained_sentence(const CftcHorizonFlowSummary& summary) {
    if (!summary.evaluated || !summary.material_net_move || (summary.horizon != 4 && summary.horizon != 13))
        return {};
    if (summary.sustained) {
        if (summary.sustained_steps > 0)
            return cftc_presentation_tr("The move was sustained: net positioning moved %1 in %2 of the last %3 weekly "
                                        "reports (at least %4 required; MarketLab threshold).")
                .arg(cftc_direction_word(summary.net_direction))
                .arg(summary.sustained_steps)
                .arg(summary.horizon)
                .arg(summary.sustained_required);
        return cftc_presentation_tr("The move was sustained: net positioning moved %1 in at least %2 of the last %3 "
                                    "weekly reports (MarketLab threshold).")
            .arg(cftc_direction_word(summary.net_direction))
            .arg(summary.sustained_required)
            .arg(summary.horizon);
    }
    if (!summary.sustained_unavailable_reason.isEmpty())
        return cftc_presentation_tr("Whether the move was sustained across the weekly reports could not be evaluated: "
                                    "%1.")
            .arg(summary.sustained_unavailable_reason);
    return {};
}

/// Recent net repositioning sentence (precedence 3). The one-report horizon is
/// reported as the latest weekly development and never overrides the broader
/// four- and thirteen-report picture; a four/thirteen disagreement is exposed
/// as mixed rather than resolved.
inline QStringList cftc_repositioning_sentences(const CftcHorizonFlowSummary& h4, const CftcHorizonFlowSummary& h13,
                                                const CftcHorizonFlowSummary& h1) {
    QStringList out;
    const bool h4_material = h4.evaluated && h4.material_net_move;
    const bool h13_material = h13.evaluated && h13.material_net_move;
    auto append_sustained = [&out](const CftcHorizonFlowSummary& summary) {
        const QString sentence = cftc_sustained_sentence(summary);
        if (!sentence.isEmpty())
            out << sentence;
    };
    if (h4_material && h13_material && h4.net_direction != h13.net_direction) {
        out << cftc_presentation_tr("Positioning is mixed across horizons: %1 over four reports but still %2 over "
                                    "thirteen reports.")
                   .arg(cftc_direction_word(h4.net_direction), cftc_direction_word(h13.net_direction));
    } else if (h4_material && h13_material) {
        out << cftc_presentation_tr("Net positioning shifted %1 over the last four and thirteen reports.")
                   .arg(cftc_direction_word(h4.net_direction));
        append_sustained(h4);
        append_sustained(h13);
    } else if (h4_material) {
        out << cftc_presentation_tr("Net positioning shifted %1 over the last four reports.")
                   .arg(cftc_direction_word(h4.net_direction));
        append_sustained(h4);
    } else if (h13_material) {
        out << cftc_presentation_tr("Net positioning shifted %1 over the last thirteen reports.")
                   .arg(cftc_direction_word(h13.net_direction));
        append_sustained(h13);
    } else if (h1.evaluated && h1.material_net_move) {
        out << cftc_presentation_tr("The latest weekly report showed a %1 change; a single week does not override the "
                                    "broader four- and thirteen-report context.")
                   .arg(cftc_direction_word(h1.net_direction));
    } else if (!h4.evaluated || !h13.evaluated) {
        const QString reason = !h4.evaluated ? h4.unavailable_reason : h13.unavailable_reason;
        out << cftc_presentation_tr("Recent net repositioning is unavailable: %1.").arg(reason);
    } else {
        out << cftc_presentation_tr("No material net repositioning was reported over the supported horizons.");
    }

    if (h1.evaluated && h1.material_net_move && (h4_material || h13_material)) {
        const int broader = h4_material ? h4.net_direction : h13.net_direction;
        if (h1.net_direction != broader)
            out << cftc_presentation_tr("The latest weekly report moved the other way (%1).")
                       .arg(cftc_direction_word(h1.net_direction));
        else
            out << cftc_presentation_tr("The latest weekly report moved in the same direction (latest weekly "
                                        "development only).");
    }
    return out;
}

/// Gross-leg mechanism sentence (precedence 4). Uses the mechanism ids Batch 4A
/// attached to the leading net-shift state when present, otherwise the gross
/// states emitted at the leading horizon. Empty when no gross leg is material.
/// `label_horizon` names the horizon in the sentence; the combined view needs
/// it because its mechanism can come from a different horizon than its
/// repositioning sentence.
inline QString cftc_gross_mechanism_sentence(const services::CftcInterpretationState* leading_net_state,
                                             const CftcHorizonFlowSummary& leading, bool label_horizon = false) {
    const QString sentence = [&]() -> QString {
        QStringList mechanisms;
        if (leading_net_state) {
            for (const QString& id : leading_net_state->mechanism_state_ids) {
                if (id == QLatin1String("LONG_ACCUMULATION") || id == QLatin1String("LONG_LIQUIDATION") ||
                    id == QLatin1String("SHORT_BUILDING") || id == QLatin1String("SHORT_COVERING"))
                    mechanisms << id;
            }
        }
        if (mechanisms.isEmpty()) {
            if (leading.long_accumulation)
                mechanisms << QStringLiteral("LONG_ACCUMULATION");
            if (leading.long_liquidation)
                mechanisms << QStringLiteral("LONG_LIQUIDATION");
            if (leading.short_building)
                mechanisms << QStringLiteral("SHORT_BUILDING");
            if (leading.short_covering)
                mechanisms << QStringLiteral("SHORT_COVERING");
        }
        if (mechanisms.isEmpty())
            return {};
        const bool accumulation = mechanisms.contains(QStringLiteral("LONG_ACCUMULATION"));
        const bool liquidation = mechanisms.contains(QStringLiteral("LONG_LIQUIDATION"));
        const bool building = mechanisms.contains(QStringLiteral("SHORT_BUILDING"));
        const bool covering = mechanisms.contains(QStringLiteral("SHORT_COVERING"));
        if (accumulation && covering)
            return cftc_presentation_tr(
                "Long exposure increased while short positions were reduced, producing a material longward shift.");
        if (liquidation && building)
            return cftc_presentation_tr(
                "Long exposure decreased while short positions increased, producing a material shortward shift.");
        if (accumulation && building)
            return cftc_presentation_tr(
                "Long and short exposure both increased materially; the net move was comparatively small.");
        if (liquidation && covering)
            return cftc_presentation_tr(
                "Long and short exposure both decreased materially; the net move was comparatively small.");
        if (accumulation)
            return cftc_presentation_tr("Long exposure increased materially.");
        if (liquidation)
            return cftc_presentation_tr("Long exposure decreased materially.");
        if (building)
            return cftc_presentation_tr("Short positions increased materially.");
        return cftc_presentation_tr("Short positions were reduced materially.");
    }();
    if (sentence.isEmpty() || !label_horizon)
        return sentence;
    QString body = sentence;
    body[0] = body[0].toLower();
    return cftc_presentation_tr("Over the last %1, %2").arg(cftc_horizon_phrase(leading.horizon), body);
}

/// Open Interest context sentence (precedence 5). Empty when no material OI
/// state or raw-net/Net-%OI disagreement exists.
inline QString cftc_open_interest_sentence(const services::CftcInterpretationResult& result) {
    for (int horizon : {4, 13, 1}) {
        for (const auto& state : result.market_context) {
            if (!state.has_horizon || state.horizon_reports != horizon)
                continue;
            if (state.state_id == QLatin1String("OI_EXPANSION"))
                return cftc_presentation_tr("Open Interest expanded over the last %1.")
                    .arg(cftc_horizon_phrase(horizon));
            if (state.state_id == QLatin1String("OI_CONTRACTION"))
                return cftc_presentation_tr("Open Interest contracted over the last %1.")
                    .arg(cftc_horizon_phrase(horizon));
        }
    }
    return {};
}

/// The raw-net / Net-%OI disagreement sentence for one horizon, with both
/// measured values and the Open Interest change that explains it. The engine
/// only emits the state when at least one of the two opposed moves is
/// material, so the sentence never narrates a noise-sized disagreement.
inline QString
cftc_net_share_disagreement_sentence_for_horizon(const services::CftcParticipantInterpretation& participant,
                                                 int horizon) {
    const services::CftcInterpretationState* state =
        cftc_presentation_state(participant.states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), horizon);
    if (!state)
        return {};
    const auto signed_text = [](double value) {
        const QString text = QString::number(value, 'f', 2);
        return value > 0.0 ? QStringLiteral("+") + text : text;
    };
    const auto* net_flow = cftc_presentation_metric(state->metrics, QStringLiteral("net_flow"));
    const auto* share_change = cftc_presentation_metric(state->metrics, QStringLiteral("net_share_change"));
    const auto* oi_change = cftc_presentation_metric(state->metrics, QStringLiteral("oi_change"));
    if (net_flow && net_flow->has_value && share_change && share_change->has_value) {
        const QString base =
            cftc_presentation_tr("Over %1 the raw net flow was %2% of prior Open Interest, but Net %OI "
                                 "changed by %3 percentage points")
                .arg(cftc_horizon_phrase(horizon), signed_text(net_flow->value), signed_text(share_change->value));
        if (oi_change && oi_change->has_value)
            return cftc_presentation_tr("%1, because Open Interest changed by %2%.")
                .arg(base, signed_text(oi_change->value));
        return cftc_presentation_tr("%1, because market size changed.").arg(base);
    }
    return cftc_presentation_tr("Over %1 the change in Net %OI moved opposite to the raw net flow because market size "
                                "changed.")
        .arg(cftc_horizon_phrase(horizon));
}

inline QString cftc_net_share_disagreement_sentence(const services::CftcParticipantInterpretation& participant) {
    for (int horizon : {4, 13, 1}) {
        const QString sentence = cftc_net_share_disagreement_sentence_for_horizon(participant, horizon);
        if (!sentence.isEmpty())
            return sentence;
    }
    return {};
}

/// Gross mechanism fragment for a price/positioning divergence sentence.
inline QString cftc_price_mechanism_fragment(const QStringList& mechanism_state_ids) {
    const bool accumulation = mechanism_state_ids.contains(QStringLiteral("LONG_ACCUMULATION"));
    const bool liquidation = mechanism_state_ids.contains(QStringLiteral("LONG_LIQUIDATION"));
    const bool building = mechanism_state_ids.contains(QStringLiteral("SHORT_BUILDING"));
    const bool covering = mechanism_state_ids.contains(QStringLiteral("SHORT_COVERING"));
    if (liquidation && building)
        return cftc_presentation_tr("long liquidation and short building");
    if (accumulation && covering)
        return cftc_presentation_tr("long accumulation and short covering");
    if (liquidation)
        return cftc_presentation_tr("long liquidation");
    if (building)
        return cftc_presentation_tr("short building");
    if (accumulation)
        return cftc_presentation_tr("long accumulation");
    if (covering)
        return cftc_presentation_tr("short covering");
    return {};
}

/// The one price assessment the presentation uses: the highest-emitted state
/// first (4R, then 13R, then 1R), then the first horizon that carries a price
/// move, then the first assessment in that same 4R→13R→1R priority. Sentences
/// and evidence share this selector so they can never describe different
/// horizons.
inline const services::CftcPricePositionAssessment*
cftc_presentation_price_primary(const services::CftcInterpretationResult& result, const QString& participant_key) {
    const services::CftcPricePositionAssessment* a1 = cftc_presentation_price(result, participant_key, 1);
    const services::CftcPricePositionAssessment* a4 = cftc_presentation_price(result, participant_key, 4);
    const services::CftcPricePositionAssessment* a13 = cftc_presentation_price(result, participant_key, 13);
    for (const auto* assessment : {a4, a13, a1}) {
        if (assessment && assessment->has_state)
            return assessment;
    }
    for (const auto* assessment : {a4, a13, a1}) {
        if (assessment && assessment->has_price_move)
            return assessment;
    }
    return a4 ? a4 : (a13 ? a13 : a1);
}

/// How the two gross legs composed a net move at one horizon, from the
/// engine's emitted flow reading (percent of prior Open Interest). A leg's
/// contribution is its flow signed toward the net direction; "mostly" is only
/// used for the leg that supplied more than half of the move, with the actual
/// shares stated, so a smaller leg is never called the primary driver merely
/// because it alone crossed its materiality threshold. Empty when the reading
/// does not carry both legs and the net move.
inline QString cftc_net_move_composition_sentence(const services::CftcParticipantInterpretation& participant,
                                                  int horizon, int direction) {
    const services::CftcHorizonFlowReading* reading = cftc_presentation_flow_reading(participant, horizon);
    if (!reading || !reading->evaluated || !reading->has_long_flow || !reading->has_short_flow ||
        !reading->has_net_flow || direction == 0)
        return {};
    const double d = direction > 0 ? 1.0 : -1.0;
    const double long_contribution = d * reading->long_flow;
    const double short_contribution = -d * reading->short_flow;
    const QString long_name = reading->long_flow > 0.0   ? cftc_presentation_tr("long accumulation")
                              : reading->long_flow < 0.0 ? cftc_presentation_tr("long liquidation")
                                                         : QString();
    const QString short_name = reading->short_flow > 0.0   ? cftc_presentation_tr("short building")
                               : reading->short_flow < 0.0 ? cftc_presentation_tr("short covering")
                                                           : QString();
    const QString word = cftc_direction_word(direction);
    if (long_contribution > 0.0 && short_contribution > 0.0) {
        const double total = long_contribution + short_contribution;
        const int long_share = static_cast<int>(std::lround(100.0 * long_contribution / total));
        const int short_share = 100 - long_share;
        if (long_share == short_share)
            return cftc_presentation_tr("The %1 shift came equally from %2 and %3.").arg(word, long_name, short_name);
        const bool long_leads = long_share > short_share;
        return cftc_presentation_tr("The %1 shift came mostly from %2 (%3% of the net move), with %4 contributing "
                                    "%5%.")
            .arg(word, long_leads ? long_name : short_name)
            .arg(long_leads ? long_share : short_share)
            .arg(long_leads ? short_name : long_name)
            .arg(long_leads ? short_share : long_share);
    }
    if (long_contribution > 0.0) {
        if (reading->short_flow == 0.0)
            return cftc_presentation_tr("The %1 shift came entirely from %2.").arg(word, long_name);
        return cftc_presentation_tr("The %1 shift came from %2; %3 partly offset it.").arg(word, long_name, short_name);
    }
    if (short_contribution > 0.0) {
        if (reading->long_flow == 0.0)
            return cftc_presentation_tr("The %1 shift came entirely from %2.").arg(word, short_name);
        return cftc_presentation_tr("The %1 shift came from %2; %3 partly offset it.").arg(word, short_name, long_name);
    }
    return {};
}

/// The price series behind a price-relationship conclusion, stated in the
/// conclusion itself: the source and proxy kind, the two actual closing
/// sessions and the move in quoted units and percent. A continuous front-month
/// series is not roll-adjusted, so a contract roll inside the window is part of
/// the move; a spot index is not the futures contract whose positions are
/// reported. Empty when the assessment carries no price move.
inline QString cftc_price_window_sentence(const services::CftcInterpretationResult& result,
                                          const services::CftcPricePositionAssessment& assessment) {
    if (!assessment.has_price_move || !assessment.price_anchor_date.isValid() ||
        !assessment.price_latest_date.isValid())
        return {};
    const QString source = result.price_source.trimmed().isEmpty() ? cftc_presentation_tr("the supplied price series")
                                                                   : result.price_source.trimmed();
    const auto signed_text = [](double value) {
        const QString text = QString::number(value, 'f', 2);
        return value > 0.0 ? QStringLiteral("+") + text : text;
    };
    const QString move = cftc_presentation_tr("%1 (%2%)")
                             .arg(signed_text(assessment.price_move), signed_text(assessment.price_move_pct));
    QString qualifier;
    if (result.price_spot_index)
        qualifier = cftc_presentation_tr("a spot index, not the futures contract whose positions are reported");
    else if (result.price_continuous_proxy)
        qualifier = cftc_presentation_tr("not roll-adjusted, so a contract roll inside the window is part of the move");
    if (qualifier.isEmpty())
        return cftc_presentation_tr("Price series: %1; closes %2 → %3: %4.")
            .arg(source, assessment.price_anchor_date.toString(Qt::ISODate),
                 assessment.price_latest_date.toString(Qt::ISODate), move);
    return cftc_presentation_tr("Price series: %1 (%2); closes %3 → %4: %5.")
        .arg(source, qualifier, assessment.price_anchor_date.toString(Qt::ISODate),
             assessment.price_latest_date.toString(Qt::ISODate), move);
}

/// The sentences that carry a divergence: the direction sentence naming the
/// participant, how the gross legs composed the positioning move, and nothing
/// predictive. Shared by the combined and the single-horizon composers so a
/// divergence can never lose its semantics in one path.
inline QStringList cftc_divergence_sentences(const services::CftcPricePositionAssessment& assessment,
                                             const services::CftcParticipantInterpretation& participant) {
    QStringList out;
    const QString horizon = cftc_horizon_phrase(assessment.horizon_reports);
    const bool shortward = assessment.state_id == QLatin1String("PRICE_UP_POSITIONING_DOWN_DIVERGENCE");
    if (shortward)
        out << cftc_presentation_tr("Price rose materially over %1 while %2 shifted materially shortward.")
                   .arg(horizon, cftc_participant_display_name(participant));
    else
        out << cftc_presentation_tr("Price fell materially over %1 while %2 shifted materially longward.")
                   .arg(horizon, cftc_participant_display_name(participant));
    const QString composition =
        cftc_net_move_composition_sentence(participant, assessment.horizon_reports, shortward ? -1 : 1);
    if (!composition.isEmpty()) {
        out << composition;
    } else {
        // Without the flow reading only the individually material legs are
        // known, so they are named without claiming which one dominated.
        const QString mechanism = cftc_price_mechanism_fragment(assessment.mechanism_state_ids);
        if (!mechanism.isEmpty())
            out << cftc_presentation_tr("The positioning change included %1.").arg(mechanism);
    }
    return out;
}

/// Price relationship sentences (precedence 6). The four states render
/// distinctly; opposing-direction states keep the word divergence and never
/// carry a reversal, catch-up or trade-advice claim. A four/thirteen conflict
/// is exposed instead of choosing one, and a divergent horizon keeps its full
/// semantics inside the conflict wording: the gross-leg mechanism is named and
/// the contemporaneous-only statement is made. The shared
/// `CftcPriceContextState` plus the caller's concrete `price_unavailable_note`
/// keep the sentence, the evidence rows and the synchronized chart describing
/// the same actual price situation.
inline QStringList cftc_price_relationship_sentences(const services::CftcInterpretationResult& result,
                                                     const services::CftcParticipantInterpretation& participant,
                                                     CftcPriceContextState price_state,
                                                     const QString& price_unavailable_note = QString()) {
    QStringList out;
    const auto* a4 = cftc_presentation_price(result, participant.participant_key, 4);
    const auto* a13 = cftc_presentation_price(result, participant.participant_key, 13);
    const services::CftcPricePositionAssessment* primary =
        cftc_presentation_price_primary(result, participant.participant_key);
    if (!primary)
        return out;

    if (price_state == CftcPriceContextState::Pending) {
        out << cftc_presentation_tr(
            "Price context is still loading; the positioning conclusions above do not depend on it.");
        return out;
    }
    if (!primary->evaluated) {
        const bool missing_context = primary->reason == services::CftcUnavailableReason::MissingPriceContext;
        const QString reason = missing_context && !price_unavailable_note.trimmed().isEmpty()
                                   ? price_unavailable_note
                                   : cftc_unavailable_reason_wording(primary->reason);
        out << cftc_presentation_tr("Price relationship unavailable: %1.").arg(reason);
        return out;
    }
    const QString primary_window = cftc_price_window_sentence(result, *primary);
    if (!primary->has_state) {
        if (!primary_window.isEmpty())
            out << primary_window;
        out << cftc_presentation_tr("Price and net positioning both changed over the last %1, but not both materially "
                                    "enough to classify the relationship.")
                   .arg(cftc_horizon_phrase(primary->horizon_reports));
        return out;
    }

    const auto is_divergence = [](const QString& state_id) {
        return state_id == QLatin1String("PRICE_UP_POSITIONING_DOWN_DIVERGENCE") ||
               state_id == QLatin1String("PRICE_DOWN_POSITIONING_UP_DIVERGENCE");
    };
    auto append_divergence = [&out, &participant](const services::CftcPricePositionAssessment& assessment) {
        out << cftc_divergence_sentences(assessment, participant);
    };

    // A genuine four/thirteen conflict is exposed instead of choosing one. A
    // divergent horizon keeps its full semantics: the gross-leg mechanism is
    // named and the contemporaneous-only statement is made, so the conflict
    // wording can never silently downgrade a divergence.
    if (a4 && a13 && a4->has_state && a13->has_state && a4->state_id != a13->state_id) {
        for (const auto* assessment : {a4, a13}) {
            const QString window = cftc_price_window_sentence(result, *assessment);
            if (!window.isEmpty())
                out << window;
        }
        out << cftc_presentation_tr("The price/positioning relationship differs across horizons: %1 over four reports "
                                    "and %2 over thirteen reports.")
                   .arg(cftc_relationship_wording(a4->state_id), cftc_relationship_wording(a13->state_id));
        bool any_divergence = false;
        for (const auto* assessment : {a4, a13}) {
            if (is_divergence(assessment->state_id)) {
                append_divergence(*assessment);
                any_divergence = true;
            }
        }
        if (any_divergence)
            out << cftc_presentation_tr("This is a contemporaneous divergence only.");
        return out;
    }

    if (!primary_window.isEmpty())
        out << primary_window;
    const QString horizon = cftc_horizon_phrase(primary->horizon_reports);
    if (primary->state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_UP")) {
        out << cftc_presentation_tr("Price and net positioning moved together upward over the last %1.").arg(horizon);
    } else if (primary->state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_DOWN")) {
        out << cftc_presentation_tr("Price and net positioning moved together downward over the last %1.").arg(horizon);
    } else {
        append_divergence(*primary);
        out << cftc_presentation_tr("This is a contemporaneous divergence only.");
    }
    return out;
}

// ── Single-horizon composition (Batch 4B correction pass) ───────────────────
//
// The user can select one interpretation horizon (1W | 4W | 13W). The level,
// persistence and historical-percentile conclusions are strictly full-history
// and identical at every selection; only the repositioning, gross-leg
// mechanism, Open Interest context, disagreement and price relationship come
// from the selected horizon's already-emitted Batch 4A states and metrics.

/// One horizon-scoped reading with its evidence status. `sentence` always
/// carries either the conclusion, the evaluated-not-material statement or the
/// concrete unavailable reason; `has_value` is true only when the engine
/// emitted a metric this horizon (never zero-filled by the presentation).
struct CftcHorizonDimension {
    CftcEvidenceStatus status = CftcEvidenceStatus::Unavailable;
    QString sentence;
    bool has_value = false;
    double value = 0.0;
    QString unavailable_reason;
};

inline bool cftc_has_extreme_transition_state(const services::CftcParticipantInterpretation& participant) {
    return cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_HIGH_EXTREME")) ||
           cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_LOW_EXTREME")) ||
           cftc_presentation_state(participant.states, QStringLiteral("EXITED_HIGH_EXTREME")) ||
           cftc_presentation_state(participant.states, QStringLiteral("EXITED_LOW_EXTREME")) ||
           cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_HIGH_EXTREME")) ||
           cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_LOW_EXTREME"));
}

/// Repositioning conclusion for one selected horizon. An evaluated horizon
/// whose move stayed below the materiality threshold says exactly that; it is
/// never reported as unavailable, and unavailable data is never reported as
/// "no material state".
inline QString cftc_repositioning_sentence_for_horizon(const CftcHorizonFlowSummary& summary, int horizon) {
    const QString phrase = cftc_horizon_phrase(horizon);
    if (!summary.evaluated)
        return cftc_presentation_tr("Net repositioning over the last %1 is unavailable: %2.")
            .arg(phrase, summary.unavailable_reason);
    if (summary.material_net_move) {
        QString text = cftc_presentation_tr("Net positioning shifted %1 over the last %2.")
                           .arg(cftc_direction_word(summary.net_direction), phrase);
        const QString sustained = cftc_sustained_sentence(summary);
        if (!sustained.isEmpty())
            text += QLatin1Char(' ') + sustained;
        return text;
    }
    return cftc_presentation_tr(
               "Net repositioning over the last %1 was evaluated but did not cross the materiality threshold.")
        .arg(phrase);
}

/// Headline phrase for an extreme transition, or empty when none was emitted.
/// The wording describes the transition the latest report shows; it never
/// implies that a move is "developing" or will continue.
inline QString cftc_extreme_transition_headline(const services::CftcParticipantInterpretation& participant) {
    if (cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_HIGH_EXTREME")) ||
        cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_LOW_EXTREME")))
        return cftc_presentation_tr("moved well back from a recent extreme");
    if (cftc_presentation_state(participant.states, QStringLiteral("EXITED_HIGH_EXTREME")) ||
        cftc_presentation_state(participant.states, QStringLiteral("EXITED_LOW_EXTREME")))
        return cftc_presentation_tr("left the extreme band in the latest report");
    if (cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_HIGH_EXTREME")) ||
        cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_LOW_EXTREME")))
        return cftc_presentation_tr("extreme persisting");
    return {};
}

/// Headline phrase for material gross legs when the net move itself was not
/// material. Both legs are always considered: two material legs moving the
/// same way are a gross expansion or reduction, never "long accumulation"
/// alone while the shorts grew even more; a single material leg says that the
/// net change was not material.
inline QString cftc_gross_only_headline(const CftcHorizonFlowSummary& summary) {
    if (summary.long_accumulation && summary.short_building)
        return cftc_presentation_tr("long and short exposure both increased");
    if (summary.long_liquidation && summary.short_covering)
        return cftc_presentation_tr("long and short exposure both decreased");
    if (summary.long_accumulation && summary.short_covering)
        return cftc_presentation_tr("longs up and shorts down without a material net change");
    if (summary.long_liquidation && summary.short_building)
        return cftc_presentation_tr("longs down and shorts up without a material net change");
    if (summary.long_accumulation)
        return cftc_presentation_tr("long exposure increased without a material net change");
    if (summary.long_liquidation)
        return cftc_presentation_tr("long exposure decreased without a material net change");
    if (summary.short_building)
        return cftc_presentation_tr("short exposure increased without a material net change");
    if (summary.short_covering)
        return cftc_presentation_tr("short exposure decreased without a material net change");
    return {};
}

/// Headline trajectory for one selected horizon. Extreme persistence, exit and
/// unwind keep precedence exactly as in the combined view; otherwise the
/// trajectory describes the selected horizon (net move first, then the
/// material gross legs when the net move did not qualify).
inline QString cftc_headline_trajectory_for_horizon(const services::CftcParticipantInterpretation& participant,
                                                    const CftcHorizonFlowSummary& summary) {
    const QString transition = cftc_extreme_transition_headline(participant);
    if (!transition.isEmpty())
        return transition;

    if (!summary.evaluated)
        return cftc_presentation_tr("recent repositioning unavailable");
    if (summary.material_net_move) {
        if (summary.sustained) {
            return summary.net_direction > 0 ? cftc_presentation_tr("sustained longward repositioning")
                                             : cftc_presentation_tr("sustained shortward repositioning");
        }
        if (summary.net_direction > 0 && summary.long_accumulation)
            return cftc_presentation_tr("long accumulation");
        if (summary.net_direction < 0 && summary.long_liquidation)
            return cftc_presentation_tr("long liquidation");
        return summary.net_direction > 0 ? cftc_presentation_tr("longward repositioning")
                                         : cftc_presentation_tr("shortward repositioning");
    }
    const QString gross = cftc_gross_only_headline(summary);
    if (!gross.isEmpty())
        return gross;
    return cftc_presentation_tr("no material repositioning");
}

/// Open Interest conclusion/evaluation for one selected horizon. A material OI
/// state carries its emitted normalized change; an evaluated-but-not-material
/// horizon says so; a missing or non-positive Open Interest keeps its concrete
/// reason from the engine's market-level record.
inline CftcHorizonDimension cftc_open_interest_dimension(const services::CftcInterpretationResult& result,
                                                         int horizon) {
    CftcHorizonDimension out;
    const auto* state = cftc_presentation_state(result.market_context, QStringLiteral("OI_EXPANSION"), horizon);
    if (!state)
        state = cftc_presentation_state(result.market_context, QStringLiteral("OI_CONTRACTION"), horizon);
    if (state) {
        out.status = CftcEvidenceStatus::Available;
        out.sentence =
            state->state_id == QLatin1String("OI_EXPANSION")
                ? cftc_presentation_tr("Open Interest expanded over the last %1.").arg(cftc_horizon_phrase(horizon))
                : cftc_presentation_tr("Open Interest contracted over the last %1.").arg(cftc_horizon_phrase(horizon));
        if (const auto* metric = cftc_presentation_metric(state->metrics, QStringLiteral("oi_change"));
            metric && metric->has_value) {
            out.has_value = true;
            out.value = metric->value;
        }
        return out;
    }
    if (const auto* record =
            cftc_presentation_unavailable(result.unavailable, QStringLiteral("OI_CONTEXT"), QString(), horizon)) {
        out.status = CftcEvidenceStatus::Unavailable;
        out.unavailable_reason = cftc_unavailable_reason_wording(record->reason);
        out.sentence = cftc_presentation_tr("Open Interest context over the last %1 is unavailable: %2.")
                           .arg(cftc_horizon_phrase(horizon), out.unavailable_reason);
        return out;
    }
    out.status = CftcEvidenceStatus::NoMaterialState;
    out.sentence =
        cftc_presentation_tr(
            "Open Interest change over the last %1 was evaluated but did not cross the materiality threshold.")
            .arg(cftc_horizon_phrase(horizon));
    return out;
}

/// Price relationship for one selected horizon. Uses only that horizon's Batch
/// 4A assessment; the pending / failed / evaluated-not-material distinction and
/// the divergence semantics match the combined composer exactly.
inline QStringList cftc_price_relationship_sentences_for_horizon(
    const services::CftcInterpretationResult& result, const services::CftcParticipantInterpretation& participant,
    int horizon, CftcPriceContextState price_state, const QString& price_unavailable_note = QString()) {
    QStringList out;
    const QString phrase = cftc_horizon_phrase(horizon);
    if (price_state == CftcPriceContextState::Pending) {
        out << cftc_presentation_tr(
            "Price context is still loading; the positioning conclusions above do not depend on it.");
        return out;
    }
    const auto* assessment = cftc_presentation_price(result, participant.participant_key, horizon);
    if (!assessment) {
        out << cftc_presentation_tr(
                   "Price relationship over the last %1 is unavailable: no assessment was emitted for this horizon.")
                   .arg(phrase);
        return out;
    }
    if (!assessment->evaluated) {
        const bool missing_context = assessment->reason == services::CftcUnavailableReason::MissingPriceContext;
        const QString reason = missing_context && !price_unavailable_note.trimmed().isEmpty()
                                   ? price_unavailable_note
                                   : cftc_unavailable_reason_wording(assessment->reason);
        out << cftc_presentation_tr("Price relationship over the last %1 is unavailable: %2.").arg(phrase, reason);
        return out;
    }
    const QString window = cftc_price_window_sentence(result, *assessment);
    if (!window.isEmpty())
        out << window;
    if (!assessment->has_state) {
        out << cftc_presentation_tr("Price and net positioning both changed over the last %1, but not both materially "
                                    "enough to classify the relationship.")
                   .arg(phrase);
        return out;
    }
    if (assessment->state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_UP")) {
        out << cftc_presentation_tr("Price and net positioning moved together upward over the last %1.").arg(phrase);
    } else if (assessment->state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_DOWN")) {
        out << cftc_presentation_tr("Price and net positioning moved together downward over the last %1.").arg(phrase);
    } else {
        out << cftc_divergence_sentences(*assessment, participant);
        out << cftc_presentation_tr("This is a contemporaneous divergence only.");
    }
    return out;
}

// ── Evidence ────────────────────────────────────────────────────────────────

/// Batch 4A `move_value`, `net_flow` and `oi_change` are normalized metrics:
/// leg and net flows are percent of the anchor report's Open Interest and the
/// Open Interest change is a percent change. The rows below state those exact
/// units; they are never relabeled as contract counts, and the price change
/// stays in the market's quoted price units.
inline bool cftc_evidence_has_flow_value(const services::CftcParticipantInterpretation& participant, int horizon,
                                         const QString& positive_state_id, const QString& negative_state_id,
                                         const QString& metric_key, double& value) {
    const auto* state = cftc_presentation_state(participant.states, positive_state_id, horizon);
    if (!state)
        state = cftc_presentation_state(participant.states, negative_state_id, horizon);
    const auto* metric = state ? cftc_presentation_metric(state->metrics, metric_key) : nullptr;
    if (!metric || !metric->has_value)
        return false;
    value = metric->value;
    return true;
}

inline QString cftc_percentile_value_text(const services::CftcParticipantInterpretation& participant) {
    return QString::number(participant.percentile * 100.0, 'f', 1) + QLatin1Char('%') +
           cftc_presentation_tr(" (n=%1)").arg(participant.percentile_reference_count);
}

inline QString cftc_percentile_unavailable_reason(const services::CftcInterpretationResult& result,
                                                  const services::CftcParticipantInterpretation& participant) {
    if (const auto* record = cftc_presentation_unavailable(
            result.unavailable, QStringLiteral("HISTORICAL_RELATIVE_STATE"), participant.participant_key)) {
        return cftc_unavailable_reason_wording(record->reason);
    }
    return cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
}

/// Signed fixed-decimal text for a quoted-price move (or any non-contract
/// measured quantity). `cftc_signed_net` is the contract-count formatter and
/// must not be used for quoted price units.
inline QString cftc_signed_decimal(double value, int decimals) {
    const QString text = QString::number(value, 'f', decimals);
    return value > 0.0 ? QStringLiteral("+") + text : text;
}

/// The actual cause of a missing Net %OI reading: a missing leg, missing Open
/// Interest, or zero/negative Open Interest are different states and must not
/// share one reason.
inline QString cftc_net_pct_unavailable_reason(const services::CftcInterpretationResult& result,
                                               const services::CftcParticipantInterpretation& participant) {
    using services::CftcUnavailableReason;
    if (!participant.net_available)
        return cftc_unavailable_reason_wording(CftcUnavailableReason::MissingParticipantLeg);
    if (!result.open_interest_available)
        return cftc_unavailable_reason_wording(CftcUnavailableReason::MissingOpenInterest);
    if (result.open_interest <= 0.0)
        return cftc_unavailable_reason_wording(CftcUnavailableReason::NonPositiveOpenInterest);
    return cftc_unavailable_reason_wording(CftcUnavailableReason::MissingParticipantLeg);
}

/// Evidence text for a price move: quoted units, percent change and the two
/// closing sessions actually used, e.g. "-87.80 (-1.99%; 2026-08-18 → 2026-09-15)".
inline QString cftc_price_move_evidence_text(const services::CftcPricePositionAssessment& assessment) {
    QString text = cftc_signed_decimal(assessment.price_move, 2);
    if (assessment.price_anchor_date.isValid() && assessment.price_latest_date.isValid()) {
        text += cftc_presentation_tr(" (%1%; %2 → %3)")
                    .arg(cftc_signed_decimal(assessment.price_move_pct, 2),
                         assessment.price_anchor_date.toString(Qt::ISODate),
                         assessment.price_latest_date.toString(Qt::ISODate));
    }
    return text;
}

/// The price-move materiality rank row: the rank of the absolute log return
/// against the previous comparable moves, marked below threshold when the
/// price move was not material, or the concrete reason it is unavailable.
inline CftcEvidenceItem cftc_price_rank_evidence(const services::CftcPricePositionAssessment* assessment,
                                                 CftcPriceContextState price_state,
                                                 const QString& price_unavailable_note, int horizon) {
    CftcEvidenceItem item;
    item.label = cftc_presentation_tr("Price move materiality rank (|log return| vs prior moves)");
    item.horizon_reports = horizon;
    if (price_state == CftcPriceContextState::Pending) {
        item.status = CftcEvidenceStatus::Pending;
        item.value = cftc_presentation_tr("pending — price context is loading");
        return item;
    }
    if (assessment && assessment->has_price_move_rank) {
        item.status = assessment->price_material ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState;
        item.value = QString::number(assessment->price_move_rank * 100.0, 'f', 1) + QLatin1Char('%') +
                     cftc_presentation_tr(" (n=%1)").arg(assessment->price_reference_count) +
                     (assessment->price_material ? QString() : cftc_below_threshold_suffix());
        return item;
    }
    const bool missing_context =
        !assessment || assessment->reason == services::CftcUnavailableReason::MissingPriceContext;
    const QString reason =
        missing_context && !price_unavailable_note.trimmed().isEmpty()
            ? price_unavailable_note
            : cftc_unavailable_reason_wording(assessment ? assessment->reason
                                                         : services::CftcUnavailableReason::MissingPriceContext);
    item.status = CftcEvidenceStatus::Unavailable;
    item.value = cftc_presentation_tr("unavailable — %1").arg(reason);
    return item;
}

/// Market-level concentration evidence and conclusion. The standard CFTC
/// concentration ratio is a market/report property (share of Open Interest held
/// by the four largest long traders) and is never attributed to a participant
/// category.
inline QVector<CftcEvidenceItem> cftc_concentration_evidence(const services::CftcInterpretationResult& result) {
    QVector<CftcEvidenceItem> out;
    const QString label = cftc_presentation_tr("Market concentration (largest 4 long traders, % of OI)");
    const services::CftcConcentrationAssessment* primary = nullptr;
    for (const auto& assessment : result.concentration) {
        if (assessment.field_key == result.config.primary_concentration_field) {
            primary = &assessment;
            break;
        }
    }
    if (!primary || !primary->has_current) {
        out.append(
            {label,
             cftc_presentation_tr("unavailable — %1")
                 .arg(cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingConcentrationField)),
             CftcEvidenceStatus::Unavailable, 0});
        return out;
    }
    const bool high = cftc_presentation_state(result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION"));
    QString value = QString::number(primary->current, 'f', 1) + QLatin1Char('%');
    if (primary->has_percentile)
        value += cftc_presentation_tr(" (percentile %1% of its own history, n=%2)")
                     .arg(QString::number(primary->percentile * 100.0, 'f', 1))
                     .arg(primary->percentile_reference_count);
    out.append({label, value, high ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState, 0});

    const QString change_label = cftc_presentation_tr("Market concentration change (latest weekly report, points)");
    if (primary->has_change) {
        const bool rising = cftc_presentation_state(result.market_context, QStringLiteral("CONCENTRATION_RISING"));
        QString change = cftc_signed_decimal(primary->change, 2);
        if (primary->has_change_rank)
            change += cftc_presentation_tr(" (rank %1%, n=%2)")
                          .arg(QString::number(primary->change_rank * 100.0, 'f', 1))
                          .arg(primary->change_reference_count);
        out.append({change_label, change + (rising ? QString() : cftc_below_threshold_suffix()),
                    rising ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState, 0});
    } else {
        out.append({change_label,
                    cftc_presentation_tr("unavailable — %1")
                        .arg(cftc_unavailable_reason_wording(primary->change_unavailable_reason)),
                    CftcEvidenceStatus::Unavailable, 0});
    }
    return out;
}

/// Market concentration conclusions, only for emitted states (a secondary
/// market-structure context; its measurements and unavailability stay visible
/// in the evidence rows).
inline QStringList cftc_concentration_sentences(const services::CftcInterpretationResult& result) {
    QStringList out;
    const services::CftcInterpretationState* high =
        cftc_presentation_state(result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION"));
    const services::CftcInterpretationState* rising =
        cftc_presentation_state(result.market_context, QStringLiteral("CONCENTRATION_RISING"));
    const services::CftcConcentrationAssessment* primary = nullptr;
    for (const auto& assessment : result.concentration) {
        if (assessment.field_key == result.config.primary_concentration_field) {
            primary = &assessment;
            break;
        }
    }
    if (high && primary && primary->has_current)
        out << cftc_presentation_tr("Market concentration is high: the four largest long traders hold %1% of Open "
                                    "Interest, in the top %2 of that ratio's own history (market-wide, not a "
                                    "participant category; MarketLab threshold).")
                   .arg(QString::number(primary->current, 'f', 1),
                        cftc_tail_share_text(1.0 - result.config.extreme_percentile));
    if (rising && primary && primary->has_change)
        out << cftc_presentation_tr("Market concentration rose by %1 percentage points in the latest weekly report, a "
                                    "material weekly increase (market-wide; MarketLab threshold).")
                   .arg(cftc_signed_decimal(primary->change, 2));
    return out;
}
/// Evidence status for one gross leg at one horizon. The engine emits a
/// GROSS_FLOW record per leg that could not produce a state: a record with the
/// leg's own direction id means the rank reference was insufficient, and a
/// record with an empty state id means that leg itself is missing. A leg with
/// an emitted state/metric is always available. This never labels an evaluated
/// leg with the other leg's missing-data reason; when no leg record exists but
/// the horizon itself is unavailable, the horizon-level reason is used.
inline CftcEvidenceStatus cftc_leg_evidence_status(const services::CftcParticipantInterpretation& participant,
                                                   const QVector<services::CftcUnavailableRecord>& unavailable,
                                                   int horizon, bool long_leg, bool has_metric, bool horizon_evaluated,
                                                   const QString& horizon_reason, QString& unavailable_reason) {
    unavailable_reason.clear();
    if (has_metric)
        return CftcEvidenceStatus::Available;
    const QString prefix = long_leg ? QStringLiteral("LONG_") : QStringLiteral("SHORT_");
    const auto matches = [&](const services::CftcUnavailableRecord& record, bool require_prefix) {
        if (record.state_family != QLatin1String("GROSS_FLOW") || record.participant_key != participant.participant_key)
            return false;
        if (record.has_horizon && record.horizon_reports != horizon)
            return false;
        return require_prefix ? record.state_id.startsWith(prefix) : record.state_id.isEmpty();
    };
    for (const auto& record : unavailable) {
        if (matches(record, true)) {
            unavailable_reason = cftc_unavailable_reason_wording(record.reason);
            return CftcEvidenceStatus::Unavailable;
        }
    }
    for (const auto& record : unavailable) {
        if (matches(record, false)) {
            unavailable_reason = cftc_unavailable_reason_wording(record.reason);
            return CftcEvidenceStatus::Unavailable;
        }
    }
    if (!horizon_evaluated) {
        unavailable_reason = horizon_reason;
        return CftcEvidenceStatus::Unavailable;
    }
    return CftcEvidenceStatus::NoMaterialState;
}

inline QVector<CftcEvidenceItem> cftc_build_evidence(const services::CftcInterpretationResult& result,
                                                     const services::CftcParticipantInterpretation& participant,
                                                     const CftcHorizonFlowSummary& h4,
                                                     const CftcHorizonFlowSummary& h13,
                                                     CftcPriceContextState price_state,
                                                     const QString& price_unavailable_note) {
    QVector<CftcEvidenceItem> out;
    auto add = [&out](const QString& label, CftcEvidenceStatus status, const QString& value, int horizon = 0) {
        out.append({label, value, status, horizon});
    };
    auto unavailable_text = [](const QString& reason) { return cftc_presentation_tr("unavailable — %1").arg(reason); };
    auto signed_percent = [](double value, int decimals) {
        const QString text = QString::number(value, 'f', decimals);
        return value > 0.0 ? QStringLiteral("+") + text : text;
    };
    const QString no_material = cftc_presentation_tr("no material state at this horizon");

    // A raw reading is only labeled "below threshold" when its materiality
    // comparison actually happened. When the strictly trailing reference was
    // too short, the row keeps the measurement and states that the comparison
    // was not performed, using the leg's or the net shift's own engine reason.
    const auto leg_materiality_reason = [&](int horizon, bool long_leg, const CftcHorizonFlowSummary& summary) {
        QString reason;
        cftc_leg_evidence_status(participant, result.unavailable, horizon, long_leg, false, summary.evaluated,
                                 summary.unavailable_reason, reason);
        if (reason.isEmpty())
            reason = cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
        return reason;
    };
    const auto net_materiality_reason = [&](int horizon, const CftcHorizonFlowSummary& summary) {
        if (const auto* record = cftc_presentation_unavailable(result.unavailable, QStringLiteral("NET_SHIFT"),
                                                               participant.participant_key, horizon))
            return cftc_unavailable_reason_wording(record->reason);
        if (!summary.unavailable_reason.isEmpty())
            return summary.unavailable_reason;
        return cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
    };

    const bool has_report_date = result.report_date_available && result.report_date.isValid();
    add(cftc_presentation_tr("Report date"),
        has_report_date ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        has_report_date
            ? result.report_date.toString(Qt::ISODate)
            : unavailable_text(cftc_unavailable_reason_wording(services::CftcUnavailableReason::StaleCurrentReport)));
    add(cftc_presentation_tr("Participant"), CftcEvidenceStatus::Available, cftc_participant_display_name(participant));

    const bool has_net_pct = participant.has_net_pct_oi;
    add(cftc_presentation_tr("Net %OI (% of current OI)"),
        has_net_pct ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        has_net_pct ? QString::number(participant.net_pct_oi, 'f', 2) + QLatin1Char('%')
                    : unavailable_text(cftc_net_pct_unavailable_reason(result, participant)));

    const bool has_percentile = participant.historical_percentile_available;
    add(cftc_interpretation_percentile_label(),
        has_percentile ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        has_percentile ? cftc_percentile_value_text(participant)
                       : unavailable_text(cftc_percentile_unavailable_reason(result, participant)));

    for (int horizon : {4, 13}) {
        const CftcHorizonFlowSummary& summary = horizon == 4 ? h4 : h13;
        const CftcEvidenceStatus summary_status =
            summary.evaluated ? CftcEvidenceStatus::NoMaterialState : CftcEvidenceStatus::Unavailable;
        const QString summary_text = summary.evaluated ? no_material : unavailable_text(summary.unavailable_reason);

        const services::CftcInterpretationState* long_state =
            cftc_presentation_state(participant.states, QStringLiteral("LONG_ACCUMULATION"), horizon);
        if (!long_state)
            long_state = cftc_presentation_state(participant.states, QStringLiteral("LONG_LIQUIDATION"), horizon);
        const services::CftcStateMetric* long_metric =
            long_state ? cftc_presentation_metric(long_state->metrics, QStringLiteral("move_value")) : nullptr;
        const bool long_state_fired = long_metric && long_metric->has_value;
        const services::CftcHorizonFlowReading* reading = cftc_presentation_flow_reading(participant, horizon);
        bool has_long_flow = false;
        double long_flow = 0.0;
        if (reading && reading->evaluated) {
            has_long_flow = reading->has_long_flow;
            long_flow = reading->long_flow;
        }
        if (!has_long_flow && long_state_fired) {
            has_long_flow = true;
            long_flow = long_metric->value;
        }
        QString long_reason;
        const CftcEvidenceStatus long_flow_status =
            cftc_leg_evidence_status(participant, result.unavailable, horizon, /*long_leg=*/true, has_long_flow,
                                     summary.evaluated, summary.unavailable_reason, long_reason);
        if (has_long_flow && !long_state_fired && reading && reading->evaluated && !reading->has_long_rank) {
            add(cftc_presentation_tr("Long leg flow (% of prior OI)"), CftcEvidenceStatus::Unavailable,
                QString::number(long_flow, 'f', 2) + QLatin1Char('%') +
                    cftc_materiality_unavailable_suffix(leg_materiality_reason(horizon, true, summary)),
                horizon);
        } else {
            add(cftc_presentation_tr("Long leg flow (% of prior OI)"),
                has_long_flow ? (long_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState)
                              : long_flow_status,
                has_long_flow ? QString::number(long_flow, 'f', 2) + QLatin1Char('%') +
                                    (long_state_fired ? QString() : cftc_below_threshold_suffix())
                              : (long_flow_status == CftcEvidenceStatus::Unavailable ? unavailable_text(long_reason)
                                                                                     : summary_text),
                horizon);
        }

        const services::CftcInterpretationState* short_state =
            cftc_presentation_state(participant.states, QStringLiteral("SHORT_BUILDING"), horizon);
        if (!short_state)
            short_state = cftc_presentation_state(participant.states, QStringLiteral("SHORT_COVERING"), horizon);
        const services::CftcStateMetric* short_metric =
            short_state ? cftc_presentation_metric(short_state->metrics, QStringLiteral("move_value")) : nullptr;
        const bool short_state_fired = short_metric && short_metric->has_value;
        bool has_short_flow = false;
        double short_flow = 0.0;
        if (reading && reading->evaluated) {
            has_short_flow = reading->has_short_flow;
            short_flow = reading->short_flow;
        }
        if (!has_short_flow && short_state_fired) {
            has_short_flow = true;
            short_flow = short_metric->value;
        }
        QString short_reason;
        const CftcEvidenceStatus short_flow_status =
            cftc_leg_evidence_status(participant, result.unavailable, horizon, /*long_leg=*/false, has_short_flow,
                                     summary.evaluated, summary.unavailable_reason, short_reason);
        if (has_short_flow && !short_state_fired && reading && reading->evaluated && !reading->has_short_rank) {
            add(cftc_presentation_tr("Short leg flow (% of prior OI)"), CftcEvidenceStatus::Unavailable,
                QString::number(short_flow, 'f', 2) + QLatin1Char('%') +
                    cftc_materiality_unavailable_suffix(leg_materiality_reason(horizon, false, summary)),
                horizon);
        } else {
            add(cftc_presentation_tr("Short leg flow (% of prior OI)"),
                has_short_flow
                    ? (short_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState)
                    : short_flow_status,
                has_short_flow ? QString::number(short_flow, 'f', 2) + QLatin1Char('%') +
                                     (short_state_fired ? QString() : cftc_below_threshold_suffix())
                               : (short_flow_status == CftcEvidenceStatus::Unavailable ? unavailable_text(short_reason)
                                                                                       : summary_text),
                horizon);
        }

        const services::CftcInterpretationState* net_state =
            cftc_presentation_state(participant.states, QStringLiteral("NET_LONGWARD_SHIFT"), horizon);
        if (!net_state)
            net_state = cftc_presentation_state(participant.states, QStringLiteral("NET_SHORTWARD_SHIFT"), horizon);
        const bool net_state_fired = net_state != nullptr;
        const services::CftcStateMetric* net_metric =
            net_state ? cftc_presentation_metric(net_state->metrics, QStringLiteral("net_flow")) : nullptr;
        bool has_net_flow = false;
        double net_flow = 0.0;
        if (reading && reading->evaluated) {
            has_net_flow = reading->has_net_flow;
            net_flow = reading->net_flow;
        }
        if (!has_net_flow && net_metric && net_metric->has_value) {
            has_net_flow = true;
            net_flow = net_metric->value;
        }
        if (!has_net_flow) {
            const auto* assessment = cftc_presentation_price(result, participant.participant_key, horizon);
            if (assessment && assessment->has_positioning_move) {
                has_net_flow = true;
                net_flow = assessment->positioning_move;
            }
        }
        if (has_net_flow && !net_state_fired && reading && reading->evaluated && !reading->has_net_rank) {
            add(cftc_presentation_tr("Net flow (% of prior OI)"), CftcEvidenceStatus::Unavailable,
                signed_percent(net_flow, 2) + QLatin1Char('%') +
                    cftc_materiality_unavailable_suffix(net_materiality_reason(horizon, summary)),
                horizon);
        } else if (has_net_flow) {
            add(cftc_presentation_tr("Net flow (% of prior OI)"),
                net_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState,
                signed_percent(net_flow, 2) + QLatin1Char('%') +
                    (net_state_fired ? QString() : cftc_below_threshold_suffix()),
                horizon);
        } else {
            add(cftc_presentation_tr("Net flow (% of prior OI)"), summary_status, summary_text, horizon);
        }

        bool has_rank = false;
        double rank = 0.0;
        int rank_reference = 0;
        if (reading && reading->evaluated && reading->has_net_rank) {
            has_rank = true;
            rank = reading->net_rank;
            rank_reference = reading->net_rank_reference_count;
        } else if (net_state && net_state->has_move_rank) {
            has_rank = true;
            rank = net_state->move_rank;
            rank_reference = net_state->move_rank_reference_count;
        }
        if (has_rank) {
            add(cftc_presentation_tr("Move materiality rank (% of prior moves)"),
                net_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState,
                QString::number(rank * 100.0, 'f', 1) + QLatin1Char('%') +
                    cftc_presentation_tr(" (n=%1)").arg(rank_reference) +
                    (net_state_fired ? QString() : cftc_below_threshold_suffix()),
                horizon);
        } else if (reading && reading->evaluated && reading->has_net_flow && !reading->has_net_rank) {
            // The raw net move exists but its strictly trailing reference was
            // too short: the rank comparison was never performed, so the row
            // states the concrete reason instead of a threshold outcome.
            add(cftc_presentation_tr("Move materiality rank (% of prior moves)"), CftcEvidenceStatus::Unavailable,
                unavailable_text(net_materiality_reason(horizon, summary)), horizon);
        } else {
            add(cftc_presentation_tr("Move materiality rank (% of prior moves)"), summary_status, summary_text,
                horizon);
        }
    }

    const services::CftcInterpretationState* oi_state = nullptr;
    const services::CftcOpenInterestReading* oi_reading = nullptr;
    int oi_horizon = 4;
    for (int horizon : {4, 13, 1}) {
        oi_state = cftc_presentation_state(result.market_context, QStringLiteral("OI_EXPANSION"), horizon);
        if (!oi_state)
            oi_state = cftc_presentation_state(result.market_context, QStringLiteral("OI_CONTRACTION"), horizon);
        if (oi_state) {
            oi_horizon = horizon;
            break;
        }
    }
    if (!oi_state) {
        for (int horizon : {4, 13, 1}) {
            const auto* reading = cftc_presentation_oi_reading(result, horizon);
            if (reading && reading->evaluated && reading->has_oi_change) {
                oi_reading = reading;
                oi_horizon = horizon;
                break;
            }
        }
    }
    const services::CftcStateMetric* oi_change =
        oi_state ? cftc_presentation_metric(oi_state->metrics, QStringLiteral("oi_change")) : nullptr;
    const bool has_oi_change = oi_change && oi_change->has_value;
    // The unavailable record is only consulted when no state or measurement was
    // emitted; if a state or reading exists its own horizon already describes
    // the value, and a record at another horizon must not mislabel it.
    const services::CftcUnavailableRecord* oi_record = nullptr;
    if (!has_oi_change && !oi_reading) {
        for (int horizon : {4, 13, 1}) {
            oi_record =
                cftc_presentation_unavailable(result.unavailable, QStringLiteral("OI_CONTEXT"), QString(), horizon);
            if (oi_record) {
                oi_horizon = horizon;
                break;
            }
        }
    }
    const QString oi_prefix = cftc_presentation_tr("Open Interest change (% change)%1")
                                  .arg(cftc_presentation_tr(" (%1)").arg(cftc_horizon_phrase(oi_horizon)));
    if (has_oi_change) {
        add(oi_prefix, CftcEvidenceStatus::Available, signed_percent(oi_change->value, 2) + QLatin1Char('%'));
    } else if (oi_reading) {
        add(oi_prefix, CftcEvidenceStatus::NoMaterialState,
            signed_percent(oi_reading->oi_change, 2) + QLatin1Char('%') + cftc_below_threshold_suffix());
    } else {
        add(oi_prefix, oi_record ? CftcEvidenceStatus::Unavailable : CftcEvidenceStatus::NoMaterialState,
            oi_record ? unavailable_text(cftc_unavailable_reason_wording(oi_record->reason)) : no_material);
    }

    const services::CftcPricePositionAssessment* price_assessment =
        cftc_presentation_price_primary(result, participant.participant_key);
    const int price_horizon = price_assessment ? price_assessment->horizon_reports : 4;
    const QString price_suffix = cftc_presentation_tr(" (%1)").arg(cftc_horizon_phrase(price_horizon));
    const bool has_price_move = price_assessment && price_assessment->has_price_move;
    CftcEvidenceStatus price_status = CftcEvidenceStatus::Unavailable;
    QString price_value;
    // Pending wins over any assessment the caller may still hold: the prose and
    // the evidence must describe the same actual price state.
    if (price_state == CftcPriceContextState::Pending) {
        price_status = CftcEvidenceStatus::Pending;
        price_value = cftc_presentation_tr("pending — price context is loading");
    } else if (has_price_move) {
        price_status = CftcEvidenceStatus::Available;
        price_value = cftc_price_move_evidence_text(*price_assessment);
    } else {
        const bool missing_context =
            !price_assessment || price_assessment->reason == services::CftcUnavailableReason::MissingPriceContext;
        const QString reason =
            missing_context && !price_unavailable_note.trimmed().isEmpty()
                ? price_unavailable_note
                : (price_assessment
                       ? cftc_unavailable_reason_wording(price_assessment->reason)
                       : cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingPriceContext));
        price_status = CftcEvidenceStatus::Unavailable;
        price_value = unavailable_text(reason);
    }
    add(cftc_presentation_tr("Price change (quoted price units)%1").arg(price_suffix), price_status, price_value);
    out.append(cftc_price_rank_evidence(price_assessment, price_state, price_unavailable_note, 0));

    CftcEvidenceStatus relationship_status = CftcEvidenceStatus::Unavailable;
    QString relationship_value;
    if (price_state == CftcPriceContextState::Pending) {
        relationship_status = CftcEvidenceStatus::Pending;
        relationship_value = cftc_presentation_tr("pending — price context is loading");
    } else if (price_assessment && price_assessment->has_state) {
        relationship_status = CftcEvidenceStatus::Available;
        relationship_value = cftc_relationship_wording(price_assessment->state_id);
    } else if (price_assessment && price_assessment->evaluated) {
        relationship_status = CftcEvidenceStatus::NoMaterialState;
        relationship_value = cftc_presentation_tr("not material enough for a relationship state");
    } else {
        const bool missing_context =
            !price_assessment || price_assessment->reason == services::CftcUnavailableReason::MissingPriceContext;
        const QString reason =
            missing_context && !price_unavailable_note.trimmed().isEmpty()
                ? price_unavailable_note
                : (price_assessment
                       ? cftc_unavailable_reason_wording(price_assessment->reason)
                       : cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingPriceContext));
        relationship_status = CftcEvidenceStatus::Unavailable;
        relationship_value = unavailable_text(reason);
    }
    add(cftc_presentation_tr("Price / positioning relationship"), relationship_status, relationship_value);

    const services::CftcInterpretationState* headline_state = nullptr;
    for (const auto& state : participant.states) {
        if (state.state_id == QLatin1String("SEVERE_LONG_EXTREME") ||
            state.state_id == QLatin1String("SEVERE_SHORT_EXTREME") ||
            state.state_id == QLatin1String("CROWDED_LONG") || state.state_id == QLatin1String("CROWDED_SHORT") ||
            state.state_id == QLatin1String("HISTORICALLY_HIGH_NET") ||
            state.state_id == QLatin1String("HISTORICALLY_LOW_NET")) {
            headline_state = &state;
            break;
        }
    }
    if (!headline_state && !participant.states.isEmpty())
        headline_state = &participant.states.first();
    out << cftc_concentration_evidence(result);
    add(cftc_presentation_tr("Evidence basis"),
        headline_state != nullptr ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        headline_state ? cftc_evidence_basis_wording(headline_state->threshold_basis)
                       : cftc_presentation_tr("no state emitted"));
    return out;
}

// ── Single-horizon evidence (Batch 4B correction pass) ──────────────────────

/// The inspectable evidence for one selected interpretation horizon. Only the
/// engine's emitted metrics are shown; a horizon that was evaluated without a
/// material state is marked as evaluated and below the threshold, and missing
/// data keeps its concrete reason. The strict 156-prior-report percentile row
/// is horizon-independent by definition and stays present at every selection.
inline QVector<CftcEvidenceItem> cftc_build_horizon_evidence(const services::CftcInterpretationResult& result,
                                                             const services::CftcParticipantInterpretation& participant,
                                                             const CftcHorizonFlowSummary& summary, int horizon,
                                                             CftcPriceContextState price_state,
                                                             const QString& price_unavailable_note) {
    QVector<CftcEvidenceItem> out;
    auto add = [&out](const QString& label, CftcEvidenceStatus status, const QString& value, int item_horizon = 0) {
        out.append({label, value, status, item_horizon});
    };
    auto unavailable_text = [](const QString& reason) { return cftc_presentation_tr("unavailable — %1").arg(reason); };
    auto signed_percent = [](double value, int decimals) {
        const QString text = QString::number(value, 'f', decimals);
        return value > 0.0 ? QStringLiteral("+") + text : text;
    };
    const QString no_material = cftc_presentation_tr("evaluated, below the materiality threshold");
    const CftcEvidenceStatus summary_status =
        summary.evaluated ? CftcEvidenceStatus::NoMaterialState : CftcEvidenceStatus::Unavailable;
    const QString summary_text = summary.evaluated ? no_material : unavailable_text(summary.unavailable_reason);

    const bool has_report_date = result.report_date_available && result.report_date.isValid();
    add(cftc_presentation_tr("Report date"),
        has_report_date ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        has_report_date
            ? result.report_date.toString(Qt::ISODate)
            : unavailable_text(cftc_unavailable_reason_wording(services::CftcUnavailableReason::StaleCurrentReport)));
    add(cftc_presentation_tr("Participant"), CftcEvidenceStatus::Available, cftc_participant_display_name(participant));

    const bool has_net_pct = participant.has_net_pct_oi;
    add(cftc_presentation_tr("Net %OI (% of current OI)"),
        has_net_pct ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        has_net_pct ? QString::number(participant.net_pct_oi, 'f', 2) + QLatin1Char('%')
                    : unavailable_text(cftc_net_pct_unavailable_reason(result, participant)));

    const bool has_percentile = participant.historical_percentile_available;
    add(cftc_interpretation_percentile_label(),
        has_percentile ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        has_percentile ? cftc_percentile_value_text(participant)
                       : unavailable_text(cftc_percentile_unavailable_reason(result, participant)));

    const services::CftcHorizonFlowReading* flow_reading = cftc_presentation_flow_reading(participant, horizon);
    const QString below_suffix = cftc_below_threshold_suffix();
    double value = 0.0;
    QString leg_reason;

    // See cftc_build_evidence: a raw reading is only "below threshold" when the
    // materiality comparison happened; a too-short reference says so explicitly.
    const auto leg_materiality_reason = [&](bool long_leg) {
        QString reason;
        cftc_leg_evidence_status(participant, result.unavailable, horizon, long_leg, false, summary.evaluated,
                                 summary.unavailable_reason, reason);
        if (reason.isEmpty())
            reason = cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
        return reason;
    };
    const auto net_materiality_reason = [&]() {
        if (const auto* record = cftc_presentation_unavailable(result.unavailable, QStringLiteral("NET_SHIFT"),
                                                               participant.participant_key, horizon))
            return cftc_unavailable_reason_wording(record->reason);
        if (!summary.unavailable_reason.isEmpty())
            return summary.unavailable_reason;
        return cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
    };

    const bool long_state_fired =
        cftc_presentation_state(participant.states, QStringLiteral("LONG_ACCUMULATION"), horizon) != nullptr ||
        cftc_presentation_state(participant.states, QStringLiteral("LONG_LIQUIDATION"), horizon) != nullptr;
    bool has_long_flow = false;
    if (flow_reading && flow_reading->evaluated) {
        has_long_flow = flow_reading->has_long_flow;
        value = flow_reading->long_flow;
    }
    if (!has_long_flow)
        has_long_flow =
            cftc_evidence_has_flow_value(participant, horizon, QStringLiteral("LONG_ACCUMULATION"),
                                         QStringLiteral("LONG_LIQUIDATION"), QStringLiteral("move_value"), value);
    if (has_long_flow && !long_state_fired && flow_reading && flow_reading->evaluated && !flow_reading->has_long_rank) {
        add(cftc_presentation_tr("Long leg flow (% of prior OI)"), CftcEvidenceStatus::Unavailable,
            QString::number(value, 'f', 2) + QLatin1Char('%') +
                cftc_materiality_unavailable_suffix(leg_materiality_reason(true)),
            horizon);
    } else if (has_long_flow) {
        const CftcEvidenceStatus status =
            long_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState;
        add(cftc_presentation_tr("Long leg flow (% of prior OI)"), status,
            QString::number(value, 'f', 2) + QLatin1Char('%') + (long_state_fired ? QString() : below_suffix), horizon);
    } else {
        const CftcEvidenceStatus status =
            cftc_leg_evidence_status(participant, result.unavailable, horizon, /*long_leg=*/true, false,
                                     summary.evaluated, summary.unavailable_reason, leg_reason);
        add(cftc_presentation_tr("Long leg flow (% of prior OI)"), status,
            status == CftcEvidenceStatus::Unavailable ? unavailable_text(leg_reason) : no_material, horizon);
    }

    leg_reason.clear();
    const bool short_state_fired =
        cftc_presentation_state(participant.states, QStringLiteral("SHORT_BUILDING"), horizon) != nullptr ||
        cftc_presentation_state(participant.states, QStringLiteral("SHORT_COVERING"), horizon) != nullptr;
    bool has_short_flow = false;
    if (flow_reading && flow_reading->evaluated) {
        has_short_flow = flow_reading->has_short_flow;
        value = flow_reading->short_flow;
    }
    if (!has_short_flow)
        has_short_flow =
            cftc_evidence_has_flow_value(participant, horizon, QStringLiteral("SHORT_BUILDING"),
                                         QStringLiteral("SHORT_COVERING"), QStringLiteral("move_value"), value);
    if (has_short_flow && !short_state_fired && flow_reading && flow_reading->evaluated &&
        !flow_reading->has_short_rank) {
        add(cftc_presentation_tr("Short leg flow (% of prior OI)"), CftcEvidenceStatus::Unavailable,
            QString::number(value, 'f', 2) + QLatin1Char('%') +
                cftc_materiality_unavailable_suffix(leg_materiality_reason(false)),
            horizon);
    } else if (has_short_flow) {
        const CftcEvidenceStatus status =
            short_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState;
        add(cftc_presentation_tr("Short leg flow (% of prior OI)"), status,
            QString::number(value, 'f', 2) + QLatin1Char('%') + (short_state_fired ? QString() : below_suffix),
            horizon);
    } else {
        const CftcEvidenceStatus status =
            cftc_leg_evidence_status(participant, result.unavailable, horizon, /*long_leg=*/false, false,
                                     summary.evaluated, summary.unavailable_reason, leg_reason);
        add(cftc_presentation_tr("Short leg flow (% of prior OI)"), status,
            status == CftcEvidenceStatus::Unavailable ? unavailable_text(leg_reason) : no_material, horizon);
    }

    const bool net_state_fired =
        cftc_presentation_state(participant.states, QStringLiteral("NET_LONGWARD_SHIFT"), horizon) != nullptr ||
        cftc_presentation_state(participant.states, QStringLiteral("NET_SHORTWARD_SHIFT"), horizon) != nullptr;
    double net_flow = 0.0;
    bool net_flow_available = false;
    if (flow_reading && flow_reading->evaluated) {
        net_flow_available = flow_reading->has_net_flow;
        net_flow = flow_reading->net_flow;
    }
    if (!net_flow_available)
        net_flow_available =
            cftc_evidence_has_flow_value(participant, horizon, QStringLiteral("NET_LONGWARD_SHIFT"),
                                         QStringLiteral("NET_SHORTWARD_SHIFT"), QStringLiteral("net_flow"), net_flow);
    if (!net_flow_available) {
        const auto* assessment = cftc_presentation_price(result, participant.participant_key, horizon);
        if (assessment && assessment->has_positioning_move) {
            net_flow_available = true;
            net_flow = assessment->positioning_move;
        }
    }
    if (net_flow_available && !net_state_fired && flow_reading && flow_reading->evaluated &&
        !flow_reading->has_net_rank) {
        add(cftc_presentation_tr("Net flow (% of prior OI)"), CftcEvidenceStatus::Unavailable,
            signed_percent(net_flow, 2) + QLatin1Char('%') +
                cftc_materiality_unavailable_suffix(net_materiality_reason()),
            horizon);
    } else if (net_flow_available) {
        const CftcEvidenceStatus status =
            net_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState;
        add(cftc_presentation_tr("Net flow (% of prior OI)"), status,
            signed_percent(net_flow, 2) + QLatin1Char('%') + (net_state_fired ? QString() : below_suffix), horizon);
    } else {
        add(cftc_presentation_tr("Net flow (% of prior OI)"), summary_status, summary_text, horizon);
    }

    const services::CftcInterpretationState* net_state =
        cftc_presentation_state(participant.states, QStringLiteral("NET_LONGWARD_SHIFT"), horizon);
    if (!net_state)
        net_state = cftc_presentation_state(participant.states, QStringLiteral("NET_SHORTWARD_SHIFT"), horizon);
    bool rank_available = false;
    double rank = 0.0;
    int rank_reference = 0;
    if (flow_reading && flow_reading->evaluated && flow_reading->has_net_rank) {
        rank_available = true;
        rank = flow_reading->net_rank;
        rank_reference = flow_reading->net_rank_reference_count;
    } else if (net_state && net_state->has_move_rank) {
        rank_available = true;
        rank = net_state->move_rank;
        rank_reference = net_state->move_rank_reference_count;
    }
    if (rank_available) {
        const CftcEvidenceStatus status =
            net_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState;
        add(cftc_presentation_tr("Move materiality rank (% of prior moves)"), status,
            QString::number(rank * 100.0, 'f', 1) + QLatin1Char('%') +
                cftc_presentation_tr(" (n=%1)").arg(rank_reference) + (net_state_fired ? QString() : below_suffix),
            horizon);
    } else if (flow_reading && flow_reading->evaluated && flow_reading->has_net_flow && !flow_reading->has_net_rank) {
        add(cftc_presentation_tr("Move materiality rank (% of prior moves)"), CftcEvidenceStatus::Unavailable,
            unavailable_text(net_materiality_reason()), horizon);
    } else {
        add(cftc_presentation_tr("Move materiality rank (% of prior moves)"), summary_status, summary_text, horizon);
    }

    const CftcHorizonDimension oi = cftc_open_interest_dimension(result, horizon);
    const services::CftcOpenInterestReading* oi_reading = cftc_presentation_oi_reading(result, horizon);
    const bool oi_state_fired = oi.status == CftcEvidenceStatus::Available;
    bool oi_value_available = false;
    double oi_value = 0.0;
    if (oi_reading && oi_reading->evaluated && oi_reading->has_oi_change) {
        oi_value_available = true;
        oi_value = oi_reading->oi_change;
    } else if (oi.has_value) {
        oi_value_available = true;
        oi_value = oi.value;
    }
    if (oi_value_available) {
        const CftcEvidenceStatus status =
            oi_state_fired ? CftcEvidenceStatus::Available : CftcEvidenceStatus::NoMaterialState;
        add(cftc_presentation_tr("Open Interest change (% change) (%1)").arg(cftc_horizon_phrase(horizon)), status,
            signed_percent(oi_value, 2) + QLatin1Char('%') + (oi_state_fired ? QString() : below_suffix), horizon);
    } else {
        add(cftc_presentation_tr("Open Interest change (% change) (%1)").arg(cftc_horizon_phrase(horizon)), oi.status,
            oi.status == CftcEvidenceStatus::Unavailable ? unavailable_text(oi.unavailable_reason) : no_material,
            horizon);
    }

    const auto* price_assessment = cftc_presentation_price(result, participant.participant_key, horizon);
    const QString price_suffix = cftc_presentation_tr(" (%1)").arg(cftc_horizon_phrase(horizon));
    const bool has_price_move = price_assessment && price_assessment->has_price_move;
    CftcEvidenceStatus price_status = CftcEvidenceStatus::Unavailable;
    QString price_value;
    if (price_state == CftcPriceContextState::Pending) {
        price_status = CftcEvidenceStatus::Pending;
        price_value = cftc_presentation_tr("pending — price context is loading");
    } else if (has_price_move) {
        price_status = CftcEvidenceStatus::Available;
        price_value = cftc_price_move_evidence_text(*price_assessment);
    } else {
        const bool missing_context =
            !price_assessment || price_assessment->reason == services::CftcUnavailableReason::MissingPriceContext;
        const QString reason =
            missing_context && !price_unavailable_note.trimmed().isEmpty()
                ? price_unavailable_note
                : (price_assessment
                       ? cftc_unavailable_reason_wording(price_assessment->reason)
                       : cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingPriceContext));
        price_status = CftcEvidenceStatus::Unavailable;
        price_value = unavailable_text(reason);
    }
    add(cftc_presentation_tr("Price change (quoted price units)%1").arg(price_suffix), price_status, price_value,
        horizon);
    out.append(cftc_price_rank_evidence(price_assessment, price_state, price_unavailable_note, horizon));

    CftcEvidenceStatus relationship_status = CftcEvidenceStatus::Unavailable;
    QString relationship_value;
    if (price_state == CftcPriceContextState::Pending) {
        relationship_status = CftcEvidenceStatus::Pending;
        relationship_value = cftc_presentation_tr("pending — price context is loading");
    } else if (price_assessment && price_assessment->has_state) {
        relationship_status = CftcEvidenceStatus::Available;
        relationship_value = cftc_relationship_wording(price_assessment->state_id);
    } else if (price_assessment && price_assessment->evaluated) {
        relationship_status = CftcEvidenceStatus::NoMaterialState;
        relationship_value = cftc_presentation_tr("evaluated, not material enough for a relationship state");
    } else {
        const bool missing_context =
            !price_assessment || price_assessment->reason == services::CftcUnavailableReason::MissingPriceContext;
        const QString reason =
            missing_context && !price_unavailable_note.trimmed().isEmpty()
                ? price_unavailable_note
                : (price_assessment
                       ? cftc_unavailable_reason_wording(price_assessment->reason)
                       : cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingPriceContext));
        relationship_status = CftcEvidenceStatus::Unavailable;
        relationship_value = unavailable_text(reason);
    }
    add(cftc_presentation_tr("Price / positioning relationship"), relationship_status, relationship_value, horizon);

    const services::CftcInterpretationState* headline_state = nullptr;
    for (const auto& state : participant.states) {
        if (state.state_id == QLatin1String("SEVERE_LONG_EXTREME") ||
            state.state_id == QLatin1String("SEVERE_SHORT_EXTREME") ||
            state.state_id == QLatin1String("CROWDED_LONG") || state.state_id == QLatin1String("CROWDED_SHORT") ||
            state.state_id == QLatin1String("HISTORICALLY_HIGH_NET") ||
            state.state_id == QLatin1String("HISTORICALLY_LOW_NET")) {
            headline_state = &state;
            break;
        }
    }
    if (!headline_state && !participant.states.isEmpty())
        headline_state = &participant.states.first();
    out << cftc_concentration_evidence(result);
    add(cftc_presentation_tr("Evidence basis"),
        headline_state != nullptr ? CftcEvidenceStatus::Available : CftcEvidenceStatus::Unavailable,
        headline_state ? cftc_evidence_basis_wording(headline_state->threshold_basis)
                       : cftc_presentation_tr("no state emitted"));
    return out;
}

// ── Composition entry point ─────────────────────────────────────────────────

/// The outdated-report notice: the interpreted report is older than the
/// engine's maximum report age relative to the evaluation date, so it is not a
/// current report (for example a discontinued contract). Empty otherwise.
inline QString cftc_outdated_report_sentence(const services::CftcInterpretationResult& result) {
    if (!result.report_outdated)
        return {};
    return cftc_presentation_tr(
               "Out of date: this is the latest report the history carries, dated %1, %2 days before "
               "today. CFTC publishes every week, so it is not a current report; the contract may have "
               "been discontinued or its reporting suspended.")
        .arg(result.report_date.toString(Qt::ISODate))
        .arg(result.report_age_days);
}

/// The single method note that states the materiality and historical
/// thresholds the conclusions use, so they read as MarketLab heuristics rather
/// than CFTC definitions.
inline QString cftc_method_sentence(const services::CftcInterpretationResult& result) {
    const services::CftcInterpretationConfig& config = result.config;
    return cftc_presentation_tr("Method: “material” means a move at or above the %1 percentile of the previous %2 "
                                "comparable moves, and historical levels compare Net %OI with the previous %2 "
                                "reports. These thresholds are MarketLab heuristics, not CFTC definitions.")
        .arg(QString::number(config.material_move_percentile * 100.0, 'g', 3) + QStringLiteral("th"))
        .arg(config.history_window);
}

inline QString cftc_interpretation_context_text(const services::CftcInterpretationResult& result) {
    QString text;
    const QString outdated = cftc_outdated_report_sentence(result);
    if (!outdated.isEmpty())
        text = outdated + QLatin1Char(' ');
    if (result.report_date_available && result.report_date.isValid()) {
        text += cftc_presentation_tr("Official CFTC report %1. Positioning is the report snapshot; COT reports are "
                                     "published with a delay relative to the observation date. This describes reported "
                                     "positioning and historical context; it is not a price forecast and not a trading "
                                     "recommendation.")
                    .arg(result.report_date.toString(Qt::ISODate));
    } else {
        text += cftc_presentation_tr("Positioning is the report snapshot; COT reports are published with a delay "
                                     "relative to the observation date. This describes reported positioning and "
                                     "historical context; it is not a price forecast and not a trading "
                                     "recommendation.");
    }
    if (!result.rule_set_version.isEmpty())
        text += QLatin1Char(' ') + cftc_presentation_tr("Interpretation engine: %1.").arg(result.rule_set_version);
    return text;
}

inline QString cftc_headline_trajectory(const CftcHorizonFlowSummary& h4, const CftcHorizonFlowSummary& h13,
                                        const CftcHorizonFlowSummary& h1,
                                        const services::CftcParticipantInterpretation& participant) {
    const QString transition = cftc_extreme_transition_headline(participant);
    if (!transition.isEmpty())
        return transition;

    const bool h4_material = h4.evaluated && h4.material_net_move;
    const bool h13_material = h13.evaluated && h13.material_net_move;
    if (h4_material && h13_material && h4.net_direction != h13.net_direction)
        return cftc_presentation_tr("mixed repositioning across horizons");
    const int direction = h4_material ? h4.net_direction : (h13_material ? h13.net_direction : 0);
    const CftcHorizonFlowSummary& leading = h4_material ? h4 : h13;
    // No "continuing": the horizon states say what happened over the window,
    // not that the move is still under way (the latest week may have
    // reversed).
    if (direction > 0)
        return leading.long_accumulation ? cftc_presentation_tr("long accumulation")
                                         : cftc_presentation_tr("longward repositioning");
    if (direction < 0)
        return leading.long_liquidation ? cftc_presentation_tr("long liquidation")
                                        : cftc_presentation_tr("shortward repositioning");
    if (h1.evaluated && h1.material_net_move)
        return h1.net_direction > 0 ? cftc_presentation_tr("longward in the latest weekly report")
                                    : cftc_presentation_tr("shortward in the latest weekly report");
    if (!h4.evaluated || !h13.evaluated)
        return cftc_presentation_tr("recent repositioning unavailable");
    return cftc_presentation_tr("no material recent repositioning");
}

/// The deterministic Batch 4B composition implementation. Pure function of the
/// Batch 4A result plus the caller's price state: it reads emitted states and
/// emitted metrics only and never recalculates them. `price_unavailable_note`
/// is a caller-known concrete price failure (for example a provider error or
/// the absence of a retained source) used only when the engine reports
/// `missing_price_context`. The principal participant is selected from the
/// finalized Batch 4A terminology contract, never from the generic speculative
/// flag, and a missing principal participant is reported as unavailable rather
/// than substituted. `horizon_reports == 0` is the combined multi-horizon view;
/// 1, 4 or 13 composes only that horizon's repositioning, gross-leg mechanism,
/// Open Interest context, disagreement and price relationship, while the
/// full-history level, persistence and 156-prior-report percentile stay
/// identical at every selection.
inline CftcInterpretationView cftc_compose_interpretation_impl(const services::CftcInterpretationResult& result,
                                                               int horizon_reports, CftcPriceContextState price_state,
                                                               const QString& price_unavailable_note) {
    CftcInterpretationView view;
    if (result.rule_set_version.isEmpty() || result.participants.isEmpty()) {
        view.headline = cftc_presentation_tr("COT interpretation unavailable");
        view.sentences << cftc_presentation_tr("The interpretation engine did not return an interpretable report.");
        return view;
    }

    const QString primary_key = cftc_principal_participant_key(result.family);
    const services::CftcParticipantInterpretation* primary = nullptr;
    if (!primary_key.isEmpty()) {
        for (const auto& participant : result.participants) {
            if (participant.participant_key == primary_key) {
                primary = &participant;
                break;
            }
        }
    }
    view.context_text = cftc_interpretation_context_text(result);
    if (!primary) {
        view.headline = cftc_presentation_tr("COT interpretation unavailable");
        view.sentences << cftc_presentation_tr(
            "The report family's principal participant could not be resolved from the finalized terminology "
            "metadata.");
        return view;
    }

    // A whole-report failure (no observations, provenance, a stale as-of
    // report) collapses the view to its report-level reason. A missing leg or
    // Open Interest of the principal participant does not: the rest of the
    // report (the other leg, Open Interest, concentration) is still valid and
    // is composed with explicit unavailable statements, the same way whether
    // the long or the short leg is the missing one.
    // The engine records a report-wide exposure failure only when no
    // participant could be interpreted at all.
    bool any_participant_states = false;
    for (const auto& participant : result.participants) {
        if (!participant.states.isEmpty()) {
            any_participant_states = true;
            break;
        }
    }
    bool whole_report_failure = false;
    QString report_reason;
    for (const auto& record : result.unavailable) {
        if (record.participant_key.isEmpty() && record.state_family == QLatin1String("NET_EXPOSURE") &&
            !any_participant_states) {
            whole_report_failure = true;
            report_reason = cftc_unavailable_reason_wording(record.reason);
            break;
        }
    }
    bool primary_has_records = false;
    for (const auto& record : result.unavailable) {
        if (record.participant_key == primary->participant_key) {
            primary_has_records = true;
            break;
        }
    }
    const bool interpreted = !whole_report_failure && (!primary->states.isEmpty() || primary_has_records);
    view.interpreted = interpreted;
    if (!interpreted) {
        // Truthful reason precedence: a report-wide failure first, then the
        // primary participant's own blocked exposure reading, and only then a
        // generic fallback.
        QString reason = report_reason;
        if (reason.isEmpty()) {
            if (const auto* record = cftc_presentation_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"),
                                                                   primary->participant_key))
                reason = cftc_unavailable_reason_wording(record->reason);
        }
        if (reason.isEmpty())
            reason = cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingParticipantLeg);
        view.headline =
            cftc_presentation_tr("%1 — interpretation unavailable").arg(cftc_participant_display_name(*primary));
        view.sentences << cftc_presentation_tr("The interpretation is unavailable: %1.").arg(reason);
        return view;
    }

    const CftcHorizonFlowSummary h4 = cftc_summarize_horizon(*primary, result.unavailable, 4);
    const CftcHorizonFlowSummary h13 = cftc_summarize_horizon(*primary, result.unavailable, 13);
    const CftcHorizonFlowSummary h1 = cftc_summarize_horizon(*primary, result.unavailable, 1);

    QString net_unavailable_reason;
    if (const auto* record = cftc_presentation_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"),
                                                           primary->participant_key)) {
        net_unavailable_reason = cftc_unavailable_reason_wording(record->reason);
    }

    view.horizon_reports = cftc_is_interpretation_horizon(horizon_reports) ? horizon_reports : 0;
    // An outdated report is flagged in the headline and as the first
    // conclusion, so a discontinued contract's last report is never read as
    // current.
    const QString outdated = cftc_outdated_report_sentence(result);
    const QString outdated_suffix = result.report_outdated
                                        ? cftc_presentation_tr(" · out-of-date report (%1, %2 days old)")
                                              .arg(result.report_date.toString(Qt::ISODate))
                                              .arg(result.report_age_days)
                                        : QString();
    auto append_level_and_extremes = [&]() {
        if (!outdated.isEmpty())
            view.sentences << outdated;
        view.sentences << cftc_level_sentence(result, *primary, net_unavailable_reason);
        if (cftc_exposure_is_crowded(*primary)) {
            const QString caveat = cftc_terminology_caveat_sentence(*primary);
            if (!caveat.isEmpty())
                view.sentences << caveat;
        }
        const QString extreme = cftc_extreme_sentence(result, *primary);
        if (!extreme.isEmpty())
            view.sentences << extreme;
    };

    if (view.horizon_reports == 0) {
        view.headline = QStringLiteral("%1 — %2; %3")
                            .arg(cftc_participant_display_name(*primary), cftc_exposure_phrase(*primary),
                                 cftc_headline_trajectory(h4, h13, h1, *primary)) +
                        outdated_suffix;

        append_level_and_extremes();
        view.sentences << cftc_repositioning_sentences(h4, h13, h1);

        // Gross-leg mechanism: the leading net move's horizon, else the horizon
        // with gross states. The mechanism ids come from the Batch 4A state
        // itself, and the sentence names its horizon because it can differ
        // from the horizon of the repositioning sentence above.
        const CftcHorizonFlowSummary& leading =
            h4.material_net_move || h4.long_accumulation || h4.long_liquidation || h4.short_building ||
                    h4.short_covering
                ? h4
                : (h13.material_net_move || h13.long_accumulation || h13.long_liquidation || h13.short_building ||
                           h13.short_covering
                       ? h13
                       : h1);
        const services::CftcInterpretationState* leading_net_state =
            cftc_presentation_state(primary->states, QStringLiteral("NET_LONGWARD_SHIFT"), leading.horizon);
        if (!leading_net_state)
            leading_net_state =
                cftc_presentation_state(primary->states, QStringLiteral("NET_SHORTWARD_SHIFT"), leading.horizon);
        const QString gross = cftc_gross_mechanism_sentence(leading_net_state, leading, /*label_horizon=*/true);
        if (!gross.isEmpty())
            view.sentences << gross;

        const QString oi = cftc_open_interest_sentence(result);
        if (!oi.isEmpty())
            view.sentences << oi;
        view.sentences << cftc_concentration_sentences(result);
        const QString disagreement = cftc_net_share_disagreement_sentence(*primary);
        if (!disagreement.isEmpty())
            view.sentences << disagreement;

        view.sentences << cftc_price_relationship_sentences(result, *primary, price_state, price_unavailable_note);
        view.sentences << cftc_method_sentence(result);

        view.evidence = cftc_build_evidence(result, *primary, h4, h13, price_state, price_unavailable_note);
        return view;
    }

    // Single-horizon view: the level, caveat, persistence and strict percentile
    // conclusions above remain full-history; everything horizon-scoped comes
    // from the selected horizon's emitted states only.
    const int horizon = view.horizon_reports;
    const CftcHorizonFlowSummary& summary = horizon == 1 ? h1 : (horizon == 4 ? h4 : h13);
    QString trajectory = cftc_headline_trajectory_for_horizon(*primary, summary);
    if (!cftc_has_extreme_transition_state(*primary) && summary.evaluated)
        trajectory = cftc_presentation_tr("%1 over the last %2").arg(trajectory, cftc_horizon_phrase(horizon));
    view.headline = QStringLiteral("%1 — %2; %3")
                        .arg(cftc_participant_display_name(*primary), cftc_exposure_phrase(*primary), trajectory) +
                    outdated_suffix;

    append_level_and_extremes();
    view.sentences << cftc_repositioning_sentence_for_horizon(summary, horizon);
    if (horizon == 1) {
        // The historical level is horizon-independent, but it compares the
        // latest Net %OI with the previous reports of the reference window,
        // not the whole history, and it may itself be unavailable.
        if (primary->historical_percentile_available)
            view.sentences << cftc_presentation_tr(
                                  "The one-report reading is a single weekly observation; the historical level above "
                                  "compares the latest Net %OI with the previous %1 reports and does not depend on the "
                                  "selected horizon.")
                                  .arg(primary->percentile_reference_count > 0 ? primary->percentile_reference_count
                                                                               : result.config.history_window);
        else
            view.sentences << cftc_presentation_tr("The one-report reading is a single weekly observation.");
    }

    const services::CftcInterpretationState* net_state =
        cftc_presentation_state(primary->states, QStringLiteral("NET_LONGWARD_SHIFT"), horizon);
    if (!net_state)
        net_state = cftc_presentation_state(primary->states, QStringLiteral("NET_SHORTWARD_SHIFT"), horizon);
    const QString gross = cftc_gross_mechanism_sentence(net_state, summary);
    if (!gross.isEmpty())
        view.sentences << gross;

    view.sentences << cftc_open_interest_dimension(result, horizon).sentence;
    view.sentences << cftc_concentration_sentences(result);
    const QString disagreement = cftc_net_share_disagreement_sentence_for_horizon(*primary, horizon);
    if (!disagreement.isEmpty())
        view.sentences << disagreement;

    view.sentences << cftc_price_relationship_sentences_for_horizon(result, *primary, horizon, price_state,
                                                                    price_unavailable_note);
    view.sentences << cftc_method_sentence(result);

    view.evidence =
        cftc_build_horizon_evidence(result, *primary, summary, horizon, price_state, price_unavailable_note);
    return view;
}

/// The deterministic Batch 4B composition entry point. Returns the combined
/// multi-horizon view.
inline CftcInterpretationView
cftc_compose_interpretation(const services::CftcInterpretationResult& result,
                            CftcPriceContextState price_state = CftcPriceContextState::Unavailable,
                            const QString& price_unavailable_note = QString()) {
    return cftc_compose_interpretation_impl(result, 0, price_state, price_unavailable_note);
}

/// The single-horizon entry point used by the CFTC Analysis page: the user's
/// 1W | 4W | 13W selection composes that horizon's interpretation. An
/// unsupported horizon falls back to the combined view rather than inventing a
/// scope.
inline CftcInterpretationView
cftc_compose_horizon_interpretation(const services::CftcInterpretationResult& result, int horizon_reports,
                                    CftcPriceContextState price_state = CftcPriceContextState::Unavailable,
                                    const QString& price_unavailable_note = QString()) {
    const int horizon = cftc_is_interpretation_horizon(horizon_reports) ? horizon_reports : 0;
    return cftc_compose_interpretation_impl(result, horizon, price_state, price_unavailable_note);
}

// ── Panel-facing input construction ─────────────────────────────────────────

/// Build the Batch 4A input from the FULL validated observation history. The
/// visible chart range is a presentation filter and must never be passed here:
/// the strict 156-prior-report reference requires the complete official history.
inline services::CftcInterpretationInput
cftc_make_interpretation_input(services::CftcFamily family, const QVector<services::CftcObservation>& full_history,
                               const QString& report_basis_code, const QVector<services::CftcPricePoint>& prices,
                               const QString& price_source, bool price_continuous_proxy, bool price_spot_index,
                               const QDate& evaluation_date = QDate()) {
    services::CftcInterpretationInput input;
    input.family = family;
    input.observations_family_code = services::cftc_family_code(family);
    input.observations = full_history;
    input.report_basis_code = report_basis_code;
    // The caller's calendar date for the report-freshness check; the engine
    // itself never reads a clock.
    input.evaluation_date = evaluation_date;
    input.prices = prices;
    input.price_source = price_source;
    input.price_continuous_proxy = price_continuous_proxy;
    input.price_spot_index = price_spot_index;
    input.config = services::cftc_default_interpretation_config();
    return input;
}

} // namespace fincept::screens
