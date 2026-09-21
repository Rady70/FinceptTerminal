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
#pragma once

#include "screens/economics/panels/CftcNetFormat.h"
#include "services/economics/CftcInterpretationModel.h"

#include <QCoreApplication>
#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fincept::screens {

/// One inspectable piece of supporting evidence. `available == false` means the
/// underlying dimension is unavailable; `value` then carries the concise reason,
/// never a zero or a neutral substitute.
struct CftcEvidenceItem {
    QString label;
    QString value;
    bool available = false;
};

/// The predefined display content for one Batch 4A result. The panel renders
/// this without deciding any wording itself.
struct CftcInterpretationView {
    bool interpreted = false; ///< Batch 4A produced an interpretable report
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
    if (state_id == QLatin1String("HISTORICALLY_HIGH_NET"))
        return cftc_presentation_tr("historically high net exposure");
    if (state_id == QLatin1String("HISTORICALLY_LOW_NET"))
        return cftc_presentation_tr("historically low net exposure");
    if (state_id == QLatin1String("CROWDED_LONG"))
        return cftc_presentation_tr("historically crowded long");
    if (state_id == QLatin1String("CROWDED_SHORT"))
        return cftc_presentation_tr("historically crowded short");
    if (state_id == QLatin1String("SEVERE_LONG_EXTREME"))
        return cftc_presentation_tr("historically extreme net long");
    if (state_id == QLatin1String("SEVERE_SHORT_EXTREME"))
        return cftc_presentation_tr("historically extreme net short");
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
        return cftc_presentation_tr("unwinding high extreme");
    if (state_id == QLatin1String("UNWINDING_LOW_EXTREME"))
        return cftc_presentation_tr("unwinding low extreme");
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
            return cftc_presentation_tr("Open Interest is missing or non-positive");
        case CftcUnavailableReason::InsufficientHistory:
            return cftc_presentation_tr("fewer than 156 prior reports are available for this reference");
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
    const QString sustained_long = horizon == 4 ? QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R")
                                                : QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R");
    const QString sustained_short = horizon == 4 ? QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_4R")
                                                 : QStringLiteral("SUSTAINED_SHORTWARD_REPOSITIONING_13R");
    out.sustained = has(sustained_long) || has(sustained_short);
    const services::CftcUnavailableRecord* record =
        cftc_presentation_unavailable(unavailable, QStringLiteral("NET_SHIFT"), participant.participant_key, horizon);
    if (record) {
        out.evaluated = false;
        out.unavailable_reason = cftc_unavailable_reason_wording(record->reason);
    }
    return out;
}

// ── Phrases and sentences ───────────────────────────────────────────────────

/// Family-appropriate exposure phrase. Crowding wording is used only when the
/// engine emitted a crowding state (which it does only where the Batch 4A
/// semantics permit it) and the participant carries the applicability flag.
inline QString cftc_exposure_phrase(const services::CftcParticipantInterpretation& participant) {
    const bool crowded_long = participant.crowding_terminology_allowed &&
                              cftc_presentation_state(participant.states, QStringLiteral("CROWDED_LONG")) != nullptr;
    const bool crowded_short = participant.crowding_terminology_allowed &&
                               cftc_presentation_state(participant.states, QStringLiteral("CROWDED_SHORT")) != nullptr;
    if (crowded_long)
        return cftc_presentation_tr("historically crowded long");
    if (crowded_short)
        return cftc_presentation_tr("historically crowded short");
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_LONG_EXTREME")))
        return cftc_presentation_tr("historically extreme net long");
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_SHORT_EXTREME")))
        return cftc_presentation_tr("historically extreme net short");
    if (cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_HIGH_NET")))
        return cftc_presentation_tr("historically high net exposure");
    if (cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_LOW_NET")))
        return cftc_presentation_tr("historically low net exposure");
    if (cftc_presentation_state(participant.states, QStringLiteral("NET_LONG")))
        return cftc_presentation_tr("net long");
    if (cftc_presentation_state(participant.states, QStringLiteral("NET_SHORT")))
        return cftc_presentation_tr("net short");
    if (cftc_presentation_state(participant.states, QStringLiteral("NET_FLAT")))
        return cftc_presentation_tr("net flat");
    return cftc_presentation_tr("positioning unavailable");
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

/// Historical-level sentence (precedence 1).
inline QString cftc_level_sentence(const services::CftcParticipantInterpretation& participant,
                                   const QString& net_unavailable_reason) {
    const QString name = cftc_participant_display_name(participant);
    if (participant.crowding_terminology_allowed &&
        cftc_presentation_state(participant.states, QStringLiteral("CROWDED_LONG")))
        return cftc_presentation_tr("%1 remains unusually net long relative to its recent history.").arg(name);
    if (participant.crowding_terminology_allowed &&
        cftc_presentation_state(participant.states, QStringLiteral("CROWDED_SHORT")))
        return cftc_presentation_tr("%1 remains unusually net short relative to its recent history.").arg(name);
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_LONG_EXTREME")))
        return cftc_presentation_tr("%1 sits at a historically extreme net-long reading relative to its recent "
                                    "history.")
            .arg(name);
    if (cftc_presentation_state(participant.states, QStringLiteral("SEVERE_SHORT_EXTREME")))
        return cftc_presentation_tr("%1 sits at a historically extreme net-short reading relative to its recent "
                                    "history.")
            .arg(name);
    if (cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_HIGH_NET")))
        return cftc_presentation_tr("%1 exposure is historically high relative to its recent history.").arg(name);
    if (cftc_presentation_state(participant.states, QStringLiteral("HISTORICALLY_LOW_NET")))
        return cftc_presentation_tr("%1 exposure is historically low relative to its recent history.").arg(name);
    if (cftc_presentation_state(participant.states, QStringLiteral("NET_LONG")))
        return cftc_presentation_tr("%1 is net long in the latest report.").arg(name);
    if (cftc_presentation_state(participant.states, QStringLiteral("NET_SHORT")))
        return cftc_presentation_tr("%1 is net short in the latest report.").arg(name);
    if (cftc_presentation_state(participant.states, QStringLiteral("NET_FLAT")))
        return cftc_presentation_tr("%1 reported long and short positions are equal in the latest report.").arg(name);
    return cftc_presentation_tr("Current %1 net exposure is unavailable: %2.")
        .arg(name, net_unavailable_reason.isEmpty()
                       ? cftc_unavailable_reason_wording(services::CftcUnavailableReason::MissingParticipantLeg)
                       : net_unavailable_reason);
}

/// Extreme persistence / exit / unwind sentence (precedence 2). Empty when no
/// extreme-transition state exists.
inline QString cftc_extreme_sentence(const services::CftcParticipantInterpretation& participant) {
    if (cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_HIGH_EXTREME")))
        return cftc_presentation_tr("The reading has moved well back from the previous high extreme, with net exposure "
                                    "decreasing.");
    if (cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_LOW_EXTREME")))
        return cftc_presentation_tr("The reading has moved well back from the previous low extreme, with net exposure "
                                    "increasing.");
    if (cftc_presentation_state(participant.states, QStringLiteral("EXITED_HIGH_EXTREME")))
        return cftc_presentation_tr("Net exposure has moved back below the high extreme band.");
    if (cftc_presentation_state(participant.states, QStringLiteral("EXITED_LOW_EXTREME")))
        return cftc_presentation_tr("Net exposure has moved back above the low extreme band.");
    if (cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_HIGH_EXTREME")))
        return cftc_presentation_tr("The elevated reading has persisted for at least three consecutive reports.");
    if (cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_LOW_EXTREME")))
        return cftc_presentation_tr("The depressed reading has persisted for at least three consecutive reports.");
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
    if (h4_material && h13_material && h4.net_direction != h13.net_direction) {
        out << cftc_presentation_tr("Positioning is mixed across horizons: %1 over four reports but still %2 over "
                                    "thirteen reports.")
                   .arg(cftc_direction_word(h4.net_direction), cftc_direction_word(h13.net_direction));
    } else if (h4_material && h13_material) {
        out << cftc_presentation_tr("Net positioning shifted %1 over the last four and thirteen reports.")
                   .arg(cftc_direction_word(h4.net_direction));
        if (h4.sustained || h13.sustained)
            out << cftc_presentation_tr("The move was sustained across the individual weekly reports.");
    } else if (h4_material) {
        out << cftc_presentation_tr("Net positioning shifted %1 over the last four reports.")
                   .arg(cftc_direction_word(h4.net_direction));
        if (h4.sustained)
            out << cftc_presentation_tr("The four-report move was sustained across the individual weekly reports.");
    } else if (h13_material) {
        out << cftc_presentation_tr("Net positioning shifted %1 over the last thirteen reports.")
                   .arg(cftc_direction_word(h13.net_direction));
        if (h13.sustained)
            out << cftc_presentation_tr("The thirteen-report move was sustained across the individual weekly reports.");
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
inline QString cftc_gross_mechanism_sentence(const services::CftcInterpretationState* leading_net_state,
                                             const CftcHorizonFlowSummary& leading) {
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

inline QString cftc_net_share_disagreement_sentence(const services::CftcParticipantInterpretation& participant) {
    for (int horizon : {4, 13, 1}) {
        if (cftc_presentation_state(participant.states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), horizon))
            return cftc_presentation_tr("Over %1 the change in Net %OI moved opposite to the raw net flow because "
                                        "market size changed.")
                .arg(cftc_horizon_phrase(horizon));
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

/// Price relationship sentences (precedence 6). The four states render
/// distinctly; opposing-direction states keep the word divergence and never
/// carry a reversal, catch-up or trade-advice claim. `price_unavailable_note`
/// replaces the generic engine reason when the caller knows the concrete
/// failure (for example a provider error) while the engine was handed no price
/// observations.
inline QStringList cftc_price_relationship_sentences(const services::CftcInterpretationResult& result,
                                                     const services::CftcParticipantInterpretation& participant,
                                                     bool price_pending,
                                                     const QString& price_unavailable_note = QString()) {
    QStringList out;
    const auto* a4 = cftc_presentation_price(result, participant.participant_key, 4);
    const auto* a13 = cftc_presentation_price(result, participant.participant_key, 13);
    const services::CftcPricePositionAssessment* primary =
        cftc_presentation_price_primary(result, participant.participant_key);
    if (!primary)
        return out;

    if (price_pending) {
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
    if (!primary->has_state) {
        out << cftc_presentation_tr("Price and net positioning both changed over the last %1, but not both materially "
                                    "enough to classify the relationship.")
                   .arg(cftc_horizon_phrase(primary->horizon_reports));
        return out;
    }

    // A genuine four/thirteen conflict is exposed instead of choosing one.
    if (a4 && a13 && a4->has_state && a13->has_state && a4->state_id != a13->state_id) {
        out << cftc_presentation_tr("The price/positioning relationship differs across horizons: %1 over four reports "
                                    "and %2 over thirteen reports.")
                   .arg(cftc_relationship_wording(a4->state_id), cftc_relationship_wording(a13->state_id));
        return out;
    }

    const QString horizon = cftc_horizon_phrase(primary->horizon_reports);
    if (primary->state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_UP")) {
        out << cftc_presentation_tr("Price and net positioning moved together upward over the last %1.").arg(horizon);
    } else if (primary->state_id == QLatin1String("PRICE_POSITION_MOVING_TOGETHER_DOWN")) {
        out << cftc_presentation_tr("Price and net positioning moved together downward over the last %1.").arg(horizon);
    } else {
        const bool price_up = primary->state_id == QLatin1String("PRICE_UP_POSITIONING_DOWN_DIVERGENCE");
        if (price_up)
            out << cftc_presentation_tr("Price rose materially over %1 while %2 shifted materially shortward.")
                       .arg(horizon, cftc_participant_display_name(participant));
        else
            out << cftc_presentation_tr("Price fell materially over %1 while %2 shifted materially longward.")
                       .arg(horizon, cftc_participant_display_name(participant));
        const QString mechanism = cftc_price_mechanism_fragment(primary->mechanism_state_ids);
        if (!mechanism.isEmpty())
            out << cftc_presentation_tr("The positioning change was driven primarily by %1.").arg(mechanism);
        out << cftc_presentation_tr("This is a contemporaneous divergence only.");
    }
    return out;
}

// ── Evidence ────────────────────────────────────────────────────────────────

inline QVector<CftcEvidenceItem> cftc_build_evidence(const services::CftcInterpretationResult& result,
                                                     const services::CftcParticipantInterpretation& participant,
                                                     const CftcHorizonFlowSummary& h4,
                                                     const CftcHorizonFlowSummary& h13) {
    QVector<CftcEvidenceItem> out;
    auto add = [&out](const QString& label, bool available, const QString& value) {
        out.append({label, value, available});
    };
    auto unavailable = [](const QString& reason) { return cftc_presentation_tr("unavailable — %1").arg(reason); };
    const bool has_report_date = result.report_date_available && result.report_date.isValid();
    add(cftc_presentation_tr("Report date"), has_report_date,
        has_report_date
            ? result.report_date.toString(Qt::ISODate)
            : unavailable(cftc_unavailable_reason_wording(services::CftcUnavailableReason::StaleCurrentReport)));
    add(cftc_presentation_tr("Participant"), true, cftc_participant_display_name(participant));

    const bool has_net_pct = participant.has_net_pct_oi;
    add(cftc_presentation_tr("Net %OI"), has_net_pct,
        has_net_pct ? QString::number(participant.net_pct_oi, 'f', 2) + QLatin1Char('%')
                    : unavailable(cftc_unavailable_reason_wording(
                          result.open_interest_available ? services::CftcUnavailableReason::MissingParticipantLeg
                                                         : services::CftcUnavailableReason::MissingOpenInterest)));

    const bool has_percentile = participant.historical_percentile_available;
    QString percentile_reason = cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
    if (const auto* record = cftc_presentation_unavailable(
            result.unavailable, QStringLiteral("HISTORICAL_RELATIVE_STATE"), participant.participant_key)) {
        percentile_reason = cftc_unavailable_reason_wording(record->reason);
    }
    add(cftc_presentation_tr("Historical percentile (156 prior reports)"), has_percentile,
        has_percentile ? QString::number(participant.percentile * 100.0, 'f', 1) + QLatin1Char('%') +
                             cftc_presentation_tr(" (n=%1)").arg(participant.percentile_reference_count)
                       : unavailable(percentile_reason));

    for (int horizon : {4, 13}) {
        const CftcHorizonFlowSummary& summary = horizon == 4 ? h4 : h13;
        const QString suffix = cftc_presentation_tr(" (%1)").arg(cftc_horizon_phrase(horizon));

        const services::CftcInterpretationState* long_state =
            cftc_presentation_state(participant.states, QStringLiteral("LONG_ACCUMULATION"), horizon);
        if (!long_state)
            long_state = cftc_presentation_state(participant.states, QStringLiteral("LONG_LIQUIDATION"), horizon);
        const services::CftcStateMetric* long_metric =
            long_state ? cftc_presentation_metric(long_state->metrics, QStringLiteral("move_value")) : nullptr;
        const bool has_long_flow = long_metric && long_metric->has_value;
        add(cftc_presentation_tr("Long leg flow%1").arg(suffix), has_long_flow,
            has_long_flow ? QString::number(long_metric->value, 'f', 2) + QLatin1Char('%')
                          : unavailable(summary.evaluated
                                            ? cftc_presentation_tr("no material long-leg move state at this horizon")
                                            : summary.unavailable_reason));

        const services::CftcInterpretationState* short_state =
            cftc_presentation_state(participant.states, QStringLiteral("SHORT_BUILDING"), horizon);
        if (!short_state)
            short_state = cftc_presentation_state(participant.states, QStringLiteral("SHORT_COVERING"), horizon);
        const services::CftcStateMetric* short_metric =
            short_state ? cftc_presentation_metric(short_state->metrics, QStringLiteral("move_value")) : nullptr;
        const bool has_short_flow = short_metric && short_metric->has_value;
        add(cftc_presentation_tr("Short leg flow%1").arg(suffix), has_short_flow,
            has_short_flow ? QString::number(short_metric->value, 'f', 2) + QLatin1Char('%')
                           : unavailable(summary.evaluated
                                             ? cftc_presentation_tr("no material short-leg move state at this horizon")
                                             : summary.unavailable_reason));

        const services::CftcInterpretationState* net_state =
            cftc_presentation_state(participant.states, QStringLiteral("NET_LONGWARD_SHIFT"), horizon);
        if (!net_state)
            net_state = cftc_presentation_state(participant.states, QStringLiteral("NET_SHORTWARD_SHIFT"), horizon);
        const services::CftcStateMetric* net_metric =
            net_state ? cftc_presentation_metric(net_state->metrics, QStringLiteral("net_flow")) : nullptr;
        bool has_net_flow = net_metric && net_metric->has_value;
        double net_flow = has_net_flow ? net_metric->value : 0.0;
        if (!has_net_flow) {
            const auto* assessment = cftc_presentation_price(result, participant.participant_key, horizon);
            if (assessment && assessment->has_positioning_move) {
                has_net_flow = true;
                net_flow = assessment->positioning_move;
            }
        }
        add(cftc_presentation_tr("Net flow%1").arg(suffix), has_net_flow,
            has_net_flow
                ? cftc_signed_net(net_flow) + QLatin1Char('%')
                : unavailable(summary.evaluated ? cftc_presentation_tr("no material net move state at this horizon")
                                                : summary.unavailable_reason));

        const bool has_rank = net_state && net_state->has_move_rank;
        add(cftc_presentation_tr("Move materiality rank%1").arg(suffix), has_rank,
            has_rank
                ? QString::number(net_state->move_rank * 100.0, 'f', 1) + QLatin1Char('%') +
                      cftc_presentation_tr(" (n=%1)").arg(net_state->move_rank_reference_count)
                : unavailable(summary.evaluated ? cftc_presentation_tr("no material net move state at this horizon")
                                                : summary.unavailable_reason));
    }

    const services::CftcInterpretationState* oi_state = nullptr;
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
    const services::CftcStateMetric* oi_change =
        oi_state ? cftc_presentation_metric(oi_state->metrics, QStringLiteral("oi_change")) : nullptr;
    const bool has_oi_change = oi_change && oi_change->has_value;
    QString oi_reason = cftc_unavailable_reason_wording(services::CftcUnavailableReason::InsufficientHistory);
    for (const auto& record : result.unavailable) {
        if (record.state_family == QLatin1String("OI_CONTEXT") && record.participant_key.isEmpty() &&
            record.has_horizon && record.horizon_reports == 4) {
            oi_reason = cftc_unavailable_reason_wording(record.reason);
            break;
        }
    }
    add(cftc_presentation_tr("Open Interest change (%1)").arg(cftc_horizon_phrase(oi_horizon)), has_oi_change,
        has_oi_change ? cftc_signed_net(oi_change->value) + QLatin1Char('%') : unavailable(oi_reason));

    const services::CftcPricePositionAssessment* price_assessment =
        cftc_presentation_price_primary(result, participant.participant_key);
    const bool has_price_move = price_assessment && price_assessment->has_price_move;
    add(cftc_presentation_tr("Price change (%1)")
            .arg(cftc_horizon_phrase(price_assessment ? price_assessment->horizon_reports : 4)),
        has_price_move,
        has_price_move ? cftc_signed_net(price_assessment->price_move)
                       : unavailable(price_assessment ? cftc_unavailable_reason_wording(price_assessment->reason)
                                                      : cftc_presentation_tr("no price context was requested")));
    const bool has_relationship = price_assessment && price_assessment->has_state;
    QString relationship_value;
    if (has_relationship)
        relationship_value = cftc_relationship_wording(price_assessment->state_id);
    else if (price_assessment && price_assessment->evaluated)
        relationship_value = cftc_presentation_tr("not material enough for a relationship state");
    else
        relationship_value = unavailable(price_assessment ? cftc_unavailable_reason_wording(price_assessment->reason)
                                                          : cftc_presentation_tr("no price context was requested"));
    add(cftc_presentation_tr("Price / positioning relationship"), has_relationship, relationship_value);

    const services::CftcInterpretationState* headline_state = nullptr;
    for (const auto& state : participant.states) {
        if (state.state_id == QLatin1String("CROWDED_LONG") || state.state_id == QLatin1String("CROWDED_SHORT") ||
            state.state_id == QLatin1String("HISTORICALLY_HIGH_NET") ||
            state.state_id == QLatin1String("HISTORICALLY_LOW_NET") ||
            state.state_id == QLatin1String("SEVERE_LONG_EXTREME") ||
            state.state_id == QLatin1String("SEVERE_SHORT_EXTREME")) {
            headline_state = &state;
            break;
        }
    }
    if (!headline_state && !participant.states.isEmpty())
        headline_state = &participant.states.first();
    add(cftc_presentation_tr("Evidence basis"), headline_state != nullptr,
        headline_state ? cftc_evidence_basis_wording(headline_state->threshold_basis)
                       : cftc_presentation_tr("no state emitted"));
    return out;
}

// ── Composition entry point ─────────────────────────────────────────────────

inline QString cftc_interpretation_context_text(const services::CftcInterpretationResult& result) {
    QString text;
    if (result.report_date_available && result.report_date.isValid()) {
        text = cftc_presentation_tr("Official CFTC report %1. Positioning is the report snapshot; COT reports are "
                                    "published with a delay relative to the observation date. This describes reported "
                                    "positioning and historical context; it is not a price forecast and not a trading "
                                    "recommendation.")
                   .arg(result.report_date.toString(Qt::ISODate));
    } else {
        text = cftc_presentation_tr("Positioning is the report snapshot; COT reports are published with a delay "
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
    if (cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_HIGH_EXTREME")) ||
        cftc_presentation_state(participant.states, QStringLiteral("UNWINDING_LOW_EXTREME")))
        return cftc_presentation_tr("unwind developing");
    if (cftc_presentation_state(participant.states, QStringLiteral("EXITED_HIGH_EXTREME")) ||
        cftc_presentation_state(participant.states, QStringLiteral("EXITED_LOW_EXTREME")))
        return cftc_presentation_tr("extreme exit developing");
    if (cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_HIGH_EXTREME")) ||
        cftc_presentation_state(participant.states, QStringLiteral("PERSISTENT_LOW_EXTREME")))
        return cftc_presentation_tr("extreme persisting");

    const bool h4_material = h4.evaluated && h4.material_net_move;
    const bool h13_material = h13.evaluated && h13.material_net_move;
    if (h4_material && h13_material && h4.net_direction != h13.net_direction)
        return cftc_presentation_tr("mixed repositioning across horizons");
    const int direction = h4_material ? h4.net_direction : (h13_material ? h13.net_direction : 0);
    const CftcHorizonFlowSummary& leading = h4_material ? h4 : h13;
    if (direction > 0)
        return leading.long_accumulation ? cftc_presentation_tr("accumulation continuing")
                                         : cftc_presentation_tr("longward repositioning");
    if (direction < 0)
        return leading.long_liquidation ? cftc_presentation_tr("liquidation continuing")
                                        : cftc_presentation_tr("shortward repositioning");
    if (h1.evaluated && h1.material_net_move)
        return h1.net_direction > 0 ? cftc_presentation_tr("longward in the latest weekly report")
                                    : cftc_presentation_tr("shortward in the latest weekly report");
    if (!h4.evaluated || !h13.evaluated)
        return cftc_presentation_tr("recent repositioning unavailable");
    return cftc_presentation_tr("no material recent repositioning");
}

/// The deterministic Batch 4B composition entry point. Pure function of the
/// Batch 4A result plus the caller's price status: it reads emitted states and
/// emitted metrics only and never recalculates them. `price_unavailable_note`
/// is a caller-known concrete price failure (for example a provider error)
/// used only when the engine reports `missing_price_context`.
inline CftcInterpretationView cftc_compose_interpretation(const services::CftcInterpretationResult& result,
                                                          bool price_pending = false,
                                                          const QString& price_unavailable_note = QString()) {
    CftcInterpretationView view;
    if (result.rule_set_version.isEmpty() || result.participants.isEmpty()) {
        view.headline = cftc_presentation_tr("COT interpretation unavailable");
        view.sentences << cftc_presentation_tr("The interpretation engine did not return an interpretable report.");
        return view;
    }

    const QVector<services::CftcParticipant> family_participants = services::cftc_family_participants(result.family);
    QString primary_key;
    const int primary_index = services::cftc_speculative_index(family_participants);
    if (primary_index >= 0)
        primary_key = family_participants[primary_index].key;
    const services::CftcParticipantInterpretation* primary = nullptr;
    if (!primary_key.isEmpty()) {
        for (const auto& participant : result.participants) {
            if (participant.participant_key == primary_key) {
                primary = &participant;
                break;
            }
        }
    }
    if (!primary)
        primary = &result.participants.first();

    view.context_text = cftc_interpretation_context_text(result);

    const bool interpreted = !primary->states.isEmpty();
    view.interpreted = interpreted;
    if (!interpreted) {
        // Truthful reason precedence: the primary participant's own blocked
        // exposure reading first, then a report-wide failure (an unavailable
        // record without a participant), and only then a generic fallback.
        QString reason;
        if (const auto* record = cftc_presentation_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"),
                                                               primary->participant_key)) {
            reason = cftc_unavailable_reason_wording(record->reason);
        }
        if (reason.isEmpty()) {
            for (const auto& record : result.unavailable) {
                if (record.participant_key.isEmpty() && record.state_family != QLatin1String("OI_CONTEXT") &&
                    record.state_family != QLatin1String("CONCENTRATION")) {
                    reason = cftc_unavailable_reason_wording(record.reason);
                    break;
                }
            }
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

    view.headline = QStringLiteral("%1 — %2; %3")
                        .arg(cftc_participant_display_name(*primary), cftc_exposure_phrase(*primary),
                             cftc_headline_trajectory(h4, h13, h1, *primary));

    view.sentences << cftc_level_sentence(*primary, net_unavailable_reason);
    const QString extreme = cftc_extreme_sentence(*primary);
    if (!extreme.isEmpty())
        view.sentences << extreme;
    view.sentences << cftc_repositioning_sentences(h4, h13, h1);

    // Gross-leg mechanism: the leading net move's horizon, else the horizon
    // with gross states. The mechanism ids come from the Batch 4A state itself.
    const CftcHorizonFlowSummary& leading =
        h4.material_net_move || h4.long_accumulation || h4.long_liquidation || h4.short_building || h4.short_covering
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
    const QString gross = cftc_gross_mechanism_sentence(leading_net_state, leading);
    if (!gross.isEmpty())
        view.sentences << gross;

    const QString oi = cftc_open_interest_sentence(result);
    if (!oi.isEmpty())
        view.sentences << oi;
    const QString disagreement = cftc_net_share_disagreement_sentence(*primary);
    if (!disagreement.isEmpty())
        view.sentences << disagreement;

    view.sentences << cftc_price_relationship_sentences(result, *primary, price_pending, price_unavailable_note);

    view.evidence = cftc_build_evidence(result, *primary, h4, h13);
    return view;
}

// ── Panel-facing input construction ─────────────────────────────────────────

/// Build the Batch 4A input from the FULL validated observation history. The
/// visible chart range is a presentation filter and must never be passed here:
/// the strict 156-prior-report reference requires the complete official history.
inline services::CftcInterpretationInput
cftc_make_interpretation_input(services::CftcFamily family, const QVector<services::CftcObservation>& full_history,
                               const QString& report_basis_code, const QVector<services::CftcPricePoint>& prices,
                               const QString& price_source, bool price_continuous_proxy, bool price_spot_index) {
    services::CftcInterpretationInput input;
    input.family = family;
    input.observations_family_code = services::cftc_family_code(family);
    input.observations = full_history;
    input.report_basis_code = report_basis_code;
    input.prices = prices;
    input.price_source = price_source;
    input.price_continuous_proxy = price_continuous_proxy;
    input.price_spot_index = price_spot_index;
    input.config = services::cftc_default_interpretation_config();
    return input;
}

} // namespace fincept::screens
