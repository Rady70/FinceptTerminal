// tests/tst_cftc_replay_validation.cpp
//
// Batch 3 of the CFTC research engine: the historically causal replay layer
// (services/economics/CftcHistoricalReplay.h).
//
// The suite pins the predeclared publication/effective convention, the genuine
// Socrata publication-metadata rule, the timing-exclusion windows, the
// minimum-history gate, the price-entry and 4W/13W outcome conventions, the
// no-lookahead guarantees (future CFTC rows, future prices and later outcomes
// never change a past state), exact equality between replay-generated states
// and direct Batch 2 engine calls, the engine's as-of input contract, and
// replay determinism. Every fixture is synthetic; no app sources are linked
// (tests/ HARD RULE).
#include "services/economics/CftcHistoricalReplay.h"

#include <QtTest>

#include <cmath>

using namespace fincept::services;

namespace {

QVector<std::optional<double>> legs_of(double net, double base) {
    return QVector<std::optional<double>>{base + std::max(0.0, net), base + std::max(0.0, -net)};
}

CftcObservation synthetic_observation(const QDate& date, double spec_net, const std::optional<double>& oi,
                                      double non_reportable_net = 0.0, double commercial_net = 0.0) {
    const auto spec = legs_of(spec_net, 5000.0);
    const auto non_reportable = legs_of(non_reportable_net, 1000.0);
    const auto commercial = legs_of(commercial_net, 4000.0);
    CftcObservation observation;
    observation.date = date;
    observation.date_label = date.toString(Qt::ISODate);
    observation.market = QStringLiteral("SYNTHETIC - TEST");
    observation.open_interest = oi;
    observation.longs = {commercial[0], spec[0], non_reportable[0]};
    observation.shorts = {commercial[1], spec[1], non_reportable[1]};
    return observation;
}

/// A deterministic legacy market: weekly reports with a slow positioning trend
/// and weekday closes with a smooth wave. Date offsets line up with the report
/// cadence so the outcome tests can assert exact entry/exit dates.
CftcReplayMarket synthetic_market(const QDate& first_report, int weeks, const QDate& price_first,
                                  const QDate& price_last, double price_base = 100.0) {
    CftcReplayMarket market;
    market.family = CftcFamily::Legacy;
    market.market_key = QStringLiteral("synthetic");
    market.label = QStringLiteral("Synthetic");
    market.asset_class = QStringLiteral("metals");
    market.price_source = QStringLiteral("synthetic test series");
    market.price_continuous_proxy = true;
    for (int i = 0; i < weeks; ++i) {
        const QDate date = first_report.addDays(7LL * i);
        const double spec = 2000.0 + 8.0 * i + 300.0 * std::sin(static_cast<double>(i) / 6.0);
        const double non_reportable = -500.0 + 4.0 * i;
        market.observations.append(
            synthetic_observation(date, spec, std::optional<double>(20000.0 + 20.0 * i), non_reportable, -100.0));
    }
    for (QDate date = price_first; date <= price_last; date = date.addDays(1)) {
        if (date.dayOfWeek() > 5)
            continue;
        const double wave =
            price_base + 5.0 * std::sin(static_cast<double>(price_first.daysTo(date)) / 11.0) + 0.01 * static_cast<double>(price_first.daysTo(date));
        market.prices.append({date, wave});
    }
    return market;
}

/// The main fixture: 160 weekly reports (so the 2Y trailing reference is
/// covered from roughly report 105) with daily closes through well past the
/// longest outcome window.
CftcReplayMarket full_market() {
    CftcReplayMarket market = synthetic_market(QDate(2023, 1, 3), 160, QDate(2022, 6, 1), QDate(2026, 12, 31));
    // One genuine recorded publication: the synthetic report dated 2025-09-30
    // was "published" 2025-11-19 (a 50-day catch-up, inside the genuine window).
    market.created_at_dates.insert(QDate(2025, 9, 30), QDate(2025, 11, 19));
    return market;
}

QString result_signature(const QDate& report_date, const CftcResearchResult& result) {
    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
        .arg(report_date.toString(Qt::ISODate), cftc_research_state_code(result.state),
             cftc_research_confidence_code(result.confidence), cftc_tactical_state_code(result.tactical_4w),
             cftc_tactical_state_code(result.swing_13w), cftc_tactical_state_code(result.regime_26w),
             cftc_historical_context_code(result.historical_context),
             result.data_available ? QStringLiteral("available") : QStringLiteral("unavailable"));
}

QString state_signature(const CftcReplayObservation& observation) {
    return result_signature(observation.report_date, observation.result);
}

QString outcome_signature(const CftcForwardOutcome& outcome) {
    return QStringLiteral("%1|%2|%3|%4|%5")
        .arg(outcome.valid ? QStringLiteral("valid") : QStringLiteral("invalid"),
             outcome.entry_date.toString(Qt::ISODate),
             QString::number(outcome.entry_price, 'g', 12),
             outcome.exit_date.toString(Qt::ISODate),
             QString::number(outcome.return_pct, 'g', 12));
}

CftcResearchInput direct_input(const CftcReplayMarket& market, const QDate& report_date) {
    CftcResearchInput input;
    input.family = market.family;
    input.observations = cftc_replay_observation_slice(market.observations, report_date);
    input.as_of = report_date;
    input.effective_date = cftc_replay_effective_date(report_date);
    input.prices = cftc_replay_price_slice(market.prices, report_date);
    input.price_source = market.price_source;
    input.price_continuous_proxy = market.price_continuous_proxy;
    input.price_spot_index = market.price_spot_index;
    input.trailing_min_reference = kCftcTrailingMinObservations;
    return input;
}

} // namespace

class TstCftcReplayValidation : public QObject {
    Q_OBJECT

  private slots:
    void nominal_release_dates();
    void conservative_effective_dates_cover_documented_delays();
    void documented_publication_dates_are_covered();
    void qualifying_publication_metadata_rule();
    void recorded_publication_wins_when_later();
    void timing_exclusion_windows_are_recorded();
    void development_and_holdout_assignment();
    void slices_contain_only_visible_history();
    void state_is_unavailable_before_effective_date();
    void same_report_becomes_available_only_at_effective_date();
    void catch_up_availability_waits_for_recorded_row_creation();
    void eligible_states_require_full_trailing_history();
    void replay_state_equals_direct_engine_call();
    void full_replay_matches_direct_engine_for_every_report();
    void engine_as_of_contract_equivalence();
    void future_cftc_rows_do_not_change_past_state();
    void future_prices_do_not_change_past_state();
    void forward_outcomes_do_not_feed_back_into_state();
    void entry_and_exit_follow_the_predeclared_convention();
    void catch_up_publication_pushes_entry_after_recorded_publication();
    void outcome_tolerance_and_entry_window();
    void return_statistics();
    void repeated_replay_is_deterministic();
};

void TstCftcReplayValidation::nominal_release_dates() {
    QCOMPARE(cftc_replay_nominal_release_date(QDate(2024, 1, 9)), QDate(2024, 1, 12));
    QCOMPARE(cftc_replay_nominal_release_date(QDate(2024, 1, 8)), QDate(2024, 1, 12));
    QCOMPARE(cftc_replay_nominal_release_date(QDate(2024, 1, 10)), QDate(2024, 1, 12));
    QCOMPARE(cftc_replay_nominal_release_date(QDate(2024, 1, 12)), QDate(2024, 1, 19));
    QCOMPARE(cftc_replay_nominal_release_date(QDate(2024, 1, 13)), QDate(2024, 1, 19));
    QCOMPARE(cftc_replay_nominal_release_date(QDate(2024, 1, 15)), QDate(2024, 1, 19));
    QVERIFY(!cftc_replay_nominal_release_date(QDate()).isValid());
}

void TstCftcReplayValidation::conservative_effective_dates_cover_documented_delays() {
    // Normal weekly cadence: Tuesday report, Friday nominal release, and the
    // effective date grants the documented one-or-two-business-day delay.
    QCOMPARE(cftc_replay_effective_date(QDate(2024, 1, 9)), QDate(2024, 1, 17));
    // Documented holiday-shifted releases from the CFTC Special Announcements:
    // 2014-12-23 was published Tuesday 2014-12-30.
    QVERIFY(cftc_replay_effective_date(QDate(2014, 12, 23)) > QDate(2014, 12, 30));
    // 2021-06-15 was published Monday 2021-06-21.
    QVERIFY(cftc_replay_effective_date(QDate(2021, 6, 15)) > QDate(2021, 6, 21));
    // 2020-12-21 (Monday report) was published Monday 2020-12-28.
    QVERIFY(cftc_replay_effective_date(QDate(2020, 12, 21)) > QDate(2020, 12, 28));
    // 2018-12-21 announcement: Monday-dated reports still published Friday.
    QVERIFY(cftc_replay_effective_date(QDate(2018, 12, 24)) > QDate(2018, 12, 28));
    // The effective date is always a weekday after the nominal release.
    const QDate effective = cftc_replay_effective_date(QDate(2025, 7, 1));
    QVERIFY(effective.dayOfWeek() <= 5);
    QVERIFY(effective > cftc_replay_nominal_release_date(QDate(2025, 7, 1)));
}

void TstCftcReplayValidation::documented_publication_dates_are_covered() {
    // Retained CFTC announcements and schedules that must never be entered
    // before the report was actually public.
    // September 11, 2001 interruption: the 2001-09-10 report was released
    // 2001-09-21 at the earliest, so the date is excluded rather than evaluated.
    CftcReplayMarket market;
    market.family = CftcFamily::Legacy;
    market.observations.append(synthetic_observation(QDate(2001, 9, 10), 100.0, std::optional<double>(1000.0)));
    const CftcReplayObservation excluded = cftc_replay_at(market, QDate(2001, 9, 10), CftcReplayOptions{});
    QVERIFY(excluded.timing_excluded);
    QVERIFY(!excluded.eligible);
    QVERIFY(!excluded.outcome_4w.valid);
    QVERIFY(!excluded.outcome_13w.valid);
    // 2015-07-03 premature/incomplete release; complete report Monday 2015-07-06.
    QVERIFY(cftc_replay_effective_date(QDate(2015, 6, 30)) > QDate(2015, 7, 6));
    // 2008 holiday schedule: Monday 2008-12-29 release.
    QVERIFY(cftc_replay_effective_date(QDate(2008, 12, 23)) > QDate(2008, 12, 29));
    // 2025-01-09 National Day of Mourning: 2025-01-07 report published Monday
    // 2025-01-13; the conservative allowance itself lands on 2025-01-15.
    QCOMPARE(cftc_replay_effective_date(QDate(2025, 1, 7), QDate(2025, 1, 13)), QDate(2025, 1, 15));
    QVERIFY(cftc_replay_effective_date(QDate(2025, 1, 7), QDate(2025, 1, 13)) > QDate(2025, 1, 13));
}

void TstCftcReplayValidation::qualifying_publication_metadata_rule() {
    // Pre-PRE rows carry the 2022-09-13 bulk-migration timestamp, never a
    // publication time.
    QVERIFY(!cftc_replay_publication_metadata_qualifies(QDate(1986, 1, 15), QDate(2022, 9, 13)));
    QVERIFY(!cftc_replay_publication_metadata_qualifies(QDate(2022, 9, 6), QDate(2022, 9, 13)));
    // The first genuine PRE publication: report 2022-09-13, created 2022-09-16.
    QVERIFY(cftc_replay_publication_metadata_qualifies(QDate(2022, 9, 13), QDate(2022, 9, 16)));
    // Catch-up releases keep their actual timestamps.
    QVERIFY(cftc_replay_publication_metadata_qualifies(QDate(2023, 1, 31), QDate(2023, 3, 3)));
    QVERIFY(cftc_replay_publication_metadata_qualifies(QDate(2025, 9, 30), QDate(2025, 11, 19)));
    // A same-day or next-day stamp cannot be a publication (three processing days).
    QVERIFY(!cftc_replay_publication_metadata_qualifies(QDate(2026, 9, 15), QDate(2026, 9, 15)));
    QVERIFY(!cftc_replay_publication_metadata_qualifies(QDate(2026, 9, 15), QDate(2026, 9, 17)));
    // A very large lag is metadata noise, not a release.
    QVERIFY(!cftc_replay_publication_metadata_qualifies(QDate(2026, 1, 6), QDate(2026, 7, 6)));
    QVERIFY(!cftc_replay_publication_metadata_qualifies(QDate(), QDate(2025, 11, 19)));
    QVERIFY(!cftc_replay_publication_metadata_qualifies(QDate(2025, 9, 30), QDate()));
}

void TstCftcReplayValidation::recorded_publication_wins_when_later() {
    // nominal release 2025-10-03 + 5 days would be 2025-10-08; the recorded
    // catch-up publication is later and wins.
    QCOMPARE(cftc_replay_effective_date(QDate(2025, 9, 30), QDate(2025, 11, 19)), QDate(2025, 11, 20));
    // A recorded publication earlier than the conservative allowance does not
    // pull the effective date forward.
    QCOMPARE(cftc_replay_effective_date(QDate(2025, 9, 30), QDate(2025, 10, 3)), QDate(2025, 10, 8));
}

void TstCftcReplayValidation::timing_exclusion_windows_are_recorded() {
    CftcReplayMarket market;
    market.family = CftcFamily::Legacy;
    const QVector<QDate> dates = {QDate(1995, 12, 26), QDate(2013, 10, 8), QDate(2018, 12, 26), QDate(2019, 3, 5)};
    for (const auto& date : dates)
        market.observations.append(synthetic_observation(date, 100.0, std::optional<double>(1000.0)));
    const CftcReplayOptions options;
    for (int i = 0; i < dates.size(); ++i) {
        const CftcReplayObservation observation = cftc_replay_at(market, dates[i], options);
        if (i < 3) {
            QVERIFY2(observation.timing_excluded, qPrintable(dates[i].toString(Qt::ISODate)));
            QVERIFY(!observation.eligible);
            QVERIFY(!observation.outcome_4w.valid);
            QVERIFY(!observation.outcome_13w.valid);
            QVERIFY(!observation.timing_exclusion_reason.isEmpty());
        } else {
            QVERIFY(!observation.timing_excluded);
        }
    }
}

void TstCftcReplayValidation::development_and_holdout_assignment() {
    CftcReplayMarket market;
    market.family = CftcFamily::Legacy;
    market.observations = {synthetic_observation(QDate(2018, 12, 18), 100.0, std::optional<double>(1000.0)),
                           synthetic_observation(QDate(2018, 12, 26), 110.0, std::optional<double>(1000.0)),
                           synthetic_observation(QDate(2019, 3, 5), 120.0, std::optional<double>(1000.0))};
    const CftcReplayOptions options;
    QVERIFY(cftc_replay_at(market, QDate(2018, 12, 18), options).development);
    QVERIFY(cftc_replay_at(market, QDate(2018, 12, 26), options).development); // excluded but still development
    QVERIFY(!cftc_replay_at(market, QDate(2019, 3, 5), options).development);
}

void TstCftcReplayValidation::slices_contain_only_visible_history() {
    const CftcReplayMarket market = full_market();
    const QDate report = market.observations[100].date;
    const auto observations = cftc_replay_observation_slice(market.observations, report);
    const auto prices = cftc_replay_price_slice(market.prices, report);
    QCOMPARE(observations.size(), 101); // the report and everything before it, nothing after
    QCOMPARE(observations.last().date, report);
    for (const auto& observation : observations)
        QVERIFY(observation.date <= report);
    QVERIFY(!prices.isEmpty());
    for (const auto& point : prices)
        QVERIFY(point.date <= report);
    QVERIFY(prices.last().date <= report);
    // The boundary is exact: a report one day after the cutoff is not visible.
    const QDate next_day = report.addDays(1);
    const auto next_observations = cftc_replay_observation_slice(market.observations, next_day);
    QCOMPARE(next_observations.size(), 101); // no weekly report exists on next_day
    QVERIFY(cftc_replay_observation_slice(market.observations, report.addDays(7)).size() == 102);
}

void TstCftcReplayValidation::state_is_unavailable_before_effective_date() {
    CftcReplayMarket market;
    market.family = CftcFamily::Legacy;
    market.market_key = QStringLiteral("synthetic");
    market.observations = {synthetic_observation(QDate(2024, 1, 9), 100.0, std::optional<double>(1000.0))};
    const CftcReplayOptions options;
    QCOMPARE(cftc_replay_effective_date(QDate(2024, 1, 9)), QDate(2024, 1, 17));
    QVERIFY(!cftc_replay_at_time(market, QDate(2024, 1, 9), options).has_value());
    QVERIFY(!cftc_replay_at_time(market, QDate(2024, 1, 16), options).has_value());
    const auto available = cftc_replay_at_time(market, QDate(2024, 1, 17), options);
    QVERIFY(available.has_value());
    QCOMPARE(available->report_date, QDate(2024, 1, 9));
    const auto after = cftc_replay_at_time(market, QDate(2024, 1, 31), options);
    QVERIFY(after.has_value());
    QCOMPARE(after->report_date, QDate(2024, 1, 9)); // no newer report exists
}

void TstCftcReplayValidation::same_report_becomes_available_only_at_effective_date() {
    CftcReplayMarket market;
    market.family = CftcFamily::Legacy;
    market.market_key = QStringLiteral("synthetic");
    for (const auto& date : {QDate(2024, 1, 9), QDate(2024, 1, 16), QDate(2024, 1, 23)})
        market.observations.append(synthetic_observation(date, 100.0, std::optional<double>(1000.0)));
    const CftcReplayOptions options;
    // Effective dates: 2024-01-09 -> 01-17, 2024-01-16 -> 01-24, 2024-01-23 -> 01-31.
    QCOMPARE(cftc_replay_effective_date(QDate(2024, 1, 16)), QDate(2024, 1, 24));
    QCOMPARE(cftc_replay_effective_date(QDate(2024, 1, 23)), QDate(2024, 1, 31));
    for (QDate date = QDate(2024, 1, 9); date < QDate(2024, 1, 17); date = date.addDays(1)) {
        const auto state = cftc_replay_at_time(market, date, options);
        QVERIFY2(!state.has_value() || state->report_date != QDate(2024, 1, 9), qPrintable(date.toString(Qt::ISODate)));
    }
    const auto first = cftc_replay_at_time(market, QDate(2024, 1, 17), options);
    QVERIFY(first.has_value());
    QCOMPARE(first->report_date, QDate(2024, 1, 9));
    const auto before_second = cftc_replay_at_time(market, QDate(2024, 1, 23), options);
    QVERIFY(before_second.has_value());
    QCOMPARE(before_second->report_date, QDate(2024, 1, 9));
    const auto second = cftc_replay_at_time(market, QDate(2024, 1, 24), options);
    QVERIFY(second.has_value());
    QCOMPARE(second->report_date, QDate(2024, 1, 16));
    // The gated state is exactly the ungated replay state for that report.
    QCOMPARE(state_signature(*second), state_signature(cftc_replay_at(market, QDate(2024, 1, 16), options)));
    const auto third = cftc_replay_at_time(market, QDate(2024, 1, 31), options);
    QVERIFY(third.has_value());
    QCOMPARE(third->report_date, QDate(2024, 1, 23));
}

void TstCftcReplayValidation::catch_up_availability_waits_for_recorded_row_creation() {
    CftcReplayMarket market;
    market.family = CftcFamily::Legacy;
    market.market_key = QStringLiteral("synthetic");
    market.observations = {synthetic_observation(QDate(2025, 9, 23), 100.0, std::optional<double>(1000.0)),
                           synthetic_observation(QDate(2025, 9, 30), 110.0, std::optional<double>(1000.0))};
    // Qualifying row-creation metadata for the second report: created 2025-11-19.
    market.created_at_dates.insert(QDate(2025, 9, 30), QDate(2025, 11, 19));
    const CftcReplayOptions options;
    QCOMPARE(cftc_replay_effective_date(QDate(2025, 9, 23)), QDate(2025, 10, 1));
    QCOMPARE(cftc_replay_effective_date(QDate(2025, 9, 30), QDate(2025, 11, 19)), QDate(2025, 11, 20));
    const auto before = cftc_replay_at_time(market, QDate(2025, 11, 19), options);
    QVERIFY(before.has_value());
    QCOMPARE(before->report_date, QDate(2025, 9, 23));
    const auto at = cftc_replay_at_time(market, QDate(2025, 11, 20), options);
    QVERIFY(at.has_value());
    QCOMPARE(at->report_date, QDate(2025, 9, 30));
}

void TstCftcReplayValidation::eligible_states_require_full_trailing_history() {
    const CftcReplayOptions options;
    const CftcReplayMarket short_market =
        synthetic_market(QDate(2025, 1, 7), 60, QDate(2024, 1, 1), QDate(2026, 12, 31));
    const auto short_replay = cftc_replay_market(short_market, options);
    QVERIFY(!short_replay.isEmpty());
    for (const auto& observation : short_replay)
        QVERIFY(!observation.eligible);
    QVERIFY(short_replay.last().ineligibility_reason.contains(QStringLiteral("2Y")));

    const CftcReplayMarket full = full_market();
    const auto full_replay = cftc_replay_market(full, options);
    QVERIFY(full_replay.last().eligible);
    QVERIFY(!full_replay.first().eligible);
    QVERIFY(!full_replay.first().ineligibility_reason.isEmpty());

    // The genuine catch-up publication is plumbed through to the effective date.
    const CftcReplayObservation* catch_up = nullptr;
    for (const auto& observation : full_replay) {
        if (observation.report_date == QDate(2025, 9, 30))
            catch_up = &observation;
    }
    QVERIFY(catch_up != nullptr);
    QVERIFY(catch_up->publication_timestamp_known);
    QCOMPARE(catch_up->effective_date, QDate(2025, 11, 20));
}

void TstCftcReplayValidation::replay_state_equals_direct_engine_call() {
    const CftcReplayMarket market = full_market();
    const CftcReplayOptions options;
    const auto replayed = cftc_replay_market(market, options);
    const CftcReplayObservation* eligible = nullptr;
    for (const auto& observation : replayed) {
        if (observation.eligible) {
            eligible = &observation;
            break;
        }
    }
    QVERIFY(eligible != nullptr);
    const CftcResearchResult direct = cftc_evaluate_research_state(direct_input(market, eligible->report_date));

    QCOMPARE(eligible->result.rule_set_version, direct.rule_set_version);
    QCOMPARE(cftc_research_state_code(eligible->result.state), cftc_research_state_code(direct.state));
    QCOMPARE(cftc_research_confidence_code(eligible->result.confidence),
             cftc_research_confidence_code(direct.confidence));
    QCOMPARE(cftc_tactical_state_code(eligible->result.tactical_4w), cftc_tactical_state_code(direct.tactical_4w));
    QCOMPARE(cftc_tactical_state_code(eligible->result.swing_13w), cftc_tactical_state_code(direct.swing_13w));
    QCOMPARE(cftc_tactical_state_code(eligible->result.regime_26w), cftc_tactical_state_code(direct.regime_26w));
    QCOMPARE(cftc_historical_context_code(eligible->result.historical_context),
             cftc_historical_context_code(direct.historical_context));
    QCOMPARE(eligible->result.groups.size(), direct.groups.size());
    for (int i = 0; i < direct.groups.size(); ++i) {
        QCOMPARE(cftc_evidence_family_code(eligible->result.groups[i].family),
                 cftc_evidence_family_code(direct.groups[i].family));
        QCOMPARE(cftc_evidence_direction_code(eligible->result.groups[i].direction),
                 cftc_evidence_direction_code(direct.groups[i].direction));
    }
    QCOMPARE(eligible->result.readings.changes_4w.net.value, direct.readings.changes_4w.net.value);
    QCOMPARE(eligible->result.readings.stats_2y.percentile, direct.readings.stats_2y.percentile);
    QCOMPARE(eligible->result.effective_date, direct.effective_date);
}

void TstCftcReplayValidation::full_replay_matches_direct_engine_for_every_report() {
    // Compare every replayed state against a direct engine call whose prefix is
    // built independently in the test (not with the replay's slice helpers), so
    // a defect inside those helpers cannot hide on both sides.
    const CftcReplayMarket market = full_market();
    const auto replayed = cftc_replay_market(market, CftcReplayOptions{});
    QCOMPARE(replayed.size(), market.observations.size());
    for (const auto& observation : replayed) {
        CftcResearchInput input;
        input.family = market.family;
        for (const auto& source : market.observations) {
            if (!(source.date > observation.report_date))
                input.observations.append(source);
        }
        input.as_of = observation.report_date;
        input.effective_date = observation.effective_date;
        for (const auto& point : market.prices) {
            if (!(point.date > observation.report_date))
                input.prices.append(point);
        }
        input.price_source = market.price_source;
        input.price_continuous_proxy = market.price_continuous_proxy;
        input.price_spot_index = market.price_spot_index;
        input.trailing_min_reference = kCftcTrailingMinObservations;
        const CftcResearchResult direct = cftc_evaluate_research_state(input);
        QCOMPARE(state_signature(observation), result_signature(observation.report_date, direct));
        QVERIFY(!direct.report_predates_history);
    }
}

void TstCftcReplayValidation::engine_as_of_contract_equivalence() {
    const CftcReplayMarket market = full_market();
    const QDate newest = market.observations.last().date;
    CftcResearchInput complete;
    complete.family = market.family;
    complete.observations = market.observations;
    complete.as_of = newest;
    complete.effective_date = cftc_replay_effective_date(newest);
    complete.prices = cftc_replay_price_slice(market.prices, newest);
    CftcResearchInput truncated = complete;
    truncated.observations = cftc_replay_observation_slice(market.observations, newest);

    const CftcResearchResult complete_result = cftc_evaluate_research_state(complete);
    const CftcResearchResult truncated_result = cftc_evaluate_research_state(truncated);
    QCOMPARE(cftc_research_state_code(complete_result.state), cftc_research_state_code(truncated_result.state));
    QCOMPARE(cftc_research_confidence_code(complete_result.confidence),
             cftc_research_confidence_code(truncated_result.confidence));
    QVERIFY(!complete_result.report_predates_history);
    QVERIFY(!truncated_result.report_predates_history);

    // The engine contract does not permit a complete dataset with an earlier
    // as_of: it refuses rather than silently slicing. The replay layer performs
    // that truncation itself (verified above).
    const QDate middle = market.observations[market.observations.size() / 2].date;
    CftcResearchInput earlier = complete;
    earlier.as_of = middle;
    const CftcResearchResult refused = cftc_evaluate_research_state(earlier);
    QVERIFY(refused.report_predates_history);
    QVERIFY(!refused.data_available);

    // Supplying the same truncated history directly matches the replay slice.
    const auto replayed = cftc_replay_market(market, CftcReplayOptions{});
    const auto* at_middle = [&]() -> const CftcReplayObservation* {
        for (const auto& observation : replayed) {
            if (observation.report_date == middle)
                return &observation;
        }
        return nullptr;
    }();
    QVERIFY(at_middle != nullptr);
    CftcResearchInput explicit_slice = earlier;
    explicit_slice.observations = cftc_replay_observation_slice(market.observations, middle);
    const CftcResearchResult slice_result = cftc_evaluate_research_state(explicit_slice);
    QCOMPARE(cftc_research_state_code(at_middle->result.state), cftc_research_state_code(slice_result.state));
    QCOMPARE(cftc_research_confidence_code(at_middle->result.confidence),
             cftc_research_confidence_code(slice_result.confidence));
}

void TstCftcReplayValidation::future_cftc_rows_do_not_change_past_state() {
    const CftcReplayMarket market = full_market();
    const CftcReplayOptions options;
    const auto original = cftc_replay_market(market, options);
    const QDate cutoff = market.observations[market.observations.size() - 20].date;

    CftcReplayMarket extended = market;
    for (int i = 1; i <= 30; ++i) {
        const QDate date = market.observations.last().date.addDays(7LL * i);
        extended.observations.append(
            synthetic_observation(date, -5000.0 + 100.0 * i, std::optional<double>(99999.0), 4000.0, 2000.0));
    }
    const auto extended_replay = cftc_replay_market(extended, options);

    int compared = 0;
    for (int i = 0; i < original.size(); ++i) {
        if (original[i].report_date > cutoff)
            break;
        QCOMPARE(state_signature(original[i]), state_signature(extended_replay[i]));
        ++compared;
    }
    QVERIFY(compared > 100);
}

void TstCftcReplayValidation::future_prices_do_not_change_past_state() {
    const CftcReplayMarket market = full_market();
    const CftcReplayOptions options;
    const auto original = cftc_replay_market(market, options);
    const QDate cutoff = market.observations[market.observations.size() - 20].date;

    CftcReplayMarket disturbed = market;
    for (auto& point : disturbed.prices) {
        if (point.date > cutoff)
            point.close *= 25.0; // a violent future move must not reach a past state
    }
    const auto disturbed_replay = cftc_replay_market(disturbed, options);

    int compared = 0;
    for (int i = 0; i < original.size(); ++i) {
        if (original[i].report_date > cutoff)
            break;
        QCOMPARE(state_signature(original[i]), state_signature(disturbed_replay[i]));
        ++compared;
    }
    QVERIFY(compared > 100);
}

void TstCftcReplayValidation::forward_outcomes_do_not_feed_back_into_state() {
    const CftcReplayMarket market = full_market();
    const CftcReplayOptions options;
    const auto original = cftc_replay_market(market, options);

    // Modify only prices that lie strictly after each state's own 13W outcome
    // window for a chosen report; that report's state cannot change.
    const QDate report = market.observations[120].date;
    CftcReplayMarket disturbed = market;
    const QDate outcome_end = report.addDays(91 + options.outcome_tolerance_days + 1);
    for (auto& point : disturbed.prices) {
        if (point.date > outcome_end)
            point.close *= 0.2;
    }
    const auto disturbed_replay = cftc_replay_market(disturbed, options);
    for (int i = 0; i < original.size(); ++i) {
        if (original[i].report_date == report) {
            QCOMPARE(state_signature(original[i]), state_signature(disturbed_replay[i]));
            return;
        }
    }
    QFAIL("chosen report date missing from the replay");
}

void TstCftcReplayValidation::entry_and_exit_follow_the_predeclared_convention() {
    const auto effective = QDate(2024, 1, 17); // a Wednesday
    QVector<CftcPricePoint> prices;
    for (QDate date = QDate(2024, 1, 8); date <= QDate(2024, 6, 28); date = date.addDays(1)) {
        if (date.dayOfWeek() > 5)
            continue;
        prices.append({date, 100.0 + static_cast<double>(QDate(2024, 1, 8).daysTo(date)) * 0.1});
    }
    const CftcReplayOptions options;
    const CftcForwardOutcome outcome = cftc_replay_forward_outcome(prices, effective, options.four_week_days, options);
    QVERIFY(outcome.valid);
    QCOMPARE(outcome.entry_date, effective);
    QCOMPARE(outcome.exit_date, effective.addDays(28));
    QCOMPARE(outcome.elapsed_days, 28);
    QVERIFY(outcome.exit_date >= outcome.entry_date.addDays(28));
    QVERIFY(outcome.entry_date >= effective);

    // Across the synthetic market, every valid outcome starts at or after the
    // state's effective date and finishes at or after its target.
    const CftcReplayMarket market = full_market();
    const auto replayed = cftc_replay_market(market, options);
    int valid_outcomes = 0;
    for (const auto& observation : replayed) {
        if (!observation.eligible || !observation.outcome_4w.valid)
            continue;
        QVERIFY(observation.outcome_4w.entry_date >= observation.effective_date);
        QVERIFY(observation.outcome_4w.exit_date >= observation.outcome_4w.entry_date.addDays(28));
        QVERIFY(observation.outcome_4w.elapsed_days >= 28);
        QVERIFY(observation.outcome_4w.elapsed_days <= 35);
        ++valid_outcomes;
    }
    QVERIFY(valid_outcomes > 0);
}

void TstCftcReplayValidation::catch_up_publication_pushes_entry_after_recorded_publication() {
    const CftcReplayMarket market = full_market();
    const auto replayed = cftc_replay_market(market, CftcReplayOptions{});
    const CftcReplayObservation* catch_up = nullptr;
    for (const auto& observation : replayed) {
        if (observation.report_date == QDate(2025, 9, 30))
            catch_up = &observation;
    }
    QVERIFY(catch_up != nullptr);
    QVERIFY(catch_up->publication_timestamp_known);
    QCOMPARE(catch_up->publication_date, QDate(2025, 11, 19));
    QCOMPARE(catch_up->effective_date, QDate(2025, 11, 20));
    QVERIFY(catch_up->outcome_4w.valid);
    QVERIFY(catch_up->outcome_4w.entry_date > catch_up->publication_date);
    QVERIFY(catch_up->outcome_13w.entry_date > catch_up->publication_date);
}

void TstCftcReplayValidation::outcome_tolerance_and_entry_window() {
    const CftcReplayOptions options;
    const QDate effective = QDate(2024, 1, 17);

    // Entry exists with a longer exit gap: valid, elapsed recorded exactly.
    const QVector<CftcPricePoint> valid_prices = {{QDate(2024, 1, 17), 100.0}, {QDate(2024, 2, 15), 110.0}};
    const CftcForwardOutcome valid = cftc_replay_forward_outcome(valid_prices, effective, 28, options);
    QVERIFY(valid.valid);
    QCOMPARE(valid.entry_date, QDate(2024, 1, 17));
    QCOMPARE(valid.exit_date, QDate(2024, 2, 15));
    QCOMPARE(valid.elapsed_days, 29);
    QVERIFY(qAbs(valid.return_pct - 10.0) < 1e-9);

    // First price is outside the 10-day entry window.
    const QVector<CftcPricePoint> late_entry = {{QDate(2024, 1, 10), 100.0}, {QDate(2024, 1, 29), 100.0}};
    const CftcForwardOutcome late = cftc_replay_forward_outcome(late_entry, effective, 28, options);
    QVERIFY(!late.valid);
    QVERIFY(late.invalid_reason.contains(QStringLiteral("entry window")));

    // First exit after the target is outside the 7-day tolerance.
    const QVector<CftcPricePoint> late_exit = {{QDate(2024, 1, 17), 100.0}, {QDate(2024, 2, 23), 110.0}};
    const CftcForwardOutcome tolerance = cftc_replay_forward_outcome(late_exit, effective, 28, options);
    QVERIFY(!tolerance.valid);
    QVERIFY(tolerance.invalid_reason.contains(QStringLiteral("past it")));

    // Price history does not reach the target.
    const QVector<CftcPricePoint> short_history = {{QDate(2024, 1, 17), 100.0}};
    const CftcForwardOutcome short_outcome = cftc_replay_forward_outcome(short_history, effective, 28, options);
    QVERIFY(!short_outcome.valid);
    QVERIFY(short_outcome.invalid_reason.contains(QStringLiteral("ends before")));

    // No price at all at or after the effective date.
    const QVector<CftcPricePoint> no_entry = {{QDate(2024, 1, 10), 100.0}};
    const CftcForwardOutcome missing = cftc_replay_forward_outcome(no_entry, effective, 28, options);
    QVERIFY(!missing.valid);
    QVERIFY(missing.invalid_reason.contains(QStringLiteral("No price observation")));

    // Exact boundaries: entry on the 10th day after the effective date is
    // inside the window; the 11th day is not.
    const QVector<CftcPricePoint> entry_day_10 = {{QDate(2024, 1, 27), 100.0}, {QDate(2024, 2, 24), 100.0}};
    QVERIFY(cftc_replay_forward_outcome(entry_day_10, effective, 28, options).valid);
    const QVector<CftcPricePoint> entry_day_11 = {{QDate(2024, 1, 28), 100.0}, {QDate(2024, 2, 25), 100.0}};
    QVERIFY(!cftc_replay_forward_outcome(entry_day_11, effective, 28, options).valid);
    // Exit exactly 7 days after the target is inside tolerance; 8 days is not.
    const QVector<CftcPricePoint> exit_day_7 = {{QDate(2024, 1, 17), 100.0}, {QDate(2024, 2, 21), 100.0}};
    QVERIFY(cftc_replay_forward_outcome(exit_day_7, effective, 28, options).valid);
    const QVector<CftcPricePoint> exit_day_8 = {{QDate(2024, 1, 17), 100.0}, {QDate(2024, 2, 22), 100.0}};
    QVERIFY(!cftc_replay_forward_outcome(exit_day_8, effective, 28, options).valid);
}

void TstCftcReplayValidation::repeated_replay_is_deterministic() {
    const CftcReplayMarket market = full_market();
    const CftcReplayOptions options;
    const auto first = cftc_replay_market(market, options);
    const auto second = cftc_replay_market(market, options);
    QCOMPARE(first.size(), second.size());
    for (int i = 0; i < first.size(); ++i) {
        QCOMPARE(state_signature(first[i]), state_signature(second[i]));
        QCOMPARE(outcome_signature(first[i].outcome_4w), outcome_signature(second[i].outcome_4w));
        QCOMPARE(outcome_signature(first[i].outcome_13w), outcome_signature(second[i].outcome_13w));
    }
}

void TstCftcReplayValidation::return_statistics() {
    const CftcReturnStats empty = cftc_return_stats({});
    QCOMPARE(empty.n, 0);
    QCOMPARE(empty.negative_rate, 0.0);
    const CftcReturnStats odd = cftc_return_stats({1.0, -1.0, 2.0});
    QCOMPARE(odd.n, 3);
    QVERIFY(qAbs(odd.mean - 2.0 / 3.0) < 1e-12);
    QCOMPARE(odd.median, 1.0);
    QCOMPARE(odd.positive_rate, 2.0 / 3.0);
    QCOMPARE(odd.negative_rate, 1.0 / 3.0);
    const CftcReturnStats even = cftc_return_stats({4.0, 1.0, 3.0, 2.0});
    QCOMPARE(even.median, 2.5);
    QCOMPARE(even.positive_rate, 1.0);
    QCOMPARE(even.negative_rate, 0.0);
}

QTEST_GUILESS_MAIN(TstCftcReplayValidation)
#include "tst_cftc_replay_validation.moc"


