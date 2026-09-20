// tests/tst_cftc_research_state.cpp
//
// Batch 2 of the CFTC research engine: the bounded evidence evaluator and the
// provisional v0 COT Research State (services/economics/CftcResearchState.h).
//
// Every fixture is synthetic so each v0 rule can be pinned independently:
// horizon states, conflicts, crowded-context continuation vs reversal,
// return-from-extreme, price/COT alignment and divergence, missing price/OI/
// history, stale series, participant semantics per report family, independence
// against correlated double counting, confidence, explanations and rule-set
// versioning. Header-only over Qt Core; no app sources (tests/ HARD RULE).
#include "services/economics/CftcResearchState.h"

#include <QSet>
#include <QtTest>

using namespace fincept::services;

namespace {

QDate latest_report() {
    return QDate(2026, 9, 29);
}

QVector<std::optional<double>> legs(double net, double base = 100.0) {
    return QVector<std::optional<double>>{base + std::max(0.0, net), base + std::max(0.0, -net)};
}

CftcObservation observation(const QDate& date, const std::optional<double>& oi,
                            const QVector<std::optional<double>>& longs, const QVector<std::optional<double>>& shorts) {
    CftcObservation obs;
    obs.date = date;
    obs.date_label = date.toString(Qt::ISODate);
    obs.market = QStringLiteral("TEST - EXCHANGE");
    obs.open_interest = oi;
    obs.longs = longs;
    obs.shorts = shorts;
    return obs;
}

CftcObservation legacy_observation(const QDate& date, double spec_net, const std::optional<double>& oi,
                                   double non_reportable_net = 0.0, double commercial_net = 0.0) {
    const auto spec = legs(spec_net, 1000.0);
    const auto non_reportable = legs(non_reportable_net);
    const auto commercial = legs(commercial_net);
    return observation(date, oi, {commercial[0], spec[0], non_reportable[0]},
                       {commercial[1], spec[1], non_reportable[1]});
}

CftcObservation disaggregated_observation(const QDate& date, double managed_money_net, const std::optional<double>& oi,
                                          double other_reportable_net = 0.0, double producer_net = 0.0,
                                          double swap_net = 0.0, double non_reportable_net = 0.0) {
    const auto producer = legs(producer_net);
    const auto swap = legs(swap_net);
    const auto managed = legs(managed_money_net, 1000.0);
    const auto other = legs(other_reportable_net);
    const auto non_reportable = legs(non_reportable_net);
    return observation(date, oi, {producer[0], swap[0], managed[0], other[0], non_reportable[0]},
                       {producer[1], swap[1], managed[1], other[1], non_reportable[1]});
}

CftcObservation tff_observation(const QDate& date, double leveraged_funds_net, const std::optional<double>& oi,
                                double asset_manager_net = 0.0, double other_reportable_net = 0.0,
                                double dealer_net = 0.0, double non_reportable_net = 0.0) {
    const auto dealer = legs(dealer_net);
    const auto asset_manager = legs(asset_manager_net);
    const auto leveraged = legs(leveraged_funds_net, 1000.0);
    const auto other = legs(other_reportable_net);
    const auto non_reportable = legs(non_reportable_net);
    return observation(date, oi, {dealer[0], asset_manager[0], leveraged[0], other[0], non_reportable[0]},
                       {dealer[1], asset_manager[1], leveraged[1], other[1], non_reportable[1]});
}

QVector<CftcObservation> legacy_series(const QVector<double>& spec_nets, const std::optional<double>& oi = 1000.0,
                                       const QVector<double>& non_reportable_nets = {},
                                       const QVector<double>& commercial_nets = {}) {
    QVector<CftcObservation> out;
    const int count = spec_nets.size();
    for (int i = 0; i < count; ++i) {
        out.append(legacy_observation(latest_report().addDays(-7LL * (count - 1 - i)), spec_nets[i], oi,
                                      i < non_reportable_nets.size() ? non_reportable_nets[i] : 0.0,
                                      i < commercial_nets.size() ? commercial_nets[i] : 0.0));
    }
    return out;
}

/// Reports at explicit day offsets from the latest report (oldest first), so a
/// missing weekly report can be represented exactly.
QVector<CftcObservation> legacy_series_at_offsets(const QVector<int>& offsets, const QVector<double>& values,
                                                  const std::optional<double>& oi = 1000.0) {
    QVector<CftcObservation> out;
    for (int i = 0; i < offsets.size(); ++i)
        out.append(legacy_observation(latest_report().addDays(-offsets[i]), values[i], oi));
    return out;
}

void strip_participant_legs(QVector<CftcObservation>& observations, int index) {
    for (auto& obs : observations) {
        if (index >= 0 && index < obs.longs.size())
            obs.longs[index] = std::nullopt;
        if (index >= 0 && index < obs.shorts.size())
            obs.shorts[index] = std::nullopt;
    }
}

QVector<CftcObservation> disaggregated_series(const QVector<double>& managed_money_nets,
                                              const std::optional<double>& oi = 1000.0,
                                              const QVector<double>& other_reportable_nets = {}) {
    QVector<CftcObservation> out;
    const int count = managed_money_nets.size();
    for (int i = 0; i < count; ++i) {
        out.append(disaggregated_observation(latest_report().addDays(-7LL * (count - 1 - i)), managed_money_nets[i], oi,
                                             i < other_reportable_nets.size() ? other_reportable_nets[i] : 0.0));
    }
    return out;
}

QVector<CftcObservation> tff_series(const QVector<double>& leveraged_funds_nets,
                                    const std::optional<double>& oi = 1000.0,
                                    const QVector<double>& asset_manager_nets = {},
                                    const QVector<double>& other_reportable_nets = {}) {
    QVector<CftcObservation> out;
    const int count = leveraged_funds_nets.size();
    for (int i = 0; i < count; ++i) {
        out.append(tff_observation(latest_report().addDays(-7LL * (count - 1 - i)), leveraged_funds_nets[i], oi,
                                   i < asset_manager_nets.size() ? asset_manager_nets[i] : 0.0,
                                   i < other_reportable_nets.size() ? other_reportable_nets[i] : 0.0));
    }
    return out;
}

QVector<CftcPricePoint> price_series(const QVector<double>& closes) {
    QVector<CftcPricePoint> out;
    const int count = closes.size();
    for (int i = 0; i < count; ++i)
        out.append({latest_report().addDays(-7LL * (count - 1 - i)), closes[i]});
    return out;
}

QVector<double> repeat(double value, int count) {
    QVector<double> out;
    for (int i = 0; i < count; ++i)
        out.append(value);
    return out;
}

QVector<double> ramp(double first, int count, double step = 1.0) {
    QVector<double> out;
    for (int i = 0; i < count; ++i)
        out.append(first + step * static_cast<double>(i));
    return out;
}

const CftcEvidenceItem* find_item(const CftcResearchResult& result, const QString& rule_id) {
    for (const auto& group : result.groups) {
        for (const auto& item : group.items) {
            if (item.rule_id == rule_id)
                return &item;
        }
    }
    return nullptr;
}

const CftcEvidenceGroup* find_group(const CftcResearchResult& result, CftcEvidenceFamily family) {
    for (const auto& group : result.groups) {
        if (group.family == family)
            return &group;
    }
    return nullptr;
}

int directional_item_count(const QVector<CftcEvidenceItem>& items) {
    int count = 0;
    for (const auto& item : items) {
        if (item.direction == CftcEvidenceDirection::Bullish || item.direction == CftcEvidenceDirection::Bearish)
            ++count;
    }
    return count;
}

bool list_has_rule(const QVector<CftcEvidenceItem>& items, const QString& rule_id) {
    for (const auto& item : items) {
        if (item.rule_id == rule_id)
            return true;
    }
    return false;
}

/// A historically crowded long series whose recent 4W Net is falling while the
/// 13W change is flat: still above 90% of the trailing 2Y reference, so the
/// only directional historical evidence is the genuine recent reversal.
QVector<double> crowded_long_reversal_values() {
    QVector<double> values = repeat(10.0, 96);
    values += QVector<double>{94, 95, 96, 97, 98, 97, 96, 95, 94, 97, 96, 95, 94, 94};
    return values;
}

QVector<double> crowded_short_reversal_values() {
    QVector<double> values = repeat(-10.0, 96);
    values += QVector<double>{-94, -95, -96, -97, -98, -97, -96, -95, -94, -97, -96, -95, -94, -94};
    return values;
}

/// Background noise, then a 48-report plateau at 100, then `current`. Used for
/// return-from-extreme (90 / 10) and persistent-extreme-without-reversal (105)
/// fixtures.
QVector<double> plateau_values(double current) {
    QVector<double> values = repeat(10.0, 61);
    values += repeat(100.0, 48);
    values.append(current);
    return values;
}

QVector<double> lower_plateau_values(double current) {
    QVector<double> values = repeat(90.0, 61);
    values += repeat(0.0, 48);
    values.append(current);
    return values;
}

} // namespace

class TstCftcResearchState : public QObject {
    Q_OBJECT
  private slots:
    // Directional core and conflicts
    void clear_4w_bullish_case_stays_hold_alone();
    void clear_4w_bearish_case_stays_hold_alone();
    void clear_13w_bullish_case();
    void clear_13w_bearish_case();
    void neutral_recent_positioning();
    void four_week_bullish_with_13w_bearish_conflict();
    void four_week_bearish_with_13w_bullish_conflict();
    void gapped_trend_readings_do_not_vote();

    // Historical context and return-from-extreme
    void crowded_long_with_continued_strengthening_is_not_sell();
    void crowded_long_with_recent_reversal_is_bearish();
    void historical_reversal_alone_does_not_create_direction();
    void crowded_short_with_continued_weakening_is_not_buy();
    void crowded_short_with_recent_reversal_is_bullish();
    void neutral_context_with_coherent_recent_direction();
    void zero_variance_reference_is_not_crowded();
    void mixed_historical_normalization_is_exposed();
    void percentile_cot_index_zscore_do_not_triple_vote();
    void return_from_upper_extreme_is_bearish();
    void return_from_lower_extreme_is_bullish();
    void persistent_extreme_without_reversal_is_context_only();

    // Independence and missing evidence
    void one_family_cannot_satisfy_independence();
    void insufficient_independent_evidence_produces_hold();
    void missing_price_is_unavailable_not_neutral();
    void stale_price_series_is_unavailable();
    void missing_open_interest_stays_unavailable();
    void missing_historical_reference_stays_unavailable();
    void stale_speculative_series_does_not_use_prior_report();
    void requested_as_of_before_history_end_reports_truthfully();
    void anchor_missing_net_reports_truthfully();
    void unavailable_data_stays_unavailable();

    // Price / OI families
    void price_cot_alignment_confirms_and_raises_coverage();
    void price_cot_opposition_is_visible_conflict();

    // Participant semantics
    void participant_confirmation_supports_direction();
    void participant_conflict_stays_visible();
    void participant_context_only_does_not_count_as_confirmation();
    void legacy_participant_semantics();
    void disaggregated_participant_semantics();
    void tff_participant_semantics_without_commercial_signal();

    // Context, confidence, structure
    void regime_26w_is_context_not_a_vote();
    void explanations_expose_values_and_horizons();
    void confidence_penalizes_conflicts_once_per_family();
    void deterministic_repeatability();
    void exact_rule_set_version_and_rule_table();
    void publication_effective_date_handling();
};

// ── Directional core and conflicts ──────────────────────────────────────────

void TstCftcResearchState::clear_4w_bullish_case_stays_hold_alone() {
    // A strong 4W rise on a history too short for the 13W anchor: the tactical
    // family is clearly bullish, but one family cannot satisfy the BUY
    // independence requirement.
    const QVector<double> values = {0, 0, 0, 0, 0, 0, 100, 200, 300, 400};
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(result.data_available);
    QVERIFY(!result.data_stale);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Unavailable);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QVERIFY2(result.confidence != CftcResearchConfidence::High,
             "a single evidence family cannot reach High confidence");
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
    const CftcEvidenceItem* position = find_item(result, QStringLiteral("R4W-NET-CHANGE"));
    QVERIFY(position);
    QVERIFY(position->available);
    QCOMPARE(position->direction, CftcEvidenceDirection::Bullish);
    QVERIFY(position->explanation.contains(QStringLiteral("Net change 400")));
}

void TstCftcResearchState::clear_4w_bearish_case_stays_hold_alone() {
    const QVector<double> values = {0, 0, 0, 0, 0, 0, -100, -200, -300, -400};
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Bearish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Unavailable);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
    const CftcEvidenceItem* position = find_item(result, QStringLiteral("R4W-NET-CHANGE"));
    QVERIFY(position);
    QCOMPARE(position->direction, CftcEvidenceDirection::Bearish);
}

void TstCftcResearchState::clear_13w_bullish_case() {
    // The rise happened 13 weeks ago and has been flat since: the 4W layer is
    // neutral but the 13W layer describes sustained accumulation.
    QVector<double> values = ramp(100.0, 13, 100.0);
    values += repeat(1300.0, 7);
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Neutral);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
    const CftcEvidenceItem* swing = find_item(result, QStringLiteral("R13W-NET-CHANGE"));
    QVERIFY(swing);
    QCOMPARE(swing->direction, CftcEvidenceDirection::Bullish);
}

void TstCftcResearchState::clear_13w_bearish_case() {
    QVector<double> values = ramp(1300.0, 13, -100.0);
    values += repeat(100.0, 7);
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Neutral);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bearish);
    QCOMPARE(result.state, CftcResearchState::Hold);
}

void TstCftcResearchState::neutral_recent_positioning() {
    CftcResearchInput input;
    input.observations = legacy_series(repeat(500.0, 20));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Neutral);
    QCOMPARE(result.swing_13w, CftcTacticalState::Neutral);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QVERIFY(result.conflicting.isEmpty());
}

void TstCftcResearchState::four_week_bullish_with_13w_bearish_conflict() {
    QVector<double> values = ramp(1400.0, 14, -100.0);
    values += QVector<double>{100, 150, 200, 250, 300, 400};
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bearish);
    QCOMPARE(result.state, CftcResearchState::Hold);
    bool has_bullish = false;
    bool has_bearish = false;
    for (const auto& item : result.conflicting) {
        has_bullish = has_bullish || item.direction == CftcEvidenceDirection::Bullish;
        has_bearish = has_bearish || item.direction == CftcEvidenceDirection::Bearish;
    }
    QVERIFY2(has_bullish && has_bearish, "both sides of a core conflict must remain visible");
}

void TstCftcResearchState::four_week_bearish_with_13w_bullish_conflict() {
    QVector<double> values = ramp(-1400.0, 14, 100.0);
    values += QVector<double>{-100, -150, -200, -250, -300, -400};
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Bearish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Hold);
}

void TstCftcResearchState::gapped_trend_readings_do_not_vote() {
    // 4W: a missing report makes the 4-report mean windows gapped while the
    // values after the gap jump 0 -> 100. If the gapped readings voted, the
    // trend would be +2 and the tactical state Bullish; they must be excluded.
    CftcResearchInput four_week;
    four_week.observations = legacy_series_at_offsets({63, 56, 49, 42, 14, 7, 0}, {0, 0, 0, 0, 0, 100, 100});
    const CftcResearchResult four = cftc_evaluate_research_state(four_week);
    const CftcEvidenceItem* trend_4w = find_item(four, QStringLiteral("R4W-TREND"));
    QVERIFY(trend_4w);
    QVERIFY2(!trend_4w->available, "a gapped trend window must not be an ordinary voting reading");
    QVERIFY(trend_4w->explanation.contains(QStringLiteral("spans a missing report")));
    bool has_gapped_metric = false;
    for (const auto& metric : trend_4w->metrics) {
        if (metric.key == QStringLiteral("window_gapped"))
            has_gapped_metric = metric.has_value && metric.value == 1.0;
    }
    QVERIFY(has_gapped_metric);
    QCOMPARE(four.tactical_4w, CftcTacticalState::Unavailable);

    // 13W: the same construction over the 13-report windows.
    CftcResearchInput thirteen_week;
    thirteen_week.observations =
        legacy_series_at_offsets({126, 119, 112, 105, 84, 77, 70, 63, 56, 49, 42, 21, 14, 7, 0},
                                 {0, 0, 0, 0, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100});
    const CftcResearchResult thirteen = cftc_evaluate_research_state(thirteen_week);
    const CftcEvidenceItem* trend_13w = find_item(thirteen, QStringLiteral("R13W-TREND"));
    QVERIFY(trend_13w);
    QVERIFY2(!trend_13w->available, "a gapped swing trend window must not vote");
    QVERIFY(trend_13w->explanation.contains(QStringLiteral("spans a missing report")));
    QCOMPARE(thirteen.swing_13w, CftcTacticalState::Unavailable);
}

// ── Historical context and return-from-extreme ──────────────────────────────

void TstCftcResearchState::crowded_long_with_continued_strengthening_is_not_sell() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.historical_context, CftcHistoricalContext::CrowdedLong);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Buy);
    QVERIFY2(result.state != CftcResearchState::Sell,
             "a crowded long that is still strengthening must not become SELL");
    const CftcEvidenceGroup* historical = find_group(result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(historical);
    QCOMPARE(historical->direction, CftcEvidenceDirection::Neutral);
    QVERIFY2(!list_has_rule(historical->items, QStringLiteral("RHIST-REVERSAL")),
             "crowding alone must not produce a reversal rule");
    QVERIFY(list_has_rule(historical->items, QStringLiteral("RHIST-CROWDED-CONTINUATION")));
    QCOMPARE(result.conflicting.size(), 0);

    // The current report is at the trailing 2Y high but still extending: the
    // extreme is continuation context, never a contrarian signal.
    const CftcEvidenceItem* extreme = find_item(result, QStringLiteral("R4W-EXTREME"));
    QVERIFY(extreme);
    QCOMPARE(extreme->direction, CftcEvidenceDirection::Neutral);
}

void TstCftcResearchState::crowded_long_with_recent_reversal_is_bearish() {
    CftcResearchInput input;
    input.observations = legacy_series(crowded_long_reversal_values());
    // A falling price over the same recent window independently confirms the
    // 4W reversal, so the state reaches SELL; the historical reversal is
    // supporting context, never the independent confirmation itself.
    input.prices = price_series(ramp(200.0, 110, -1.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.historical_context, CftcHistoricalContext::CrowdedLong);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bearish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Neutral);
    QCOMPARE(result.state, CftcResearchState::Sell);
    const CftcEvidenceItem* reversal = find_item(result, QStringLiteral("RHIST-REVERSAL"));
    QVERIFY(reversal);
    QVERIFY(reversal->available);
    QCOMPARE(reversal->direction, CftcEvidenceDirection::Bearish);
    QVERIFY(reversal->explanation.contains(QStringLiteral("4W")));
    QVERIFY(reversal->explanation.contains(QStringLiteral("decreased")));
    QVERIFY(reversal->explanation.contains(QStringLiteral("3")));
    const CftcEvidenceGroup* historical = find_group(result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(historical);
    QVERIFY2(!historical->counts_for_independence,
             "the same positioning series cannot satisfy the independence requirement through history");
}

void TstCftcResearchState::historical_reversal_alone_does_not_create_direction() {
    // Crowded long with a genuine 4W reversal but no independent confirmation:
    // the reversal is visible and bearish, yet the state must stay HOLD.
    CftcResearchInput input;
    input.observations = legacy_series(crowded_long_reversal_values());
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Bearish);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
    const CftcEvidenceItem* reversal = find_item(result, QStringLiteral("RHIST-REVERSAL"));
    QVERIFY(reversal);
    QCOMPARE(reversal->direction, CftcEvidenceDirection::Bearish);
    QVERIFY2(list_has_rule(result.supporting, QStringLiteral("RHIST-REVERSAL")),
             "the reversal must remain visible as supporting context");
}

void TstCftcResearchState::crowded_short_with_continued_weakening_is_not_buy() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(110.0, 110, -1.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.historical_context, CftcHistoricalContext::CrowdedShort);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bearish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bearish);
    QCOMPARE(result.state, CftcResearchState::Sell);
    QVERIFY2(result.state != CftcResearchState::Buy, "a crowded short that is still weakening must not become BUY");
    const CftcEvidenceGroup* historical = find_group(result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(historical);
    QVERIFY(!list_has_rule(historical->items, QStringLiteral("RHIST-REVERSAL")));
    QVERIFY(list_has_rule(historical->items, QStringLiteral("RHIST-CROWDED-CONTINUATION")));
}

void TstCftcResearchState::crowded_short_with_recent_reversal_is_bullish() {
    CftcResearchInput input;
    input.observations = legacy_series(crowded_short_reversal_values());
    // A rising price independently confirms the bullish reversal.
    input.prices = price_series(ramp(1.0, 110));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.historical_context, CftcHistoricalContext::CrowdedShort);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Neutral);
    QCOMPARE(result.state, CftcResearchState::Buy);
    const CftcEvidenceItem* reversal = find_item(result, QStringLiteral("RHIST-REVERSAL"));
    QVERIFY(reversal);
    QCOMPARE(reversal->direction, CftcEvidenceDirection::Bullish);
}

void TstCftcResearchState::neutral_context_with_coherent_recent_direction() {
    QVector<double> values = repeat(80.0, 55);
    values += repeat(120.0, 35);
    values += ramp(95.0, 20);
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.historical_context, CftcHistoricalContext::Neutral);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Buy);
    QCOMPARE(result.confidence, CftcResearchConfidence::Medium);
}

void TstCftcResearchState::zero_variance_reference_is_not_crowded() {
    // A perfectly flat 2Y reference has no distribution to rank against. Batch
    // 1's percentile tie rule keeps the percentile defined (100 for a tie), but
    // that must not become a crowded classification.
    CftcResearchInput input;
    input.observations = legacy_series(repeat(10.0, 110));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(result.readings.stats_2y.reference_covered);
    QVERIFY(result.readings.stats_2y.zero_variance);
    QVERIFY(result.readings.stats_2y.has_percentile);
    QCOMPARE(result.readings.stats_2y.percentile, 100.0);
    QCOMPARE(result.historical_context, CftcHistoricalContext::Unavailable);
    const CftcEvidenceGroup* historical = find_group(result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(historical);
    QVERIFY2(!historical->available, "a zero-variance reference is unavailable context, not a crowding signal");
    const CftcEvidenceItem* crowding = find_item(result, QStringLiteral("RHIST-CROWDING"));
    QVERIFY(crowding);
    QVERIFY(!crowding->available);
    QVERIFY(crowding->explanation.contains(QStringLiteral("zero variance")));
    const CftcEvidenceItem* extreme = find_item(result, QStringLiteral("R4W-EXTREME"));
    QVERIFY(extreme);
    QVERIFY(!extreme->available);
    QVERIFY2(extreme->explanation.contains(QStringLiteral("zero variance")),
             "a flat reference defines no extreme band and must say so");
    QCOMPARE(result.state, CftcResearchState::Hold);
}

void TstCftcResearchState::mixed_historical_normalization_is_exposed() {
    // A wide, skewed reference: the current value is at the 92nd percentile
    // (long view) while sitting 8% into the min/max range (short view). The
    // related statistics disagree, so the context must be Mixed rather than
    // resolved in favor of whichever condition is tested first.
    QVector<double> values = repeat(-10.0, 96);
    values += repeat(110.0, 8);
    values += repeat(-10.0, 5);
    values.append(0.0);
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(result.readings.stats_2y.has_percentile);
    QVERIFY(result.readings.stats_2y.percentile >= 90.0);
    QVERIFY(result.readings.stats_2y.has_cot_index);
    QVERIFY(result.readings.stats_2y.cot_index <= 10.0);
    QCOMPARE(result.historical_context, CftcHistoricalContext::Mixed);
    const CftcEvidenceItem* crowding = find_item(result, QStringLiteral("RHIST-CROWDING"));
    QVERIFY(crowding);
    QVERIFY(crowding->available);
    QVERIFY2(crowding->conflicted, "disagreeing historical normalization must be marked conflicted");
    QVERIFY(crowding->explanation.contains(QStringLiteral("disagrees")));
    QVERIFY2(find_item(result, QStringLiteral("RHIST-REVERSAL")) == nullptr,
             "a mixed context must not produce a crowding reversal");
    QCOMPARE(result.state, CftcResearchState::Hold);
}

void TstCftcResearchState::percentile_cot_index_zscore_do_not_triple_vote() {
    // All three strictly trailing statistics are extreme in the same direction,
    // but they are one historical-context family and must not become several
    // directional votes.
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(result.readings.stats_2y.has_percentile);
    QCOMPARE(result.readings.stats_2y.percentile, 100.0);
    QVERIFY(result.readings.stats_2y.has_cot_index);
    QVERIFY(result.readings.stats_2y.cot_index >= 90.0);
    QVERIFY(result.readings.stats_2y.has_zscore);
    QVERIFY(result.readings.stats_2y.zscore >= 1.5);
    const CftcEvidenceGroup* historical = find_group(result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(historical);
    QCOMPARE(directional_item_count(historical->items), 0);
    QCOMPARE(historical->direction, CftcEvidenceDirection::Neutral);

    // Even with a genuine reversal, the historical family emits at most one
    // directional item regardless of how many statistics are extreme.
    CftcResearchInput reversal_input;
    reversal_input.observations = legacy_series(crowded_long_reversal_values());
    const CftcResearchResult reversal_result = cftc_evaluate_research_state(reversal_input);
    const CftcEvidenceGroup* reversal_group = find_group(reversal_result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(reversal_group);
    QCOMPARE(directional_item_count(reversal_group->items), 1);
}

void TstCftcResearchState::return_from_upper_extreme_is_bearish() {
    CftcResearchInput input;
    input.observations = legacy_series(plateau_values(90.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    const CftcEvidenceItem* extreme = find_item(result, QStringLiteral("R4W-EXTREME"));
    QVERIFY(extreme);
    QVERIFY(extreme->available);
    QCOMPARE(extreme->direction, CftcEvidenceDirection::Bearish);
    bool has_left_upper = false;
    for (const auto& metric : extreme->metrics) {
        if (metric.key == QStringLiteral("left_upper"))
            has_left_upper = metric.has_value && metric.value == 1.0;
    }
    QVERIFY2(has_left_upper, "the previous report must be recorded as leaving the upper extreme");
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bearish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bearish);
    QCOMPARE(result.state, CftcResearchState::Sell);
}

void TstCftcResearchState::return_from_lower_extreme_is_bullish() {
    CftcResearchInput input;
    input.observations = legacy_series(lower_plateau_values(10.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    const CftcEvidenceItem* extreme = find_item(result, QStringLiteral("R4W-EXTREME"));
    QVERIFY(extreme);
    QCOMPARE(extreme->direction, CftcEvidenceDirection::Bullish);
    bool has_left_lower = false;
    for (const auto& metric : extreme->metrics) {
        if (metric.key == QStringLiteral("left_lower"))
            has_left_lower = metric.has_value && metric.value == 1.0;
    }
    QVERIFY(has_left_lower);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Buy);
}

void TstCftcResearchState::persistent_extreme_without_reversal_is_context_only() {
    CftcResearchInput input;
    input.observations = legacy_series(plateau_values(105.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.historical_context, CftcHistoricalContext::CrowdedLong);
    QCOMPARE(result.state, CftcResearchState::Buy);
    QVERIFY2(result.state != CftcResearchState::Sell,
             "a persistent extreme with continued strengthening must not become SELL");
    const CftcEvidenceItem* extreme = find_item(result, QStringLiteral("R4W-EXTREME"));
    QVERIFY(extreme);
    QCOMPARE(extreme->direction, CftcEvidenceDirection::Neutral);
    bool has_at_upper = false;
    bool has_weeks = false;
    for (const auto& metric : extreme->metrics) {
        if (metric.key == QStringLiteral("at_upper"))
            has_at_upper = metric.has_value && metric.value == 1.0;
        if (metric.key == QStringLiteral("weeks_at_upper"))
            has_weeks = metric.has_value && metric.value >= 2.0;
    }
    QVERIFY(has_at_upper);
    QVERIFY2(has_weeks, "persistence must be recorded");
    const CftcEvidenceGroup* historical = find_group(result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(historical);
    QVERIFY2(!list_has_rule(historical->items, QStringLiteral("RHIST-REVERSAL")),
             "no reversal rule may fire without a recent reversal");
}

// ── Independence and missing evidence ───────────────────────────────────────

void TstCftcResearchState::one_family_cannot_satisfy_independence() {
    // One family (the 4W tactical layer) with several correlated bullish
    // metrics still cannot produce BUY on its own.
    const QVector<double> values = {0, 0, 0, 0, 0, 0, 100, 200, 300, 400};
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
    int independent_directional_families = 0;
    for (const auto& group : result.groups) {
        if (!group.counts_for_independence || !group.available)
            continue;
        if (group.direction == CftcEvidenceDirection::Bullish || group.direction == CftcEvidenceDirection::Bearish)
            ++independent_directional_families;
    }
    QCOMPARE(independent_directional_families, 1);
}

void TstCftcResearchState::insufficient_independent_evidence_produces_hold() {
    // 4W bullish with a flat 13W and no other available family: HOLD, not BUY.
    QVector<double> values = repeat(580.0, 7);
    values += repeat(500.0, 8);
    values += QVector<double>{520, 540, 560, 570, 580};
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Neutral);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
}

void TstCftcResearchState::missing_price_is_unavailable_not_neutral() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(!result.price_used);
    QCOMPARE(result.state, CftcResearchState::Buy);
    const CftcEvidenceGroup* price = find_group(result, CftcEvidenceFamily::PriceCot);
    QVERIFY(price);
    QVERIFY2(!price->available, "missing price evidence is unavailable, not neutral");
    QCOMPARE(price->direction, CftcEvidenceDirection::Unavailable);
    for (const auto& item : price->items) {
        QVERIFY(!item.available);
        QCOMPARE(item.direction, CftcEvidenceDirection::Unavailable);
    }
    QVERIFY(list_has_rule(result.unavailable, QStringLiteral("RPC-4W-RELATIONSHIP")));
    QVERIFY2(result.confidence != CftcResearchConfidence::High, "missing price evidence must cap confidence");
}

void TstCftcResearchState::stale_price_series_is_unavailable() {
    // A price series that stops years before the report is not a current price:
    // it must leave price/COT evidence unavailable rather than anchor on the old
    // close and present it as a current comparison.
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110));
    const QDate old_end(2021, 5, 18);
    for (int i = 0; i < 40; ++i)
        input.prices.append({old_end.addDays(-7LL * (39 - i)), 100.0 + static_cast<double>(i)});
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(!result.readings.price_series_fresh);
    QVERIFY(!result.price_used);
    QVERIFY(!result.readings.price_change_4w_available);
    const CftcEvidenceGroup* price = find_group(result, CftcEvidenceFamily::PriceCot);
    QVERIFY(price);
    QVERIFY(!price->available);
    const CftcEvidenceItem* relationship = find_item(result, QStringLiteral("RPC-4W-RELATIONSHIP"));
    QVERIFY(relationship);
    QVERIFY(!relationship->available);
    QVERIFY(relationship->explanation.contains(QStringLiteral("stale")));
    QCOMPARE(result.state, CftcResearchState::Buy);
}

void TstCftcResearchState::requested_as_of_before_history_end_reports_truthfully() {
    // The as-of report predates the newest returned observation: the engine must
    // say so rather than claim the report lacks a usable Net position.
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 20));
    input.as_of = latest_report().addDays(-7);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(!result.data_available);
    QVERIFY(!result.data_stale);
    QVERIFY(result.report_predates_history);
    QCOMPARE(result.report_date, latest_report().addDays(-7));
    QCOMPARE(result.latest_available_report_date, latest_report());
    QCOMPARE(result.state, CftcResearchState::Hold);
    const CftcEvidenceItem* freshness = find_item(result, QStringLiteral("RDQ-REPORT-CURRENT"));
    QVERIFY(freshness);
    QVERIFY(!freshness->available);
    QVERIFY(freshness->explanation.contains(QStringLiteral("predates")));
}

void TstCftcResearchState::anchor_missing_net_reports_truthfully() {
    // The shared 4W anchor report is valid but carries no speculative legs: the
    // truthful reason is missing data at the anchor, not a horizon-tolerance
    // failure.
    QVector<CftcObservation> observations = legacy_series({0, 0, 0, 0, 0, 0, 100, 200, 300, 400});
    QCOMPARE(observations.size(), 10);
    observations[5].longs[1] = std::nullopt;
    observations[5].shorts[1] = std::nullopt;
    CftcResearchInput input;
    input.observations = observations;
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(!result.readings.changes_4w.net.has_value);
    QVERIFY(result.readings.changes_4w.net.has_anchor);
    QVERIFY(!result.readings.changes_4w.net.stale);
    const CftcEvidenceItem* position = find_item(result, QStringLiteral("R4W-NET-CHANGE"));
    QVERIFY(position);
    QVERIFY(!position->available);
    QVERIFY2(position->explanation.contains(QStringLiteral("missing at the shared horizon anchor")),
             "the reason must name the missing anchor value, not the tolerance");
    QVERIFY(!position->explanation.contains(QStringLiteral("tolerance")));
}

void TstCftcResearchState::missing_open_interest_stays_unavailable() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110), std::nullopt);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(result.readings.changes_4w.net.has_value);
    QVERIFY2(!result.readings.changes_4w.net_pct_oi.has_value,
             "a missing open interest leaves the normalized change undefined");
    const CftcEvidenceGroup* oi = find_group(result, CftcEvidenceFamily::OpenInterest);
    QVERIFY(oi);
    QVERIFY(!oi->available);
    QCOMPARE(oi->direction, CftcEvidenceDirection::Unavailable);
    QVERIFY(list_has_rule(result.unavailable, QStringLiteral("ROI-4W-RELATIONSHIP")));
    QCOMPARE(result.state, CftcResearchState::Buy);
}

void TstCftcResearchState::missing_historical_reference_stays_unavailable() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 40));
    input.prices = price_series(ramp(1.0, 40));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(result.readings.stats_2y.has_percentile == false);
    QCOMPARE(result.historical_context, CftcHistoricalContext::Unavailable);
    const CftcEvidenceGroup* historical = find_group(result, CftcEvidenceFamily::HistoricalContext);
    QVERIFY(historical);
    QVERIFY2(!historical->available, "insufficient history is unavailable, not normal positioning");
    QVERIFY(list_has_rule(result.unavailable, QStringLiteral("RHIST-CROWDING")));
    QCOMPARE(result.state, CftcResearchState::Buy);
    QVERIFY2(result.confidence != CftcResearchConfidence::High, "incomplete historical context must cap confidence");
}

void TstCftcResearchState::stale_speculative_series_does_not_use_prior_report() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 20));
    input.as_of = latest_report().addDays(7);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(!result.data_available);
    QVERIFY(result.data_stale);
    QCOMPARE(result.report_date, latest_report().addDays(7));
    QCOMPARE(result.latest_available_report_date, latest_report());
    QCOMPARE(result.state, CftcResearchState::Hold);
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Unavailable);
    QCOMPARE(result.swing_13w, CftcTacticalState::Unavailable);
    QVERIFY(result.readings.changes_4w.net.stale);
    const CftcEvidenceItem* freshness = find_item(result, QStringLiteral("RDQ-REPORT-CURRENT"));
    QVERIFY(freshness);
    QVERIFY(!freshness->available);
    QVERIFY(freshness->explanation.contains(QStringLiteral("prior report is not used")));
}

void TstCftcResearchState::unavailable_data_stays_unavailable() {
    const CftcResearchResult result = cftc_evaluate_research_state(CftcResearchInput{});

    QVERIFY(result.report_missing);
    QVERIFY(!result.data_available);
    QCOMPARE(result.state, CftcResearchState::Hold);
    QCOMPARE(result.confidence, CftcResearchConfidence::Low);
    QCOMPARE(result.tactical_4w, CftcTacticalState::Unavailable);
    QCOMPARE(result.swing_13w, CftcTacticalState::Unavailable);
    QCOMPARE(result.regime_26w, CftcTacticalState::Unavailable);
    QCOMPARE(result.historical_context, CftcHistoricalContext::Unavailable);
    for (const auto& group : result.groups) {
        if (!group.available)
            QCOMPARE(group.direction, CftcEvidenceDirection::Unavailable);
    }
}

// ── Price / OI families ─────────────────────────────────────────────────────

void TstCftcResearchState::price_cot_alignment_confirms_and_raises_coverage() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110));
    input.prices = price_series(ramp(1.0, 110));
    input.price_source = QStringLiteral("test weekly closes");
    input.price_continuous_proxy = true;
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QVERIFY(result.price_used);
    QCOMPARE(result.price_source, QStringLiteral("test weekly closes"));
    QVERIFY(result.price_continuous_proxy);
    const CftcEvidenceGroup* price = find_group(result, CftcEvidenceFamily::PriceCot);
    QVERIFY(price);
    QVERIFY(price->available);
    QCOMPARE(price->direction, CftcEvidenceDirection::Bullish);
    QCOMPARE(result.state, CftcResearchState::Buy);
    QCOMPARE(result.confidence, CftcResearchConfidence::High);
    QVERIFY(list_has_rule(result.supporting, QStringLiteral("RPC-13W-RELATIONSHIP")));
}

void TstCftcResearchState::price_cot_opposition_is_visible_conflict() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110));
    input.prices = price_series(ramp(110.0, 110, -1.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Bullish);
    QCOMPARE(result.swing_13w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Buy);
    const CftcEvidenceGroup* price = find_group(result, CftcEvidenceFamily::PriceCot);
    QVERIFY(price);
    QCOMPARE(price->direction, CftcEvidenceDirection::Neutral);
    const CftcEvidenceItem* relationship = find_item(result, QStringLiteral("RPC-13W-RELATIONSHIP"));
    QVERIFY(relationship);
    QVERIFY(relationship->conflicted);
    QVERIFY(relationship->explanation.contains(QStringLiteral("divergence")));
    QVERIFY2(relationship->explanation.contains(QStringLiteral("13W")) &&
                 relationship->explanation.contains(QStringLiteral("13.00")) &&
                 relationship->explanation.contains(QStringLiteral("13")),
             "a conflicting explanation must name the horizon and the actual values");
    QVERIFY2(list_has_rule(result.conflicting, QStringLiteral("RPC-13W-RELATIONSHIP")),
             "divergence must remain visible as conflicting evidence");
    QCOMPARE(result.confidence, CftcResearchConfidence::Medium);
}

// ── Participant semantics ───────────────────────────────────────────────────

void TstCftcResearchState::participant_confirmation_supports_direction() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 40), 1000.0, ramp(1.0, 40));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    const CftcEvidenceGroup* participant = find_group(result, CftcEvidenceFamily::Participant);
    QVERIFY(participant);
    QVERIFY(participant->available);
    QCOMPARE(participant->direction, CftcEvidenceDirection::Bullish);
    const CftcEvidenceItem* confirmation = find_item(result, QStringLiteral("RPART-LEG-NON-REPORTABLE"));
    QVERIFY(confirmation);
    QCOMPARE(confirmation->direction, CftcEvidenceDirection::Bullish);
    QVERIFY(!confirmation->conflicted);
    QCOMPARE(result.state, CftcResearchState::Buy);
    QVERIFY(list_has_rule(result.supporting, QStringLiteral("RPART-LEG-NON-REPORTABLE")));
}

void TstCftcResearchState::participant_conflict_stays_visible() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 40), 1000.0, ramp(40.0, 40, -1.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    const CftcEvidenceItem* conflict = find_item(result, QStringLiteral("RPART-LEG-NON-REPORTABLE"));
    QVERIFY(conflict);
    QVERIFY(conflict->conflicted);
    QCOMPARE(conflict->direction, CftcEvidenceDirection::Bearish);
    QVERIFY(conflict->explanation.contains(QStringLiteral("opposite direction")));
    QVERIFY(list_has_rule(result.conflicting, QStringLiteral("RPART-LEG-NON-REPORTABLE")));
    QCOMPARE(result.state, CftcResearchState::Buy);
}

void TstCftcResearchState::participant_context_only_does_not_count_as_confirmation() {
    // Full coverage including an evaluated participant confirmation reaches High.
    CftcResearchInput full;
    full.observations = legacy_series(ramp(1.0, 110));
    full.prices = price_series(ramp(1.0, 110));
    const CftcResearchResult full_result = cftc_evaluate_research_state(full);
    QCOMPARE(full_result.confidence, CftcResearchConfidence::High);

    // With the confirmation class absent but the Commercial context present,
    // the participant family must not count as covered: the same evidence minus
    // participant confirmation cannot reach High.
    CftcResearchInput context_only = full;
    strip_participant_legs(context_only.observations, 2); // legacy non_reportable
    const CftcResearchResult context_result = cftc_evaluate_research_state(context_only);
    const CftcEvidenceItem* confirmation = find_item(context_result, QStringLiteral("RPART-LEG-NON-REPORTABLE"));
    QVERIFY(confirmation);
    QVERIFY2(!confirmation->available, "the confirmation class has no 13W change");
    const CftcEvidenceItem* commercial = find_item(context_result, QStringLiteral("RPART-LEG-COMMERCIAL"));
    QVERIFY(commercial);
    QVERIFY(commercial->available);
    const CftcEvidenceGroup* participant = find_group(context_result, CftcEvidenceFamily::Participant);
    QVERIFY(participant);
    QVERIFY2(!participant->counts_for_confidence,
             "context-only participant classes must not satisfy confirmation coverage");
    QCOMPARE(context_result.confidence, CftcResearchConfidence::Medium);
    QCOMPARE(context_result.state, CftcResearchState::Buy);
}

void TstCftcResearchState::legacy_participant_semantics() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 40), 1000.0, ramp(1.0, 40), ramp(-1.0, 40, -1.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.family, CftcFamily::Legacy);
    QCOMPARE(result.family_code, QStringLiteral("legacy"));
    QCOMPARE(result.primary_speculative_key, QStringLiteral("non_commercial"));
    QVERIFY(find_item(result, QStringLiteral("RPART-LEG-NON-REPORTABLE")));
    const CftcEvidenceItem* commercial = find_item(result, QStringLiteral("RPART-LEG-COMMERCIAL"));
    QVERIFY(commercial);
    QCOMPARE(commercial->direction, CftcEvidenceDirection::Neutral);
    QVERIFY2(commercial->explanation.contains(QStringLiteral("context only")),
             "a hedging class is context, never a directional vote");
}

void TstCftcResearchState::disaggregated_participant_semantics() {
    CftcResearchInput input;
    input.family = CftcFamily::Disaggregated;
    input.observations = disaggregated_series(ramp(1.0, 40), 1000.0, ramp(1.0, 40));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.family_code, QStringLiteral("disaggregated"));
    QCOMPARE(result.primary_speculative_key, QStringLiteral("managed_money"));
    const CftcEvidenceItem* confirmation = find_item(result, QStringLiteral("RPART-DIS-OTHER-REPORTABLE"));
    QVERIFY(confirmation);
    QCOMPARE(confirmation->direction, CftcEvidenceDirection::Bullish);
    // Producer/Merchant and Swap Dealers stay separately identifiable and are
    // never collapsed into one generic commercial group.
    const CftcEvidenceItem* producer = find_item(result, QStringLiteral("RPART-DIS-PRODUCER-MERCHANT"));
    const CftcEvidenceItem* swap = find_item(result, QStringLiteral("RPART-DIS-SWAP-DEALER"));
    QVERIFY(producer);
    QVERIFY(swap);
    QCOMPARE(producer->direction, CftcEvidenceDirection::Neutral);
    QCOMPARE(swap->direction, CftcEvidenceDirection::Neutral);
}

void TstCftcResearchState::tff_participant_semantics_without_commercial_signal() {
    CftcResearchInput input;
    input.family = CftcFamily::Tff;
    input.observations = tff_series(ramp(1.0, 40), 1000.0, ramp(1.0, 40), ramp(1.0, 40));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.family_code, QStringLiteral("tff"));
    QCOMPARE(result.primary_speculative_key, QStringLiteral("leveraged_funds"));
    QVERIFY(find_item(result, QStringLiteral("RPART-TFF-ASSET-MANAGER")));
    QVERIFY(find_item(result, QStringLiteral("RPART-TFF-OTHER-REPORTABLE")));
    QVERIFY(find_item(result, QStringLiteral("RPART-TFF-DEALER")));
    QVERIFY(find_item(result, QStringLiteral("RPART-TFF-NON-REPORTABLE")));
    for (const auto& group : result.groups) {
        for (const auto& item : group.items) {
            QVERIFY2(!item.rule_id.contains(QStringLiteral("commercial"), Qt::CaseInsensitive),
                     "TFF must not invent a Commercial signal");
            QVERIFY2(!item.explanation.contains(QStringLiteral("Commercial"), Qt::CaseInsensitive),
                     "TFF must not invent a Commercial signal");
        }
    }
}

// ── Context, confidence, structure ──────────────────────────────────────────

void TstCftcResearchState::regime_26w_is_context_not_a_vote() {
    // A 26W rise that has been flat for the 13W/4W windows: the regime layer is
    // bullish context but cannot create a state or satisfy independence.
    QVector<double> values = ramp(100.0, 10, 100.0);
    values += repeat(1000.0, 20);
    CftcResearchInput input;
    input.observations = legacy_series(values);
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.tactical_4w, CftcTacticalState::Neutral);
    QCOMPARE(result.swing_13w, CftcTacticalState::Neutral);
    QCOMPARE(result.regime_26w, CftcTacticalState::Bullish);
    QCOMPARE(result.state, CftcResearchState::Hold);
    const CftcEvidenceGroup* regime = find_group(result, CftcEvidenceFamily::Regime26W);
    QVERIFY(regime);
    QVERIFY2(!regime->counts_for_independence, "the 26W regime must never vote");
    QVERIFY2(!list_has_rule(result.supporting, QStringLiteral("R26W-REGIME")),
             "the 26W regime must not appear as state support");
    QVERIFY2(!list_has_rule(result.conflicting, QStringLiteral("R26W-REGIME")),
             "the 26W regime must not appear as state conflict");
}

void TstCftcResearchState::explanations_expose_values_and_horizons() {
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 110));
    input.prices = price_series(ramp(1.0, 110));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    const CftcEvidenceItem* position = find_item(result, QStringLiteral("R4W-NET-CHANGE"));
    QVERIFY(position);
    QVERIFY(position->explanation.contains(QStringLiteral("Net change 4")));
    QVERIFY(position->explanation.contains(QStringLiteral("Net %OI change 0.40")));

    const CftcEvidenceItem* price = find_item(result, QStringLiteral("RPC-13W-RELATIONSHIP"));
    QVERIFY(price);
    QVERIFY(price->explanation.contains(QStringLiteral("13W")));
    QVERIFY(price->explanation.contains(QStringLiteral("13.00")));
    QVERIFY(price->explanation.contains(QStringLiteral("13")));

    const auto names_a_horizon = [](const CftcEvidenceItem& item) {
        return item.explanation.contains(QStringLiteral("4W")) || item.explanation.contains(QStringLiteral("13W")) ||
               item.explanation.contains(QStringLiteral("26W")) || item.explanation.contains(QStringLiteral("2Y")) ||
               item.explanation.contains(QStringLiteral("1W"));
    };
    for (const auto& item : result.supporting) {
        QVERIFY2(!item.explanation.trimmed().isEmpty(), "every supporting item must explain itself");
        QVERIFY2(names_a_horizon(item), "every supporting explanation must name its horizon");
    }
    QVERIFY(!result.supporting.isEmpty());

    // Conflicting evidence must be explained just as concretely.
    CftcResearchInput conflict_input;
    conflict_input.observations = legacy_series(ramp(1.0, 110));
    conflict_input.prices = price_series(ramp(110.0, 110, -1.0));
    const CftcResearchResult conflict_result = cftc_evaluate_research_state(conflict_input);
    QVERIFY(!conflict_result.conflicting.isEmpty());
    for (const auto& item : conflict_result.conflicting) {
        QVERIFY2(!item.explanation.trimmed().isEmpty(), "every conflicting item must explain itself");
        QVERIFY2(names_a_horizon(item), "every conflicting explanation must name its horizon");
    }
}

void TstCftcResearchState::confidence_penalizes_conflicts_once_per_family() {
    // Both price horizons diverge against the same positioning move. The price
    // family is penalized once, not once per item: coverage 3 (4W, 13W, price)
    // + core agreement 1 - one family conflict = 3 -> Medium. Item-level
    // counting would subtract two and drop the result to Low.
    CftcResearchInput input;
    input.observations = legacy_series(ramp(1.0, 40), std::nullopt);
    strip_participant_legs(input.observations, 2);
    input.prices = price_series(ramp(200.0, 40, -1.0));
    const CftcResearchResult result = cftc_evaluate_research_state(input);

    QCOMPARE(result.state, CftcResearchState::Buy);
    QCOMPARE(result.conflicting.size(), 2);
    QVERIFY(list_has_rule(result.conflicting, QStringLiteral("RPC-4W-RELATIONSHIP")));
    QVERIFY(list_has_rule(result.conflicting, QStringLiteral("RPC-13W-RELATIONSHIP")));
    QCOMPARE(result.confidence, CftcResearchConfidence::Medium);
}

void TstCftcResearchState::deterministic_repeatability() {
    CftcResearchInput input;
    input.observations = legacy_series(crowded_long_reversal_values(), 1000.0, ramp(1.0, 110));
    input.prices = price_series(ramp(1.0, 110));
    const CftcResearchResult first = cftc_evaluate_research_state(input);
    const CftcResearchResult second = cftc_evaluate_research_state(input);

    QCOMPARE(first.rule_set_version, second.rule_set_version);
    QCOMPARE(first.state, second.state);
    QCOMPARE(first.confidence, second.confidence);
    QCOMPARE(first.tactical_4w, second.tactical_4w);
    QCOMPARE(first.swing_13w, second.swing_13w);
    QCOMPARE(first.regime_26w, second.regime_26w);
    QCOMPARE(first.historical_context, second.historical_context);
    QCOMPARE(first.groups.size(), second.groups.size());
    QCOMPARE(first.supporting.size(), second.supporting.size());
    QCOMPARE(first.conflicting.size(), second.conflicting.size());
    QCOMPARE(first.unavailable.size(), second.unavailable.size());
    for (int g = 0; g < first.groups.size(); ++g) {
        QCOMPARE(first.groups[g].items.size(), second.groups[g].items.size());
        for (int i = 0; i < first.groups[g].items.size(); ++i) {
            const CftcEvidenceItem& a = first.groups[g].items[i];
            const CftcEvidenceItem& b = second.groups[g].items[i];
            QCOMPARE(a.rule_id, b.rule_id);
            QCOMPARE(a.direction, b.direction);
            QCOMPARE(a.strength, b.strength);
            QCOMPARE(a.available, b.available);
            QCOMPARE(a.conflicted, b.conflicted);
            QCOMPARE(a.explanation, b.explanation);
        }
    }
}

void TstCftcResearchState::exact_rule_set_version_and_rule_table() {
    QCOMPARE(cftc_research_rule_set_version(), QStringLiteral("cftc-cot-state-v0"));
    const QVector<CftcResearchRule>& rules = cftc_research_rules();
    QVERIFY(!rules.isEmpty());
    QSet<QString> ids;
    for (const auto& rule : rules) {
        QVERIFY(!rule.id.isEmpty());
        QVERIFY(!rule.family.isEmpty());
        QVERIFY(!rule.horizon.isEmpty());
        QVERIFY(!rule.required_inputs.isEmpty());
        QVERIFY(!rule.condition.isEmpty());
        QVERIFY(!rule.interpretation.isEmpty());
        QCOMPARE(rule.version, QStringLiteral("cftc-cot-state-v0"));
        QVERIFY2(!ids.contains(rule.id), "rule ids must be stable and unique");
        ids.insert(rule.id);
    }
    // The final state and confidence are produced by versioned aggregation
    // metadata too, so Batch 3 can replay the exact rules that emitted a state.
    QVERIFY(ids.contains(QStringLiteral("RSTATE-HORIZON-AGGREGATION")));
    QVERIFY(ids.contains(QStringLiteral("RSTATE-INDEPENDENCE-GATE")));
    QVERIFY(ids.contains(QStringLiteral("RSTATE-CONFIDENCE")));

    CftcResearchInput input;
    input.observations = legacy_series(crowded_long_reversal_values(), 1000.0, ramp(1.0, 110));
    const CftcResearchResult result = cftc_evaluate_research_state(input);
    QCOMPARE(result.rule_set_version, QStringLiteral("cftc-cot-state-v0"));
    for (const auto& group : result.groups) {
        for (const auto& item : group.items)
            QVERIFY2(ids.contains(item.rule_id), qPrintable(QStringLiteral("unknown rule id %1").arg(item.rule_id)));
    }
}

void TstCftcResearchState::publication_effective_date_handling() {
    CftcResearchInput unknown_input;
    unknown_input.observations = legacy_series(ramp(1.0, 40));
    const CftcResearchResult unknown = cftc_evaluate_research_state(unknown_input);
    QVERIFY(!unknown.effective_date_known);
    QVERIFY(!unknown.effective_date.isValid());
    QCOMPARE(unknown.report_date, latest_report());

    // The caller's authoritative publication/effective date is preserved as a
    // separate concept and is never synthesized from the report date.
    CftcResearchInput known_input = unknown_input;
    known_input.effective_date = QDate(2026, 10, 2);
    const CftcResearchResult known = cftc_evaluate_research_state(known_input);
    QVERIFY(known.effective_date_known);
    QCOMPARE(known.effective_date, QDate(2026, 10, 2));
    QCOMPARE(known.report_date, latest_report());
    QVERIFY(known.effective_date != known.report_date);
}

QTEST_GUILESS_MAIN(TstCftcResearchState)
#include "tst_cftc_research_state.moc"
