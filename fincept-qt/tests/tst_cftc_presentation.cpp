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

QString primary_key(CftcFamily family) {
    const auto participants = cftc_family_participants(family);
    const int index = cftc_speculative_index(participants);
    return index >= 0 ? participants[index].key : QString();
}

CftcParticipantInterpretation* primary_participant(CftcInterpretationResult& result) {
    const QString key = primary_key(result.family);
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
    void insufficient_history_does_not_suppress_direct_states();
    void visible_range_does_not_change_interpretation();
    void deterministic_repeatability();
    void vocabulary_audit();
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
    QVERIFY(!text.contains(QStringLiteral("speculat"), Qt::CaseInsensitive));
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
    QVERIFY(relationship->available);
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

    const CftcInterpretationView pending = cftc_compose_interpretation(result, /*price_pending=*/true);
    QVERIFY(pending.headline.contains(QStringLiteral("net long")));
    QVERIFY(join_sentences(pending).contains(QStringLiteral("Price context is still loading")));
    QVERIFY(!join_sentences(pending).contains(QStringLiteral("Price relationship unavailable")));
}

void TstCftcPresentation::concrete_price_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(result);
    primary->states << make_state(QStringLiteral("NET_LONG"), primary->participant_key);
    for (int horizon : {1, 4, 13})
        add_missing_price_assessment(result, primary->participant_key, horizon);

    const CftcInterpretationView view =
        cftc_compose_interpretation(result, false, QStringLiteral("the Yahoo Finance history request failed"));
    QVERIFY(join_sentences(view).contains(
        QStringLiteral("Price relationship unavailable: the Yahoo Finance history request failed.")));
    QVERIFY(!join_sentences(view).contains(QStringLiteral("no price observations were supplied")));
    QVERIFY(view.headline.contains(QStringLiteral("net long")));
}

void TstCftcPresentation::participant_specific_failure_reason_is_reported() {
    CftcInterpretationResult result = make_result(CftcFamily::Legacy);
    const QString key = primary_key(CftcFamily::Legacy);
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
    const QString key = primary_key(CftcFamily::Legacy);
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
        if (item.label.contains(QStringLiteral("percentile")) && !item.available)
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
    QVERIFY(full_view.headline.contains(QStringLiteral("crowded long")));
    QVERIFY(!window_view.headline.contains(QStringLiteral("crowded long")));
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
        QCOMPARE(first.evidence[i].available, second.evidence[i].available);
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

QTEST_GUILESS_MAIN(TstCftcPresentation)
#include "tst_cftc_presentation.moc"
