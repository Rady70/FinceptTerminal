// tests/tst_cftc_interpretation.cpp
//
// Batch 4A of the CFTC work: the deterministic descriptive interpretation
// engine (services/economics/CftcInterpretationModel.h) governed by
// CFTC_DESCRIPTIVE_INTERPRETATION_PLAN.md. Covers exposure, strict 156-prior
// report Net %OI percentiles with midpoint ties, strictly trailing move
// materiality, exact 1/4/13-report gross and net flows with prior-OI
// denominators, sustained repositioning, extreme persistence/exit/unwind,
// Open Interest context, raw-net versus Net-%OI disagreement, market-level
// concentration, price/positioning alignment and both divergence states,
// missing-data truthfulness, family terminology applicability and the absence
// of any BUY/HOLD/SELL, bullish/bearish, confidence or expected-return output.
// Rule set v2 (the 2026-09-24 audit corrections) adds the 5-10-day weekly
// neighbour, the 4-day report-price lag with distinct sessions, log-return
// price materiality, the materiality-gated Net %OI disagreement, the 13-report
// unwind lookback, row report-basis verification, report freshness and the
// default 156-report boundary cases. Header-only over Qt Core; no app sources
// (tests/ HARD RULE).
#include "services/economics/CftcInterpretationModel.h"

#include <QRegularExpression>
#include <QtTest>

#include <cmath>

using namespace fincept::services;

namespace {

const QDate kLatest(2026, 9, 15);

struct Legs {
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
};

int participant_index(CftcFamily family, const QString& key) {
    const auto participants = cftc_family_participants(family);
    for (int i = 0; i < participants.size(); ++i) {
        if (participants[i].key == key)
            return i;
    }
    return -1;
}

CftcObservation observation(const QDate& date, const std::optional<double>& oi,
                            const QVector<std::optional<double>>& longs, const QVector<std::optional<double>>& shorts,
                            const std::optional<double>& concentration_gross_4_long = std::nullopt) {
    CftcObservation obs;
    obs.date = date;
    obs.date_label = date.toString(Qt::ISODate);
    obs.market = QStringLiteral("TEST - EXCHANGE");
    obs.contract_code = QStringLiteral("000000");
    obs.units = QStringLiteral("Test units");
    obs.open_interest = oi;
    obs.longs = longs;
    obs.shorts = shorts;
    obs.concentration_gross_4_long = concentration_gross_4_long;
    return obs;
}

/// One observation for `family` where participant `target` carries the given
/// legs and every other participant carries a balanced neutral 100/100.
CftcObservation family_observation(CftcFamily family, const QDate& date, const std::optional<double>& oi, int target,
                                   const std::optional<double>& long_leg, const std::optional<double>& short_leg,
                                   const std::optional<double>& concentration_gross_4_long = std::nullopt) {
    const int count = cftc_family_participants(family).size();
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
    for (int i = 0; i < count; ++i) {
        if (i == target) {
            longs.append(long_leg);
            shorts.append(short_leg);
        } else {
            longs.append(100.0);
            shorts.append(100.0);
        }
    }
    return observation(date, oi, longs, shorts, concentration_gross_4_long);
}

QVector<CftcObservation> make_series(CftcFamily family, int target, const QVector<std::optional<double>>& longs,
                                     const QVector<std::optional<double>>& shorts,
                                     const std::optional<double>& oi = 1000.0) {
    QVector<CftcObservation> out;
    const int n = longs.size();
    for (int i = 0; i < n; ++i) {
        out.append(family_observation(family, kLatest.addDays(-7LL * (n - 1 - i)), oi, target, longs[i], shorts[i]));
    }
    return out;
}

QVector<CftcObservation> make_series_with_oi(CftcFamily family, int target, const QVector<std::optional<double>>& longs,
                                             const QVector<std::optional<double>>& shorts,
                                             const QVector<std::optional<double>>& oi) {
    QVector<CftcObservation> out;
    const int n = longs.size();
    for (int i = 0; i < n; ++i) {
        out.append(family_observation(family, kLatest.addDays(-7LL * (n - 1 - i)), oi[i], target, longs[i], shorts[i]));
    }
    return out;
}

/// Balance net contracts into positive long/short legs around a base.
Legs net_legs(const QVector<double>& nets, double base = 500.0) {
    Legs out;
    for (double net : nets) {
        out.longs.append(base + std::max(0.0, net));
        out.shorts.append(base + std::max(0.0, -net));
    }
    return out;
}

/// Net %OI values (OI = 1000) as legs.
Legs percent_legs(const QVector<double>& percents) {
    QVector<double> nets;
    for (double percent : percents)
        nets.append(10.0 * percent);
    return net_legs(nets);
}

QVector<std::optional<double>> optional_values(const QVector<double>& values) {
    QVector<std::optional<double>> out;
    out.reserve(values.size());
    for (double value : values)
        out.append(value);
    return out;
}

QVector<CftcObservation> legacy_series(const QVector<double>& target_longs, const QVector<double>& target_shorts,
                                       double oi = 1000.0) {
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
    for (double value : target_longs)
        longs.append(value);
    for (double value : target_shorts)
        shorts.append(value);
    return make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")),
                       longs, shorts, oi);
}

QVector<CftcObservation> legacy_percent_series(const QVector<double>& percents, double oi = 1000.0) {
    const Legs legs = percent_legs(percents);
    return make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")),
                       legs.longs, legs.shorts, oi);
}

QVector<CftcPricePoint> price_points(const QVector<double>& closes) {
    QVector<CftcPricePoint> out;
    const int n = closes.size();
    for (int i = 0; i < n; ++i)
        out.append({kLatest.addDays(-7LL * (n - 1 - i)), closes[i]});
    return out;
}

CftcInterpretationConfig small_window_config(int window = 4) {
    CftcInterpretationConfig config;
    config.history_window = window;
    return config;
}

CftcInterpretationInput family_input(CftcFamily family, const QVector<CftcObservation>& observations, int window = 4) {
    CftcInterpretationInput input;
    input.family = family;
    input.observations_family_code = cftc_family_code(family);
    input.observations = observations;
    input.report_basis_code = QStringLiteral("futures_and_options_combined");
    input.config = small_window_config(window);
    return input;
}

/// Declare the fail-closed provenance fields on a manually built input.
void declare_family(CftcInterpretationInput& input, CftcFamily family) {
    input.family = family;
    input.observations_family_code = cftc_family_code(family);
    input.report_basis_code = QStringLiteral("futures_and_options_combined");
}

CftcInterpretationInput legacy_input(const QVector<CftcObservation>& observations, int window = 4) {
    return family_input(CftcFamily::Legacy, observations, window);
}

const CftcParticipantInterpretation* find_participant(const CftcInterpretationResult& result, const QString& key) {
    for (const auto& participant : result.participants) {
        if (participant.participant_key == key)
            return &participant;
    }
    return nullptr;
}

const CftcInterpretationState* find_state(const QVector<CftcInterpretationState>& states, const QString& state_id,
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

bool has_state(const QVector<CftcInterpretationState>& states, const QString& state_id, int horizon = -1) {
    return find_state(states, state_id, horizon) != nullptr;
}

const CftcUnavailableRecord* find_unavailable(const QVector<CftcUnavailableRecord>& records, const QString& family,
                                              const QString& participant_key, int horizon = -1,
                                              const QString& state_id = QString()) {
    for (const auto& record : records) {
        if (record.state_family != family)
            continue;
        if (!participant_key.isNull() && record.participant_key != participant_key)
            continue;
        if (horizon >= 0 && (!record.has_horizon || record.horizon_reports != horizon))
            continue;
        if (!state_id.isNull() && record.state_id != state_id)
            continue;
        return &record;
    }
    return nullptr;
}

const CftcUnavailableRecord* find_market_unavailable(const QVector<CftcUnavailableRecord>& records,
                                                     const QString& family) {
    for (const auto& record : records) {
        if (record.state_family == family && record.participant_key.isEmpty())
            return &record;
    }
    return nullptr;
}

const CftcPricePositionAssessment* find_price(const CftcInterpretationResult& result, const QString& participant_key,
                                              int horizon) {
    for (const auto& assessment : result.price_context) {
        if (assessment.participant_key == participant_key && assessment.horizon_reports == horizon)
            return &assessment;
    }
    return nullptr;
}

const CftcStateMetric* find_metric(const QVector<CftcStateMetric>& metrics, const QString& key) {
    for (const auto& metric : metrics) {
        if (metric.key == key)
            return &metric;
    }
    return nullptr;
}

const CftcConcentrationAssessment* find_concentration(const CftcInterpretationResult& result,
                                                      const QString& field_key) {
    for (const auto& assessment : result.concentration) {
        if (assessment.field_key == field_key)
            return &assessment;
    }
    return nullptr;
}

QVector<CftcObservation> remove_report(const QVector<CftcObservation>& observations, int index) {
    QVector<CftcObservation> out = observations;
    out.removeAt(index);
    return out;
}

QString result_signature(const CftcInterpretationResult& result) {
    QString out;
    auto add = [&out](const QString& line) {
        out += line;
        out += QLatin1Char('\n');
    };
    add(result.rule_set_version);
    add(result.family_code);
    add(result.report_basis);
    add(result.report_date.toString(Qt::ISODate));
    add(result.latest_observation_date.toString(Qt::ISODate));
    add(result.market + QStringLiteral("|") + result.contract_code + QStringLiteral("|") + result.units);
    add(QString::number(result.open_interest, 'g', 17));
    add(QString::number(result.price_requested ? 1 : 0) + result.price_source);
    for (const auto& participant : result.participants) {
        add(QStringLiteral("P|") + participant.participant_key + QLatin1Char('|') + participant.terminology_code +
            QLatin1Char('|') + participant.terminology_caveat_code + QLatin1Char('|') +
            (participant.net_available ? QString::number(participant.net_position, 'g', 17)
                                       : QStringLiteral("no-net")) +
            QLatin1Char('|') +
            (participant.historical_percentile_available ? QString::number(participant.percentile, 'g', 17)
                                                         : QStringLiteral("no-percentile")));
        for (const auto& state : participant.states) {
            add(QStringLiteral("S|") + state.state_id + QLatin1Char('|') + QString::number(state.horizon_reports) +
                QLatin1Char('|') + cftc_interpretation_scope_code(state.scope) + QLatin1Char('|') +
                cftc_evidence_basis_code(state.threshold_basis) + QLatin1Char('|') +
                (state.available ? QStringLiteral("available") : QStringLiteral("unavailable")) + QLatin1Char('|') +
                cftc_unavailable_reason_code(state.reason) + QLatin1Char('|') +
                QString::number(state.percentile, 'g', 17) + QLatin1Char('|') +
                QString::number(state.move_rank, 'g', 17) + QLatin1Char('|') +
                QString::number(state.move_large ? 1 : 0) + QLatin1Char('|') +
                state.mechanism_state_ids.join(QLatin1Char(',')));
            for (const auto& metric : state.metrics) {
                add(QStringLiteral("M|") + metric.key + QLatin1Char('|') +
                    (metric.has_value ? QString::number(metric.value, 'g', 17) : QStringLiteral("missing")));
            }
        }
    }
    for (const auto& state : result.market_context) {
        add(QStringLiteral("C|") + state.state_id + QLatin1Char('|') + QString::number(state.horizon_reports) +
            QLatin1Char('|') + cftc_interpretation_scope_code(state.scope) + QLatin1Char('|') +
            cftc_evidence_basis_code(state.threshold_basis) + QLatin1Char('|') +
            QString::number(state.percentile, 'g', 17) + QLatin1Char('|') + QString::number(state.move_rank, 'g', 17));
    }
    for (const auto& assessment : result.price_context) {
        add(QStringLiteral("R|") + assessment.participant_key + QLatin1Char('|') +
            QString::number(assessment.horizon_reports) + QLatin1Char('|') +
            cftc_interpretation_scope_code(assessment.scope) + QLatin1Char('|') +
            cftc_evidence_basis_code(assessment.threshold_basis) + QLatin1Char('|') +
            (assessment.evaluated ? QStringLiteral("evaluated") : QStringLiteral("not-evaluated")) + QLatin1Char('|') +
            assessment.state_id + QLatin1Char('|') + cftc_unavailable_reason_code(assessment.reason) +
            QLatin1Char('|') + QString::number(assessment.price_move, 'g', 17) + QLatin1Char('|') +
            QString::number(assessment.positioning_move, 'g', 17) + QLatin1Char('|') +
            assessment.mechanism_state_ids.join(QLatin1Char(',')));
    }
    for (const auto& record : result.unavailable) {
        add(QStringLiteral("U|") + record.state_family + QLatin1Char('|') + record.state_id + QLatin1Char('|') +
            record.participant_key + QLatin1Char('|') + QString::number(record.horizon_reports) + QLatin1Char('|') +
            cftc_interpretation_scope_code(record.scope) + QLatin1Char('|') +
            cftc_evidence_basis_code(record.threshold_basis) + QLatin1Char('|') +
            cftc_unavailable_reason_code(record.reason));
    }
    return out;
}

QStringList result_identifiers(const CftcInterpretationResult& result) {
    QStringList out;
    out << result.rule_set_version << result.family_code << result.report_basis;
    for (const auto& participant : result.participants) {
        out << participant.participant_key << participant.label << participant.terminology_code
            << participant.terminology_caveat_code;
        for (const auto& state : participant.states) {
            out << state.state_id << cftc_interpretation_scope_code(state.scope)
                << cftc_evidence_basis_code(state.threshold_basis);
            out << state.mechanism_state_ids;
        }
    }
    for (const auto& state : result.market_context) {
        out << state.state_id << cftc_interpretation_scope_code(state.scope)
            << cftc_evidence_basis_code(state.threshold_basis);
    }
    for (const auto& assessment : result.price_context) {
        out << assessment.state_id << cftc_unavailable_reason_code(assessment.reason);
    }
    for (const auto& record : result.unavailable) {
        out << record.state_family << record.state_id << cftc_interpretation_scope_code(record.scope)
            << cftc_evidence_basis_code(record.threshold_basis) << cftc_unavailable_reason_code(record.reason);
    }
    for (const auto& assessment : result.concentration)
        out << assessment.field_key;
    return out;
}

/// A rich fixture exercising most state families, used by the vocabulary and
/// contract checks.
CftcInterpretationResult rich_result() {
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
    for (int i = 0; i < 8; ++i) {
        longs.append(500.0 + 5.0 * i);
        shorts.append(600.0 - 5.0 * i);
    }
    longs.append(2000.0);
    shorts.append(200.0);
    QVector<CftcObservation> observations = make_series(
        CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")), longs, shorts);
    for (int i = 0; i < observations.size(); ++i)
        observations[i].concentration_gross_4_long = 10.0 + 5.0 * i;
    CftcInterpretationInput input = legacy_input(observations);
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 300});
    input.price_source = QStringLiteral("TEST source");
    input.report_basis_code = QStringLiteral("futures_only");
    return cftc_interpret(input);
}

} // namespace

class TstCftcInterpretation : public QObject {
    Q_OBJECT
  private slots:
    // Contract and configuration
    void default_rule_set_configuration();
    void rule_catalog_covers_the_evidence_bases();
    void rule_catalog_covers_the_complete_state_taxonomy();
    void custom_config_gets_distinct_rule_set_identity();
    void invalid_horizon_configuration_is_ignored();
    void unsupported_positive_horizons_produce_no_states();

    // Provenance
    void provenance_market_name_change_is_allowed();
    void provenance_mixed_contract_is_unavailable();
    void provenance_blank_contract_code_is_unavailable();
    void provenance_family_mismatch_is_unavailable();
    void provenance_participant_slots_must_match_family();
    void provenance_report_basis_is_required();

    // Exposure and historical relative position
    void exposure_states_net_long_short_flat();
    void historical_percentile_requires_full_default_history();
    void historical_percentile_excludes_current_observation();
    void historical_percentile_midpoint_ties();
    void historical_extreme_90_10_boundaries();
    void severe_extreme_975_25_boundaries();
    void crowded_states_require_permitted_terminology();

    // Exact report flows
    void exact_report_flow_values_one_four_thirteen();
    void flow_prior_open_interest_denominator();
    void missing_or_nonpositive_open_interest_stays_unavailable();
    void missing_participant_leg_stays_unavailable();
    void missing_current_open_interest_keeps_raw_exposure();
    void report_gap_breaks_horizon_continuity();
    void materiality_is_strictly_trailing();
    void materiality_75_90_boundaries();
    void flow_states_long_accumulation_and_liquidation();
    void flow_states_short_building_and_covering();
    void gross_leg_states_are_composable();
    void gross_leg_states_cover_all_compositions();
    void net_shift_states_carry_gross_mechanisms();
    void sustained_4r_requires_3_of_4();
    void sustained_13r_requires_9_of_13();
    void persistent_extreme_requires_three_consecutive();
    void extreme_transitions_require_weekly_adjacency();
    void extreme_transitions_report_insufficient_history();
    void extreme_exit_boundaries();
    void extreme_unwind_boundaries();

    // Open Interest and concentration
    void oi_expansion_and_contraction();
    void net_share_raw_disagreement();
    void net_share_raw_disagreement_requires_opposite_signs();
    void concentration_states_are_market_level();
    void concentration_missing_field_stays_unavailable();
    void concentration_states_report_their_own_availability();
    void high_concentration_uses_full_trailing_reference();

    // Price versus positioning
    void price_position_moving_together_up_and_down();
    void price_position_divergence_both_directions();
    void divergence_mechanism_from_gross_legs();
    void non_material_price_or_positioning_yields_no_relationship();
    void missing_price_context_stays_unavailable();
    void unspecified_price_source_stays_unavailable();
    void stale_price_context_stays_unavailable();
    void unusable_price_context_stays_unavailable();

    // Semantics, truthfulness and determinism
    void raw_flow_readings_cover_every_horizon();
    void below_threshold_flow_reading_keeps_its_value();
    void positioning_measurement_survives_missing_price();
    void open_interest_readings_are_emitted();
    void participant_terminology_maps_each_family();
    void terminology_caveats_are_exposed();
    void legacy_disaggregated_tff_applicability();
    void prohibited_cross_family_terminology();
    void result_contains_no_directional_or_predictive_output();
    void deterministic_repeatability();
    void stale_as_of_report_is_not_current();
    void empty_history_is_unavailable();

    // Rule set v2: 2026-09-24 audit corrections
    void weekly_neighbour_requires_five_to_ten_days();
    void report_price_lag_is_at_most_four_days();
    void truncated_price_series_is_not_a_zero_move();
    void identical_price_sessions_are_unavailable();
    void price_materiality_ranks_log_returns();
    void continuous_front_month_proxy_fails_closed();
    void gradual_unwind_within_lookback();
    void outdated_report_is_flagged();
    void row_report_basis_must_match_declared_basis();
    void default_window_percentile_boundaries();
    void default_window_materiality_boundaries();
    void per_cause_insufficient_history_reasons();
};

// ── Contract and configuration ──────────────────────────────────────────────

void TstCftcInterpretation::default_rule_set_configuration() {
    QCOMPARE(cftc_interpretation_rule_set_version(), QStringLiteral("cftc-descriptive-interpretation-v2"));
    const CftcInterpretationConfig config;
    QCOMPARE(config.history_window, 156);
    QCOMPARE(config.extreme_percentile, 0.90);
    QCOMPARE(config.low_extreme_percentile, 0.10);
    QCOMPARE(config.severe_extreme_percentile, 0.975);
    QCOMPARE(config.low_severe_extreme_percentile, 0.025);
    QCOMPARE(config.unwind_reentry_percentile, 0.75);
    QCOMPARE(config.low_unwind_reentry_percentile, 0.25);
    QCOMPARE(config.persistent_extreme_reports, 3);
    QCOMPARE(config.material_move_percentile, 0.75);
    QCOMPARE(config.large_move_percentile, 0.90);
    QCOMPARE(config.horizons_reports, QVector<int>({1, 4, 13}));
    QCOMPARE(config.primary_concentration_field, QStringLiteral("concentration_gross_4_long"));
    QCOMPARE(cftc_unwind_low_reentry_percentile(config), 0.25);
    QCOMPARE(cftc_low_extreme_percentile(config), 0.10);
    QCOMPARE(cftc_low_severe_percentile(config), 0.025);
    QCOMPARE(cftc_interpretation_rule_set_version(), QStringLiteral("cftc-descriptive-interpretation-v2"));
    QVERIFY(cftc_interpretation_custom_rule_set_version() != cftc_interpretation_rule_set_version());
    QVERIFY(cftc_interpretation_config_is_default(cftc_default_interpretation_config()));
    QVERIFY(!cftc_interpretation_config_is_default(small_window_config(4)));
    CftcInterpretationConfig narrow_horizons;
    narrow_horizons.horizons_reports = {1, 4};
    QVERIFY(!cftc_interpretation_config_is_default(narrow_horizons));
    CftcInterpretationConfig narrow_concentration;
    narrow_concentration.primary_concentration_field = QStringLiteral("concentration_net_4_short");
    QVERIFY(!cftc_interpretation_config_is_default(narrow_concentration));
    QCOMPARE(cftc_interpretation_rule_set_version_for(cftc_default_interpretation_config()),
             cftc_interpretation_rule_set_version());
    QCOMPARE(cftc_interpretation_rule_set_version_for(small_window_config(4)),
             cftc_interpretation_custom_rule_set_version());
    QVERIFY(cftc_effective_interpretation_config(cftc_default_interpretation_config()).horizons_reports ==
            QVector<int>({1, 4, 13}));
    QVERIFY(cftc_is_supported_interpretation_horizon(1));
    QVERIFY(cftc_is_supported_interpretation_horizon(4));
    QVERIFY(cftc_is_supported_interpretation_horizon(13));
    QVERIFY(!cftc_is_supported_interpretation_horizon(2));
    QVERIFY(!cftc_is_supported_interpretation_horizon(0));
}

void TstCftcInterpretation::rule_catalog_covers_the_evidence_bases() {
    const QVector<CftcInterpretationRule> rules = cftc_interpretation_rule_catalog();
    QVERIFY(!rules.isEmpty());
    bool has_cftc_defined = false;
    bool has_derived_identity = false;
    bool has_literature = false;
    bool has_practitioner = false;
    bool has_heuristic = false;
    for (const auto& rule : rules) {
        QVERIFY(!rule.id.isEmpty());
        QVERIFY(!rule.condition.isEmpty());
        switch (rule.basis) {
            case CftcEvidenceBasis::CftcDefined:
                has_cftc_defined = true;
                break;
            case CftcEvidenceBasis::DerivedIdentity:
                has_derived_identity = true;
                break;
            case CftcEvidenceBasis::LiteratureSupportedMeasure:
                has_literature = true;
                break;
            case CftcEvidenceBasis::PractitionerConvention:
                has_practitioner = true;
                break;
            case CftcEvidenceBasis::EngineHeuristic:
                has_heuristic = true;
                break;
        }
        QVERIFY(!cftc_interpretation_scope_code(rule.scope).isEmpty());
    }
    QVERIFY(has_cftc_defined);
    QVERIFY(has_derived_identity);
    QVERIFY(has_literature);
    QVERIFY(has_practitioner);
    QVERIFY(has_heuristic);
}

void TstCftcInterpretation::rule_catalog_covers_the_complete_state_taxonomy() {
    const QVector<CftcInterpretationRule> catalog = cftc_interpretation_rule_catalog();
    QStringList catalog_ids;
    for (const auto& rule : catalog)
        catalog_ids.append(rule.id);
    const QStringList taxonomy = cftc_interpretation_state_ids();
    QVERIFY(taxonomy.size() >= 30);
    for (const QString& state_id : taxonomy) {
        QVERIFY2(catalog_ids.contains(state_id), qPrintable(state_id));
        QCOMPARE(taxonomy.count(state_id), 1);
    }
    for (const QString& id : catalog_ids)
        QCOMPARE(catalog_ids.count(id), 1);
    // Every state id the engine can emit must be in the taxonomy list.
    const CftcInterpretationResult result = rich_result();
    for (const auto& participant : result.participants) {
        for (const auto& state : participant.states)
            QVERIFY2(taxonomy.contains(state.state_id), qPrintable(state.state_id));
    }
    for (const auto& state : result.market_context)
        QVERIFY2(taxonomy.contains(state.state_id), qPrintable(state.state_id));
}

void TstCftcInterpretation::invalid_horizon_configuration_is_ignored() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.config.horizons_reports = {0, 2, -3, 4, 4, 13};
    const CftcInterpretationResult result = cftc_interpret(input);
    QCOMPARE(result.config.horizons_reports, QVector<int>({4, 13}));
    QCOMPARE(result.rule_set_version, cftc_interpretation_custom_rule_set_version());
    for (const auto& participant : result.participants) {
        for (const auto& state : participant.states) {
            if (state.has_horizon) {
                QVERIFY2(state.horizon_reports == 4 || state.horizon_reports == 13,
                         "only the supported 1/4/13 horizons may be interpreted");
                QVERIFY(state.horizon_reports != 2);
            }
        }
    }
    for (const auto& assessment : result.price_context) {
        QVERIFY(assessment.horizon_reports == 4 || assessment.horizon_reports == 13);
    }
    for (const auto& record : result.unavailable) {
        if (!record.has_horizon)
            continue;
        // Sustained repositioning has its own fixed 4/13 taxonomy and is not
        // governed by the configured flow horizons.
        QVERIFY((record.horizon_reports == 4 || record.horizon_reports == 13) ||
                record.state_family == QStringLiteral("SUSTAINED_REPOSITIONING"));
        QVERIFY(record.horizon_reports != 2);
    }
}

void TstCftcInterpretation::custom_config_gets_distinct_rule_set_identity() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    const CftcInterpretationResult v1 = cftc_interpret(legacy_input(legacy_series(longs, shorts), 156));
    QCOMPARE(v1.rule_set_version, cftc_interpretation_rule_set_version());
    QVERIFY(v1.config_is_default);
    const CftcInterpretationResult custom = cftc_interpret(legacy_input(legacy_series(longs, shorts), 4));
    QCOMPARE(custom.rule_set_version, cftc_interpretation_custom_rule_set_version());
    QVERIFY2(!custom.config_is_default, "an overridden configuration must not claim the plain v1 identity");
    QCOMPARE(custom.config.history_window, 4);
}

void TstCftcInterpretation::unsupported_positive_horizons_produce_no_states() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.config.horizons_reports = {2};
    const CftcInterpretationResult result = cftc_interpret(input);
    QVERIFY(result.config.horizons_reports.isEmpty());
    QCOMPARE(result.rule_set_version, cftc_interpretation_custom_rule_set_version());
    for (const auto& participant : result.participants) {
        for (const auto& state : participant.states) {
            if (!state.has_horizon)
                continue;
            // Sustained repositioning keeps its own fixed 4/13 taxonomy.
            QVERIFY2(state.state_id.startsWith(QStringLiteral("SUSTAINED_")),
                     "no configured horizon may produce a flow/price state");
            QVERIFY(state.horizon_reports == 4 || state.horizon_reports == 13);
        }
    }
    QVERIFY(result.price_context.isEmpty());
    for (const auto& record : result.unavailable) {
        if (record.has_horizon)
            QVERIFY(record.state_family == QStringLiteral("SUSTAINED_REPOSITIONING"));
    }
}

void TstCftcInterpretation::provenance_market_name_change_is_allowed() {
    // The CFTC documents contract/market name changes while the
    // contract-market code stays fixed, so the display name is descriptive
    // metadata, not the stable identity.
    QVector<CftcObservation> observations = legacy_series({500.0, 505.0}, {400.0, 400.0});
    observations[1].market = QStringLiteral("RENAMED - EXCHANGE");
    const CftcInterpretationResult result = cftc_interpret(legacy_input(observations));
    const CftcUnavailableRecord* record =
        find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY2(record == nullptr, "a market-name change with a stable contract code is valid history");
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("NET_LONG")));
    QCOMPARE(result.market, QStringLiteral("RENAMED - EXCHANGE"));
    QCOMPARE(result.contract_code, QStringLiteral("000000"));
}

void TstCftcInterpretation::provenance_mixed_contract_is_unavailable() {
    QVector<CftcObservation> observations = legacy_series({500.0, 505.0}, {400.0, 400.0});
    observations[1].contract_code = QStringLiteral("999999");
    const CftcInterpretationResult result = cftc_interpret(legacy_input(observations));
    const CftcUnavailableRecord* record =
        find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::MixedContractIdentity);
    for (const auto& participant : result.participants)
        QVERIFY(participant.states.isEmpty());
}

void TstCftcInterpretation::provenance_blank_contract_code_is_unavailable() {
    QVector<CftcObservation> observations = legacy_series({500.0, 505.0}, {400.0, 400.0});
    observations[1].contract_code.clear();
    const CftcInterpretationResult result = cftc_interpret(legacy_input(observations));
    const CftcUnavailableRecord* record =
        find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::MissingContractIdentity);
    for (const auto& participant : result.participants)
        QVERIFY(participant.states.isEmpty());

    // An entirely blank contract history is equally unspecified.
    QVector<CftcObservation> blank = legacy_series({500.0, 505.0}, {400.0, 400.0});
    for (auto& observation : blank)
        observation.contract_code.clear();
    const CftcInterpretationResult blank_result = cftc_interpret(legacy_input(blank));
    const CftcUnavailableRecord* blank_record =
        find_unavailable(blank_result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY(blank_record);
    QCOMPARE(blank_record->reason, CftcUnavailableReason::MissingContractIdentity);
}

void TstCftcInterpretation::provenance_family_mismatch_is_unavailable() {
    const QVector<CftcObservation> observations = legacy_series({500.0}, {400.0});
    for (const QString& code :
         {QStringLiteral("tff"), QStringLiteral("disaggregated"), QStringLiteral("bogus"), QStringLiteral("")}) {
        CftcInterpretationInput input = legacy_input(observations);
        input.observations_family_code = code;
        const CftcInterpretationResult result = cftc_interpret(input);
        const CftcUnavailableRecord* record =
            find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
        QVERIFY2(record, qPrintable(code));
        QCOMPARE(record->reason, CftcUnavailableReason::FamilyProvenanceMismatch);
        for (const auto& participant : result.participants)
            QVERIFY(participant.states.isEmpty());
    }
}

void TstCftcInterpretation::provenance_participant_slots_must_match_family() {
    // Disaggregated rows carry five participant slots; declaring them as
    // Legacy must fail closed instead of silently reading slot 0..2.
    const QVector<CftcObservation> disaggregated =
        make_series(CftcFamily::Disaggregated, 2, optional_values({500.0}), optional_values({400.0}));
    QCOMPARE(disaggregated.first().longs.size(), 5);
    CftcInterpretationInput input = family_input(CftcFamily::Disaggregated, disaggregated);
    declare_family(input, CftcFamily::Legacy);
    const CftcInterpretationResult result = cftc_interpret(input);
    const CftcUnavailableRecord* record =
        find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::ParticipantCountMismatch);

    // The mirror case: legacy rows declared as a five-slot family.
    CftcInterpretationInput reverse = legacy_input(legacy_series({500.0}, {400.0}));
    declare_family(reverse, CftcFamily::Tff);
    const CftcInterpretationResult reverse_result = cftc_interpret(reverse);
    const CftcUnavailableRecord* reverse_record =
        find_unavailable(reverse_result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY(reverse_record);
    QCOMPARE(reverse_record->reason, CftcUnavailableReason::ParticipantCountMismatch);
}

void TstCftcInterpretation::provenance_report_basis_is_required() {
    const QVector<CftcObservation> observations = legacy_series({500.0}, {400.0});
    for (const QString& code :
         {QStringLiteral(""), QStringLiteral("combined"), QStringLiteral("options_and_futures")}) {
        CftcInterpretationInput input = legacy_input(observations);
        input.report_basis_code = code;
        const CftcInterpretationResult result = cftc_interpret(input);
        const CftcUnavailableRecord* record =
            find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
        QVERIFY2(record, qPrintable(code));
        QCOMPARE(record->reason, CftcUnavailableReason::ReportBasisUnspecified);
        QVERIFY(result.report_basis.isEmpty());
    }

    // An explicit valid basis still interprets and is echoed truthfully.
    CftcInterpretationInput futures_only = legacy_input(observations);
    futures_only.report_basis_code = QStringLiteral("futures_only");
    const CftcInterpretationResult result = cftc_interpret(futures_only);
    QVERIFY(result.futures_only);
    QCOMPARE(result.report_basis, QStringLiteral("futures_only"));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("NET_LONG")));
}

// ── Exposure and historical relative position ───────────────────────────────

void TstCftcInterpretation::exposure_states_net_long_short_flat() {
    struct Case {
        double long_leg;
        double short_leg;
        QString state_id;
    };
    const QVector<Case> cases = {{150.0, 50.0, QStringLiteral("NET_LONG")},
                                 {50.0, 150.0, QStringLiteral("NET_SHORT")},
                                 {100.0, 100.0, QStringLiteral("NET_FLAT")}};
    for (const Case& test_case : cases) {
        const CftcInterpretationResult result =
            cftc_interpret(legacy_input(legacy_series({test_case.long_leg}, {test_case.short_leg})));
        const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
        QVERIFY(participant);
        QVERIFY(participant->net_available);
        QCOMPARE(participant->net_position, test_case.long_leg - test_case.short_leg);
        QVERIFY(has_state(participant->states, test_case.state_id));
        for (const QString& other :
             {QStringLiteral("NET_LONG"), QStringLiteral("NET_SHORT"), QStringLiteral("NET_FLAT")}) {
            if (other != test_case.state_id)
                QVERIFY2(!has_state(participant->states, other), qPrintable(other));
        }
        QVERIFY(participant->states.first().available);
        QCOMPARE(participant->states.first().scope, CftcInterpretationScope::AccountingFact);
        QCOMPARE(participant->states.first().threshold_basis, CftcEvidenceBasis::DerivedIdentity);
    }
}

void TstCftcInterpretation::historical_percentile_requires_full_default_history() {
    const QString key = QStringLiteral("non_commercial");
    QVector<double> sufficient;
    for (int i = 1; i <= 157; ++i)
        sufficient.append(static_cast<double>(i));
    const CftcInterpretationResult complete = cftc_interpret(legacy_input(legacy_percent_series(sufficient), 156));
    const CftcParticipantInterpretation* full = find_participant(complete, key);
    QVERIFY(full);
    QVERIFY(full->historical_percentile_available);
    QCOMPARE(full->percentile_reference_count, 156);
    QCOMPARE(full->percentile, 1.0);
    QVERIFY(has_state(full->states, QStringLiteral("HISTORICALLY_HIGH_NET")));

    QVector<double> insufficient;
    for (int i = 1; i <= 156; ++i)
        insufficient.append(static_cast<double>(i));
    const CftcInterpretationResult short_history =
        cftc_interpret(legacy_input(legacy_percent_series(insufficient), 156));
    const CftcParticipantInterpretation* limited = find_participant(short_history, key);
    QVERIFY(limited);
    QVERIFY2(!limited->historical_percentile_available, "155 prior reports cannot produce a 156-report percentile");
    QVERIFY(!has_state(limited->states, QStringLiteral("HISTORICALLY_HIGH_NET")));
    const CftcUnavailableRecord* record =
        find_unavailable(short_history.unavailable, QStringLiteral("HISTORICAL_RELATIVE_STATE"), key);
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::InsufficientHistory);
    QCOMPARE(record->scope, CftcInterpretationScope::HistoricalRelativeState);
}

void TstCftcInterpretation::historical_percentile_excludes_current_observation() {
    QVector<double> percents;
    for (int i = 0; i < 157; ++i)
        percents.append(1.0);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_percent_series(percents), 156));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(participant->historical_percentile_available);
    // 156 prior reports all equal the current value: the midpoint rule gives
    // (0 + 0.5 * 156) / 156, not the 1.0 an inclusive reference would give.
    QCOMPARE(participant->percentile, 0.5);
    QVERIFY(!has_state(participant->states, QStringLiteral("HISTORICALLY_HIGH_NET")));
    QVERIFY(!has_state(participant->states, QStringLiteral("HISTORICALLY_LOW_NET")));
}

void TstCftcInterpretation::historical_percentile_midpoint_ties() {
    const QVector<CftcDatedValue> series = {
        {QDate(2026, 1, 6), QStringLiteral("2026-01-06"), 1.0},
        {QDate(2026, 1, 13), QStringLiteral("2026-01-13"), 2.0},
        {QDate(2026, 1, 20), QStringLiteral("2026-01-20"), 2.0},
        {QDate(2026, 1, 27), QStringLiteral("2026-01-27"), 3.0},
        {QDate(2026, 2, 3), QStringLiteral("2026-02-03"), 2.0},
    };
    const CftcTrailingPercentile tied = cftc_trailing_percentile_at(series, QDate(2026, 2, 3), 4);
    QVERIFY(tied.available);
    QCOMPARE(tied.reference_count, 4);
    QCOMPARE(tied.percentile, 0.5);

    const QVector<CftcDatedValue> upper = {
        {QDate(2026, 1, 6), QStringLiteral("2026-01-06"), 1.0},
        {QDate(2026, 1, 13), QStringLiteral("2026-01-13"), 2.0},
        {QDate(2026, 1, 20), QStringLiteral("2026-01-20"), 2.0},
        {QDate(2026, 1, 27), QStringLiteral("2026-01-27"), 3.0},
        {QDate(2026, 2, 3), QStringLiteral("2026-02-03"), 3.0},
    };
    const CftcTrailingPercentile one_tie = cftc_trailing_percentile_at(upper, QDate(2026, 2, 3), 4);
    QVERIFY(one_tie.available);
    QCOMPARE(one_tie.percentile, 0.875);
}

void TstCftcInterpretation::historical_extreme_90_10_boundaries() {
    const QString key = QStringLiteral("non_commercial");
    CftcInterpretationConfig config = small_window_config(10);
    auto run = [&](const QVector<double>& percents) {
        CftcInterpretationInput input;
        declare_family(input, CftcFamily::Legacy);
        input.observations = legacy_percent_series(percents);
        input.config = config;
        return cftc_interpret(input);
    };

    // Exact 0.90 boundary: 9 of 10 references below, none equal.
    QVector<double> at_high = {1, 2, 3, 4, 5, 6, 7, 8, 9, 100, 10};
    const CftcInterpretationResult at_high_result = run(at_high);
    const CftcParticipantInterpretation* high = find_participant(at_high_result, key);
    QVERIFY(high);
    QVERIFY(high->historical_percentile_available);
    QCOMPARE(high->percentile, 0.90);
    QVERIFY(has_state(high->states, QStringLiteral("HISTORICALLY_HIGH_NET")));

    // Just below 0.90: 8 below plus one tie.
    QVector<double> below_high = {1, 2, 3, 4, 5, 6, 7, 8, 9, 100, 9};
    const CftcInterpretationResult below_high_result = run(below_high);
    const CftcParticipantInterpretation* below = find_participant(below_high_result, key);
    QVERIFY(below);
    QCOMPARE(below->percentile, 0.85);
    QVERIFY(!has_state(below->states, QStringLiteral("HISTORICALLY_HIGH_NET")));

    // Exact 0.10 boundary and just above it.
    QVector<double> at_low = {1, 100, 101, 102, 103, 104, 105, 106, 107, 108, 2};
    const CftcInterpretationResult at_low_result = run(at_low);
    const CftcParticipantInterpretation* low_boundary = find_participant(at_low_result, key);
    QVERIFY(low_boundary);
    QCOMPARE(low_boundary->percentile, 0.10);
    QVERIFY(has_state(low_boundary->states, QStringLiteral("HISTORICALLY_LOW_NET")));

    QVector<double> below_all = {1, 2, 100, 101, 102, 103, 104, 105, 106, 107, 3};
    const CftcInterpretationResult below_all_result = run(below_all);
    const CftcParticipantInterpretation* low = find_participant(below_all_result, key);
    QVERIFY(low);
    QCOMPARE(low->percentile, 0.20);
    QVERIFY(!has_state(low->states, QStringLiteral("HISTORICALLY_LOW_NET")));

    // Default 156-report window: 141/156 is above the 0.90 band, 140/156 is not.
    QVector<double> default_above;
    for (int i = 0; i < 141; ++i)
        default_above.append(i + 1.0);
    for (int i = 0; i < 15; ++i)
        default_above.append(1000.0 + i);
    default_above.append(500.0);
    const CftcInterpretationResult default_result =
        cftc_interpret(legacy_input(legacy_percent_series(default_above), 156));
    const CftcParticipantInterpretation* above = find_participant(default_result, key);
    QVERIFY(above);
    QCOMPARE(above->percentile_reference_count, 156);
    QVERIFY(above->percentile >= 0.90);
    QVERIFY(has_state(above->states, QStringLiteral("HISTORICALLY_HIGH_NET")));

    QVector<double> default_below;
    for (int i = 0; i < 140; ++i)
        default_below.append(i + 1.0);
    for (int i = 0; i < 16; ++i)
        default_below.append(1000.0 + i);
    default_below.append(500.0);
    const CftcInterpretationResult default_lower =
        cftc_interpret(legacy_input(legacy_percent_series(default_below), 156));
    const CftcParticipantInterpretation* lower = find_participant(default_lower, key);
    QVERIFY(lower);
    QVERIFY(lower->percentile < 0.90);
    QVERIFY(!has_state(lower->states, QStringLiteral("HISTORICALLY_HIGH_NET")));
}

void TstCftcInterpretation::severe_extreme_975_25_boundaries() {
    const QString key = QStringLiteral("non_commercial");
    CftcInterpretationConfig config = small_window_config(40);
    auto run = [&](const QVector<double>& percents) {
        CftcInterpretationInput input;
        declare_family(input, CftcFamily::Legacy);
        input.observations = legacy_percent_series(percents);
        input.config = config;
        return cftc_interpret(input);
    };

    QVector<double> severe_high;
    for (int i = 1; i <= 39; ++i)
        severe_high.append(static_cast<double>(i));
    severe_high.append(1000.0);
    severe_high.append(500.0);
    const CftcInterpretationResult severe_high_result = run(severe_high);
    const CftcParticipantInterpretation* high = find_participant(severe_high_result, key);
    QVERIFY(high);
    QCOMPARE(high->percentile, 0.975);
    QVERIFY(has_state(high->states, QStringLiteral("SEVERE_LONG_EXTREME")));

    QVector<double> below_severe;
    for (int i = 1; i <= 38; ++i)
        below_severe.append(static_cast<double>(i));
    below_severe.append(1000.0);
    below_severe.append(1001.0);
    below_severe.append(500.0);
    const CftcInterpretationResult below_severe_result = run(below_severe);
    const CftcParticipantInterpretation* not_severe = find_participant(below_severe_result, key);
    QVERIFY(not_severe);
    QVERIFY(not_severe->percentile < 0.975);
    QVERIFY(!has_state(not_severe->states, QStringLiteral("SEVERE_LONG_EXTREME")));

    QVector<double> severe_low;
    severe_low.append(-1000.0);
    for (int i = 0; i < 39; ++i)
        severe_low.append(-100.0 - i);
    severe_low.append(-500.0);
    const CftcInterpretationResult severe_low_result = run(severe_low);
    const CftcParticipantInterpretation* low = find_participant(severe_low_result, key);
    QVERIFY(low);
    QCOMPARE(low->percentile, 0.025);
    QVERIFY(has_state(low->states, QStringLiteral("SEVERE_SHORT_EXTREME")));

    QVector<double> above_severe_low;
    above_severe_low.append(-1000.0);
    above_severe_low.append(-999.0);
    for (int i = 0; i < 38; ++i)
        above_severe_low.append(-100.0 - i);
    above_severe_low.append(-500.0);
    const CftcInterpretationResult above_severe_low_result = run(above_severe_low);
    const CftcParticipantInterpretation* not_severe_low = find_participant(above_severe_low_result, key);
    QVERIFY(not_severe_low);
    QVERIFY(not_severe_low->percentile > 0.025);
    QVERIFY(!has_state(not_severe_low->states, QStringLiteral("SEVERE_SHORT_EXTREME")));
}

void TstCftcInterpretation::crowded_states_require_permitted_terminology() {
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));
    const int commercial = participant_index(CftcFamily::Legacy, QStringLiteral("commercial"));
    // 11 reports: 10 references and a current top value.
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
    for (int i = 0; i < 10; ++i) {
        longs.append(200.0 + i * 10.0);
        shorts.append(400.0 - i * 10.0);
    }
    longs.append(500.0);
    shorts.append(100.0);

    CftcInterpretationInput input;
    declare_family(input, CftcFamily::Legacy);
    input.observations = make_series(CftcFamily::Legacy, speculative, longs, shorts);
    input.config = small_window_config(10);
    const CftcInterpretationResult result = cftc_interpret(input);

    const CftcParticipantInterpretation* spec = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(spec);
    QVERIFY(spec->crowding_terminology_allowed);
    QVERIFY(has_state(spec->states, QStringLiteral("CROWDED_LONG")));
    QCOMPARE(spec->terminology, CftcTerminologyClass::BroadNonCommercial);
    QCOMPARE(spec->terminology_code, QStringLiteral("broad_non_commercial"));

    const CftcParticipantInterpretation* comm = find_participant(result, QStringLiteral("commercial"));
    QVERIFY(comm);
    QVERIFY(!comm->crowding_terminology_allowed);
    if (comm->net_position > 0.0 && comm->historical_percentile_available &&
        comm->percentile >= input.config.extreme_percentile) {
        QVERIFY(has_state(comm->states, QStringLiteral("HISTORICALLY_HIGH_NET")));
    }
    QVERIFY2(!has_state(comm->states, QStringLiteral("CROWDED_LONG")),
             "commercial exposure must keep neutral terminology");
    QVERIFY2(!has_state(comm->states, QStringLiteral("CROWDED_SHORT")),
             "commercial exposure must keep neutral terminology");
    QCOMPARE(comm->terminology, CftcTerminologyClass::CommercialNeutral);

    // The commercial participant in the same fixture has balanced 100/100
    // legs, so give it a dedicated top-percentile fixture.
    QVector<std::optional<double>> commercial_longs;
    QVector<std::optional<double>> commercial_shorts;
    for (int i = 0; i < 10; ++i) {
        commercial_longs.append(200.0 + i * 10.0);
        commercial_shorts.append(400.0 - i * 10.0);
    }
    commercial_longs.append(500.0);
    commercial_shorts.append(100.0);
    CftcInterpretationInput commercial_input;
    declare_family(commercial_input, CftcFamily::Legacy);
    commercial_input.observations = make_series(CftcFamily::Legacy, commercial, commercial_longs, commercial_shorts);
    commercial_input.config = small_window_config(10);
    const CftcInterpretationResult commercial_result = cftc_interpret(commercial_input);
    const CftcParticipantInterpretation* commercial_participant =
        find_participant(commercial_result, QStringLiteral("commercial"));
    QVERIFY(commercial_participant);
    QVERIFY(commercial_participant->historical_percentile_available);
    QCOMPARE(commercial_participant->percentile, 1.0);
    QVERIFY(has_state(commercial_participant->states, QStringLiteral("HISTORICALLY_HIGH_NET")));
    QVERIFY(!has_state(commercial_participant->states, QStringLiteral("CROWDED_LONG")));
}

// ── Exact report flows ──────────────────────────────────────────────────────

void TstCftcInterpretation::exact_report_flow_values_one_four_thirteen() {
    QVector<double> longs;
    QVector<double> shorts;
    for (int i = 0; i < 14; ++i) {
        longs.append(100.0 + 10.0 * i);
        shorts.append(200.0 - 5.0 * i);
    }
    const QVector<CftcObservation> series = legacy_series(longs, shorts);
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));

    const CftcReportFlow one = cftc_report_flow(series, speculative, 1, 13);
    QVERIFY(one.anchored);
    QVERIFY(one.continuous);
    QVERIFY(one.has_long_flow);
    QVERIFY(one.has_short_flow);
    QVERIFY(one.has_net_flow);
    QCOMPARE(one.long_flow, 1.0);
    QCOMPARE(one.short_flow, -0.5);
    QCOMPARE(one.net_flow, 1.5);
    QCOMPARE(one.has_net_share_change, true);
    QCOMPARE(one.net_share_change, 1.5);

    const CftcReportFlow four = cftc_report_flow(series, speculative, 4, 13);
    QVERIFY(four.continuous);
    QCOMPARE(four.long_flow, 4.0);
    QCOMPARE(four.short_flow, -2.0);
    QCOMPARE(four.net_flow, 6.0);
    QCOMPARE(four.net_share_change, 6.0);

    const CftcReportFlow thirteen = cftc_report_flow(series, speculative, 13, 13);
    QVERIFY(thirteen.continuous);
    QCOMPARE(thirteen.long_flow, 13.0);
    QCOMPARE(thirteen.short_flow, -6.5);
    QCOMPARE(thirteen.net_flow, 19.5);
    QCOMPARE(thirteen.net_share_change, 19.5);
    QCOMPARE(thirteen.anchor_date, kLatest.addDays(-91));
}

void TstCftcInterpretation::flow_prior_open_interest_denominator() {
    QVector<std::optional<double>> oi = {2000.0, 2000.0, 2000.0, 2000.0, 1000.0};
    QVector<std::optional<double>> longs = {100.0, 100.0, 100.0, 100.0, 200.0};
    QVector<std::optional<double>> shorts = {50.0, 50.0, 50.0, 50.0, 50.0};
    const QVector<CftcObservation> series = make_series_with_oi(
        CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")), longs, shorts, oi);
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));
    const CftcReportFlow flow = cftc_report_flow(series, speculative, 4, 4);
    QVERIFY(flow.continuous);
    // The prior-OI denominator is 2000, not the current 1000.
    QCOMPARE(flow.long_flow, 5.0);
    QCOMPARE(flow.short_flow, 0.0);
    QCOMPARE(flow.net_flow, 5.0);
    // Net share change uses each report's own Open Interest.
    QCOMPARE(flow.net_share_change, 12.5);
    QCOMPARE(flow.oi_change, -50.0);
}

void TstCftcInterpretation::missing_or_nonpositive_open_interest_stays_unavailable() {
    const QString key = QStringLiteral("non_commercial");
    const int speculative = participant_index(CftcFamily::Legacy, key);
    auto series_with_anchor_oi = [&](const std::optional<double>& anchor_oi) {
        QVector<std::optional<double>> oi;
        oi.append(anchor_oi);
        for (int i = 1; i < 14; ++i)
            oi.append(1000.0);
        QVector<std::optional<double>> longs;
        QVector<std::optional<double>> shorts;
        for (int i = 0; i < 14; ++i) {
            longs.append(500.0 + 10.0 * i);
            shorts.append(400.0);
        }
        return make_series_with_oi(CftcFamily::Legacy, speculative, longs, shorts, oi);
    };

    QVector<CftcObservation> missing = series_with_anchor_oi(std::nullopt);
    CftcInterpretationResult missing_result = cftc_interpret(legacy_input(missing));
    const CftcParticipantInterpretation* missing_participant = find_participant(missing_result, key);
    QVERIFY(missing_participant);
    QVERIFY(!has_state(missing_participant->states, QStringLiteral("LONG_ACCUMULATION"), 13));
    const CftcUnavailableRecord* missing_record =
        find_unavailable(missing_result.unavailable, QStringLiteral("GROSS_FLOW"), key, 13);
    QVERIFY(missing_record);
    QCOMPARE(missing_record->reason, CftcUnavailableReason::MissingOpenInterest);
    const CftcUnavailableRecord* missing_oi =
        find_market_unavailable(missing_result.unavailable, QStringLiteral("OI_CONTEXT"));
    QVERIFY(missing_oi);
    QCOMPARE(missing_oi->reason, CftcUnavailableReason::MissingOpenInterest);

    QVector<CftcObservation> zero = series_with_anchor_oi(0.0);
    CftcInterpretationResult zero_result = cftc_interpret(legacy_input(zero));
    const CftcParticipantInterpretation* zero_participant = find_participant(zero_result, key);
    QVERIFY(zero_participant);
    QVERIFY(!has_state(zero_participant->states, QStringLiteral("LONG_ACCUMULATION"), 13));
    const CftcUnavailableRecord* zero_record =
        find_unavailable(zero_result.unavailable, QStringLiteral("GROSS_FLOW"), key, 13);
    QVERIFY(zero_record);
    QCOMPARE(zero_record->reason, CftcUnavailableReason::NonPositiveOpenInterest);
}

void TstCftcInterpretation::missing_participant_leg_stays_unavailable() {
    const QString key = QStringLiteral("non_commercial");
    QVector<std::optional<double>> longs = {std::nullopt};
    QVector<std::optional<double>> shorts = {150.0};
    const QVector<CftcObservation> series =
        make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, key), longs, shorts);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(series));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    QVERIFY2(!participant->net_available, "a missing leg must not fabricate a net");
    QVERIFY(!participant->has_net_pct_oi);
    QVERIFY(participant->states.isEmpty());
    const CftcUnavailableRecord* exposure = find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), key);
    QVERIFY(exposure);
    QCOMPARE(exposure->reason, CftcUnavailableReason::MissingParticipantLeg);
    const CftcUnavailableRecord* historical =
        find_unavailable(result.unavailable, QStringLiteral("HISTORICAL_RELATIVE_STATE"), key);
    QVERIFY(historical);
    QCOMPARE(historical->reason, CftcUnavailableReason::MissingParticipantLeg);
}

void TstCftcInterpretation::missing_current_open_interest_keeps_raw_exposure() {
    const QString key = QStringLiteral("non_commercial");
    const int speculative = participant_index(CftcFamily::Legacy, key);
    QVector<std::optional<double>> oi;
    for (int i = 0; i < 8; ++i)
        oi.append(1000.0);
    oi.append(std::nullopt);
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
    for (int i = 0; i < 8; ++i) {
        longs.append(500.0 + 5.0 * i);
        shorts.append(400.0);
    }
    longs.append(2000.0);
    shorts.append(400.0);
    const CftcInterpretationResult result =
        cftc_interpret(legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, longs, shorts, oi)));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    // The raw net remains a truthful accounting fact; the normalized metric
    // stays absent.
    QVERIFY(participant->net_available);
    QVERIFY(participant->net_position > 0.0);
    QVERIFY(!participant->has_net_pct_oi);
    QVERIFY2(has_state(participant->states, QStringLiteral("NET_LONG")),
             "raw-net exposure is available even when Open Interest is missing");
    QVERIFY(!participant->historical_percentile_available);
    const CftcUnavailableRecord* historical =
        find_unavailable(result.unavailable, QStringLiteral("HISTORICAL_RELATIVE_STATE"), key);
    QVERIFY(historical);
    QCOMPARE(historical->reason, CftcUnavailableReason::MissingOpenInterest);
    // The Net-%OI disagreement needs the current denominator and stays unavailable.
    QVERIFY(!has_state(participant->states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), 1));
    const CftcUnavailableRecord* disagreement =
        find_unavailable(result.unavailable, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), key, 1);
    QVERIFY(disagreement);
    QCOMPARE(disagreement->reason, CftcUnavailableReason::MissingOpenInterest);
}

void TstCftcInterpretation::report_gap_breaks_horizon_continuity() {
    QVector<double> longs;
    QVector<double> shorts;
    for (int i = 0; i < 15; ++i) {
        longs.append(500.0 + 10.0 * i);
        shorts.append(400.0);
    }
    longs[14] = 5000.0; // a material current weekly move next to the gap
    const QVector<CftcObservation> gapped = remove_report(legacy_series(longs, shorts), 5);
    QCOMPARE(gapped.size(), 14);
    QVERIFY(cftc_report_sequence_continuous(gapped, 0, 4));
    QVERIFY(!cftc_report_sequence_continuous(gapped, 0, 13));

    const CftcInterpretationResult result = cftc_interpret(legacy_input(gapped));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY2(!has_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 13),
             "a gapped window must not be relabelled as a 13-report move");
    const CftcUnavailableRecord* record =
        find_unavailable(result.unavailable, QStringLiteral("GROSS_FLOW"), QStringLiteral("non_commercial"), 13);
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::BrokenReportSequence);
    // The horizon that does not span the gap stays available.
    QVERIFY(has_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 1));
}

void TstCftcInterpretation::materiality_is_strictly_trailing() {
    const QVector<CftcDatedValue> moves = {
        {QDate(2026, 1, 6), QStringLiteral("2026-01-06"), 100.0},
        {QDate(2026, 1, 13), QStringLiteral("2026-01-13"), 1.0},
        {QDate(2026, 1, 20), QStringLiteral("2026-01-20"), 2.0},
        {QDate(2026, 1, 27), QStringLiteral("2026-01-27"), 3.0},
        {QDate(2026, 2, 3), QStringLiteral("2026-02-03"), 4.0},
        {QDate(2026, 2, 10), QStringLiteral("2026-02-10"), 9999.0},
    };
    const CftcTrailingMoveRank rank = cftc_trailing_move_rank(moves, QDate(2026, 2, 3), 4);
    QVERIFY(rank.has_current);
    QVERIFY(rank.available);
    QCOMPARE(rank.reference_count, 4);
    // Reference is {1,2,3,100}: only three are below 4, so 3/4 is the rank;
    // the later 9999 and the current move itself never contribute.
    QCOMPARE(rank.rank, 0.75);

    // Default 156-report reference: exactly 156 prior moves available fires,
    // 155 does not.
    QVector<double> longs;
    QVector<double> shorts;
    for (int i = 0; i < 157; ++i) {
        longs.append(100.0 + i);
        shorts.append(200.0);
    }
    longs.append(10000.0);
    shorts.append(200.0);
    const QVector<CftcObservation> complete = legacy_series(longs, shorts);
    CftcInterpretationResult complete_result = cftc_interpret(legacy_input(complete, 156));
    const CftcParticipantInterpretation* full = find_participant(complete_result, QStringLiteral("non_commercial"));
    QVERIFY(full);
    const CftcInterpretationState* state = find_state(full->states, QStringLiteral("LONG_ACCUMULATION"), 1);
    QVERIFY(state);
    QCOMPARE(state->move_rank_reference_count, 156);
    QCOMPARE(state->move_rank, 1.0);
    QVERIFY(state->move_large);

    QVector<double> short_longs;
    QVector<double> short_shorts;
    for (int i = 0; i < 156; ++i) {
        short_longs.append(100.0 + i);
        short_shorts.append(200.0);
    }
    short_longs.append(10000.0);
    short_shorts.append(200.0);
    const QVector<CftcObservation> insufficient = legacy_series(short_longs, short_shorts);
    CftcInterpretationResult insufficient_result = cftc_interpret(legacy_input(insufficient, 156));
    const CftcParticipantInterpretation* limited =
        find_participant(insufficient_result, QStringLiteral("non_commercial"));
    QVERIFY(limited);
    QVERIFY(!has_state(limited->states, QStringLiteral("LONG_ACCUMULATION"), 1));
    const CftcUnavailableRecord* record = find_unavailable(
        insufficient_result.unavailable, QStringLiteral("GROSS_FLOW"), QStringLiteral("non_commercial"), 1);
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::InsufficientHistory);
}

void TstCftcInterpretation::materiality_75_90_boundaries() {
    auto rank_with_reference = [](const QVector<double>& reference, double current) {
        QVector<CftcDatedValue> moves;
        QDate date(2026, 1, 6);
        for (double value : reference) {
            moves.append({date, date.toString(Qt::ISODate), value});
            date = date.addDays(7);
        }
        moves.append({date, date.toString(Qt::ISODate), current});
        return cftc_trailing_move_rank(moves, date, 4);
    };
    const CftcTrailingMoveRank exact = rank_with_reference({1, 2, 3, 10}, 4);
    QVERIFY(exact.available);
    QCOMPARE(exact.rank, 0.75);
    const CftcTrailingMoveRank below = rank_with_reference({1, 2, 4, 10}, 3);
    QCOMPARE(below.rank, 0.5);
    const CftcTrailingMoveRank tied = rank_with_reference({1, 2, 3, 4}, 4);
    QCOMPARE(tied.rank, 0.875);
    const CftcTrailingMoveRank large = rank_with_reference({1, 2, 3, 4}, 5);
    QCOMPARE(large.rank, 1.0);

    // Engine level: a 0.75-rank move is material and produces the state.
    QVector<double> longs = {100.0, 101.0, 103.0, 106.0, 116.0, 120.0};
    QVector<double> shorts = {50.0, 50.0, 50.0, 50.0, 50.0, 50.0};
    CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* state = find_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 1);
    QVERIFY(state);
    QCOMPARE(state->move_rank, 0.75);
    QVERIFY(!state->move_large);

    longs = {100.0, 101.0, 103.0, 106.0, 116.0, 119.0};
    result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(!has_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 1));

    // Exact large-move boundary at 0.90: nine of ten references below.
    QVector<double> large_longs = {500.0};
    for (int i = 0; i < 9; ++i)
        large_longs.append(large_longs.last() + 1.0);
    large_longs.append(large_longs.last() + 50.0);
    large_longs.append(large_longs.last() + 10.0);
    QVector<double> flat_shorts(large_longs.size(), 400.0);
    result = cftc_interpret(legacy_input(legacy_series(large_longs, flat_shorts), 10));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* large_state =
        find_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 1);
    QVERIFY(large_state);
    QCOMPARE(large_state->move_rank, 0.90);
    QVERIFY2(large_state->move_large, "rank >= 0.90 is a large move");
}

void TstCftcInterpretation::flow_states_long_accumulation_and_liquidation() {
    QVector<double> rising = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> flat_shorts(9, 400.0);
    CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(rising, flat_shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* accumulation =
        find_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 1);
    QVERIFY(accumulation);
    QCOMPARE(accumulation->scope, CftcInterpretationScope::DescriptiveFlow);
    QCOMPARE(accumulation->threshold_basis, CftcEvidenceBasis::EngineHeuristic);
    const CftcStateMetric* move = find_metric(accumulation->metrics, QStringLiteral("move_value"));
    QVERIFY(move);
    QVERIFY(move->has_value);
    QCOMPARE(move->value, 146.5);
    QVERIFY(has_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 1));

    QVector<double> falling = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    result = cftc_interpret(legacy_input(legacy_series(falling, flat_shorts)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* liquidation = find_state(participant->states, QStringLiteral("LONG_LIQUIDATION"), 1);
    QVERIFY(liquidation);
    const CftcStateMetric* liquidation_move = find_metric(liquidation->metrics, QStringLiteral("move_value"));
    QVERIFY(liquidation_move);
    QCOMPARE(liquidation_move->value, -46.5);
    QVERIFY(!has_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 1));
}

void TstCftcInterpretation::flow_states_short_building_and_covering() {
    QVector<double> flat_longs(9, 400.0);
    QVector<double> building = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(flat_longs, building)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("SHORT_BUILDING"), 1));
    QVERIFY(has_state(participant->states, QStringLiteral("NET_SHORTWARD_SHIFT"), 1));
    QVERIFY(!has_state(participant->states, QStringLiteral("SHORT_COVERING"), 1));

    QVector<double> covering = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    result = cftc_interpret(legacy_input(legacy_series(flat_longs, covering)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("SHORT_COVERING"), 1));
    QVERIFY(has_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 1));
    QVERIFY(!has_state(participant->states, QStringLiteral("SHORT_BUILDING"), 1));
}

void TstCftcInterpretation::gross_leg_states_are_composable() {
    QVector<double> longs = {500.0, 502.0, 504.0, 506.0, 508.0, 510.0, 512.0, 514.0, 814.0};
    QVector<double> shorts = {500.0, 502.0, 504.0, 506.0, 508.0, 510.0, 512.0, 514.0, 814.0};
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY2(has_state(participant->states, QStringLiteral("LONG_ACCUMULATION"), 4),
             "both gross legs can rise at once");
    QVERIFY2(has_state(participant->states, QStringLiteral("SHORT_BUILDING"), 4), "both gross legs can rise at once");
    QVERIFY2(!has_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 4),
             "a flat net must not fabricate a net direction");
    QVERIFY2(!has_state(participant->states, QStringLiteral("NET_SHORTWARD_SHIFT"), 4),
             "a flat net must not fabricate a net direction");
}

void TstCftcInterpretation::gross_leg_states_cover_all_compositions() {
    // Longs fall while shorts rise: both gross mechanisms are true and the net
    // shifts shortward.
    QVector<double> falling_longs = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    QVector<double> rising_shorts = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(falling_longs, rising_shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("LONG_LIQUIDATION"), 4));
    QVERIFY(has_state(participant->states, QStringLiteral("SHORT_BUILDING"), 4));
    const CftcInterpretationState* shortward =
        find_state(participant->states, QStringLiteral("NET_SHORTWARD_SHIFT"), 4);
    QVERIFY(shortward);
    QVERIFY(shortward->mechanism_state_ids.contains(QStringLiteral("LONG_LIQUIDATION")));
    QVERIFY(shortward->mechanism_state_ids.contains(QStringLiteral("SHORT_BUILDING")));

    // Both legs fall: liquidation and covering coexist, net stays flat.
    QVector<double> both_falling_longs = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    QVector<double> both_falling_shorts = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    result = cftc_interpret(legacy_input(legacy_series(both_falling_longs, both_falling_shorts)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("LONG_LIQUIDATION"), 4));
    QVERIFY(has_state(participant->states, QStringLiteral("SHORT_COVERING"), 4));
    QVERIFY(!has_state(participant->states, QStringLiteral("NET_SHORTWARD_SHIFT"), 4));
    QVERIFY(!has_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 4));

    // Asymmetric reference availability: the short leg has one historical
    // report missing, so its 156-prior-move reference is unavailable while the
    // long leg's is complete. The long state must still fire and the short leg
    // must carry its own unavailable record.
    QVector<std::optional<double>> asymmetric_longs;
    QVector<std::optional<double>> asymmetric_shorts;
    for (int i = 0; i < 158; ++i) {
        asymmetric_longs.append(100.0 + i);
        asymmetric_shorts.append(i == 2 ? std::optional<double>() : std::optional<double>(300.0 + i));
    }
    asymmetric_longs.append(10000.0);
    asymmetric_shorts.append(10000.0);
    const QVector<CftcObservation> asymmetric =
        make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")),
                    asymmetric_longs, asymmetric_shorts);
    const CftcInterpretationResult asymmetric_result = cftc_interpret(legacy_input(asymmetric, 156));
    const CftcParticipantInterpretation* asymmetric_participant =
        find_participant(asymmetric_result, QStringLiteral("non_commercial"));
    QVERIFY(asymmetric_participant);
    QVERIFY2(has_state(asymmetric_participant->states, QStringLiteral("LONG_ACCUMULATION"), 1),
             "a complete long reference must not be suppressed by a short-leg gap");
    QVERIFY2(!has_state(asymmetric_participant->states, QStringLiteral("SHORT_BUILDING"), 1),
             "the short leg has no full reference and cannot be labelled material");
    const CftcUnavailableRecord* short_record =
        find_unavailable(asymmetric_result.unavailable, QStringLiteral("GROSS_FLOW"), QStringLiteral("non_commercial"),
                         1, QStringLiteral("SHORT_BUILDING"));
    QVERIFY(short_record);
    QCOMPARE(short_record->reason, CftcUnavailableReason::InsufficientHistory);
}

void TstCftcInterpretation::net_shift_states_carry_gross_mechanisms() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* longward = find_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 4);
    QVERIFY(longward);
    QVERIFY(longward->mechanism_state_ids.contains(QStringLiteral("LONG_ACCUMULATION")));

    QVector<double> falling = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    result = cftc_interpret(legacy_input(legacy_series(falling, shorts)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* shortward =
        find_state(participant->states, QStringLiteral("NET_SHORTWARD_SHIFT"), 4);
    QVERIFY(shortward);
    QVERIFY(shortward->mechanism_state_ids.contains(QStringLiteral("LONG_LIQUIDATION")));
}

void TstCftcInterpretation::sustained_4r_requires_3_of_4() {
    // One-report net steps: +30,+30,+30,0,+40,+40,+40,-1. The last four are
    // +40,+40,+40,-1 (three positive, one negative), and the material 4-report
    // net flow is 119 at the 0.75 rank.
    const QVector<double> nets = {0.0, 30.0, 60.0, 90.0, 90.0, 130.0, 170.0, 210.0, 209.0};
    const Legs legs = net_legs(nets);
    CftcInterpretationResult result = cftc_interpret(legacy_input(
        make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")),
                    legs.longs, legs.shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* state =
        find_state(participant->states, QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R"), 4);
    QVERIFY2(state, "3 of the last 4 one-report net flows positive must sustain a material 4-report move");
    QCOMPARE(state->move_rank, 0.75);
    QVERIFY(!state->move_large);

    // Same material 4-report move, but only 2 of the last 4 one-report flows
    // are positive: the sustained state must not fire.
    const QVector<double> two_positive = {0.0, 1.0, 2.0, 3.0, 4.0, 54.0, 52.0, 102.0, 100.0};
    const Legs two_legs = net_legs(two_positive);
    result = cftc_interpret(legacy_input(
        make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")),
                    two_legs.longs, two_legs.shorts)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 4));
    QVERIFY(!has_state(participant->states, QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_4R"), 4));
}

void TstCftcInterpretation::sustained_13r_requires_9_of_13() {
    QVector<double> flows(18, 0.0);
    flows[5] = -1.0;
    flows[6] = -1.0;
    flows[7] = -1.0;
    flows[8] = -1.0;
    for (int i = 9; i <= 17; ++i)
        flows[i] = 10.0;
    QVector<double> nets;
    double value = 0.0;
    for (int i = 0; i < 18; ++i) {
        value += flows[i];
        nets.append(value);
    }
    Legs legs = net_legs(nets);
    CftcInterpretationResult result = cftc_interpret(legacy_input(
        make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")),
                    legs.longs, legs.shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY2(has_state(participant->states, QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R"), 13),
             "9 of the last 13 one-report net flows positive must sustain a material 13-report move");

    // Only 8 of the last 13 one-report flows positive, with the same material
    // 13-report net flow.
    flows[17] = -1.0;
    nets.clear();
    value = 0.0;
    for (int i = 0; i < 18; ++i) {
        value += flows[i];
        nets.append(value);
    }
    legs = net_legs(nets);
    result = cftc_interpret(legacy_input(
        make_series(CftcFamily::Legacy, participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial")),
                    legs.longs, legs.shorts)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 13));
    QVERIFY(!has_state(participant->states, QStringLiteral("SUSTAINED_LONGWARD_REPOSITIONING_13R"), 13));
}

void TstCftcInterpretation::persistent_extreme_requires_three_consecutive() {
    const QString key = QStringLiteral("non_commercial");
    QVector<double> increasing;
    for (int i = 1; i <= 9; ++i)
        increasing.append(static_cast<double>(i));
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_percent_series(increasing)));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("PERSISTENT_HIGH_EXTREME")));
    QVERIFY(!has_state(participant->states, QStringLiteral("PERSISTENT_LOW_EXTREME")));

    // The third-from-last report ties its own reference maximum, so it is not
    // an extreme and the three-consecutive rule must not fire.
    QVector<double> boundary = {1, 2, 3, 4, 5, 6, 6, 8, 9};
    const CftcInterpretationResult boundary_result = cftc_interpret(legacy_input(legacy_percent_series(boundary)));
    const CftcParticipantInterpretation* boundary_participant = find_participant(boundary_result, key);
    QVERIFY(boundary_participant);
    // The current report is extreme, but the third-from-last report (0.875)
    // breaks the three-consecutive run.
    QCOMPARE(boundary_participant->percentile, 1.0);
    QVERIFY(!has_state(boundary_participant->states, QStringLiteral("PERSISTENT_HIGH_EXTREME")));

    QVector<double> decreasing;
    for (int i = 9; i >= 1; --i)
        decreasing.append(static_cast<double>(i));
    const CftcInterpretationResult low_result = cftc_interpret(legacy_input(legacy_percent_series(decreasing)));
    const CftcParticipantInterpretation* low_participant = find_participant(low_result, key);
    QVERIFY(low_participant);
    QVERIFY(has_state(low_participant->states, QStringLiteral("PERSISTENT_LOW_EXTREME")));
}
void TstCftcInterpretation::extreme_transitions_require_weekly_adjacency() {
    const QString key = QStringLiteral("non_commercial");
    QVector<double> increasing;
    for (int i = 1; i <= 9; ++i)
        increasing.append(static_cast<double>(i));

    // A missing report between the previous and the current report breaks the
    // three-consecutive run: the transition is unevaluable, not false.
    const QVector<CftcObservation> gap_before_current = remove_report(legacy_percent_series(increasing), 7);
    QCOMPARE(gap_before_current.size(), 8);
    const CftcInterpretationResult gapped = cftc_interpret(legacy_input(gap_before_current));
    const CftcParticipantInterpretation* gapped_participant = find_participant(gapped, key);
    QVERIFY(gapped_participant);
    QVERIFY2(!has_state(gapped_participant->states, QStringLiteral("PERSISTENT_HIGH_EXTREME")),
             "a gap before the current report must break the consecutive run");
    const CftcUnavailableRecord* persistence = find_unavailable(
        gapped.unavailable, QStringLiteral("EXTREME_TRANSITION"), key, -1, QStringLiteral("PERSISTENT_HIGH_EXTREME"));
    QVERIFY2(persistence, "the broken run must be reported unavailable, not silently absent");
    QCOMPARE(persistence->reason, CftcUnavailableReason::BrokenReportSequence);

    // Exit and unwind are weekly transitions: the same fall across a 14-day
    // step must not produce EXITED_HIGH_EXTREME or UNWINDING_HIGH_EXTREME; both
    // must be reported unavailable with the broken-sequence reason.
    const QVector<CftcObservation> transition_contiguous = legacy_percent_series({10, 11, 12, 13, 14, 15, 16, 17, 5});
    const CftcInterpretationResult contiguous_result = cftc_interpret(legacy_input(transition_contiguous));
    const CftcParticipantInterpretation* contiguous_participant = find_participant(contiguous_result, key);
    QVERIFY(contiguous_participant);
    QVERIFY(has_state(contiguous_participant->states, QStringLiteral("EXITED_HIGH_EXTREME")));
    QVERIFY(has_state(contiguous_participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));

    const CftcInterpretationResult gapped_transition_result =
        cftc_interpret(legacy_input(remove_report(transition_contiguous, 7)));
    const CftcParticipantInterpretation* gapped_transition = find_participant(gapped_transition_result, key);
    QVERIFY(gapped_transition);
    QVERIFY2(!has_state(gapped_transition->states, QStringLiteral("EXITED_HIGH_EXTREME")),
             "an exit cannot span a report gap");
    QVERIFY2(!has_state(gapped_transition->states, QStringLiteral("UNWINDING_HIGH_EXTREME")),
             "an unwind cannot span a report gap");
    const CftcUnavailableRecord* exit =
        find_unavailable(gapped_transition_result.unavailable, QStringLiteral("EXTREME_TRANSITION"), key, -1,
                         QStringLiteral("EXITED_HIGH_EXTREME"));
    QVERIFY(exit);
    QCOMPARE(exit->reason, CftcUnavailableReason::BrokenReportSequence);
    const CftcUnavailableRecord* unwind =
        find_unavailable(gapped_transition_result.unavailable, QStringLiteral("EXTREME_TRANSITION"), key, -1,
                         QStringLiteral("UNWINDING_HIGH_EXTREME"));
    QVERIFY(unwind);
    QCOMPARE(unwind->reason, CftcUnavailableReason::BrokenReportSequence);

    // A gap one report before the run does not break the three consecutive
    // weekly reports themselves.
    const QVector<CftcObservation> gap_outside_run = remove_report(legacy_percent_series(increasing), 5);
    const CftcInterpretationResult outside = cftc_interpret(legacy_input(gap_outside_run));
    const CftcParticipantInterpretation* outside_participant = find_participant(outside, key);
    QVERIFY(outside_participant);
    QVERIFY2(has_state(outside_participant->states, QStringLiteral("PERSISTENT_HIGH_EXTREME")),
             "a gap outside the run must not suppress the consecutive reports themselves");
}

void TstCftcInterpretation::extreme_transitions_report_insufficient_history() {
    const QString key = QStringLiteral("non_commercial");
    // Exactly one more report than the 156-report reference: the current
    // percentile exists, but the previous/exit offsets cannot be evaluated and
    // must be reported as unavailable instead of silently absent.
    QVector<double> percents;
    for (int i = 1; i <= 157; ++i)
        percents.append(static_cast<double>(i));
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_percent_series(percents), 156));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    QVERIFY(participant->historical_percentile_available);
    QVERIFY(!has_state(participant->states, QStringLiteral("PERSISTENT_HIGH_EXTREME")));
    const CftcUnavailableRecord* persistence = find_unavailable(
        result.unavailable, QStringLiteral("EXTREME_TRANSITION"), key, -1, QStringLiteral("PERSISTENT_HIGH_EXTREME"));
    QVERIFY(persistence);
    QCOMPARE(persistence->reason, CftcUnavailableReason::InsufficientHistory);
    const CftcUnavailableRecord* exit = find_unavailable(result.unavailable, QStringLiteral("EXTREME_TRANSITION"), key,
                                                         -1, QStringLiteral("EXITED_HIGH_EXTREME"));
    QVERIFY(exit);
    QCOMPARE(exit->reason, CftcUnavailableReason::InsufficientHistory);
}

void TstCftcInterpretation::extreme_exit_boundaries() {
    const QString key = QStringLiteral("non_commercial");
    const CftcInterpretationResult result =
        cftc_interpret(legacy_input(legacy_percent_series({10, 11, 12, 13, 14, 15, 16, 5})));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    const CftcInterpretationState* exit = find_state(participant->states, QStringLiteral("EXITED_HIGH_EXTREME"));
    QVERIFY(exit);
    QVERIFY(exit->has_percentile);
    QCOMPARE(exit->percentile, 0.0);

    // A current percentile exactly at 0.90 is not an exit below the band.
    const CftcInterpretationConfig wide = small_window_config(10);
    QVector<double> at_band;
    for (int i = 1; i <= 10; ++i)
        at_band.append(static_cast<double>(i));
    at_band.append(100.0);
    at_band.append(50.0);
    CftcInterpretationInput at_band_input;
    declare_family(at_band_input, CftcFamily::Legacy);
    at_band_input.observations = legacy_percent_series(at_band);
    at_band_input.config = wide;
    CftcInterpretationResult at_band_result = cftc_interpret(at_band_input);
    const CftcParticipantInterpretation* at_band_participant = find_participant(at_band_result, key);
    QVERIFY(at_band_participant);
    QCOMPARE(at_band_participant->percentile, 0.90);
    QVERIFY(!has_state(at_band_participant->states, QStringLiteral("EXITED_HIGH_EXTREME")));

    // A fall from a prior extreme to below the band is an exit.
    QVector<double> fell = at_band;
    fell.last() = 9.5;
    CftcInterpretationInput fell_input;
    declare_family(fell_input, CftcFamily::Legacy);
    fell_input.observations = legacy_percent_series(fell);
    fell_input.config = wide;
    CftcInterpretationResult fell_result = cftc_interpret(fell_input);
    const CftcParticipantInterpretation* fell_participant = find_participant(fell_result, key);
    QVERIFY(fell_participant);
    QVERIFY(fell_participant->percentile < 0.90);
    QVERIFY(has_state(fell_participant->states, QStringLiteral("EXITED_HIGH_EXTREME")));

    const CftcInterpretationResult low_result =
        cftc_interpret(legacy_input(legacy_percent_series({10, 9, 8, 7, 6, 5, 4, 15})));
    const CftcParticipantInterpretation* low_participant = find_participant(low_result, key);
    QVERIFY(low_participant);
    QVERIFY(has_state(low_participant->states, QStringLiteral("EXITED_LOW_EXTREME")));
}

void TstCftcInterpretation::extreme_unwind_boundaries() {
    const QString key = QStringLiteral("non_commercial");
    const CftcInterpretationResult result =
        cftc_interpret(legacy_input(legacy_percent_series({10, 11, 12, 13, 14, 15, 16, 5})));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));

    // The current percentile must re-enter at or below the unwind threshold.
    // With a 0.30 threshold the same 0.375 reading is not an unwind.
    CftcInterpretationInput strict_input;
    declare_family(strict_input, CftcFamily::Legacy);
    strict_input.observations = legacy_percent_series({10, 11, 12, 13, 14, 15, 16, 14});
    strict_input.config = small_window_config(4);
    strict_input.config.unwind_reentry_percentile = 0.30;
    const CftcInterpretationResult strict_result = cftc_interpret(strict_input);
    const CftcParticipantInterpretation* strict_participant = find_participant(strict_result, key);
    QVERIFY(strict_participant);
    QCOMPARE(strict_participant->percentile, 0.375);
    QVERIFY(has_state(strict_participant->states, QStringLiteral("EXITED_HIGH_EXTREME")));
    QVERIFY(!has_state(strict_participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));

    const CftcInterpretationResult low_result =
        cftc_interpret(legacy_input(legacy_percent_series({10, 9, 8, 7, 6, 5, 4, 15})));
    const CftcParticipantInterpretation* low_participant = find_participant(low_result, key);
    QVERIFY(low_participant);
    QVERIFY(has_state(low_participant->states, QStringLiteral("UNWINDING_LOW_EXTREME")));

    // Exact high re-entry boundary: percentile 0.75 is an unwind; 0.75 above a
    // 0.70 threshold is not.
    QVector<double> high_boundary;
    for (int i = 0; i <= 11; ++i)
        high_boundary.append(static_cast<double>(i));
    high_boundary.append(9.5);
    CftcInterpretationInput high_input;
    declare_family(high_input, CftcFamily::Legacy);
    high_input.observations = legacy_percent_series(high_boundary);
    high_input.config = small_window_config(8);
    const CftcInterpretationResult high_result = cftc_interpret(high_input);
    const CftcParticipantInterpretation* high_participant = find_participant(high_result, key);
    QVERIFY(high_participant);
    QCOMPARE(high_participant->percentile, 0.75);
    QVERIFY(has_state(high_participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));
    high_input.config.unwind_reentry_percentile = 0.70;
    const CftcInterpretationResult strict_high = cftc_interpret(high_input);
    const CftcParticipantInterpretation* strict_high_participant = find_participant(strict_high, key);
    QVERIFY(strict_high_participant);
    QVERIFY(!has_state(strict_high_participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));

    // Exact low re-entry boundary: percentile 0.25 is an unwind; 0.25 below a
    // 0.30 threshold is not.
    QVector<double> low_boundary;
    for (int i = 0; i <= 11; ++i)
        low_boundary.append(20.0 - i);
    low_boundary.append(10.5);
    CftcInterpretationInput low_input;
    declare_family(low_input, CftcFamily::Legacy);
    low_input.observations = legacy_percent_series(low_boundary);
    low_input.config = small_window_config(8);
    const CftcInterpretationResult low_boundary_result = cftc_interpret(low_input);
    const CftcParticipantInterpretation* low_boundary_participant = find_participant(low_boundary_result, key);
    QVERIFY(low_boundary_participant);
    QCOMPARE(low_boundary_participant->percentile, 0.25);
    QVERIFY(has_state(low_boundary_participant->states, QStringLiteral("UNWINDING_LOW_EXTREME")));
    low_input.config.low_unwind_reentry_percentile = 0.30;
    const CftcInterpretationResult strict_low = cftc_interpret(low_input);
    const CftcParticipantInterpretation* strict_low_participant = find_participant(strict_low, key);
    QVERIFY(strict_low_participant);
    QVERIFY(!has_state(strict_low_participant->states, QStringLiteral("UNWINDING_LOW_EXTREME")));
}

// ── Open Interest and concentration ─────────────────────────────────────────

void TstCftcInterpretation::oi_expansion_and_contraction() {
    QVector<std::optional<double>> expanding = {1000.0, 1010.0, 1020.0, 1030.0, 1040.0, 1050.0, 1060.0, 1070.0, 2000.0};
    QVector<std::optional<double>> longs(9, 500.0);
    QVector<std::optional<double>> shorts(9, 400.0);
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));
    CftcInterpretationResult result =
        cftc_interpret(legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, longs, shorts, expanding)));
    const CftcInterpretationState* expansion = find_state(result.market_context, QStringLiteral("OI_EXPANSION"), 1);
    QVERIFY(expansion);
    QVERIFY2(expansion->participant_key.isEmpty(), "Open Interest is a market property");
    QCOMPARE(expansion->scope, CftcInterpretationScope::MarketStructure);
    QVERIFY(expansion->metrics.first().value == 2000.0);
    QVERIFY(!has_state(result.market_context, QStringLiteral("OI_CONTRACTION"), 1));

    QVector<std::optional<double>> contracting = {1000.0, 1010.0, 1020.0, 1030.0, 1040.0,
                                                  1050.0, 1060.0, 1070.0, 500.0};
    result =
        cftc_interpret(legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, longs, shorts, contracting)));
    const CftcInterpretationState* contraction = find_state(result.market_context, QStringLiteral("OI_CONTRACTION"), 1);
    QVERIFY(contraction);
    QVERIFY(!has_state(result.market_context, QStringLiteral("OI_EXPANSION"), 1));
}

void TstCftcInterpretation::net_share_raw_disagreement() {
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));

    // Material case: a +50-contract raw net move (+5 % of prior OI, larger than
    // every prior move) while Open Interest doubles, so Net %OI falls from 10 %
    // to 7.5 %. v2 emits the state with both values and the OI change.
    QVector<std::optional<double>> oi = {1000.0, 1000.0, 1000.0, 1000.0, 1000.0,
                                         1000.0, 1000.0, 1000.0, 1000.0, 2000.0};
    QVector<std::optional<double>> longs = {600.0, 600.0, 600.0, 600.0, 600.0, 600.0, 600.0, 600.0, 600.0, 650.0};
    QVector<std::optional<double>> shorts(10, 500.0);
    CftcInterpretationResult result =
        cftc_interpret(legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, longs, shorts, oi)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcInterpretationState* disagreement =
        find_state(participant->states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), 4);
    QVERIFY(disagreement);
    QCOMPARE(disagreement->threshold_basis, CftcEvidenceBasis::EngineHeuristic);
    const CftcStateMetric* net_flow = find_metric(disagreement->metrics, QStringLiteral("net_flow"));
    const CftcStateMetric* share_change = find_metric(disagreement->metrics, QStringLiteral("net_share_change"));
    const CftcStateMetric* oi_change = find_metric(disagreement->metrics, QStringLiteral("oi_change"));
    const CftcStateMetric* net_rank = find_metric(disagreement->metrics, QStringLiteral("net_flow_rank"));
    QVERIFY(net_flow);
    QVERIFY(share_change);
    QVERIFY(oi_change && oi_change->has_value);
    QVERIFY(net_rank && net_rank->has_value);
    QCOMPARE(net_flow->value, 5.0);
    QCOMPARE(share_change->value, -2.5);
    QCOMPARE(oi_change->value, 100.0);
    QCOMPARE(net_rank->value, 1.0);

    // Noise case: prior weekly net moves of 2 % of OI, then a +0.1 % raw move
    // with a 2 % OI increase (Net %OI 10.00 % -> 9.90 %). The signs are
    // opposed, but neither move is material, so v2 emits nothing and records
    // nothing (both references exist).
    QVector<std::optional<double>> noise_oi = {1000.0, 1000.0, 1000.0, 1000.0, 1000.0,
                                               1000.0, 1000.0, 1000.0, 1000.0, 1020.0};
    QVector<std::optional<double>> noise_longs = {600.0, 620.0, 600.0, 620.0, 600.0, 620.0, 600.0, 620.0, 600.0, 601.0};
    result = cftc_interpret(
        legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, noise_longs, shorts, noise_oi)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY2(!has_state(participant->states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), 1),
             "an opposed-but-trivial move is not narrated");
    QVERIFY(!find_unavailable(result.unavailable, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"),
                              QStringLiteral("non_commercial"), 1));

    // Too-short reference: the gate cannot be decided, so the state is
    // recorded unavailable instead of being emitted or silently dropped.
    QVector<std::optional<double>> short_oi = {1000.0, 1000.0, 1000.0, 1000.0, 2000.0};
    QVector<std::optional<double>> short_longs = {600.0, 600.0, 600.0, 600.0, 650.0};
    QVector<std::optional<double>> short_shorts(5, 500.0);
    result = cftc_interpret(
        legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, short_longs, short_shorts, short_oi)));
    participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(!has_state(participant->states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), 4));
    const CftcUnavailableRecord* record = find_unavailable(
        result.unavailable, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), QStringLiteral("non_commercial"), 4);
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::InsufficientHistory);
}

void TstCftcInterpretation::net_share_raw_disagreement_requires_opposite_signs() {
    const QString key = QStringLiteral("non_commercial");
    const int speculative = participant_index(CftcFamily::Legacy, key);

    // Same sign: raw net rises and Net %OI also rises.
    QVector<std::optional<double>> constant_oi(5, 1000.0);
    QVector<std::optional<double>> raw_longs = {600.0, 600.0, 600.0, 600.0, 650.0};
    QVector<std::optional<double>> flat_shorts(5, 500.0);
    CftcInterpretationResult result = cftc_interpret(
        legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, raw_longs, flat_shorts, constant_oi)));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    QVERIFY(!has_state(participant->states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), 4));

    // Zero raw net flow with a changing denominator: not a disagreement.
    QVector<std::optional<double>> expanded_oi = {1000.0, 1000.0, 1000.0, 1000.0, 2000.0};
    QVector<std::optional<double>> flat_longs(5, 600.0);
    result = cftc_interpret(
        legacy_input(make_series_with_oi(CftcFamily::Legacy, speculative, flat_longs, flat_shorts, expanded_oi)));
    participant = find_participant(result, key);
    QVERIFY(participant);
    QVERIFY(!has_state(participant->states, QStringLiteral("NET_SHARE_RAW_DISAGREEMENT"), 4));
}

void TstCftcInterpretation::concentration_states_are_market_level() {
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));
    QVector<CftcObservation> series;
    const QVector<double> concentration = {10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 90.0};
    for (int i = 0; i < concentration.size(); ++i) {
        series.append(family_observation(CftcFamily::Legacy, kLatest.addDays(-7LL * (concentration.size() - 1 - i)),
                                         1000.0, speculative, 500.0, 400.0, concentration[i]));
    }
    const CftcInterpretationResult result = cftc_interpret(legacy_input(series));
    const CftcInterpretationState* high =
        find_state(result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION"));
    QVERIFY(high);
    QVERIFY2(high->participant_key.isEmpty(), "concentration is a market/report property");
    QCOMPARE(high->scope, CftcInterpretationScope::MarketStructure);
    QCOMPARE(high->percentile, 1.0);
    QCOMPARE(high->percentile_reference_count, 9);
    const CftcInterpretationState* rising = find_state(result.market_context, QStringLiteral("CONCENTRATION_RISING"));
    QVERIFY(rising);
    QVERIFY(rising->participant_key.isEmpty());
    QCOMPARE(rising->move_rank, 1.0);

    QCOMPARE(result.concentration.size(), 8);
    for (const auto& assessment : result.concentration)
        QVERIFY(!assessment.field_key.isEmpty());

    // No participant may carry a concentration state.
    for (const auto& participant : result.participants) {
        for (const auto& state : participant.states) {
            QVERIFY2(!cftc_is_market_concentration_state(state.state_id),
                     "concentration must never be assigned to a participant");
        }
    }
}

void TstCftcInterpretation::concentration_missing_field_stays_unavailable() {
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));
    QVector<std::optional<double>> longs(6, 500.0);
    QVector<std::optional<double>> shorts(6, 400.0);
    const CftcInterpretationResult result =
        cftc_interpret(legacy_input(make_series(CftcFamily::Legacy, speculative, longs, shorts)));
    QVERIFY(!has_state(result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION")));
    QVERIFY(!has_state(result.market_context, QStringLiteral("CONCENTRATION_RISING")));
    const CftcUnavailableRecord* record = find_market_unavailable(result.unavailable, QStringLiteral("CONCENTRATION"));
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::MissingConcentrationField);

    // An unknown selected field must also report its own unavailability.
    CftcInterpretationInput unknown_field = legacy_input(make_series(CftcFamily::Legacy, speculative, longs, shorts));
    unknown_field.config.primary_concentration_field = QStringLiteral("concentration_not_a_field");
    const CftcInterpretationResult unknown_result = cftc_interpret(unknown_field);
    const CftcUnavailableRecord* unknown_record =
        find_market_unavailable(unknown_result.unavailable, QStringLiteral("CONCENTRATION"));
    QVERIFY(unknown_record);
    QCOMPARE(unknown_record->reason, CftcUnavailableReason::MissingConcentrationField);
    QVERIFY(!has_state(unknown_result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION")));
}

void TstCftcInterpretation::concentration_states_report_their_own_availability() {
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));
    // A percentile-qualified field whose change cannot be evaluated must not
    // read as an unavailable concentration family at the same time. The full
    // trailing reference needs at least the Batch 1 minimum of eight readings.
    QVector<CftcObservation> series;
    const QVector<double> concentration = {10.0, 11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 0.0, 90.0};
    for (int i = 0; i < concentration.size(); ++i) {
        const std::optional<double> value = i == 8 ? std::nullopt : std::optional<double>(concentration[i]);
        series.append(family_observation(CftcFamily::Legacy, kLatest.addDays(-7LL * (concentration.size() - 1 - i)),
                                         1000.0, speculative, 500.0, 400.0, value));
    }
    const CftcInterpretationResult result = cftc_interpret(legacy_input(series));
    const CftcInterpretationState* high =
        find_state(result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION"));
    QVERIFY(high);
    QCOMPARE(high->percentile, 1.0);
    QCOMPARE(high->percentile_reference_count, 8);
    QVERIFY(!has_state(result.market_context, QStringLiteral("CONCENTRATION_RISING")));
    const CftcUnavailableRecord* rising = find_unavailable(result.unavailable, QStringLiteral("CONCENTRATION"),
                                                           QString(), -1, QStringLiteral("CONCENTRATION_RISING"));
    QVERIFY(rising);
    QCOMPARE(rising->reason, CftcUnavailableReason::MissingConcentrationField);
    QVERIFY2(find_unavailable(result.unavailable, QStringLiteral("CONCENTRATION"), QString(), -1,
                              QStringLiteral("HIGH_MARKET_CONCENTRATION")) == nullptr,
             "an emitted HIGH state must not also be reported unavailable");
}

void TstCftcInterpretation::high_concentration_uses_full_trailing_reference() {
    const int speculative = participant_index(CftcFamily::Legacy, QStringLiteral("non_commercial"));
    const QString field = QStringLiteral("concentration_gross_4_long");
    auto build = [&](const QVector<double>& values) {
        QVector<CftcObservation> series;
        for (int i = 0; i < values.size(); ++i) {
            series.append(family_observation(CftcFamily::Legacy, kLatest.addDays(-7LL * (values.size() - 1 - i)),
                                             1000.0, speculative, 500.0, 400.0, values[i]));
        }
        return series;
    };

    // 200 prior readings: the most recent 156 are all below the current value,
    // but the full reference (which includes 44 older highs) is not extreme.
    // A 156-report window would wrongly report 1.0 and fire HIGH.
    QVector<double> windowed_high;
    for (int i = 0; i < 200; ++i)
        windowed_high.append(i < 44 ? 1000.0 : 1.0);
    windowed_high.append(500.0);
    CftcInterpretationResult result = cftc_interpret(legacy_input(build(windowed_high), 156));
    const CftcConcentrationAssessment* assessment = find_concentration(result, field);
    QVERIFY(assessment);
    QVERIFY(assessment->has_percentile);
    QCOMPARE(assessment->percentile_reference_count, 200);
    QCOMPARE(assessment->percentile, 0.78);
    QVERIFY2(!has_state(result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION")),
             "the full trailing reference, not the 156-report window, classifies concentration");

    // 200 prior readings where the full reference reaches 0.90 only because of
    // 44 older lows outside the 156-report window (window: 136/156 = 0.872).
    QVector<double> full_high;
    for (int i = 0; i < 200; ++i)
        full_high.append(i < 180 ? 1.0 : 1000.0);
    full_high.append(500.0);
    result = cftc_interpret(legacy_input(build(full_high), 156));
    assessment = find_concentration(result, field);
    QVERIFY(assessment);
    QVERIFY(assessment->has_percentile);
    QCOMPARE(assessment->percentile_reference_count, 200);
    QCOMPARE(assessment->percentile, 0.90);
    QVERIFY(has_state(result.market_context, QStringLiteral("HIGH_MARKET_CONCENTRATION")));
}

// ── Price versus positioning ────────────────────────────────────────────────

void TstCftcInterpretation::price_position_moving_together_up_and_down() {
    const QString key = QStringLiteral("non_commercial");
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 300});
    input.price_source = QStringLiteral("TEST roll-free series");
    CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* up = find_price(result, key, 4);
    QVERIFY(up);
    QVERIFY(up->evaluated);
    QCOMPARE(up->state_id, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"));
    QCOMPARE(up->scope, CftcInterpretationScope::ContemporaneousRelation);
    QCOMPARE(up->threshold_basis, CftcEvidenceBasis::EngineHeuristic);
    QCOMPARE(up->price_move, 196.0);
    QVERIFY(up->price_material);
    QVERIFY(up->positioning_material);
    QCOMPARE(up->mechanism_state_ids, QStringList({QStringLiteral("LONG_ACCUMULATION")}));
    QVERIFY(!result.price_continuous_proxy);

    QVector<double> rising_shorts = {400.0, 405.0, 410.0, 415.0, 420.0, 425.0, 430.0, 435.0, 1400.0};
    QVector<double> flat_longs(9, 500.0);
    input = legacy_input(legacy_series(flat_longs, rising_shorts));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 40});
    input.price_source = QStringLiteral("TEST spot index");
    input.price_spot_index = true;
    result = cftc_interpret(input);
    const CftcPricePositionAssessment* down = find_price(result, key, 4);
    QVERIFY(down);
    QVERIFY(down->evaluated);
    QCOMPARE(down->state_id, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_DOWN"));
    QVERIFY(down->price_move < 0.0);
    QVERIFY(result.price_spot_index);
    QVERIFY(!result.price_continuous_proxy);
}

void TstCftcInterpretation::price_position_divergence_both_directions() {
    const QString key = QStringLiteral("non_commercial");
    // Price up while positioning shifts shortward.
    QVector<double> falling = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    QVector<double> flat_shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(falling, flat_shorts));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 300});
    input.price_source = QStringLiteral("TEST roll-free series");
    CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* up_divergence = find_price(result, key, 4);
    QVERIFY(up_divergence);
    QCOMPARE(up_divergence->state_id, QStringLiteral("PRICE_UP_POSITIONING_DOWN_DIVERGENCE"));
    QVERIFY(up_divergence->price_move > 0.0);
    QVERIFY(up_divergence->positioning_move < 0.0);

    // Price down while positioning shifts longward.
    QVector<double> flat_longs(9, 400.0);
    QVector<double> covering = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    input = legacy_input(legacy_series(flat_longs, covering));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 40});
    input.price_source = QStringLiteral("TEST roll-free series");
    result = cftc_interpret(input);
    const CftcPricePositionAssessment* down_divergence = find_price(result, key, 4);
    QVERIFY(down_divergence);
    QCOMPARE(down_divergence->state_id, QStringLiteral("PRICE_DOWN_POSITIONING_UP_DIVERGENCE"));
    QVERIFY(down_divergence->price_move < 0.0);
    QVERIFY(down_divergence->positioning_move > 0.0);
}

void TstCftcInterpretation::divergence_mechanism_from_gross_legs() {
    const QString key = QStringLiteral("non_commercial");
    // Long liquidation drives the shortward positioning move.
    QVector<double> falling = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    QVector<double> flat_shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(falling, flat_shorts));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 300});
    input.price_source = QStringLiteral("TEST roll-free series");
    CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* divergence = find_price(result, key, 4);
    QVERIFY(divergence);
    QVERIFY(divergence->mechanism_state_ids.contains(QStringLiteral("LONG_LIQUIDATION")));
    QVERIFY2(!divergence->mechanism_state_ids.contains(QStringLiteral("SHORT_BUILDING")),
             "a flat short leg is not new short building");

    // Short covering drives the longward positioning move.
    QVector<double> flat_longs(9, 400.0);
    QVector<double> covering = {700.0, 695.0, 690.0, 685.0, 680.0, 675.0, 670.0, 665.0, 200.0};
    input = legacy_input(legacy_series(flat_longs, covering));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 40});
    input.price_source = QStringLiteral("TEST roll-free series");
    result = cftc_interpret(input);
    divergence = find_price(result, key, 4);
    QVERIFY(divergence);
    QVERIFY(divergence->mechanism_state_ids.contains(QStringLiteral("SHORT_COVERING")));
    QVERIFY2(!divergence->mechanism_state_ids.contains(QStringLiteral("LONG_ACCUMULATION")),
             "a flat long leg is not new accumulation");
}

void TstCftcInterpretation::non_material_price_or_positioning_yields_no_relationship() {
    const QString key = QStringLiteral("non_commercial");
    // Material positioning, non-material price: no relationship state, but the
    // assessment is still evaluated and reports the materiality flags.
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.prices = price_points({100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 100.0});
    input.price_source = QStringLiteral("TEST roll-free series");
    CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* quiet_price = find_price(result, key, 4);
    QVERIFY(quiet_price);
    QVERIFY(quiet_price->evaluated);
    QVERIFY(!quiet_price->price_material);
    QVERIFY(!quiet_price->has_state);
    QVERIFY(quiet_price->state_id.isEmpty());
    QVERIFY(quiet_price->has_price_move);
    QCOMPARE(quiet_price->price_move, 0.0);

    // Material price, non-material positioning.
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 300});
    QVector<double> flat_longs(9, 500.0);
    QVector<double> flat_shorts(9, 400.0);
    input.observations = legacy_series(flat_longs, flat_shorts);
    input.price_source = QStringLiteral("TEST roll-free series");
    result = cftc_interpret(input);
    const CftcPricePositionAssessment* quiet_position = find_price(result, key, 4);
    QVERIFY(quiet_position);
    QVERIFY(quiet_position->evaluated);
    QVERIFY(quiet_position->price_material);
    QVERIFY(!quiet_position->positioning_material);
    QVERIFY(!quiet_position->has_state);
}

void TstCftcInterpretation::missing_price_context_stays_unavailable() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    QVERIFY(!result.price_requested);
    const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), 4);
    QVERIFY(assessment);
    QVERIFY2(!assessment->evaluated, "missing price context is unavailable, not flat");
    QCOMPARE(assessment->reason, CftcUnavailableReason::MissingPriceContext);
    QVERIFY(!assessment->has_state);
}

void TstCftcInterpretation::unspecified_price_source_stays_unavailable() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 300});
    const CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), 4);
    QVERIFY(assessment);
    QVERIFY(!assessment->evaluated);
    QCOMPARE(assessment->reason, CftcUnavailableReason::UnspecifiedPriceSource);
}

void TstCftcInterpretation::stale_price_context_stays_unavailable() {
    const CftcReportPrice stale = cftc_report_price({{kLatest.addDays(-40), 100.0}}, kLatest);
    QVERIFY(!stale.available);
    const CftcReportPrice contemporaneous = cftc_report_price({{kLatest.addDays(-3), 100.0}}, kLatest);
    QVERIFY(contemporaneous.available);
    QCOMPARE(contemporaneous.close, 100.0);
    QCOMPARE(contemporaneous.date, kLatest.addDays(-3));

    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    QVector<CftcPricePoint> prices;
    for (int i = 0; i < 9; ++i)
        prices.append({kLatest.addDays(-7LL * (8 - i) - 30), 100.0 + i});
    input.prices = prices;
    input.price_source = QStringLiteral("TEST roll-free series");
    const CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), 4);
    QVERIFY(assessment);
    QVERIFY(!assessment->evaluated);
    QCOMPARE(assessment->reason, CftcUnavailableReason::PriceContextStale);
}

void TstCftcInterpretation::unusable_price_context_stays_unavailable() {
    // No observation at or before the report date.
    const CftcReportPrice incomplete = cftc_report_price({{kLatest.addDays(7), 100.0}}, kLatest);
    QVERIFY(!incomplete.available);
    QCOMPARE(incomplete.reason, CftcUnavailableReason::PriceHistoryIncomplete);
    // A zero or non-finite close is not a usable price.
    const CftcReportPrice zero = cftc_report_price({{kLatest, 0.0}}, kLatest);
    QVERIFY(!zero.available);
    QCOMPARE(zero.reason, CftcUnavailableReason::PriceContextUnusable);
    const CftcReportPrice negative = cftc_report_price({{kLatest, -5.0}}, kLatest);
    QVERIFY(!negative.available);
    QCOMPARE(negative.reason, CftcUnavailableReason::PriceContextUnusable);

    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 0});
    input.price_source = QStringLiteral("TEST roll-free series");
    const CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), 4);
    QVERIFY(assessment);
    QVERIFY(!assessment->evaluated);
    QCOMPARE(assessment->reason, CftcUnavailableReason::PriceContextUnusable);
}
// ── Semantics, truthfulness and determinism ─────────────────────────────────

void TstCftcInterpretation::participant_terminology_maps_each_family() {
    QCOMPARE(cftc_participant_terminology(CftcFamily::Legacy, QStringLiteral("non_commercial")),
             CftcTerminologyClass::BroadNonCommercial);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Legacy, QStringLiteral("commercial")),
             CftcTerminologyClass::CommercialNeutral);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Legacy, QStringLiteral("non_reportable")),
             CftcTerminologyClass::NonReportableNeutral);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Disaggregated, QStringLiteral("managed_money")),
             CftcTerminologyClass::ManagedMoney);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Disaggregated, QStringLiteral("producer_merchant")),
             CftcTerminologyClass::ProducerMerchant);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Disaggregated, QStringLiteral("swap_dealer")),
             CftcTerminologyClass::SwapDealer);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Tff, QStringLiteral("leveraged_funds")),
             CftcTerminologyClass::LeveragedFunds);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Tff, QStringLiteral("asset_manager")),
             CftcTerminologyClass::AssetManagerInstitutional);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Tff, QStringLiteral("dealer")),
             CftcTerminologyClass::DealerIntermediary);

    QVERIFY(cftc_crowding_terminology_allowed(CftcFamily::Legacy, QStringLiteral("non_commercial")));
    QVERIFY(cftc_crowding_terminology_allowed(CftcFamily::Disaggregated, QStringLiteral("managed_money")));
    QVERIFY(cftc_crowding_terminology_allowed(CftcFamily::Tff, QStringLiteral("leveraged_funds")));
    QVERIFY(!cftc_crowding_terminology_allowed(CftcFamily::Legacy, QStringLiteral("commercial")));
    QVERIFY(!cftc_crowding_terminology_allowed(CftcFamily::Legacy, QStringLiteral("non_reportable")));
    QVERIFY(!cftc_crowding_terminology_allowed(CftcFamily::Disaggregated, QStringLiteral("producer_merchant")));
    QVERIFY(!cftc_crowding_terminology_allowed(CftcFamily::Disaggregated, QStringLiteral("swap_dealer")));
    QVERIFY(!cftc_crowding_terminology_allowed(CftcFamily::Tff, QStringLiteral("asset_manager")));
    QVERIFY(!cftc_crowding_terminology_allowed(CftcFamily::Tff, QStringLiteral("dealer")));

    // A key from another family is never silently reassigned.
    QCOMPARE(cftc_participant_terminology(CftcFamily::Tff, QStringLiteral("commercial")),
             CftcTerminologyClass::Unknown);
    QCOMPARE(cftc_participant_terminology(CftcFamily::Legacy, QStringLiteral("managed_money")),
             CftcTerminologyClass::Unknown);
}

void TstCftcInterpretation::terminology_caveats_are_exposed() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);

    const CftcInterpretationResult legacy = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    const CftcParticipantInterpretation* non_commercial = find_participant(legacy, QStringLiteral("non_commercial"));
    QVERIFY(non_commercial);
    QCOMPARE(non_commercial->terminology_caveat_code, QStringLiteral("broad_category_caveat"));
    const CftcParticipantInterpretation* commercial = find_participant(legacy, QStringLiteral("commercial"));
    QVERIFY(commercial);
    QVERIFY(commercial->terminology_caveat_code.isEmpty());

    CftcInterpretationInput tff_input;
    declare_family(tff_input, CftcFamily::Tff);
    tff_input.observations = make_series(CftcFamily::Tff, 2, optional_values(longs), optional_values(shorts));
    tff_input.config = small_window_config(4);
    const CftcInterpretationResult tff = cftc_interpret(tff_input);
    const CftcParticipantInterpretation* leveraged = find_participant(tff, QStringLiteral("leveraged_funds"));
    QVERIFY(leveraged);
    QCOMPARE(leveraged->terminology_caveat_code, QStringLiteral("not_all_outright_speculation_caveat"));
    const CftcParticipantInterpretation* dealer = find_participant(tff, QStringLiteral("dealer"));
    QVERIFY(dealer);
    QVERIFY(dealer->terminology_caveat_code.isEmpty());
}

void TstCftcInterpretation::legacy_disaggregated_tff_applicability() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);

    const QVector<CftcFamily> families = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    for (CftcFamily family : families) {
        CftcInterpretationInput input;
        declare_family(input, family);
        // Index 2 is a real member of every family (legacy non-reportable,
        // disaggregated managed money, TFF leveraged funds), so the state
        // identity assertions always exercise an existing participant.
        input.observations = make_series(family, 2, optional_values(longs), optional_values(shorts));
        input.config = small_window_config(4);
        const CftcInterpretationResult result = cftc_interpret(input);
        QCOMPARE(result.family, family);
        QCOMPARE(result.family_code, cftc_family_code(family));
        const auto expected = cftc_family_participants(family);
        QCOMPARE(result.participants.size(), expected.size());
        for (int i = 0; i < expected.size(); ++i) {
            QCOMPARE(result.participants[i].participant_key, expected[i].key);
            QCOMPARE(result.participants[i].terminology, cftc_participant_terminology(family, expected[i].key));
            for (const auto& state : result.participants[i].states)
                QCOMPARE(state.participant_key, expected[i].key);
        }
    }
}

void TstCftcInterpretation::prohibited_cross_family_terminology() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input;
    declare_family(input, CftcFamily::Tff);
    input.observations = make_series(CftcFamily::Tff, 2, optional_values(longs), optional_values(shorts));
    input.config = small_window_config(4);
    const CftcInterpretationResult result = cftc_interpret(input);
    for (const auto& participant : result.participants) {
        QVERIFY2(participant.participant_key != QStringLiteral("commercial"),
                 "TFF must never be reconstructed into Legacy categories");
        QVERIFY2(participant.participant_key != QStringLiteral("non_commercial"),
                 "TFF must never be reconstructed into Legacy categories");
        QVERIFY(participant.terminology != CftcTerminologyClass::CommercialNeutral);
        QVERIFY(participant.terminology != CftcTerminologyClass::BroadNonCommercial);
        QVERIFY2(!participant.terminology_code.contains(QStringLiteral("commercial")),
                 "TFF categories use their own terminology");
        QVERIFY2(!participant.terminology_code.contains(QStringLiteral("speculat")), "TFF has no speculator category");
    }
    const CftcParticipantInterpretation* leveraged = find_participant(result, QStringLiteral("leveraged_funds"));
    QVERIFY(leveraged);
    QCOMPARE(leveraged->terminology_code, QStringLiteral("leveraged_funds"));
    const CftcParticipantInterpretation* dealer = find_participant(result, QStringLiteral("dealer"));
    QVERIFY(dealer);
    QCOMPARE(dealer->terminology_code, QStringLiteral("dealer_intermediary"));
    QVERIFY(!dealer->crowding_terminology_allowed);
}

void TstCftcInterpretation::result_contains_no_directional_or_predictive_output() {
    const CftcInterpretationResult result = rich_result();
    QVERIFY(!result.participants.isEmpty());
    QVERIFY(!result.market_context.isEmpty());
    const QRegularExpression forbidden(
        QStringLiteral("\\b(buy|sell|hold|bullish|bearish|confidence|expected|forecast|signal|recommendation|predict|"
                       "prediction|reversal|overbought|oversold|squeeze|smart|dumb|retail)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QStringList identifiers = result_identifiers(result);
    for (const QString& identifier : identifiers) {
        const QRegularExpressionMatch match = forbidden.match(identifier);
        QVERIFY2(!match.hasMatch(), qPrintable(identifier + QStringLiteral(" -> ") + match.captured(0)));
    }
    for (const auto& rule : cftc_interpretation_rule_catalog()) {
        const QStringList fields = {rule.id, rule.condition, cftc_interpretation_scope_code(rule.scope),
                                    cftc_evidence_basis_code(rule.basis)};
        for (const QString& field : fields) {
            QVERIFY2(!forbidden.match(field).hasMatch(), qPrintable(field));
        }
    }
    // The scope vocabulary itself contains no forecast/signal/expected-return scope.
    const QStringList scopes = {cftc_interpretation_scope_code(CftcInterpretationScope::AccountingFact),
                                cftc_interpretation_scope_code(CftcInterpretationScope::HistoricalRelativeState),
                                cftc_interpretation_scope_code(CftcInterpretationScope::DescriptiveFlow),
                                cftc_interpretation_scope_code(CftcInterpretationScope::MarketStructure),
                                cftc_interpretation_scope_code(CftcInterpretationScope::ContemporaneousRelation)};
    for (const QString& scope : scopes)
        QVERIFY2(!forbidden.match(scope).hasMatch(), qPrintable(scope));
}

void TstCftcInterpretation::deterministic_repeatability() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    QVector<CftcObservation> observations = legacy_series(longs, shorts);
    for (int i = 0; i < observations.size(); ++i)
        observations[i].concentration_gross_4_long = 10.0 + 5.0 * i;
    CftcInterpretationInput input = legacy_input(observations);
    input.prices = price_points({100, 101, 102, 103, 104, 105, 106, 107, 300});
    input.price_source = QStringLiteral("TEST roll-free series");
    const CftcInterpretationResult first = cftc_interpret(input);
    const CftcInterpretationResult second = cftc_interpret(input);
    QCOMPARE(result_signature(first), result_signature(second));
    QVERIFY(!result_signature(first).isEmpty());
}

void TstCftcInterpretation::stale_as_of_report_is_not_current() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.as_of = kLatest.addDays(3); // between two reports: not a report date
    const CftcInterpretationResult result = cftc_interpret(input);
    QVERIFY(result.current_report_stale);
    QVERIFY(!result.report_date_available);
    QCOMPARE(result.report_date, kLatest.addDays(3));
    QCOMPARE(result.latest_observation_date, kLatest);
    for (const auto& participant : result.participants)
        QVERIFY(participant.states.isEmpty());
    const CftcUnavailableRecord* stale =
        find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY(stale);
    QCOMPARE(stale->reason, CftcUnavailableReason::StaleCurrentReport);

    CftcInterpretationInput before_first = legacy_input(legacy_series(longs, shorts));
    before_first.as_of = kLatest.addDays(-7LL * 20);
    const CftcInterpretationResult before_result = cftc_interpret(before_first);
    QVERIFY(before_result.current_report_stale);
}

void TstCftcInterpretation::empty_history_is_unavailable() {
    CftcInterpretationInput input;
    input.family = CftcFamily::Legacy;
    input.as_of = kLatest;
    const CftcInterpretationResult result = cftc_interpret(input);
    QVERIFY(result.participants.size() == cftc_family_participants(CftcFamily::Legacy).size());
    for (const auto& participant : result.participants) {
        QVERIFY(!participant.net_available);
        QVERIFY(participant.states.isEmpty());
    }
    const CftcUnavailableRecord* record =
        find_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"), QString());
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::NoObservations);
}

void TstCftcInterpretation::raw_flow_readings_cover_every_horizon() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 540.0, 545.0};
    QVector<double> shorts(10, 400.0);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QCOMPARE(participant->flow_readings.size(), 3);
    for (int horizon : {1, 4, 13}) {
        const CftcHorizonFlowReading* reading = nullptr;
        for (const auto& candidate : participant->flow_readings) {
            if (candidate.horizon_reports == horizon)
                reading = &candidate;
        }
        QVERIFY2(reading != nullptr, qPrintable(QStringLiteral("missing reading at %1").arg(horizon)));
        if (horizon == 13) {
            QVERIFY(!reading->evaluated);
            QCOMPARE(reading->reason, CftcUnavailableReason::InsufficientHorizonHistory);
        } else {
            QVERIFY(reading->evaluated);
            QVERIFY(reading->has_long_flow);
            QVERIFY(reading->has_short_flow);
            QVERIFY(reading->has_net_flow);
            QVERIFY(reading->has_net_rank);
            QVERIFY(reading->net_rank_reference_count > 0);
        }
    }
}

void TstCftcInterpretation::below_threshold_flow_reading_keeps_its_value() {
    // Prior moves are large and varied, so the tiny last change ranks low and
    // no material state fires, while the measurement itself stays emitted.
    const QVector<double> longs = {500.0, 550.0, 610.0, 680.0, 760.0, 850.0, 950.0, 1060.0, 1180.0, 1185.0};
    QVector<double> shorts(10, 400.0);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcHorizonFlowReading* one = nullptr;
    for (const auto& reading : participant->flow_readings) {
        if (reading.horizon_reports == 1)
            one = &reading;
    }
    QVERIFY(one != nullptr);
    QVERIFY(one->evaluated);
    QVERIFY(one->has_net_flow);
    QVERIFY(one->net_flow != 0.0);
    QVERIFY(one->has_net_rank);
    QVERIFY(one->net_rank < 0.75);
    // No material state fired, yet the measurement is still emitted.
    QVERIFY(!find_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 1));
    QVERIFY(!find_state(participant->states, QStringLiteral("NET_SHORTWARD_SHIFT"), 1));
}

void TstCftcInterpretation::positioning_measurement_survives_missing_price() {
    QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    QVector<double> shorts(9, 400.0);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    QVERIFY(!result.price_requested);
    const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), 4);
    QVERIFY(assessment);
    QVERIFY2(!assessment->evaluated, "missing price context is unavailable, not flat");
    QCOMPARE(assessment->reason, CftcUnavailableReason::MissingPriceContext);
    // The CFTC-only positioning measurement is still emitted: a missing price
    // must not suppress it from the presentation.
    QVERIFY(assessment->has_positioning_move);
    QVERIFY(assessment->has_positioning_move_rank);
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    const CftcHorizonFlowReading* reading = nullptr;
    for (const auto& candidate : participant->flow_readings) {
        if (candidate.horizon_reports == 4)
            reading = &candidate;
    }
    QVERIFY(reading != nullptr);
    QVERIFY(reading->has_net_flow);
    QCOMPARE(assessment->positioning_move, reading->net_flow);
}

void TstCftcInterpretation::open_interest_readings_are_emitted() {
    QVector<double> longs(10, 500.0);
    QVector<double> shorts(10, 400.0);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(legacy_series(longs, shorts)));
    QVERIFY(result.open_interest_readings.size() >= 2);
    bool saw_evaluated = false;
    for (const auto& reading : result.open_interest_readings) {
        if (reading.evaluated) {
            saw_evaluated = true;
            QVERIFY(reading.has_oi_change);
            QVERIFY(reading.has_rank);
            QVERIFY(reading.rank_reference_count > 0);
        } else {
            QCOMPARE(reading.reason, CftcUnavailableReason::InsufficientHorizonHistory);
        }
    }
    QVERIFY(saw_evaluated);
}

// ── Rule set v2: 2026-09-24 audit corrections ───────────────────────────────

void TstCftcInterpretation::weekly_neighbour_requires_five_to_ten_days() {
    // The 3-day steps in the official history (2001-12-18 -> 2001-12-21) are
    // extra reports, not weekly intervals; holiday 6/8-day steps are weekly.
    QVERIFY(!cftc_weekly_neighbour(QDate(2001, 12, 18), QDate(2001, 12, 21)));
    const QDate day(2026, 9, 1);
    QVERIFY(!cftc_weekly_neighbour(day, day.addDays(4)));
    QVERIFY(cftc_weekly_neighbour(day, day.addDays(5)));
    QVERIFY(cftc_weekly_neighbour(day, day.addDays(6)));
    QVERIFY(cftc_weekly_neighbour(day, day.addDays(8)));
    QVERIFY(cftc_weekly_neighbour(day, day.addDays(10)));
    QVERIFY(!cftc_weekly_neighbour(day, day.addDays(11)));

    // An extra report three days after the previous one breaks the exact
    // one-report horizon instead of being read as a weekly change.
    QVector<double> longs = {500, 505, 510, 515, 520, 525, 530, 535, 540, 545};
    QVector<CftcObservation> observations = legacy_series(longs, QVector<double>(10, 400.0));
    observations.last().date = observations[observations.size() - 2].date.addDays(3);
    observations.last().date_label = observations.last().date.toString(Qt::ISODate);
    const CftcInterpretationResult result = cftc_interpret(legacy_input(observations));
    const CftcUnavailableRecord* record =
        find_unavailable(result.unavailable, QStringLiteral("GROSS_FLOW"), QStringLiteral("non_commercial"), 1);
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::BrokenReportSequence);
}

void TstCftcInterpretation::report_price_lag_is_at_most_four_days() {
    const QDate report(2026, 9, 15); // Tuesday
    // Friday's close for a Tuesday report (Monday and Tuesday not traded): 4 days.
    const CftcReportPrice friday = cftc_report_price({{QDate(2026, 9, 11), 100.0}}, report);
    QVERIFY(friday.available);
    QCOMPARE(friday.date, QDate(2026, 9, 11));
    // Five days is no longer the report date's session.
    const CftcReportPrice thursday = cftc_report_price({{QDate(2026, 9, 10), 100.0}}, report);
    QVERIFY(!thursday.available);
    QCOMPARE(thursday.reason, CftcUnavailableReason::PriceContextStale);
    // The previous report's close (7 days) never stands in for this report.
    const CftcReportPrice previous_report = cftc_report_price({{QDate(2026, 9, 8), 100.0}}, report);
    QVERIFY(!previous_report.available);
    QCOMPARE(previous_report.reason, CftcUnavailableReason::PriceContextStale);
    QCOMPARE(cftc_default_interpretation_config().price_max_lag_days, 4);
}

void TstCftcInterpretation::truncated_price_series_is_not_a_zero_move() {
    // The audit scenario: the price series ends one report before the latest
    // CFTC report. v1 reused the previous report's close for the latest report,
    // so the one-report "move" was an evaluated 0.00 and the longer windows
    // silently ended a week early. v2 keeps every price window unavailable.
    QVector<double> longs = {500, 500, 500, 500, 500, 500, 500, 500, 500, 600};
    QVector<CftcObservation> observations = legacy_series(longs, QVector<double>(10, 400.0));
    QVector<CftcPricePoint> prices;
    for (int i = 0; i + 1 < observations.size(); ++i)
        prices.append({observations[i].date, 100.0 + i});
    CftcInterpretationInput input = legacy_input(observations);
    input.prices = prices;
    input.price_source = QStringLiteral("TEST source");
    const CftcInterpretationResult result = cftc_interpret(input);
    for (int horizon : {1, 4}) {
        const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), horizon);
        QVERIFY(assessment);
        QVERIFY2(!assessment->has_price_move, "a missing close must not become a price move");
        QVERIFY(!assessment->evaluated);
        QVERIFY(!assessment->has_state);
        QCOMPARE(assessment->reason, CftcUnavailableReason::PriceContextStale);
    }
}

void TstCftcInterpretation::identical_price_sessions_are_unavailable() {
    // With a (custom) lag long enough for both report dates to resolve to the
    // same session, the window carries no price move and is unavailable, never
    // an evaluated 0.00.
    QVector<double> longs = {500, 500, 500, 500, 500, 500, 600};
    QVector<CftcObservation> observations = legacy_series(longs, QVector<double>(7, 400.0));
    QVector<CftcPricePoint> prices;
    for (int i = 0; i + 1 < observations.size(); ++i)
        prices.append({observations[i].date, 100.0 + i});
    CftcInterpretationInput input = legacy_input(observations);
    input.config.price_max_lag_days = 7;
    input.prices = prices;
    input.price_source = QStringLiteral("TEST source");
    const CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), 1);
    QVERIFY(assessment);
    QCOMPARE(assessment->reason, CftcUnavailableReason::PriceSessionsNotDistinct);
    QVERIFY(!assessment->has_price_move);
    QVERIFY(!assessment->evaluated);
    QCOMPARE(assessment->price_anchor_date, assessment->price_latest_date);
}

void TstCftcInterpretation::price_materiality_ranks_log_returns() {
    // Closes 100, 105, 100, 200, 195, 201: the latest raw move (+6) is larger
    // than three of the four prior raw moves (5, 5, 100, 5 -> raw rank 0.75,
    // material), but as a return (+3.08 %) it is smaller than three of the four
    // prior returns (4.9 %, 5.1 %, 69 %, 2.5 % -> rank 0.25). Price materiality
    // is scale-invariant in v2, so no relationship state is emitted.
    QVector<double> longs = {500, 500, 500, 500, 500, 550};
    const QVector<CftcObservation> observations = legacy_series(longs, QVector<double>(6, 400.0));
    CftcInterpretationInput input = legacy_input(observations);
    input.prices = price_points({100, 105, 100, 200, 195, 201});
    input.price_source = QStringLiteral("TEST source");
    const CftcInterpretationResult result = cftc_interpret(input);
    const CftcPricePositionAssessment* assessment = find_price(result, QStringLiteral("non_commercial"), 1);
    QVERIFY(assessment);
    QVERIFY(assessment->evaluated);
    QVERIFY(assessment->positioning_material);
    QCOMPARE(assessment->price_move, 6.0);
    QCOMPARE(assessment->price_anchor_close, 195.0);
    QCOMPARE(assessment->price_latest_close, 201.0);
    QVERIFY(qAbs(assessment->price_move_pct - 100.0 * (201.0 / 195.0 - 1.0)) < 1e-12);
    QVERIFY(assessment->has_price_move_rank);
    QCOMPARE(assessment->price_move_rank, 0.25);
    QCOMPARE(assessment->price_reference_count, 4);
    QVERIFY(!assessment->price_material);
    QVERIFY(!assessment->has_state);

    // The series itself holds log returns.
    const QVector<CftcDatedValue> moves = cftc_price_move_series(input.prices, observations, 1);
    QCOMPARE(moves.size(), 5);
    QVERIFY(qAbs(moves.last().value - std::log(201.0 / 195.0)) < 1e-15);
}

void TstCftcInterpretation::continuous_front_month_proxy_fails_closed() {
    // The audit's H2 scenario. A continuous front-month series rolls to the
    // next contract at the latest report: the front contract drifts from
    // 100.2 to 99.9 over the last four reports (a fall), but the next contract
    // trades 6.00 higher, so the spliced series shows 100.2 -> 105.9 (+5.69 %).
    // Read as one roll-free series, that roll gap is ranked material and the
    // relationship reads "moved together upward"; declared as the continuous
    // front-month proxy it is (Yahoo GC=F), nothing is derived from it.
    const QString key = QStringLiteral("non_commercial");
    const QVector<double> longs = {500.0, 505.0, 510.0, 515.0, 520.0, 525.0, 530.0, 535.0, 2000.0};
    const QVector<double> shorts(9, 400.0);
    const QVector<double> spliced = {100.0, 100.4, 100.1, 100.5, 100.2, 100.6, 100.3, 100.7, 105.9};

    CftcInterpretationInput input = legacy_input(legacy_series(longs, shorts));
    input.prices = price_points(spliced);
    input.price_source = QStringLiteral("TEST front-month continuous futures");

    // Control: the same closes declared roll-free are ranked and classified.
    const CftcInterpretationResult roll_free = cftc_interpret(input);
    const CftcPricePositionAssessment* control = find_price(roll_free, key, 4);
    QVERIFY(control);
    QVERIFY(control->evaluated);
    QVERIFY(control->has_price_move);
    QVERIFY(qAbs(control->price_move - 5.7) < 1e-9);
    QVERIFY(control->price_material);
    QCOMPARE(control->state_id, QStringLiteral("PRICE_POSITION_MOVING_TOGETHER_UP"));

    // Declared as a continuous front-month proxy: every assessment fails
    // closed with the roll reason; no move, rank or state is derived, and the
    // CFTC positioning side stays populated.
    input.price_continuous_proxy = true;
    const CftcInterpretationResult result = cftc_interpret(input);
    QVERIFY(result.price_requested);
    QVERIFY(result.price_continuous_proxy);
    QCOMPARE(result.price_context.size(), 9); // three Legacy participants x three horizons
    for (const auto& assessment : result.price_context) {
        QCOMPARE(assessment.reason, CftcUnavailableReason::PriceSeriesNotRollSafe);
        QVERIFY(!assessment.evaluated);
        QVERIFY(!assessment.has_price_move);
        QVERIFY(!assessment.has_price_move_rank);
        QVERIFY(!assessment.price_material);
        QVERIFY(!assessment.has_state);
        QVERIFY(assessment.state_id.isEmpty());
        QVERIFY(!assessment.price_anchor_date.isValid());
        QVERIFY(!assessment.price_latest_date.isValid());
    }
    for (int horizon : {1, 4}) {
        const CftcPricePositionAssessment* assessment = find_price(result, key, horizon);
        QVERIFY(assessment);
        QVERIFY2(assessment->has_positioning_move, "the CFTC positioning measurement does not depend on price");
        QVERIFY(assessment->positioning_material);
    }
    QCOMPARE(cftc_unavailable_reason_code(CftcUnavailableReason::PriceSeriesNotRollSafe),
             QStringLiteral("price_series_not_roll_safe"));

    // A missing series or source is still reported as such first.
    CftcInterpretationInput no_prices = input;
    no_prices.prices.clear();
    const CftcInterpretationResult no_prices_result = cftc_interpret(no_prices);
    const CftcPricePositionAssessment* no_prices_assessment = find_price(no_prices_result, key, 4);
    QVERIFY(no_prices_assessment);
    QCOMPARE(no_prices_assessment->reason, CftcUnavailableReason::MissingPriceContext);
    CftcInterpretationInput no_source = input;
    no_source.price_source.clear();
    const CftcInterpretationResult no_source_result = cftc_interpret(no_source);
    const CftcPricePositionAssessment* no_source_assessment = find_price(no_source_result, key, 4);
    QVERIFY(no_source_assessment);
    QCOMPARE(no_source_assessment->reason, CftcUnavailableReason::UnspecifiedPriceSource);

    // A spot index has no contract roll and stays evaluated.
    CftcInterpretationInput spot = input;
    spot.price_continuous_proxy = false;
    spot.price_spot_index = true;
    spot.price_source = QStringLiteral("TEST spot index");
    const CftcInterpretationResult spot_result = cftc_interpret(spot);
    const CftcPricePositionAssessment* spot_assessment = find_price(spot_result, key, 4);
    QVERIFY(spot_assessment);
    QVERIFY(spot_assessment->evaluated);
    QCOMPARE(spot_assessment->reason, CftcUnavailableReason::None);
}

void TstCftcInterpretation::gradual_unwind_within_lookback() {
    // Net %OI 10..16 is a persistent high (percentile 1.0 at 14, 15, 16), then
    // 16 (0.875) and 15.9 (0.5). The unwind to or below 0.75 happens two
    // reports after the persistent run: v1 required the persistent state at
    // the immediately preceding report and missed it; v2 looks back 13 reports.
    const QString key = QStringLiteral("non_commercial");
    const CftcInterpretationResult result =
        cftc_interpret(legacy_input(legacy_percent_series({10, 11, 12, 13, 14, 15, 16, 16, 15.9})));
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    QCOMPARE(participant->percentile, 0.5);
    QVERIFY(has_state(participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));
    QVERIFY(!has_state(participant->states, QStringLiteral("EXITED_HIGH_EXTREME")));

    // It is a transition: the next report, already below the re-entry band,
    // does not repeat it.
    const CftcInterpretationResult next =
        cftc_interpret(legacy_input(legacy_percent_series({10, 11, 12, 13, 14, 15, 16, 16, 15.9, 15.5})));
    const CftcParticipantInterpretation* next_participant = find_participant(next, key);
    QVERIFY(next_participant);
    QVERIFY(!has_state(next_participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));

    // A one-report lookback reproduces the narrower v1 reading.
    CftcInterpretationInput narrow = legacy_input(legacy_percent_series({10, 11, 12, 13, 14, 15, 16, 16, 15.9}));
    narrow.config.unwind_lookback_reports = 1;
    const CftcInterpretationResult narrow_result = cftc_interpret(narrow);
    const CftcParticipantInterpretation* narrow_participant = find_participant(narrow_result, key);
    QVERIFY(narrow_participant);
    QVERIFY(!has_state(narrow_participant->states, QStringLiteral("UNWINDING_HIGH_EXTREME")));
    QCOMPARE(narrow_result.rule_set_version, cftc_interpretation_custom_rule_set_version());
}

void TstCftcInterpretation::outdated_report_is_flagged() {
    QVector<double> longs(10, 500.0);
    CftcInterpretationInput input = legacy_input(legacy_series(longs, QVector<double>(10, 400.0)));

    CftcInterpretationResult result = cftc_interpret(input);
    QVERIFY2(!result.report_age_available, "without an evaluation date the engine reads no clock");
    QVERIFY(!result.report_outdated);

    input.evaluation_date = kLatest.addDays(14);
    result = cftc_interpret(input);
    QVERIFY(result.report_age_available);
    QCOMPARE(result.report_age_days, 14);
    QVERIFY2(!result.report_outdated, "a holiday-delayed weekly release is still current");

    input.evaluation_date = kLatest.addDays(15);
    result = cftc_interpret(input);
    QVERIFY(result.report_outdated);

    // The discontinued-contract case: the report is still interpreted as of
    // its own date, but flagged.
    input.evaluation_date = kLatest.addDays(205);
    result = cftc_interpret(input);
    QVERIFY(result.report_outdated);
    QCOMPARE(result.report_age_days, 205);
    QVERIFY(result.report_date_available);
    const CftcParticipantInterpretation* participant = find_participant(result, QStringLiteral("non_commercial"));
    QVERIFY(participant);
    QVERIFY(has_state(participant->states, QStringLiteral("NET_LONG")));
}

void TstCftcInterpretation::row_report_basis_must_match_declared_basis() {
    QVector<double> longs(10, 500.0);
    QVector<CftcObservation> combined_rows = legacy_series(longs, QVector<double>(10, 400.0));
    for (auto& observation : combined_rows)
        observation.report_basis = QStringLiteral("Combined");

    // Combined rows declared Futures Only: refused, not relabelled.
    CftcInterpretationInput mislabelled = legacy_input(combined_rows);
    mislabelled.report_basis_code = QStringLiteral("futures_only");
    CftcInterpretationResult result = cftc_interpret(mislabelled);
    const CftcUnavailableRecord* record = find_market_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"));
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::ReportBasisMismatch);
    for (const auto& participant : result.participants)
        QVERIFY(participant.states.isEmpty());

    // The same rows declared Combined: interpreted and verified.
    CftcInterpretationInput matching = legacy_input(combined_rows);
    matching.report_basis_code = QStringLiteral("futures_and_options_combined");
    result = cftc_interpret(matching);
    QVERIFY(result.report_basis_verified);
    QVERIFY(!find_market_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE")));

    // Only some rows stating a basis is an unverifiable splice.
    QVector<CftcObservation> spliced = legacy_series(longs, QVector<double>(10, 400.0));
    spliced.last().report_basis = QStringLiteral("FutOnly");
    CftcInterpretationInput splice_input = legacy_input(spliced);
    splice_input.report_basis_code = QStringLiteral("futures_only");
    result = cftc_interpret(splice_input);
    record = find_market_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE"));
    QVERIFY(record);
    QCOMPARE(record->reason, CftcUnavailableReason::ReportBasisMismatch);

    // Rows without basis metadata stay a caller declaration, reported as such.
    result = cftc_interpret(legacy_input(legacy_series(longs, QVector<double>(10, 400.0))));
    QVERIFY(!result.report_basis_verified);
    QVERIFY(!find_market_unavailable(result.unavailable, QStringLiteral("NET_EXPOSURE")));
}

void TstCftcInterpretation::default_window_percentile_boundaries() {
    // The default 156-report reference: 0.90 needs 140.5 of 156 (141 below,
    // or 140 below plus one tie); 140 below is 0.8974 and not high.
    const QString key = QStringLiteral("non_commercial");
    auto percentile_result = [&](int below, int equal, int above) {
        QVector<double> values;
        for (int i = 0; i < below; ++i)
            values.append(1.0);
        for (int i = 0; i < equal; ++i)
            values.append(5.0);
        for (int i = 0; i < above; ++i)
            values.append(9.0);
        values.append(5.0);
        CftcInterpretationInput input = legacy_input(legacy_percent_series(values), 156);
        return cftc_interpret(input);
    };
    CftcInterpretationResult result = percentile_result(140, 0, 16);
    QVERIFY(result.config_is_default);
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    QCOMPARE(participant->percentile_reference_count, 156);
    QCOMPARE(participant->percentile, 140.0 / 156.0);
    QVERIFY(!has_state(participant->states, QStringLiteral("HISTORICALLY_HIGH_NET")));

    result = percentile_result(141, 0, 15);
    participant = find_participant(result, key);
    QVERIFY(participant);
    QCOMPARE(participant->percentile, 141.0 / 156.0);
    QVERIFY(has_state(participant->states, QStringLiteral("HISTORICALLY_HIGH_NET")));
    QVERIFY(has_state(participant->states, QStringLiteral("CROWDED_LONG")));

    result = percentile_result(140, 1, 15);
    participant = find_participant(result, key);
    QVERIFY(participant);
    QCOMPARE(participant->percentile, 140.5 / 156.0);
    QVERIFY(has_state(participant->states, QStringLiteral("HISTORICALLY_HIGH_NET")));
}

void TstCftcInterpretation::default_window_materiality_boundaries() {
    // With the default 156-move reference, 0.75 is exactly 117 of 156 moves
    // strictly below the current absolute move; 116 below plus one tie is
    // 0.7468 and not material.
    const QString key = QStringLiteral("non_commercial");
    auto net_move_result = [&](int small, int equal, int large) {
        QVector<double> percents = {0.0};
        double level = 0.0;
        int step = 0;
        auto move = [&](double size) {
            level += (step % 2 == 0) ? size : -size;
            ++step;
            percents.append(level);
        };
        for (int i = 0; i < small; ++i)
            move(1.0);
        for (int i = 0; i < equal; ++i)
            move(2.0);
        for (int i = 0; i < large; ++i)
            move(3.0);
        percents.append(level + 2.0); // the current +2 % move
        CftcInterpretationInput input = legacy_input(legacy_percent_series(percents), 156);
        return cftc_interpret(input);
    };
    CftcInterpretationResult result = net_move_result(117, 0, 39);
    QVERIFY(result.config_is_default);
    const CftcParticipantInterpretation* participant = find_participant(result, key);
    QVERIFY(participant);
    const CftcInterpretationState* shift = find_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 1);
    QVERIFY(shift);
    QCOMPARE(shift->move_rank, 0.75);
    QCOMPARE(shift->move_rank_reference_count, 156);

    result = net_move_result(116, 1, 39);
    participant = find_participant(result, key);
    QVERIFY(participant);
    QVERIFY(!has_state(participant->states, QStringLiteral("NET_LONGWARD_SHIFT"), 1));
    const CftcHorizonFlowReading* reading = nullptr;
    for (const auto& candidate : participant->flow_readings) {
        if (candidate.horizon_reports == 1)
            reading = &candidate;
    }
    QVERIFY(reading);
    QCOMPARE(reading->net_rank, 116.5 / 156.0);
}

void TstCftcInterpretation::per_cause_insufficient_history_reasons() {
    // Horizon anchor before the history start.
    const CftcInterpretationResult short_history =
        cftc_interpret(legacy_input(legacy_series(QVector<double>(10, 500.0), QVector<double>(10, 400.0))));
    const CftcUnavailableRecord* horizon =
        find_unavailable(short_history.unavailable, QStringLiteral("NET_SHIFT"), QStringLiteral("non_commercial"), 13);
    QVERIFY(horizon);
    QCOMPARE(horizon->reason, CftcUnavailableReason::InsufficientHorizonHistory);

    // Concentration reference shorter than its own minimum (8 readings).
    QVector<CftcObservation> concentrated = legacy_series(QVector<double>(6, 500.0), QVector<double>(6, 400.0));
    for (int i = 0; i < concentrated.size(); ++i)
        concentrated[i].concentration_gross_4_long = 10.0 + i;
    const CftcInterpretationResult concentration = cftc_interpret(legacy_input(concentrated));
    const CftcUnavailableRecord* high = nullptr;
    for (const auto& record : concentration.unavailable) {
        if (record.state_family == QLatin1String("CONCENTRATION") &&
            record.state_id == QLatin1String("HIGH_MARKET_CONCENTRATION"))
            high = &record;
    }
    QVERIFY(high);
    QCOMPARE(high->reason, CftcUnavailableReason::InsufficientConcentrationHistory);

    // Price reference shorter than the move window while positioning is fully
    // referenced.
    QVector<double> longs(20, 500.0);
    longs.last() = 600.0;
    const QVector<CftcObservation> observations = legacy_series(longs, QVector<double>(20, 400.0));
    CftcInterpretationInput input = legacy_input(observations, 8);
    for (int i = observations.size() - 6; i < observations.size(); ++i)
        input.prices.append({observations[i].date, 100.0 + i});
    input.price_source = QStringLiteral("TEST source");
    const CftcInterpretationResult price = cftc_interpret(input);
    const CftcPricePositionAssessment* assessment = find_price(price, QStringLiteral("non_commercial"), 1);
    QVERIFY(assessment);
    QCOMPARE(assessment->reason, CftcUnavailableReason::InsufficientPriceHistory);
    QVERIFY(assessment->has_price_move);
    QVERIFY(!assessment->evaluated);

    for (CftcUnavailableReason reason :
         {CftcUnavailableReason::InsufficientHorizonHistory, CftcUnavailableReason::InsufficientPriceHistory,
          CftcUnavailableReason::InsufficientConcentrationHistory, CftcUnavailableReason::PriceSessionsNotDistinct,
          CftcUnavailableReason::ReportBasisMismatch, CftcUnavailableReason::PriceSeriesNotRollSafe})
        QVERIFY(!cftc_unavailable_reason_code(reason).isEmpty());
}

QTEST_GUILESS_MAIN(TstCftcInterpretation)
#include "tst_cftc_interpretation.moc"
