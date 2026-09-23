// tests/tst_cftc_presentation.cpp
//
// Batch 4B of the CFTC work: the deterministic presentation/composition layer
// (screens/economics/panels/CftcInterpretationPresentation.h) governed by
// CFTC_DESCRIPTIVE_INTERPRETATION_PLAN.md. It proves that finalized Batch 4A
// state ids map to the intended predefined wording, that headline precedence
// follows the governing order, that crowded/accumulation and crowded/unwind
// combinations render, that gross-leg mechanisms stay distinguishable, that
// 4-report/13-report disagreement is exposed as mixed, that Legacy,
// Disaggregated and TFF terminology stays correct (with no TFF
// commercial/speculator reconstruction), that both divergence directions are
// explicit and non-predictive, that missing price or insufficient history does
// not suppress available direct states, that the visible chart range cannot
// change the interpretation, and that the composed conclusion contains no
// BUY/HOLD/SELL, bullish/bearish, confidence, expected-return, forecast or AI
// language. Header-only over Qt Core; no app sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcInterpretationPresentation.h"

#include <QRegularExpression>
#include <QtTest>

using namespace fincept::screens;
using namespace fincept::services;

namespace {

const QDate kLatest(2026, 9, 15);

CftcInterpretationState make_state(const QString& state_id, const QString& participant_key, int horizon = -1) {
    CftcInterpretationState state;
    state.state_id = state_id;
    state.participant_key = participant_key;
    if (horizon > 0) {
        state.has_horizon = true;
        state.horizon_reports = horizon;
    }
    state.threshold_basis = CftcEvidenceBasis::EngineHeuristic;
    state.scope = CftcInterpretationScope::DescriptiveFlow;
    return state;
}

void add_metric(CftcInterpretationState& state, const QString& key, double value) {
    CftcStateMetric metric;
    metric.key = key;
    metric.has_value = true;
    metric.value = value;
    state.metrics.append(metric);
}

CftcInterpretationResult make_result(CftcFamily family) {
    CftcInterpretationResult result;
    result.rule_set_version = cftc_interpretation_rule_set_version();
    result.config_is_v1 = true;
    result.family = family;
    result.family_code = cftc_family_code(family);
    result.futures_only = false;
    result.report_basis = QStringLiteral("futures_and_options_combined");
    result.report_date = kLatest;
    result.report_date_available = true;
    result.latest_observation_date = kLatest;
    result.open_interest_available = true;
    result.open_interest = 1000000.0;
    for (const auto& candidate : cftc_family_participants(family)) {
        CftcParticipantInterpretation participant;
        participant.participant_key = candidate.key;
        participant.label = candidate.label;
        participant.terminology = cftc_participant_terminology(family, candidate.key);
        participant.terminology_code = cftc_terminology_code(participant.terminology);
        participant.crowding_terminology_allowed = cftc_crowding_terminology_allowed(family, candidate.key);
        if (participant.terminology == CftcTerminologyClass::BroadNonCommercial)
            participant.terminology_caveat_code = QStringLiteral("broad_category_caveat");
        else if (participant.terminology == CftcTerminologyClass::LeveragedFunds)
            participant.terminology_caveat_code = QStringLiteral("not_all_outright_speculation_caveat");
        participant.net_available = true;
        participant.net_position = 100000.0;
        participant.has_net_pct_oi = true;
        participant.net_pct_oi = 12.5;
        participant.historical_percentile_available = true;
        participant.percentile = 0.95;
        participant.percentile_reference_count = 156;
        result.participants.append(participant);
    }
    return result;
}

/// The expected principal participant per family, hard-coded here rather than
/// derived through the helper under test or through the generic speculative
/// flag. The expected key's Batch 4A terminology class is asserted separately.
QString expected_principal_key(CftcFamily family) {
    switch (family) {
        case CftcFamily::Disaggregated:
            return QStringLiteral("managed_money");
        case CftcFamily::Tff:
            return QStringLiteral("leveraged_funds");
        case CftcFamily::Legacy:
            break;
    }
    return QStringLiteral("non_commercial");
}

CftcTerminologyClass expected_principal_terminology(CftcFamily family) {
    switch (family) {
        case CftcFamily::Disaggregated:
            return CftcTerminologyClass::ManagedMoney;
        case CftcFamily::Tff:
            return CftcTerminologyClass::LeveragedFunds;
        case CftcFamily::Legacy:
            break;
    }
    return CftcTerminologyClass::BroadNonCommercial;
}

CftcParticipantInterpretation* primary_participant(CftcInterpretationResult& result) {
    const QString key = expected_principal_key(result.family);
    for (auto& participant : result.participants) {
        if (participant.participant_key == key)
            return &participant;
    }
    return result.participants.isEmpty() ? nullptr : &result.participants.first();
}

const CftcParticipantInterpretation* find_participant(const CftcInterpretationResult& result, const QString& key) {
    for (const auto& participant : result.participants) {
        if (participant.participant_key == key)
            return &participant;
    }
    return nullptr;
}

void add_price_assessment(CftcInterpretationResult& result, const QString& participant_key, int horizon,
                          const QString& state_id, const QStringList& mechanisms, double price_move,
                          double positioning_move) {
    CftcPricePositionAssessment assessment;
    assessment.participant_key = participant_key;
    assessment.horizon_reports = horizon;
    assessment.evaluated = !state_id.isEmpty();
    assessment.has_price_move = assessment.evaluated;
    assessment.price_move = price_move;
    assessment.price_material = assessment.evaluated;
    assessment.has_positioning_move = assessment.evaluated;
    assessment.positioning_move = positioning_move;
    assessment.positioning_material = assessment.evaluated;
    assessment.has_state = !state_id.isEmpty();
    assessment.state_id = state_id;
    assessment.mechanism_state_ids = mechanisms;
    assessment.has_price_move_rank = assessment.evaluated;
    assessment.price_move_rank = 0.9;
    assessment.has_positioning_move_rank = assessment.evaluated;
    assessment.positioning_move_rank = 0.9;
    result.price_context.append(assessment);
}

void add_missing_price_assessment(CftcInterpretationResult& result, const QString& participant_key, int horizon) {
    CftcPricePositionAssessment assessment;
    assessment.participant_key = participant_key;
    assessment.horizon_reports = horizon;
    assessment.evaluated = false;
    assessment.reason = CftcUnavailableReason::MissingPriceContext;
    result.price_context.append(assessment);
}

void add_unavailable(CftcInterpretationResult& result, const QString& state_family, const QString& participant_key,
                     CftcUnavailableReason reason, int horizon = -1, const QString& state_id = QString()) {
    CftcUnavailableRecord record;
    record.state_family = state_family;
    record.participant_key = participant_key;
    record.reason = reason;
    record.state_id = state_id;
    if (horizon > 0) {
        record.has_horizon = true;
        record.horizon_reports = horizon;
    }
    result.unavailable.append(record);
}

QString conclusions_text(const CftcInterpretationView& view) {
    QStringList parts;
    parts << view.headline << view.sentences;
    for (const auto& item : view.evidence)
        parts << item.label << item.value;
    return parts.join(QLatin1Char('\n'));
}

QString join_sentences(const CftcInterpretationView& view) {
    return view.sentences.join(QLatin1Char(' '));
}

CftcObservation legacy_observation(const QDate& date, double open_interest, double long_leg, double short_leg) {
    CftcObservation observation;
    observation.date = date;
    observation.date_label = date.toString(Qt::ISODate);
    observation.market = QStringLiteral("TEST - EXCHANGE");
    observation.contract_code = QStringLiteral("000000");
    observation.units = QStringLiteral("Test units");
    observation.open_interest = open_interest;
    observation.longs = {100.0, long_leg, 100.0};
    observation.shorts = {100.0, short_leg, 100.0};
    return observation;
}

QVector<CftcObservation> make_legacy_series(int count, double long_start, double long_end, double short_start,
                                            double short_end, double open_interest = 1000.0) {
    QVector<CftcObservation> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        const double t = count > 1 ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.0;
        const double long_leg = long_start + (long_end - long_start) * t;
        const double short_leg = short_start + (short_end - short_start) * t;
        out.append(legacy_observation(kLatest.addDays(-7LL * (count - 1 - i)), open_interest, long_leg, short_leg));
    }
    return out;
}

CftcInterpretationInput legacy_input(const QVector<CftcObservation>& observations) {
    CftcInterpretationInput input;
    input.family = CftcFamily::Legacy;
    input.observations_family_code = cftc_family_code(CftcFamily::Legacy);
    input.observations = observations;
    input.report_basis_code = QStringLiteral("futures_and_options_combined");
    input.config = cftc_default_interpretation_config();
    return input;
}

const QStringList kForbiddenPatterns = {QStringLiteral("\\bBUY\\b"),         QStringLiteral("\\bSELL\\b"),
                                        QStringLiteral("\\bHOLD\\b"),        QStringLiteral("\\bbullish\\b"),
                                        QStringLiteral("\\bbearish\\b"),     QStringLiteral("\\boverbought\\b"),
                                        QStringLiteral("\\boversold\\b"),    QStringLiteral("\\bsmart money\\b"),
                                        QStringLiteral("\\bdumb money\\b"),  QStringLiteral("\\bexpected return\\b"),
                                        QStringLiteral("\\bconfidence\\b"),  QStringLiteral("\\bforecast\\b"),
                                        QStringLiteral("\\bsignal\\b"),      QStringLiteral("\\breversal\\b"),
                                        QStringLiteral("\\brecommend"),      QStringLiteral("\\bAI\\b"),
                                        QStringLiteral("\\btarget price\\b")};

QString forbidden_language(const QString& text) {
    for (const QString& pattern : kForbiddenPatterns) {
        const QRegularExpression expression(pattern, QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = expression.match(text);
        if (match.hasMatch())
            return QStringLiteral("%1 … %2").arg(match.captured(0), text.left(300));
    }
    return {};
}

} // namespace

class TstCftcPresentation : public QObject {
    Q_OBJECT
  private slots:
    void state_ids_map_to_predefined_wording();
    void headline_precedence_places_level_before_repositioning();
    void crowded_long_with_continuing_accumulation();
    void crowded_long_with_unwind();
    void long_accumulation_with_short_covering_composition();
    void long_liquidation_with_short_building_composition();
    void four_and_thirteen_report_conflict_is_exposed_as_mixed();
    void four_and_thirteen_relationship_conflict_keeps_divergence_semantics();
    void legacy_terminology_is_neutral_and_crowding_gated();
    void disaggregated_terminology();
    void tff_terminology();
    void tff_is_never_reconstructed_into_commercial_speculator();
    void both_divergence_directions_are_distinct();
    void moving_together_is_not_predictive_confirmation();
    void divergence_wording_contains_no_forecast();
    void missing_price_preserves_cftc_only_conclusions();
    void concrete_price_failure_reason_is_reported();
    void participant_specific_failure_reason_is_reported();
    void report_wide_failure_reason_is_reported();
    void participant_specific_reason_wins_over_report_wide_record();
    void net_share_disagreement_wording_is_exact();
    void principal_mapping_follows_terminology_contract();
    void missing_principal_participant_is_unavailable_not_fallback();
    void evidence_flow_metrics_use_exact_units();
    void evidence_distinguishes_no_material_state_from_unavailable();
    void open_interest_evidence_uses_the_state_horizon();
    void terminology_caveats_render_only_where_required();
    void severe_extreme_is_not_downgraded_to_crowding();
    void insufficient_history_does_not_suppress_direct_states();
    void visible_range_does_not_change_interpretation();
    void deterministic_repeatability();
    void vocabulary_audit();
    void horizon_selection_changes_the_horizon_specific_interpretation();
    void horizon_selection_keeps_the_156_report_state_and_percentile();
    void horizon_view_distinguishes_evaluated_not_material_from_unavailable();
    void horizon_view_carries_concrete_unavailable_reasons();
    void horizon_view_price_pending_versus_failed();
    void horizon_view_open_interest_context();
    void horizon_view_evidence_is_tagged_with_the_selected_horizon();
    void unsupported_horizon_falls_back_to_combined_view();
    void neutral_legacy_metric_label_has_no_speculator_wording();
    void horizon_views_keep_the_forbidden_language_audit();
    void leg_evidence_uses_the_leg_own_record();
    void non_material_open_interest_evidence_is_explicit();
    void price_evidence_keeps_quoted_units_precision();
    void horizon_evidence_uses_emitted_readings();
    void raw_reading_without_materiality_reference_is_not_below_threshold();
    void net_delta_reason_never_comes_from_price();
};

void TstCftcPresentation::state_ids_map_to_predefined_wording() {
    const QStringList state_ids = cftc_interpretation_state_ids();
    QVERIFY(state_ids.size() >= 30);
    for (const QString& state_id : state_ids) {
        const QString short_wording = cftc_state_short_wording(state_id);
        const QString relationship = cftc_relationship_wording(state_id);
        QVERIFY2(!short_wording.isEmpty() || !relationship.isEmpty(),
                 qPrintable(QStringLiteral("state id without predefined wording: %1").arg(state_id)));
    }
    QVERIFY(cftc_state_short_wording(QStringLiteral("NOT_A_STATE")).isEmpty());
    QVERIFY(cftc_relationship_wording(QStringLiteral("NOT_A_STATE")).isEmpty());
}

void TstCftcPresentation::headline_precedence_places_level_before_repositioning() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const int level = view.headline.indexOf(QStringLiteral("historically crowded long"));
    const int trajectory = view.headline.indexOf(QStringLiteral("accumulation continuing"));
    QVERIFY(level >= 0);
    QVERIFY(trajectory > level);
    QVERIFY(view.sentences.first().contains(QStringLiteral("remains unusually net long")));
}

void TstCftcPresentation::crowded_long_with_continuing_accumulation() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    add_metric(net_state, QStringLiteral("net_flow"), 4.2);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
    primary->states << net_state;
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("SHORT_COVERING"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(
        view.headline.startsWith(QStringLiteral("Managed Money — historically crowded long; accumulation continuing")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("remains unusually net long")));
    QVERIFY(
        join_sentences(view).contains(QStringLiteral("Net positioning shifted longward over the last four reports")));
    QVERIFY(join_sentences(view).contains(QStringLiteral(
        "Long exposure increased while short positions were reduced, producing a material longward shift.")));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::crowded_long_with_unwind() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("UNWINDING_HIGH_EXTREME"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_LIQUIDATION")};
    primary->states << net_state;
    primary->states << make_state(QStringLiteral("LONG_LIQUIDATION"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.contains(QStringLiteral("historically crowded long")));
    QVERIFY(view.headline.contains(QStringLiteral("unwind developing")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("moved well back from the previous high extreme")));
    QVERIFY(
        join_sentences(view).contains(QStringLiteral("Net positioning shifted shortward over the last four reports")));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::long_accumulation_with_short_covering_composition() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
    primary->states << net_state;

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(join_sentences(view).contains(QStringLiteral(
        "Long exposure increased while short positions were reduced, producing a material longward shift.")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("positioning increased")));
}

void TstCftcPresentation::long_liquidation_with_short_building_composition() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 13);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_LIQUIDATION"), QStringLiteral("SHORT_BUILDING")};
    primary->states << net_state;

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(join_sentences(view).contains(QStringLiteral(
        "Long exposure decreased while short positions increased, producing a material shortward shift.")));
}

void TstCftcPresentation::four_and_thirteen_report_conflict_is_exposed_as_mixed() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 13);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(join_sentences(view).contains(QStringLiteral("Positioning is mixed across horizons: longward over four "
                                                         "reports but still shortward over thirteen reports.")));
    QVERIFY(view.headline.contains(QStringLiteral("mixed repositioning across horizons")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("shifted longward over the last four and thirteen")));
}

void TstCftcPresentation::four_and_thirteen_relationship_conflict_keeps_divergence_semantics() {
    // 4R moving together, 13R divergence: the conflict is exposed and the
    // divergent horizon keeps its gross-leg mechanism and its
    // contemporaneous-only statement. Before the fix the conflict early return
    // dropped both.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         100.0, 2000.0);
    add_price_assessment(result, primary->participant_key, 13, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 300.0, -4000.0);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("relationship differs across horizons")));
    QVERIFY(text.contains(QStringLiteral("Price rose materially over thirteen reports")));
    QVERIFY(text.contains(QStringLiteral("Non-Commercial shifted materially shortward")));
    QVERIFY(text.contains(QStringLiteral("driven primarily by long liquidation")));
    QVERIFY(text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));

    // Divergence at 4R, moving together at 13R: same guarantees, other order.
    CftcInterpretationResult reverse = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* reverse_primary = primary_participant(reverse);
    reverse_primary->states << make_state(QStringLiteral("NET_SHORT"), reverse_primary->participant_key);
    add_price_assessment(reverse, reverse_primary->participant_key, 4,
                         QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"), {QStringLiteral("SHORT_COVERING")},
                         -100.0, 2000.0);
    add_price_assessment(reverse, reverse_primary->participant_key, 13,
                         QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"), {}, -300.0, -4000.0);
    const QString reverse_text = join_sentences(cftc_compose_interpretation(reverse));
    QVERIFY(reverse_text.contains(QStringLiteral("relationship differs across horizons")));
    QVERIFY(reverse_text.contains(QStringLiteral("Price fell materially over four reports")));
    QVERIFY(reverse_text.contains(QStringLiteral("driven primarily by short covering")));
    QVERIFY(reverse_text.contains(QStringLiteral("contemporaneous divergence")));

    // A conflict between two moving-together states carries no divergence
    // claim: the contemporaneous-only statement must not be invented.
    CftcInterpretationResult moving = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* moving_primary = primary_participant(moving);
    moving_primary->states << make_state(QStringLiteral("NET_LONG"), moving_primary->participant_key);
    add_price_assessment(moving, moving_primary->participant_key, 4,
                         QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {}, 100.0, 2000.0);
    add_price_assessment(moving, moving_primary->participant_key, 13,
                         QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"), {}, -300.0, -4000.0);
    const QString moving_text = join_sentences(cftc_compose_interpretation(moving));
    QVERIFY(moving_text.contains(QStringLiteral("relationship differs across horizons")));
    QVERIFY(!moving_text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY(!moving_text.contains(QStringLiteral("driven primarily by")));
}

void TstCftcPresentation::legacy_terminology_is_neutral_and_crowding_gated() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.startsWith(QStringLiteral("Non-Commercial — historically crowded long")));
    QVERIFY(!view.headline.contains(QStringLiteral("Speculator")));

    // Legacy Commercial is a neutral exposure class: no crowded wording.
    const CftcParticipantInterpretation* commercial = find_participant(result, QStringLiteral("commercial"));
    QVERIFY(commercial != nullptr);
    CftcParticipantInterpretation neutral = *commercial;
    neutral.states.clear();
    neutral.states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), neutral.participant_key);
    neutral.states << make_state(QStringLiteral("CROWDED_LONG"), neutral.participant_key);
    const QString phrase = cftc_exposure_phrase(neutral);
    QVERIFY(phrase.contains(QStringLiteral("historically high net exposure")));
    QVERIFY(!phrase.contains(QStringLiteral("crowded")));

    const CftcParticipantInterpretation* non_reportable = find_participant(result, QStringLiteral("non_reportable"));
    QVERIFY(non_reportable != nullptr);
    CftcParticipantInterpretation neutral_nr = *non_reportable;
    neutral_nr.states.clear();
    neutral_nr.states << make_state(QStringLiteral("NET_SHORT"), neutral_nr.participant_key);
    QCOMPARE(cftc_exposure_phrase(neutral_nr), QStringLiteral("net short"));
}

void TstCftcPresentation::disaggregated_terminology() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    QCOMPARE(primary->participant_key, QStringLiteral("managed_money"));
    QVERIFY(primary->crowding_terminology_allowed);
    primary->states << make_state(QStringLiteral("CROWDED_SHORT"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.startsWith(QStringLiteral("Managed Money — historically crowded short")));

    const CftcParticipantInterpretation* producer = find_participant(result, QStringLiteral("producer_merchant"));
    QVERIFY(producer != nullptr);
    QVERIFY(!producer->crowding_terminology_allowed);
    CftcParticipantInterpretation neutral = *producer;
    neutral.states.clear();
    neutral.states << make_state(QStringLiteral("HISTORICALLY_LOW_NET"), neutral.participant_key);
    QVERIFY(cftc_exposure_phrase(neutral).contains(QStringLiteral("historically low net exposure")));
    QCOMPARE(cftc_participant_display_name(neutral), QStringLiteral("Producer/Merchant/Processor/User"));

    const CftcParticipantInterpretation* swap = find_participant(result, QStringLiteral("swap_dealer"));
    QVERIFY(swap != nullptr);
    CftcParticipantInterpretation neutral_swap = *swap;
    neutral_swap.states.clear();
    neutral_swap.states << make_state(QStringLiteral("NET_LONG"), neutral_swap.participant_key);
    QCOMPARE(cftc_exposure_phrase(neutral_swap), QStringLiteral("net long"));
}

void TstCftcPresentation::tff_terminology() {
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    QCOMPARE(primary->participant_key, QStringLiteral("leveraged_funds"));
    QVERIFY(primary->crowding_terminology_allowed);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.startsWith(QStringLiteral("Leveraged Funds — historically crowded long")));

    const CftcParticipantInterpretation* asset_manager = find_participant(result, QStringLiteral("asset_manager"));
    QVERIFY(asset_manager != nullptr);
    QVERIFY(!asset_manager->crowding_terminology_allowed);
    QCOMPARE(cftc_participant_display_name(*asset_manager), QStringLiteral("Asset Manager/Institutional"));
    CftcParticipantInterpretation neutral = *asset_manager;
    neutral.states.clear();
    neutral.states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), neutral.participant_key);
    QVERIFY(cftc_exposure_phrase(neutral).contains(QStringLiteral("historically high net exposure")));

    const CftcParticipantInterpretation* dealer = find_participant(result, QStringLiteral("dealer"));
    QVERIFY(dealer != nullptr);
    QCOMPARE(cftc_participant_display_name(*dealer), QStringLiteral("Dealer/Intermediary"));
}

void TstCftcPresentation::tff_is_never_reconstructed_into_commercial_speculator() {
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 100.0, -2000.0);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = conclusions_text(view) + QLatin1Char('\n') + view.context_text;
    QVERIFY(!text.contains(QStringLiteral("commercial"), Qt::CaseInsensitive));
    // The mandated Leveraged Funds caveat uses the word "speculation"; what is
    // forbidden is reconstructing a speculator category or a Commercial split.
    // The caveat must be the only sentence carrying that stem.
    QVERIFY(!text.contains(QStringLiteral("speculator"), Qt::CaseInsensitive));
    int speculation_sentences = 0;
    for (const QString& sentence : view.sentences) {
        if (sentence.contains(QStringLiteral("speculat"), Qt::CaseInsensitive))
            ++speculation_sentences;
    }
    QCOMPARE(speculation_sentences, 1);
    QVERIFY(join_sentences(view).contains(QStringLiteral("outright speculation")));
    QVERIFY(text.contains(QStringLiteral("Leveraged Funds")));
}

void TstCftcPresentation::both_divergence_directions_are_distinct() {
    CftcInterpretationResult up = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* up_primary = primary_participant(up);
    up_primary->states << make_state(QStringLiteral("NET_LONG"), up_primary->participant_key);
    add_price_assessment(up, up_primary->participant_key, 4, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 100.0, -2000.0);
    const CftcInterpretationView up_view = cftc_compose_interpretation(up);
    const QString up_text = join_sentences(up_view);
    QVERIFY(up_text.contains(QStringLiteral("Price rose materially over four reports")));
    QVERIFY(up_text.contains(QStringLiteral("Managed Money shifted materially shortward")));
    QVERIFY(up_text.contains(QStringLiteral("driven primarily by long liquidation")));
    QVERIFY(up_text.contains(QStringLiteral("contemporaneous divergence")));

    CftcInterpretationResult down = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* down_primary = primary_participant(down);
    down_primary->states << make_state(QStringLiteral("NET_SHORT"), down_primary->participant_key);
    add_price_assessment(down, down_primary->participant_key, 4, QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"),
                         {QStringLiteral("SHORT_COVERING")}, -100.0, 2000.0);
    const CftcInterpretationView down_view = cftc_compose_interpretation(down);
    const QString down_text = join_sentences(down_view);
    QVERIFY(down_text.contains(QStringLiteral("Price fell materially over four reports")));
    QVERIFY(down_text.contains(QStringLiteral("Managed Money shifted materially longward")));
    QVERIFY(down_text.contains(QStringLiteral("driven primarily by short covering")));
    QVERIFY(down_text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY(up_text != down_text);

    const CftcEvidenceItem* relationship = nullptr;
    for (const auto& item : up_view.evidence) {
        if (item.label.contains(QStringLiteral("relationship")))
            relationship = &item;
    }
    QVERIFY(relationship != nullptr);
    QCOMPARE(relationship->status, CftcEvidenceStatus::Available);
    QVERIFY(relationship->value.contains(QStringLiteral("Divergence")));
}

void TstCftcPresentation::moving_together_is_not_predictive_confirmation() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         100.0, 2000.0);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("moved together upward")));
    QVERIFY(!text.contains(QStringLiteral("confirm"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("signal"), Qt::CaseInsensitive));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));

    CftcInterpretationResult down = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* down_primary = primary_participant(down);
    down_primary->states << make_state(QStringLiteral("NET_SHORT"), down_primary->participant_key);
    add_price_assessment(down, down_primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"),
                         {}, -100.0, -2000.0);
    const QString down_text = join_sentences(cftc_compose_interpretation(down));
    QVERIFY(down_text.contains(QStringLiteral("moved together downward")));
}

void TstCftcPresentation::divergence_wording_contains_no_forecast() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                         {QStringLiteral("LONG_LIQUIDATION")}, 100.0, -2000.0);
    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("contemporaneous divergence")));
    QVERIFY(text.contains(QStringLiteral("driven primarily by long liquidation")));
    QVERIFY(!text.contains(QStringLiteral("reversal"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("should"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("catch up"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("expect"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("predict"), Qt::CaseInsensitive));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::missing_price_preserves_cftc_only_conclusions() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    for (int horizon : {1, 4, 13})
        add_missing_price_assessment(result, primary->participant_key, horizon);

    const CftcInterpretationView unavailable = cftc_compose_interpretation(result);
    QVERIFY(unavailable.headline.contains(QStringLiteral("net long")));
    QVERIFY(join_sentences(unavailable).contains(QStringLiteral("is net long in the latest report")));
    QVERIFY(join_sentences(unavailable).contains(QStringLiteral("Net positioning shifted longward")));
    QVERIFY(join_sentences(unavailable)
                .contains(QStringLiteral("Price relationship unavailable: no price observations were supplied.")));

    const CftcInterpretationView pending = cftc_compose_interpretation(result, CftcPriceContextState::Pending);
    QVERIFY(pending.headline.contains(QStringLiteral("net long")));
    QVERIFY(join_sentences(pending).contains(QStringLiteral("Price context is still loading")));
    QVERIFY(!join_sentences(pending).contains(QStringLiteral("Price relationship unavailable")));
    for (const auto& item : pending.evidence) {
        if (item.label.startsWith(QStringLiteral("Price")))
            QVERIFY(item.status == CftcEvidenceStatus::Pending);
    }
}

void TstCftcPresentation::concrete_price_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    for (int horizon : {1, 4, 13})
        add_missing_price_assessment(result, primary->participant_key, horizon);

    const CftcInterpretationView view = cftc_compose_interpretation(
        result, CftcPriceContextState::Unavailable, QStringLiteral("the Yahoo Finance history request failed"));
    QVERIFY(join_sentences(view).contains(
        QStringLiteral("Price relationship unavailable: the Yahoo Finance history request failed.")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("no price observations were supplied")));
    QVERIFY(view.headline.contains(QStringLiteral("net long")));
    // The evidence rows carry the same concrete reason as the prose.
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Price"))) {
            QVERIFY(item.status == CftcEvidenceStatus::Unavailable);
            QVERIFY(item.value.contains(QStringLiteral("the Yahoo Finance history request failed")));
            QVERIFY(!item.value.contains(QStringLiteral("no price observations were supplied")));
        }
    }
}

void TstCftcPresentation::participant_specific_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    const QString key = expected_principal_key(CftcFamily::Legacy);
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), key, CftcUnavailableReason::MissingParticipantLeg);
    add_unavailable(result, QStringLiteral("HISTORICAL_RELATIVE_STATE"), key,
                    CftcUnavailableReason::MissingParticipantLeg);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(!view.interpreted);
    QVERIFY(view.headline.contains(QStringLiteral("interpretation unavailable")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("the reported long or short leg is missing")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("no official CFTC observations")));
}

void TstCftcPresentation::report_wide_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), QString(), CftcUnavailableReason::NoObservations);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(!view.interpreted);
    QVERIFY(join_sentences(view).contains(QStringLiteral("no official CFTC observations are available")));
}

void TstCftcPresentation::participant_specific_reason_wins_over_report_wide_record() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    const QString key = expected_principal_key(CftcFamily::Legacy);
    // A report-wide record must not shadow the primary participant's own
    // blocked reading, even when it appears first in the unavailable list.
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), QString(), CftcUnavailableReason::NoObservations);
    add_unavailable(result, QStringLiteral("NET_EXPOSURE"), key, CftcUnavailableReason::MissingParticipantLeg);
    // Another participant carries states (the mixed-report case): the primary
    // participant's own reading is still what the headline reports on.
    for (auto& participant : result.participants) {
        if (participant.participant_key == QStringLiteral("commercial"))
            participant.states << make_state(QStringLiteral("NET_LONG"), participant.participant_key);
    }

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(!view.interpreted);
    QVERIFY(join_sentences(view).contains(QStringLiteral("the reported long or short leg is missing")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("no official CFTC observations")));
}

void TstCftcPresentation::net_share_disagreement_wording_is_exact() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), primary->participant_key, 4);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const QString text = join_sentences(view);
    QVERIFY(text.contains(QStringLiteral("the change in Net %OI moved opposite to the raw net flow")));
    QVERIFY(!text.contains(QStringLiteral("%%")));
}

void TstCftcPresentation::principal_mapping_follows_terminology_contract() {
    const CftcFamily families[] = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    for (CftcFamily family : families) {
        const QString key = cftc_principal_participant_key(family);
        QCOMPARE(key, expected_principal_key(family));
        QVERIFY(!key.isEmpty());
        // The mapping is proven through the finalized Batch 4A terminology
        // metadata, not through the generic speculative flag.
        QCOMPARE(cftc_participant_terminology(family, key), expected_principal_terminology(family));
    }
    // A key from another family is never reassigned by the terminology table.
    QCOMPARE(cftc_participant_terminology(CftcFamily::Tff, QStringLiteral("non_commercial")),
             CftcTerminologyClass::Unknown);
}

void TstCftcPresentation::missing_principal_participant_is_unavailable_not_fallback() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    for (int i = result.participants.size() - 1; i >= 0; --i) {
        if (result.participants[i].participant_key == QStringLiteral("managed_money"))
            result.participants.removeAt(i);
    }
    // Another participant still carries a state; the composer must not
    // substitute it for the missing principal participant.
    for (auto& participant : result.participants) {
        if (participant.participant_key == QStringLiteral("producer_merchant"))
            participant.states << make_state(QStringLiteral("NET_LONG"), participant.participant_key);
    }

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(!view.interpreted);
    QVERIFY(view.headline.contains(QStringLiteral("interpretation unavailable")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("principal participant could not be resolved")));
    QVERIFY(!conclusions_text(view).contains(QStringLiteral("Producer/Merchant")));
}

void TstCftcPresentation::evidence_flow_metrics_use_exact_units() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);

    CftcInterpretationState long_state = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    add_metric(long_state, QStringLiteral("move_value"), 12.5);
    primary->states << long_state;

    CftcInterpretationState short_state = make_state(QStringLiteral("SHORT_COVERING"), primary->participant_key, 13);
    add_metric(short_state, QStringLiteral("move_value"), -8.25);
    primary->states << short_state;

    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    add_metric(net_state, QStringLiteral("net_flow"), 3.5);
    net_state.has_move_rank = true;
    net_state.move_rank = 0.75;
    net_state.move_rank_reference_count = 156;
    primary->states << net_state;

    CftcInterpretationState oi_state = make_state(QStringLiteral("OI_EXPANSION"), QString());
    oi_state.has_horizon = true;
    oi_state.horizon_reports = 4;
    add_metric(oi_state, QStringLiteral("oi_change"), 7.5);
    result.market_context << oi_state;

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    auto row = [&view](const QString& label, int horizon = -1) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label != label)
                continue;
            if (horizon >= 0 && item.horizon_reports != horizon)
                continue;
            return &item;
        }
        return nullptr;
    };

    const CftcEvidenceItem* long_row = row(QStringLiteral("Long leg flow (% of prior OI)"), 4);
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(long_row->value, QStringLiteral("12.50%"));
    QCOMPARE(long_row->horizon_reports, 4);

    const CftcEvidenceItem* short_row = row(QStringLiteral("Short leg flow (% of prior OI)"), 13);
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(short_row->value, QStringLiteral("-8.25%"));
    QCOMPARE(short_row->horizon_reports, 13);

    const CftcEvidenceItem* net_row = row(QStringLiteral("Net flow (% of prior OI)"), 4);
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(net_row->value, QStringLiteral("+3.50%"));

    const CftcEvidenceItem* rank_row = row(QStringLiteral("Move materiality rank (% of prior moves)"), 4);
    QVERIFY(rank_row != nullptr);
    QCOMPARE(rank_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(rank_row->value, QStringLiteral("75.0% (n=156)"));

    const CftcEvidenceItem* oi_row = row(QStringLiteral("Open Interest change (% change) (four reports)"));
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(oi_row->value, QStringLiteral("+7.50%"));

    const CftcEvidenceItem* net_pct_row = row(QStringLiteral("Net %OI (% of current OI)"));
    QVERIFY(net_pct_row != nullptr);
    QCOMPARE(net_pct_row->value, QStringLiteral("12.50%"));

    // No evidence row relabels a normalized Batch 4A metric as contract counts.
    for (const auto& item : view.evidence) {
        QVERIFY(!item.label.contains(QStringLiteral("contracts"), Qt::CaseInsensitive));
        QVERIFY(!item.value.contains(QStringLiteral("contracts"), Qt::CaseInsensitive));
    }
}

void TstCftcPresentation::evidence_distinguishes_no_material_state_from_unavailable() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    // A 4-report price assessment that was evaluated but produced no state.
    CftcPricePositionAssessment assessed;
    assessed.participant_key = primary->participant_key;
    assessed.horizon_reports = 4;
    assessed.evaluated = true;
    assessed.has_price_move = true;
    assessed.price_move = 5.0;
    assessed.has_positioning_move = true;
    assessed.positioning_move = 2.0;
    result.price_context << assessed;

    // The 13-report horizon is genuinely unevaluable.
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::BrokenReportSequence, 13);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    auto row = [&view](const QString& label, int horizon = -1) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label != label)
                continue;
            if (horizon >= 0 && item.horizon_reports != horizon)
                continue;
            return &item;
        }
        return nullptr;
    };

    const CftcEvidenceItem* long_4 = row(QStringLiteral("Long leg flow (% of prior OI)"), 4);
    QVERIFY(long_4 != nullptr);
    QCOMPARE(long_4->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(long_4->value, QStringLiteral("no material state at this horizon"));

    const CftcEvidenceItem* net_13 = row(QStringLiteral("Net flow (% of prior OI)"), 13);
    QVERIFY(net_13 != nullptr);
    QCOMPARE(net_13->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(net_13->value.contains(QStringLiteral("unavailable")));
    QVERIFY(net_13->value.contains(QStringLiteral("weekly report sequence is broken")));

    const CftcEvidenceItem* relationship = row(QStringLiteral("Price / positioning relationship"));
    QVERIFY(relationship != nullptr);
    QCOMPARE(relationship->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(relationship->value, QStringLiteral("not material enough for a relationship state"));

    const CftcEvidenceItem* price = row(QStringLiteral("Price change (quoted price units) (four reports)"));
    QVERIFY(price != nullptr);
    QCOMPARE(price->status, CftcEvidenceStatus::Available);
    QCOMPARE(price->value, QStringLiteral("+5.00"));
}

void TstCftcPresentation::open_interest_evidence_uses_the_state_horizon() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcInterpretationState oi_state = make_state(QStringLiteral("OI_EXPANSION"), QString());
    oi_state.has_horizon = true;
    oi_state.horizon_reports = 4;
    add_metric(oi_state, QStringLiteral("oi_change"), 7.5);
    result.market_context << oi_state;
    // An unavailable record at a different horizon must not relabel the value.
    add_unavailable(result, QStringLiteral("OI_CONTEXT"), QString(), CftcUnavailableReason::InsufficientHistory, 13);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    const CftcEvidenceItem* oi_row = nullptr;
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Open Interest change")))
            oi_row = &item;
    }
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(oi_row->value, QStringLiteral("+7.50%"));
    QVERIFY(oi_row->label.contains(QStringLiteral("four reports")));
    QVERIFY(!oi_row->label.contains(QStringLiteral("thirteen reports")));
}

void TstCftcPresentation::terminology_caveats_render_only_where_required() {
    CftcInterpretationResult legacy = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* legacy_primary = primary_participant(legacy);
    legacy_primary->states << make_state(QStringLiteral("CROWDED_LONG"), legacy_primary->participant_key);
    const CftcInterpretationView legacy_view = cftc_compose_interpretation(legacy);
    QVERIFY(join_sentences(legacy_view).contains(QStringLiteral("broad non-commercial category")));
    int caveat_count = 0;
    for (const QString& sentence : legacy_view.sentences) {
        if (sentence.contains(QStringLiteral("broad non-commercial category")))
            ++caveat_count;
    }
    QCOMPARE(caveat_count, 1);

    // The caveat is not repeated when no crowding wording is rendered.
    CftcInterpretationResult neutral_legacy = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* neutral_primary = primary_participant(neutral_legacy);
    neutral_primary->states << make_state(QStringLiteral("NET_LONG"), neutral_primary->participant_key);
    QVERIFY(!join_sentences(cftc_compose_interpretation(neutral_legacy))
                 .contains(QStringLiteral("broad non-commercial category")));

    CftcInterpretationResult tff = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* tff_primary = primary_participant(tff);
    tff_primary->states << make_state(QStringLiteral("CROWDED_SHORT"), tff_primary->participant_key);
    const CftcInterpretationView tff_view = cftc_compose_interpretation(tff);
    QVERIFY(join_sentences(tff_view).contains(QStringLiteral("leveraged-funds category")));
    QVERIFY(join_sentences(tff_view).contains(QStringLiteral("outright speculation")));

    // Managed Money and the neutral classes carry no caveat code, so no caveat
    // sentence is invented for them.
    CftcInterpretationResult managed_money = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* mm_primary = primary_participant(managed_money);
    mm_primary->states << make_state(QStringLiteral("CROWDED_LONG"), mm_primary->participant_key);
    const QString mm_text = join_sentences(cftc_compose_interpretation(managed_money));
    QVERIFY(!mm_text.contains(QStringLiteral("broad non-commercial category")));
    QVERIFY(!mm_text.contains(QStringLiteral("leveraged-funds category")));

    const CftcParticipantInterpretation* commercial = find_participant(legacy, QStringLiteral("commercial"));
    QVERIFY(commercial != nullptr);
    QVERIFY(commercial->terminology_caveat_code.isEmpty());
    QVERIFY(cftc_terminology_caveat_sentence(*commercial).isEmpty());
}

void TstCftcPresentation::severe_extreme_is_not_downgraded_to_crowding() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("SEVERE_LONG_EXTREME"), primary->participant_key);
    primary->states << make_state(QStringLiteral("HISTORICALLY_HIGH_NET"), primary->participant_key);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.headline.contains(QStringLiteral("historically severe net long")));
    QVERIFY(!view.headline.contains(QStringLiteral("crowded")));
    QVERIFY(join_sentences(view).contains(QStringLiteral("severe historical net-long extreme")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("broad non-commercial category")));

    CftcInterpretationResult short_result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* short_primary = primary_participant(short_result);
    short_primary->states << make_state(QStringLiteral("SEVERE_SHORT_EXTREME"), short_primary->participant_key);
    short_primary->states << make_state(QStringLiteral("CROWDED_SHORT"), short_primary->participant_key);
    short_primary->states << make_state(QStringLiteral("NET_SHORT"), short_primary->participant_key);
    const CftcInterpretationView short_view = cftc_compose_interpretation(short_result);
    QVERIFY(short_view.headline.contains(QStringLiteral("historically severe net short")));
    QVERIFY(!short_view.headline.contains(QStringLiteral("crowded")));
}

void TstCftcPresentation::insufficient_history_does_not_suppress_direct_states() {
    const QVector<CftcObservation> observations = make_legacy_series(10, 500.0, 900.0, 600.0, 400.0);
    CftcInterpretationInput input = legacy_input(observations);
    const CftcInterpretationResult result = cftc_interpret(input);

    const CftcInterpretationView view = cftc_compose_interpretation(result);
    QVERIFY(view.interpreted);
    QVERIFY(view.headline.contains(QStringLiteral("net long")));
    QVERIFY(!view.headline.contains(QStringLiteral("crowded")));
    QVERIFY(!view.headline.contains(QStringLiteral("historically")));

    bool percentile_unavailable = false;
    for (const auto& item : view.evidence) {
        if (item.label.contains(QStringLiteral("percentile")) && item.status == CftcEvidenceStatus::Unavailable)
            percentile_unavailable = true;
    }
    QVERIFY(percentile_unavailable);
    QVERIFY(!conclusions_text(view).contains(QStringLiteral("crowded"), Qt::CaseInsensitive));
    QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
             qPrintable(forbidden_language(conclusions_text(view))));
}

void TstCftcPresentation::visible_range_does_not_change_interpretation() {
    // 190 reports: a quiet history followed by a sharp long build-up, so the
    // full-history result is historically crowded while a one-year window is
    // not. This makes the invariant below non-vacuous.
    QVector<CftcObservation> observations = make_legacy_series(190, 400.0, 400.0, 600.0, 600.0);
    for (int i = 170; i < observations.size(); ++i) {
        const int step = i - 169;
        observations[i].longs[1] = 400.0 + 300.0 * step;
        observations[i].shorts[1] = 600.0 - 20.0 * step;
    }

    const CftcInterpretationInput full_input = legacy_input(observations);
    const CftcInterpretationResult full_result = cftc_interpret(full_input);
    const QVector<CftcObservation> window = cftc_filter_range(observations, CftcRange::OneYear);
    QVERIFY(window.size() < observations.size());
    CftcInterpretationInput window_input = legacy_input(window);
    const CftcInterpretationResult window_result = cftc_interpret(window_input);

    const CftcInterpretationView full_view = cftc_compose_interpretation(full_result);
    const CftcInterpretationView window_view = cftc_compose_interpretation(window_result);
    QVERIFY(full_view.headline.contains(QStringLiteral("historically severe net long")));
    QVERIFY(!window_view.headline.contains(QStringLiteral("crowded")));
    QVERIFY(!window_view.headline.contains(QStringLiteral("severe")));
    QVERIFY(full_view.headline != window_view.headline);

    // The panel builds the engine input from the full validated history: the
    // constructor takes that full vector and has no range parameter, so a range
    // change cannot alter the interpreted observations.
    const CftcInterpretationInput rebuilt = cftc_make_interpretation_input(
        CftcFamily::Legacy, observations, QStringLiteral("futures_and_options_combined"), {}, QString(), false, false);
    QCOMPARE(rebuilt.observations.size(), observations.size());
    const CftcInterpretationView rebuilt_view = cftc_compose_interpretation(cftc_interpret(rebuilt));
    QCOMPARE(rebuilt_view.headline, full_view.headline);
    QCOMPARE(rebuilt_view.sentences, full_view.sentences);
}

void TstCftcPresentation::deterministic_repeatability() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
    primary->states << net_state;
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         50.0, 1500.0);

    const CftcInterpretationView first = cftc_compose_interpretation(result);
    const CftcInterpretationView second = cftc_compose_interpretation(result);
    QCOMPARE(first.headline, second.headline);
    QCOMPARE(first.sentences, second.sentences);
    QCOMPARE(first.context_text, second.context_text);
    QCOMPARE(first.evidence.size(), second.evidence.size());
    for (int i = 0; i < first.evidence.size(); ++i) {
        QCOMPARE(first.evidence[i].label, second.evidence[i].label);
        QCOMPARE(first.evidence[i].value, second.evidence[i].value);
        QCOMPARE(first.evidence[i].status, second.evidence[i].status);
    }
}

void TstCftcPresentation::vocabulary_audit() {
    CftcFamily families[] = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    for (CftcFamily family : families) {
        CftcInterpretationResult result = make_result(family);
        CftcParticipantInterpretation* primary = primary_participant(result);
        primary->states << make_state(QStringLiteral("SEVERE_LONG_EXTREME"), primary->participant_key);
        primary->states << make_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), primary->participant_key);
        primary->states << make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
        primary->states << make_state(QStringLiteral("LONG_LIQUIDATION"), primary->participant_key, 4);
        primary->states << make_state(QStringLiteral("SHORT_BUILDING"), primary->participant_key, 4);
        add_price_assessment(result, primary->participant_key, 4,
                             QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"),
                             {QStringLiteral("LONG_LIQUIDATION"), QStringLiteral("SHORT_BUILDING")}, 100.0, -2000.0);
        const CftcInterpretationView view = cftc_compose_interpretation(result);
        QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
                 qPrintable(forbidden_language(conclusions_text(view))));
        QVERIFY(view.context_text.contains(QStringLiteral("not a price forecast")));
        QVERIFY(view.context_text.contains(QStringLiteral("historical context")));
    }
}

void TstCftcPresentation::horizon_selection_changes_the_horizon_specific_interpretation() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcInterpretationState h1 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 1);
    add_metric(h1, QStringLiteral("net_flow"), 1.5);
    h1.has_move_rank = true;
    h1.move_rank = 0.80;
    h1.move_rank_reference_count = 156;
    primary->states << h1;

    CftcInterpretationState h4 = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
    add_metric(h4, QStringLiteral("net_flow"), -2.5);
    h4.has_move_rank = true;
    h4.move_rank = 0.77;
    h4.move_rank_reference_count = 156;
    primary->states << h4;

    CftcInterpretationState h13 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 13);
    add_metric(h13, QStringLiteral("net_flow"), 8.0);
    h13.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION")};
    h13.has_move_rank = true;
    h13.move_rank = 0.95;
    h13.move_rank_reference_count = 156;
    primary->states << h13;
    CftcInterpretationState accum13 = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 13);
    add_metric(accum13, QStringLiteral("move_value"), 12.5);
    primary->states << accum13;

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);

    QCOMPARE(one.horizon_reports, 1);
    QCOMPARE(four.horizon_reports, 4);
    QCOMPARE(thirteen.horizon_reports, 13);

    QVERIFY(one.headline.contains(QStringLiteral("longward repositioning over the last one report")));
    QVERIFY(four.headline.contains(QStringLiteral("shortward repositioning over the last four reports")));
    QVERIFY(thirteen.headline.contains(QStringLiteral("long accumulation over the last thirteen reports")));

    QVERIFY(join_sentences(one).contains(QStringLiteral("single weekly observation")));
    QVERIFY(join_sentences(four).contains(QStringLiteral("shortward over the last four reports")));
    QVERIFY(join_sentences(thirteen).contains(QStringLiteral("Long exposure increased materially")));

    auto evidence_value = [](const CftcInterpretationView& view, const QString& label) {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return item.value;
        }
        return QString();
    };
    const QString net_flow_label = QStringLiteral("Net flow (% of prior OI)");
    QCOMPARE(evidence_value(one, net_flow_label), QStringLiteral("+1.50%"));
    QCOMPARE(evidence_value(four, net_flow_label), QStringLiteral("-2.50%"));
    QCOMPARE(evidence_value(thirteen, net_flow_label), QStringLiteral("+8.00%"));

    // The level conclusion is full-history and identical at every selection.
    QCOMPARE(one.sentences.first(), four.sentences.first());
    QCOMPARE(four.sentences.first(), thirteen.sentences.first());
}

void TstCftcPresentation::horizon_selection_keeps_the_156_report_state_and_percentile() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
    primary->states << make_state(QStringLiteral("PERSISTENT_HIGH_EXTREME"), primary->participant_key);

    CftcInterpretationState h1 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 1);
    add_metric(h1, QStringLiteral("net_flow"), 1.0);
    primary->states << h1;
    CftcInterpretationState h4 = make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);
    add_metric(h4, QStringLiteral("net_flow"), -1.0);
    primary->states << h4;
    CftcInterpretationState h13 = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 13);
    add_metric(h13, QStringLiteral("net_flow"), 2.0);
    primary->states << h13;

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);

    // The crowding/persistence state and the trailing percentile are computed
    // from the full 156-prior-report history, so switching 1W | 4W | 13W cannot
    // change them.
    QCOMPARE(one.headline, four.headline);
    QCOMPARE(four.headline, thirteen.headline);
    QVERIFY(one.headline.contains(QStringLiteral("historically crowded long")));
    QVERIFY(one.headline.contains(QStringLiteral("extreme persisting")));
    QCOMPARE(one.sentences.first(), four.sentences.first());
    QCOMPARE(four.sentences.first(), thirteen.sentences.first());

    const QString percentile_label = cftc_interpretation_percentile_label();
    auto percentile_value = [&percentile_label](const CftcInterpretationView& view) {
        for (const auto& item : view.evidence) {
            if (item.label == percentile_label)
                return item.value;
        }
        return QString();
    };
    QCOMPARE(percentile_value(one), QStringLiteral("95.0% (n=156)"));
    QCOMPARE(percentile_value(four), percentile_value(one));
    QCOMPARE(percentile_value(thirteen), percentile_value(one));
}

void TstCftcPresentation::horizon_view_distinguishes_evaluated_not_material_from_unavailable() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::BrokenReportSequence, 4);

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);

    QVERIFY(join_sentences(one).contains(QStringLiteral("was evaluated but did not cross the materiality threshold")));
    QVERIFY(join_sentences(four).contains(QStringLiteral("weekly report sequence is broken")));
    QVERIFY(!join_sentences(four).contains(QStringLiteral("no material state")));

    auto row = [](const CftcInterpretationView& view, const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* one_long = row(one, QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(one_long != nullptr);
    QCOMPARE(one_long->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(one_long->value, QStringLiteral("evaluated, below the materiality threshold"));

    const CftcEvidenceItem* four_long = row(four, QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(four_long != nullptr);
    QCOMPARE(four_long->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(four_long->value.contains(QStringLiteral("unavailable")));
    QVERIFY(four_long->value.contains(QStringLiteral("weekly report sequence is broken")));

    const CftcEvidenceItem* one_oi = row(one, QStringLiteral("Open Interest change (% change) (one report)"));
    QVERIFY(one_oi != nullptr);
    QCOMPARE(one_oi->status, CftcEvidenceStatus::NoMaterialState);
}

void TstCftcPresentation::horizon_view_carries_concrete_unavailable_reasons() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 13);

    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);
    QVERIFY(join_sentences(thirteen).contains(QStringLiteral("fewer than 156 prior reports")));
    const CftcEvidenceItem* net_row = nullptr;
    for (const auto& item : thirteen.evidence) {
        if (item.label == QStringLiteral("Net flow (% of prior OI)"))
            net_row = &item;
    }
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(net_row->value.contains(QStringLiteral("fewer than 156 prior reports")));

    // The same participant at the four-report horizon is evaluated, so its
    // non-material statement is not confused with the thirteen-report reason.
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    QVERIFY(join_sentences(four).contains(QStringLiteral("was evaluated but did not cross the materiality threshold")));
}

void TstCftcPresentation::horizon_view_price_pending_versus_failed() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    CftcInterpretationState net_state = make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    add_metric(net_state, QStringLiteral("net_flow"), 4.0);
    primary->states << net_state;
    add_missing_price_assessment(result, primary->participant_key, 4);

    const CftcInterpretationView pending =
        cftc_compose_horizon_interpretation(result, 4, CftcPriceContextState::Pending);
    QVERIFY(join_sentences(pending).contains(QStringLiteral("Price context is still loading")));
    QVERIFY(join_sentences(pending).contains(QStringLiteral("shifted longward over the last four reports")));

    const QString failure = QStringLiteral("the Yahoo Finance history request failed");
    const CftcInterpretationView failed =
        cftc_compose_horizon_interpretation(result, 4, CftcPriceContextState::Unavailable, failure);
    QVERIFY(join_sentences(failed).contains(failure));
    QVERIFY(!join_sentences(failed).contains(QStringLiteral("still loading")));
    QVERIFY(join_sentences(failed).contains(QStringLiteral("shifted longward over the last four reports")));

    auto row = [](const CftcInterpretationView& view, const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label.startsWith(label))
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* pending_price = row(pending, QStringLiteral("Price change"));
    QVERIFY(pending_price != nullptr);
    QCOMPARE(pending_price->status, CftcEvidenceStatus::Pending);
    QVERIFY(pending_price->value.contains(QStringLiteral("pending")));

    const CftcEvidenceItem* failed_price = row(failed, QStringLiteral("Price change"));
    QVERIFY(failed_price != nullptr);
    QCOMPARE(failed_price->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(failed_price->value.contains(failure));

    const CftcEvidenceItem* failed_relationship = row(failed, QStringLiteral("Price / positioning relationship"));
    QVERIFY(failed_relationship != nullptr);
    QCOMPARE(failed_relationship->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(failed_relationship->value.contains(failure));
}

void TstCftcPresentation::horizon_view_open_interest_context() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcInterpretationState oi_state = make_state(QStringLiteral("OI_EXPANSION"), QString(), 4);
    add_metric(oi_state, QStringLiteral("oi_change"), 7.5);
    result.market_context << oi_state;
    add_unavailable(result, QStringLiteral("OI_CONTEXT"), QString(), CftcUnavailableReason::NonPositiveOpenInterest,
                    13);

    const CftcInterpretationView one = cftc_compose_horizon_interpretation(result, 1);
    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    const CftcInterpretationView thirteen = cftc_compose_horizon_interpretation(result, 13);

    QVERIFY(join_sentences(four).contains(QStringLiteral("Open Interest expanded over the last four reports")));
    QVERIFY(join_sentences(one).contains(
        QStringLiteral("Open Interest change over the last one report was evaluated but did not cross the "
                       "materiality threshold")));
    QVERIFY(join_sentences(thirteen).contains(QStringLiteral("Open Interest is missing or non-positive")));
    QVERIFY(!join_sentences(thirteen).contains(QStringLiteral("no material state")));

    auto oi_row = [](const CftcInterpretationView& view) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label.startsWith(QStringLiteral("Open Interest change")))
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* four_oi = oi_row(four);
    QVERIFY(four_oi != nullptr);
    QCOMPARE(four_oi->status, CftcEvidenceStatus::Available);
    QCOMPARE(four_oi->value, QStringLiteral("+7.50%"));
    const CftcEvidenceItem* one_oi = oi_row(one);
    QVERIFY(one_oi != nullptr);
    QCOMPARE(one_oi->status, CftcEvidenceStatus::NoMaterialState);
    const CftcEvidenceItem* thirteen_oi = oi_row(thirteen);
    QVERIFY(thirteen_oi != nullptr);
    QCOMPARE(thirteen_oi->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(thirteen_oi->value.contains(QStringLiteral("Open Interest is missing or non-positive")));
}

void TstCftcPresentation::horizon_view_evidence_is_tagged_with_the_selected_horizon() {
    CftcInterpretationResult result = make_result(CftcFamily::Tff);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView four = cftc_compose_horizon_interpretation(result, 4);
    bool saw_flow_row = false;
    for (const auto& item : four.evidence) {
        if (item.label == QStringLiteral("Long leg flow (% of prior OI)") ||
            item.label == QStringLiteral("Net flow (% of prior OI)")) {
            saw_flow_row = true;
            QCOMPARE(item.horizon_reports, 4);
        }
    }
    QVERIFY(saw_flow_row);
}

void TstCftcPresentation::unsupported_horizon_falls_back_to_combined_view() {
    CftcInterpretationResult result = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView combined = cftc_compose_interpretation(result);
    const CftcInterpretationView invalid = cftc_compose_horizon_interpretation(result, 2);
    QCOMPARE(invalid.horizon_reports, 0);
    QCOMPARE(invalid.headline, combined.headline);
    QCOMPARE(invalid.sentences, combined.sentences);
    QCOMPARE(invalid.evidence.size(), combined.evidence.size());
}

void TstCftcPresentation::neutral_legacy_metric_label_has_no_speculator_wording() {
    const QString neutral = cftc_metric_participant_display_name(CftcFamily::Legacy, QStringLiteral("non_commercial"),
                                                                 QStringLiteral("Non-Commercial (Speculators)"));
    QCOMPARE(neutral, QStringLiteral("Non-Commercial"));
    QVERIFY(!neutral.contains(QStringLiteral("Speculator"), Qt::CaseInsensitive));

    QCOMPARE(cftc_metric_participant_display_name(CftcFamily::Legacy, QStringLiteral("commercial"),
                                                  QStringLiteral("Commercial")),
             QStringLiteral("Commercial"));
    QCOMPARE(cftc_metric_participant_display_name(CftcFamily::Disaggregated, QStringLiteral("managed_money"),
                                                  QStringLiteral("Managed Money")),
             QStringLiteral("Managed Money"));
    QCOMPARE(cftc_metric_participant_display_name(CftcFamily::Tff, QStringLiteral("leveraged_funds"),
                                                  QStringLiteral("Leveraged Funds")),
             QStringLiteral("Leveraged Funds"));
}

void TstCftcPresentation::horizon_views_keep_the_forbidden_language_audit() {
    CftcFamily families[] = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    for (CftcFamily family : families) {
        CftcInterpretationResult result = make_result(family);
        CftcParticipantInterpretation* primary = primary_participant(result);
        primary->states << make_state(QStringLiteral("CROWDED_LONG"), primary->participant_key);
        primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
        CftcInterpretationState net_state =
            make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
        net_state.mechanism_state_ids = {QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("SHORT_COVERING")};
        add_metric(net_state, QStringLiteral("net_flow"), 5.0);
        primary->states << net_state;
        CftcInterpretationState accum = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
        add_metric(accum, QStringLiteral("move_value"), 5.0);
        primary->states << accum;
        add_price_assessment(result, primary->participant_key, 4,
                             QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"),
                             {QStringLiteral("LONG_ACCUMULATION")}, -50.0, 2000.0);

        for (int horizon : {1, 4, 13}) {
            const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, horizon);
            QVERIFY2(forbidden_language(conclusions_text(view)).isEmpty(),
                     qPrintable(forbidden_language(conclusions_text(view))));
        }
    }
}

void TstCftcPresentation::leg_evidence_uses_the_leg_own_record() {
    // The short leg is missing while the long leg is evaluated and material:
    // the long row must stay available and must not inherit the short leg's
    // missing-data reason.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    CftcInterpretationState long_state = make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    add_metric(long_state, QStringLiteral("move_value"), 5.0);
    primary->states << long_state;
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::MissingParticipantLeg, 4);
    add_unavailable(result, QStringLiteral("GROSS_FLOW"), primary->participant_key,
                    CftcUnavailableReason::MissingParticipantLeg, 4);

    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    auto row = [&view](const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* long_row = row(QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(long_row->value, QStringLiteral("5.00%"));
    const CftcEvidenceItem* short_row = row(QStringLiteral("Short leg flow (% of prior OI)"));
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(short_row->value.contains(QStringLiteral("long or short leg is missing")));

    // A leg whose direction is known but whose rank reference is too short is
    // unavailable with that reason, not evaluated-not-material.
    CftcInterpretationResult short_ref = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* short_primary = primary_participant(short_ref);
    short_primary->states << make_state(QStringLiteral("NET_LONG"), short_primary->participant_key);
    add_unavailable(short_ref, QStringLiteral("NET_SHIFT"), short_primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4);
    add_unavailable(short_ref, QStringLiteral("GROSS_FLOW"), short_primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4, QStringLiteral("LONG_ACCUMULATION"));
    const CftcInterpretationView short_ref_view = cftc_compose_horizon_interpretation(short_ref, 4);
    const CftcEvidenceItem* ref_long = nullptr;
    const CftcEvidenceItem* ref_short = nullptr;
    for (const auto& item : short_ref_view.evidence) {
        if (item.label == QStringLiteral("Long leg flow (% of prior OI)"))
            ref_long = &item;
        if (item.label == QStringLiteral("Short leg flow (% of prior OI)"))
            ref_short = &item;
    }
    QVERIFY(ref_long != nullptr);
    QCOMPARE(ref_long->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(ref_long->value.contains(QStringLiteral("fewer than 156 prior reports")));
    // The short leg has no record of its own, so the horizon-level reason is
    // the truthful fallback rather than a claimed evaluation.
    QVERIFY(ref_short != nullptr);
    QCOMPARE(ref_short->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(ref_short->value.contains(QStringLiteral("fewer than 156 prior reports")));
}

void TstCftcPresentation::non_material_open_interest_evidence_is_explicit() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4);
    const CftcEvidenceItem* oi_row = nullptr;
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Open Interest change")))
            oi_row = &item;
    }
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(oi_row->value, QStringLiteral("evaluated, below the materiality threshold"));
}

void TstCftcPresentation::price_evidence_keeps_quoted_units_precision() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    add_price_assessment(result, primary->participant_key, 4, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"), {},
                         -87.8002929688, -2.0);

    const CftcInterpretationView view = cftc_compose_horizon_interpretation(result, 4, CftcPriceContextState::Ready);
    const CftcEvidenceItem* price_row = nullptr;
    for (const auto& item : view.evidence) {
        if (item.label.startsWith(QStringLiteral("Price change")))
            price_row = &item;
    }
    QVERIFY(price_row != nullptr);
    QCOMPARE(price_row->status, CftcEvidenceStatus::Available);
    QCOMPARE(price_row->value, QStringLiteral("-87.80"));

    // The combined contract keeps the same quoted-price precision.
    const CftcInterpretationView combined = cftc_compose_interpretation(result, CftcPriceContextState::Ready);
    const CftcEvidenceItem* combined_price = nullptr;
    for (const auto& item : combined.evidence) {
        if (item.label.startsWith(QStringLiteral("Price change")))
            combined_price = &item;
    }
    QVERIFY(combined_price != nullptr);
    QCOMPARE(combined_price->value, QStringLiteral("-87.80"));
}

void TstCftcPresentation::horizon_evidence_uses_emitted_readings() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcHorizonFlowReading reading;
    reading.horizon_reports = 4;
    reading.evaluated = true;
    reading.has_long_flow = true;
    reading.long_flow = 0.2848;
    reading.has_short_flow = true;
    reading.short_flow = -1.7211;
    reading.has_net_flow = true;
    reading.net_flow = 2.0059;
    reading.has_long_rank = true;
    reading.long_rank = 0.25;
    reading.has_short_rank = true;
    reading.short_rank = 0.25;
    reading.has_net_rank = true;
    reading.net_rank = 0.25;
    reading.net_rank_reference_count = 156;
    primary->flow_readings << reading;

    CftcOpenInterestReading oi;
    oi.horizon_reports = 4;
    oi.evaluated = true;
    oi.has_oi_change = true;
    oi.oi_change = 0.8957;
    oi.has_rank = true;
    oi.rank = 0.30;
    oi.rank_reference_count = 156;
    result.open_interest_readings << oi;

    // Price context failed: the CFTC-only measurements must still be shown.
    const CftcInterpretationView view = cftc_compose_horizon_interpretation(
        result, 4, CftcPriceContextState::Unavailable, QStringLiteral("provider failed"));

    auto row = [&view](const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };
    const CftcEvidenceItem* long_row = row(QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(long_row->value, QStringLiteral("0.28% (below threshold)"));
    const CftcEvidenceItem* short_row = row(QStringLiteral("Short leg flow (% of prior OI)"));
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->value, QStringLiteral("-1.72% (below threshold)"));
    const CftcEvidenceItem* net_row = row(QStringLiteral("Net flow (% of prior OI)"));
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(net_row->value, QStringLiteral("+2.01% (below threshold)"));
    const CftcEvidenceItem* rank_row = row(QStringLiteral("Move materiality rank (% of prior moves)"));
    QVERIFY(rank_row != nullptr);
    QCOMPARE(rank_row->value, QStringLiteral("25.0% (n=156) (below threshold)"));
    const CftcEvidenceItem* oi_row = row(QStringLiteral("Open Interest change (% change) (four reports)"));
    QVERIFY(oi_row != nullptr);
    QCOMPARE(oi_row->status, CftcEvidenceStatus::NoMaterialState);
    QCOMPARE(oi_row->value, QStringLiteral("+0.90% (below threshold)"));
}

void TstCftcPresentation::raw_reading_without_materiality_reference_is_not_below_threshold() {
    // The raw 4R flows exist but the strictly trailing reference is too short,
    // so no materiality comparison was performed. The rows keep the values and
    // must state that the materiality rank is unavailable; they must not claim
    // the move was compared with the threshold and found below it.
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);

    CftcHorizonFlowReading reading;
    reading.horizon_reports = 4;
    reading.evaluated = true;
    reading.has_long_flow = true;
    reading.long_flow = 0.2848;
    reading.has_short_flow = true;
    reading.short_flow = -1.7211;
    reading.has_net_flow = true;
    reading.net_flow = 2.0059;
    reading.has_long_rank = false;
    reading.has_short_rank = false;
    reading.has_net_rank = false;
    primary->flow_readings << reading;
    add_unavailable(result, QStringLiteral("GROSS_FLOW"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4, QStringLiteral("LONG_ACCUMULATION"));
    add_unavailable(result, QStringLiteral("GROSS_FLOW"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4, QStringLiteral("SHORT_COVERING"));
    add_unavailable(result, QStringLiteral("NET_SHIFT"), primary->participant_key,
                    CftcUnavailableReason::InsufficientHistory, 4);

    const QString reason = QStringLiteral("fewer than 156 prior reports are available for this reference");
    auto row = [](const CftcInterpretationView& view, const QString& label) -> const CftcEvidenceItem* {
        for (const auto& item : view.evidence) {
            if (item.label == label)
                return &item;
        }
        return nullptr;
    };

    const CftcInterpretationView horizon_view = cftc_compose_horizon_interpretation(result, 4);
    const CftcEvidenceItem* long_row = row(horizon_view, QStringLiteral("Long leg flow (% of prior OI)"));
    QVERIFY(long_row != nullptr);
    QCOMPARE(long_row->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(long_row->value, QStringLiteral("0.28% (materiality unavailable — ") + reason + QLatin1Char(')'));
    QVERIFY(!long_row->value.contains(QStringLiteral("below threshold")));
    const CftcEvidenceItem* short_row = row(horizon_view, QStringLiteral("Short leg flow (% of prior OI)"));
    QVERIFY(short_row != nullptr);
    QCOMPARE(short_row->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(short_row->value, QStringLiteral("-1.72% (materiality unavailable — ") + reason + QLatin1Char(')'));
    const CftcEvidenceItem* net_row = row(horizon_view, QStringLiteral("Net flow (% of prior OI)"));
    QVERIFY(net_row != nullptr);
    QCOMPARE(net_row->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(net_row->value, QStringLiteral("+2.01% (materiality unavailable — ") + reason + QLatin1Char(')'));
    QVERIFY(!net_row->value.contains(QStringLiteral("below threshold")));
    const CftcEvidenceItem* rank_row = row(horizon_view, QStringLiteral("Move materiality rank (% of prior moves)"));
    QVERIFY(rank_row != nullptr);
    QCOMPARE(rank_row->status, CftcEvidenceStatus::Unavailable);
    QVERIFY(rank_row->value.contains(reason));
    QVERIFY(!rank_row->value.contains(QStringLiteral("no material state")));

    // The combined contract view carries the same truthful wording.
    const CftcInterpretationView combined = cftc_compose_interpretation(result);
    const CftcEvidenceItem* combined_net = row(combined, QStringLiteral("Net flow (% of prior OI)"));
    QVERIFY(combined_net != nullptr);
    QCOMPARE(combined_net->status, CftcEvidenceStatus::Unavailable);
    QCOMPARE(combined_net->value, QStringLiteral("+2.01% (materiality unavailable — ") + reason + QLatin1Char(')'));
}

void TstCftcPresentation::net_delta_reason_never_comes_from_price() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    const QString key = expected_principal_key(CftcFamily::Legacy);
    // The CFTC net measurement is unavailable because the report sequence is
    // broken while the price assessment also reports a missing price context:
    // the positioning cell reason must come from the CFTC path.
    add_unavailable(result, QStringLiteral("NET_SHIFT"), key, CftcUnavailableReason::BrokenReportSequence, 4);
    CftcPricePositionAssessment assessment;
    assessment.participant_key = key;
    assessment.horizon_reports = 4;
    assessment.reason = CftcUnavailableReason::MissingPriceContext;
    result.price_context.append(assessment);
    QCOMPARE(cftc_net_delta_unavailable_reason(result, key, 4), QStringLiteral("the weekly report sequence is broken"));
    QVERIFY(!cftc_net_delta_unavailable_reason(result, key, 4).contains(QStringLiteral("price")));

    // Without a NET_SHIFT record the participant's own horizon reading carries
    // the reason; with no CFTC record at all the helper stays empty instead of
    // borrowing the price wording.
    CftcInterpretationResult fallback = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(fallback);
    QCOMPARE(cftc_net_delta_unavailable_reason(fallback, key, 13), QString());
    CftcHorizonFlowReading reading;
    reading.horizon_reports = 13;
    reading.evaluated = false;
    reading.reason = CftcUnavailableReason::BrokenReportSequence;
    primary->flow_readings << reading;
    QCOMPARE(cftc_net_delta_unavailable_reason(fallback, key, 13),
             QStringLiteral("the weekly report sequence is broken"));
}

QTEST_GUILESS_MAIN(TstCftcPresentation)
#include "tst_cftc_presentation.moc"
