// tests/tst_cftc_metrics.cpp
//
// Batch 1 of the CFTC research engine: the pure COT metric core extracted to
// services/economics/CftcMetricModel.h. Covers position/%OI normalization,
// dated horizon anchors (1W/4W/13W/26W), gross long/short changes, positioning
// momentum, strictly trailing signal-reference normalization (current report
// excluded from its own distribution), extreme-state / return-from-extreme
// primitives and the price/OI direction combination, plus the provider-input
// validation of the 2026-09-24 audit corrections (non-finite and impossible
// values, report-family and report-basis checks). Header-only over Qt Core; no
// app sources (tests/ HARD RULE).
#include "services/economics/CftcMetricModel.h"

#include <QtTest>

using namespace fincept::services;

namespace {

CftcObservation observation(const QString& date, const std::optional<double>& oi,
                            const QVector<std::optional<double>>& longs, const QVector<std::optional<double>>& shorts) {
    CftcObservation obs;
    obs.date_label = date;
    obs.date = QDate::fromString(date, Qt::ISODate);
    obs.market = QStringLiteral("TEST - EXCHANGE");
    obs.open_interest = oi;
    obs.longs = longs;
    obs.shorts = shorts;
    return obs;
}

CftcDatedValue dated(int year, int month, int day, double value) {
    CftcDatedValue point;
    point.date = QDate(year, month, day);
    point.date_label = point.date.toString(Qt::ISODate);
    point.value = value;
    return point;
}

QVector<CftcDatedValue> weekly_values(const QVector<double>& values, const QDate& start) {
    QVector<CftcDatedValue> series;
    for (int i = 0; i < values.size(); ++i) {
        CftcDatedValue point;
        point.date = start.addDays(7LL * i);
        point.date_label = point.date.toString(Qt::ISODate);
        point.value = values[i];
        series.append(point);
    }
    return series;
}

QVector<double> ramp(int first, int count) {
    QVector<double> values;
    for (int i = 0; i < count; ++i)
        values.append(static_cast<double>(first + i));
    return values;
}

QJsonArray legacy_rows_with_trader_context() {
    QJsonArray rows;
    QJsonObject row;
    row[QStringLiteral("report_date_as_yyyy_mm_dd")] = QStringLiteral("2026-09-01");
    row[QStringLiteral("open_interest_all")] = 1000.0;
    row[QStringLiteral("commercial_long")] = 300.0;
    row[QStringLiteral("commercial_short")] = 100.0;
    row[QStringLiteral("non_commercial_long")] = 400.0;
    row[QStringLiteral("non_commercial_short")] = 150.0;
    row[QStringLiteral("non_reportable_long")] = 200.0;
    row[QStringLiteral("non_reportable_short")] = 250.0;
    row[QStringLiteral("traders_total")] = QStringLiteral("208");
    row[QStringLiteral("traders_reportable_long")] = 65.0;
    row[QStringLiteral("traders_reportable_short")] = 70.0;
    row[QStringLiteral("concentration_gross_4_long")] = QStringLiteral("12.5");
    row[QStringLiteral("concentration_gross_4_short")] = 19.3;
    row[QStringLiteral("concentration_gross_8_long")] = 23.1;
    row[QStringLiteral("concentration_gross_8_short")] = 30.3;
    row[QStringLiteral("concentration_net_4_long")] = 12.5;
    row[QStringLiteral("concentration_net_4_short")] = 18.6;
    row[QStringLiteral("concentration_net_8_long")] = 21.7;
    row[QStringLiteral("concentration_net_8_short")] = 27.1;
    rows.append(row);
    return rows;
}

} // namespace

class TstCftcMetrics : public QObject {
    Q_OBJECT
  private slots:
    // Position metrics and series
    void position_metrics_normalized_values();
    void position_metrics_zero_or_missing_open_interest();
    void metric_series_skips_missing_values();
    void metric_series_rejects_out_of_range_participants();
    void position_metrics_follow_family_participant_index();

    // Horizon changes
    void horizon_change_weekly_neighbour_rule();
    void horizon_change_4w_and_26w_date_anchors();
    void horizon_change_13w_date_anchor();
    void horizon_change_selects_by_report_date_not_vector_index();
    void horizon_change_gapped_or_missing_anchor_stays_unavailable();
    void horizon_change_tolerance_boundaries();
    void horizon_change_stale_series_is_unavailable();
    void participant_changes_keep_gross_long_and_short_separate();
    void position_changes_default_as_of_uses_official_latest();
    void position_changes_share_one_official_anchor();
    void open_interest_change_uses_the_same_anchor_rule();

    // Momentum
    void moving_average_requires_full_window_and_latest_report();
    void moving_average_marks_internal_report_gaps();
    void net_minus_moving_average();
    void moving_average_slope_uses_one_prior_report();
    void positioning_persistence_counts_same_direction_steps();

    // Signal-reference trailing normalization
    void trailing_stats_known_series();
    void trailing_stats_exclude_the_current_observation();
    void trailing_stats_longer_windows_compute_full_references();
    void trailing_stats_minimum_reference_gate();
    void trailing_stats_percentile_ties();
    void trailing_stats_insufficient_history_is_unavailable();
    void trailing_stats_zero_variance_keeps_percentile_and_descriptives();
    void trailing_stats_stale_or_short_reference();
    void trailing_window_codes_and_starts();

    // Extreme state
    void extreme_state_current_and_remaining();
    void extreme_state_exit_and_return_from_prior_extreme();
    void extreme_state_gap_breaks_persistence();
    void extreme_state_without_thresholds_or_prior();

    // Price / OI primitives
    void price_oi_directions_never_substitute_zero();
    void price_and_positioning_change_combination();
    void price_change_respects_the_report_as_of();

    // Provider parsing
    void parse_accepts_cftc_report_date_forms();
    void trader_context_fields_parse_and_missing_stays_absent();
    // Provider input validation (2026-09-24 audit corrections)
    void parse_rejects_non_finite_values();
    void parse_rejects_impossible_positions();
    void parse_rejects_rows_of_another_family();
    void parse_reads_and_checks_the_report_basis();
};

// ── Position metrics and series ─────────────────────────────────────────────

void TstCftcMetrics::position_metrics_normalized_values() {
    const CftcObservation obs =
        observation(QStringLiteral("2026-09-01"), 1000.0, {300.0, 400.0, 200.0}, {100.0, 150.0, 250.0});
    const CftcPositionMetrics first = cftc_position_metrics(obs, 0);
    QVERIFY(first.has_long);
    QCOMPARE(first.long_leg, 300.0);
    QVERIFY(first.has_short);
    QCOMPARE(first.short_leg, 100.0);
    QVERIFY(first.has_net);
    QCOMPARE(first.net_position, 200.0);
    QVERIFY(first.normalizable);
    QCOMPARE(first.long_pct_oi, 30.0);
    QCOMPARE(first.short_pct_oi, 10.0);
    QCOMPARE(first.net_pct_oi, 20.0);

    const CftcPositionMetrics second = cftc_position_metrics(obs, 1);
    QCOMPARE(second.net_position, 250.0);
    QCOMPARE(second.net_pct_oi, 25.0);

    const CftcPositionMetrics out_of_range = cftc_position_metrics(obs, 9);
    QVERIFY(!out_of_range.has_long);
    QVERIFY(!out_of_range.has_short);
    QVERIFY(!out_of_range.has_net);
    QVERIFY(!out_of_range.normalizable);
}

void TstCftcMetrics::position_metrics_zero_or_missing_open_interest() {
    const CftcObservation zero_oi =
        observation(QStringLiteral("2026-09-01"), 0.0, {300.0, std::nullopt, 200.0}, {100.0, 150.0, std::nullopt});
    const CftcPositionMetrics zero = cftc_position_metrics(zero_oi, 0);
    QVERIFY(zero.has_open_interest);
    QVERIFY2(!zero.normalizable, "zero open interest must not be divided by");
    QVERIFY(!zero.has_long_pct_oi);
    QVERIFY(!zero.has_short_pct_oi);
    QVERIFY2(!zero.has_net_pct_oi, "a zero open interest yields no normalized net");
    QVERIFY2(zero.has_net, "the raw net is still a real reported difference");
    QCOMPARE(zero.net_position, 200.0);

    const CftcObservation missing_oi = observation(QStringLiteral("2026-09-01"), std::nullopt, {300.0}, {100.0});
    const CftcPositionMetrics missing = cftc_position_metrics(missing_oi, 0);
    QVERIFY(!missing.has_open_interest);
    QVERIFY(!missing.normalizable);
    QVERIFY(!missing.has_net_pct_oi);
    QVERIFY2(missing.has_net, "missing open interest does not erase the raw legs");

    const CftcObservation missing_leg =
        observation(QStringLiteral("2026-09-01"), 1000.0, {300.0, 400.0}, {100.0, std::nullopt});
    const CftcPositionMetrics leg = cftc_position_metrics(missing_leg, 1);
    QVERIFY(leg.has_long);
    QVERIFY(!leg.has_short);
    QVERIFY2(!leg.has_net, "a net with a missing leg stays unavailable");
    QVERIFY(!leg.has_net_pct_oi);
    QVERIFY(leg.has_long_pct_oi);

    const CftcObservation negative_oi = observation(QStringLiteral("2026-09-01"), -5.0, {1.0}, {1.0});
    QVERIFY2(!cftc_position_metrics(negative_oi, 0).normalizable,
             "a non-positive open interest is never a denominator");
}

void TstCftcMetrics::metric_series_skips_missing_values() {
    QVector<CftcObservation> observations;
    observations.append(observation(QStringLiteral("2026-09-01"), 1000.0, {300.0, 400.0}, {100.0, 150.0}));
    observations.append(observation(QStringLiteral("2026-09-08"), 0.0, {320.0, std::nullopt}, {90.0, 160.0}));

    const QVector<CftcDatedValue> net = cftc_metric_series(observations, 1, CftcMetricKind::Net);
    QCOMPARE(net.size(), 1);
    QCOMPARE(net.first().date, QDate(2026, 9, 1));
    QCOMPARE(net.first().value, 250.0);

    const QVector<CftcDatedValue> long_leg = cftc_metric_series(observations, 0, CftcMetricKind::Long);
    QCOMPARE(long_leg.size(), 2);
    QCOMPARE(long_leg.last().value, 320.0);

    const QVector<CftcDatedValue> net_pct = cftc_metric_series(observations, 1, CftcMetricKind::NetPctOi);
    QCOMPARE(net_pct.size(), 1);
    QCOMPARE(net_pct.first().value, 25.0);

    const QVector<CftcDatedValue> oi = cftc_open_interest_series(observations);
    QCOMPARE(oi.size(), 2);
    QCOMPARE(oi.last().value, 0.0);
}

void TstCftcMetrics::metric_series_rejects_out_of_range_participants() {
    const CftcObservation obs = observation(QStringLiteral("2026-09-01"), 1000.0, {300.0, 400.0}, {100.0, 150.0});
    QVERIFY(!cftc_metric_value(obs, 9, CftcMetricKind::Long).has_value());
    QVERIFY(!cftc_metric_value(obs, -1, CftcMetricKind::Net).has_value());
    QVERIFY(cftc_metric_series(QVector<CftcObservation>{obs}, 9, CftcMetricKind::Long).isEmpty());
    QVERIFY(cftc_metric_series(QVector<CftcObservation>{obs}, -1, CftcMetricKind::Net).isEmpty());
    QVERIFY(!cftc_position_metrics(obs, 9).normalizable);
}

void TstCftcMetrics::position_metrics_follow_family_participant_index() {
    const auto legacy = cftc_family_participants(CftcFamily::Legacy);
    const auto disaggregated = cftc_family_participants(CftcFamily::Disaggregated);
    const auto tff = cftc_family_participants(CftcFamily::Tff);
    QCOMPARE(cftc_speculative_index(legacy), 1);
    QCOMPARE(cftc_speculative_index(disaggregated), 2);
    QCOMPARE(cftc_speculative_index(tff), 2);
    QCOMPARE(disaggregated[cftc_speculative_index(disaggregated)].key, QStringLiteral("managed_money"));
    QCOMPARE(tff[cftc_speculative_index(tff)].key, QStringLiteral("leveraged_funds"));

    // The same positional slots resolve through each family's own mapping, so a
    // family's speculative class is never borrowed from another family's names.
    const CftcObservation obs =
        observation(QStringLiteral("2026-09-01"), 1000.0, {10.0, 20.0, 30.0, 40.0, 50.0}, {1.0, 2.0, 3.0, 4.0, 5.0});
    const CftcPositionMetrics spec = cftc_position_metrics(obs, cftc_speculative_index(disaggregated));
    QCOMPARE(spec.net_position, 27.0);
    QCOMPARE(spec.net_pct_oi, 2.7);

    // A family with missing legs yields unavailable legs without inventing zero.
    const CftcObservation missing = observation(QStringLiteral("2026-09-01"), 1000.0,
                                                {10.0, 20.0, std::nullopt, 40.0, 50.0}, {1.0, 2.0, 3.0, 4.0, 5.0});
    const CftcPositionMetrics missing_spec = cftc_position_metrics(missing, 2);
    QVERIFY(!missing_spec.has_long);
    QVERIFY(!missing_spec.has_net);
    QVERIFY(!missing_spec.has_net_pct_oi);
}

// ── Horizon changes ─────────────────────────────────────────────────────────

void TstCftcMetrics::horizon_change_weekly_neighbour_rule() {
    const auto weekly = weekly_values({10, 20, 30}, QDate(2026, 9, 1));
    const CftcHorizonChange change = cftc_horizon_change(weekly, CftcHorizon::OneReport);
    QVERIFY(change.has_anchor);
    QVERIFY(change.has_value);
    QVERIFY(change.adjacent);
    QCOMPARE(change.days, 7);
    QCOMPARE(change.gap_days, 7);
    QCOMPARE(change.value, 10.0);
    QCOMPARE(change.anchor_date, QDate(2026, 9, 8));

    auto gapped = weekly;
    gapped.last().date = QDate(2026, 10, 6);
    const CftcHorizonChange stale = cftc_horizon_change(gapped, CftcHorizon::OneReport);
    QVERIFY(stale.has_anchor);
    QVERIFY2(!stale.has_value, "a 28-day hole is not a one-report change");
    QCOMPARE(stale.gap_days, 28);

    const CftcHorizonChange single =
        cftc_horizon_change(weekly_values({10}, QDate(2026, 9, 1)), CftcHorizon::OneReport);
    QVERIFY(!single.has_anchor);
    QVERIFY(!single.has_value);
}

void TstCftcMetrics::horizon_change_4w_and_26w_date_anchors() {
    const auto series = weekly_values({10, 20, 30, 40, 50}, QDate(2026, 9, 1));
    const CftcHorizonChange four = cftc_horizon_change(series, CftcHorizon::FourWeeks);
    QVERIFY(four.has_value);
    QCOMPARE(four.days, 28);
    QCOMPARE(four.gap_days, 28);
    QCOMPARE(four.anchor_date, QDate(2026, 9, 1));
    QCOMPARE(four.value, 40.0);
    QVERIFY(!four.adjacent);

    const CftcHorizonChange twenty_six = cftc_horizon_change(series, CftcHorizon::TwentySixWeeks);
    QVERIFY(!twenty_six.has_anchor);
    QVERIFY(!twenty_six.has_value);
    QCOMPARE(twenty_six.days, 182);

    QCOMPARE(cftc_horizon_code(CftcHorizon::OneReport), QStringLiteral("1W"));
    QCOMPARE(cftc_horizon_code(CftcHorizon::FourWeeks), QStringLiteral("4W"));
    QCOMPARE(cftc_horizon_code(CftcHorizon::ThirteenWeeks), QStringLiteral("13W"));
    QCOMPARE(cftc_horizon_code(CftcHorizon::TwentySixWeeks), QStringLiteral("26W"));
}

void TstCftcMetrics::horizon_change_13w_date_anchor() {
    // 15 weekly reports: latest 2026-09-08, 91 days back is 2026-06-09, which
    // is the second report. Its value is 2, so Δ = 15 − 2 = 13.
    const auto series = weekly_values(ramp(1, 15), QDate(2026, 6, 2));
    const CftcHorizonChange thirteen = cftc_horizon_change(series, CftcHorizon::ThirteenWeeks);
    QVERIFY(thirteen.has_value);
    QCOMPARE(thirteen.days, 91);
    QCOMPARE(thirteen.gap_days, 91);
    QCOMPARE(thirteen.anchor_date, QDate(2026, 6, 9));
    QCOMPARE(thirteen.value, 13.0);

    const CftcHorizonChange stale = cftc_horizon_change(series, CftcHorizon::ThirteenWeeks, QDate(2026, 9, 15));
    QVERIFY(stale.stale);
    QVERIFY(!stale.has_value);
}

void TstCftcMetrics::horizon_change_selects_by_report_date_not_vector_index() {
    // Six weekly reports with a 14-day hole before the latest. A 4W change
    // must anchor on 2026-09-22 (an actual 28-day span), not on the fourth
    // vector position (2026-09-15), which is a 35-day span.
    QVector<CftcDatedValue> series;
    series.append(dated(2026, 9, 1, 10));
    series.append(dated(2026, 9, 8, 20));
    series.append(dated(2026, 9, 15, 30));
    series.append(dated(2026, 9, 22, 40));
    series.append(dated(2026, 10, 6, 45));
    series.append(dated(2026, 10, 20, 50));

    const CftcHorizonChange four = cftc_horizon_change(series, CftcHorizon::FourWeeks);
    QVERIFY(four.has_value);
    QCOMPARE(four.anchor_date, QDate(2026, 9, 22));
    QCOMPARE(four.gap_days, 28);
    QCOMPARE(four.value, 10.0);
    QVERIFY2(!four.adjacent, "the immediately preceding report is 2026-10-06, not the anchor");

    // The 14-day hole is not a one-report change, but the anchorless case is
    // reported as a gapped pair rather than as an exact weekly neighbour.
    const CftcHorizonChange weekly = cftc_horizon_change(series, CftcHorizon::OneReport);
    QVERIFY(weekly.has_anchor);
    QVERIFY(!weekly.has_value);
    QCOMPARE(weekly.gap_days, 14);
}

void TstCftcMetrics::horizon_change_gapped_or_missing_anchor_stays_unavailable() {
    // A 4W anchor that falls outside [latest − 38 days, latest − 28 days] must
    // not be relabelled: the only older observation is a season away.
    QVector<CftcDatedValue> gapped;
    gapped.append(dated(2026, 1, 6, 10));
    gapped.append(dated(2026, 4, 7, 20));
    gapped.append(dated(2026, 4, 14, 30));
    const CftcHorizonChange four = cftc_horizon_change(gapped, CftcHorizon::FourWeeks);
    QVERIFY(four.has_anchor);
    QVERIFY2(!four.has_value, "a spring-to-autumn span is not a 4-week change");
    QCOMPARE(four.anchor_date, QDate(2026, 1, 6));

    const CftcHorizonChange thirteen = cftc_horizon_change(gapped, CftcHorizon::ThirteenWeeks);
    // The 1/6 report is 98 days before 4/14: inside the 91 (+10) day tolerance,
    // so the 13W comparison is available and carries its real 98-day span.
    QVERIFY(thirteen.has_value);
    QCOMPARE(thirteen.gap_days, 98);
    QCOMPARE(thirteen.anchor_date, QDate(2026, 1, 6));
    QCOMPARE(thirteen.value, 20.0);

    const CftcHorizonChange short_series =
        cftc_horizon_change(weekly_values({10, 12}, QDate(2026, 9, 1)), CftcHorizon::FourWeeks);
    QVERIFY(!short_series.has_anchor);
    QVERIFY(!short_series.has_value);
}

void TstCftcMetrics::horizon_change_tolerance_boundaries() {
    // 13W allows a 91-day target plus kCftcWeeklyGapDays (10) of slack: an
    // anchor exactly 101 days back is the last admissible span, 102 is not.
    QVector<CftcDatedValue> allowed;
    allowed.append(dated(2026, 7, 11, 10)); // 101 days before 2026-10-20
    allowed.append(dated(2026, 10, 20, 20));
    const CftcHorizonChange allowed_change = cftc_horizon_change(allowed, CftcHorizon::ThirteenWeeks);
    QVERIFY(allowed_change.has_anchor);
    QVERIFY2(allowed_change.has_value, "101 days is inside the 91 + 10 day tolerance");
    QCOMPARE(allowed_change.gap_days, 101);

    QVector<CftcDatedValue> rejected;
    rejected.append(dated(2026, 7, 10, 10)); // 102 days before 2026-10-20
    rejected.append(dated(2026, 10, 20, 20));
    const CftcHorizonChange rejected_change = cftc_horizon_change(rejected, CftcHorizon::ThirteenWeeks);
    QVERIFY(rejected_change.has_anchor);
    QVERIFY2(!rejected_change.has_value, "102 days is outside the 91 + 10 day tolerance");
    QCOMPARE(rejected_change.gap_days, 102);

    // The same boundary for 4W: 28 + 10 = 38 days admissible, 39 not.
    QVector<CftcDatedValue> four_allowed;
    four_allowed.append(dated(2026, 9, 12, 10)); // 38 days before 2026-10-20
    four_allowed.append(dated(2026, 10, 20, 20));
    QVERIFY(cftc_horizon_change(four_allowed, CftcHorizon::FourWeeks).has_value);

    QVector<CftcDatedValue> four_rejected;
    four_rejected.append(dated(2026, 9, 11, 10)); // 39 days before 2026-10-20
    four_rejected.append(dated(2026, 10, 20, 20));
    QVERIFY(!cftc_horizon_change(four_rejected, CftcHorizon::FourWeeks).has_value);
}

void TstCftcMetrics::horizon_change_stale_series_is_unavailable() {
    const auto series = weekly_values({10, 20, 30}, QDate(2026, 9, 1));
    for (const CftcHorizon horizon :
         {CftcHorizon::OneReport, CftcHorizon::FourWeeks, CftcHorizon::ThirteenWeeks, CftcHorizon::TwentySixWeeks}) {
        const CftcHorizonChange stale = cftc_horizon_change(series, horizon, QDate(2026, 9, 22));
        QVERIFY(stale.stale);
        QVERIFY(!stale.has_anchor);
        QVERIFY(!stale.has_value);
    }
}

void TstCftcMetrics::participant_changes_keep_gross_long_and_short_separate() {
    QVector<CftcObservation> observations;
    const QVector<double> longs = {100, 110, 120, 130, 150};
    const QVector<double> shorts = {90, 85, 80, 75, 70};
    for (int i = 0; i < longs.size(); ++i) {
        const QDate date = QDate(2026, 9, 1).addDays(7LL * i);
        observations.append(
            observation(date.toString(Qt::ISODate), 1000.0, {longs[i], 0.0, 0.0}, {shorts[i], 0.0, 0.0}));
    }
    const CftcPositionChanges changes = cftc_position_changes(observations, 0, CftcHorizon::FourWeeks);
    QVERIFY(changes.long_leg.has_value);
    QVERIFY(changes.short_leg.has_value);
    QVERIFY(changes.net.has_value);
    QVERIFY(changes.net_pct_oi.has_value);
    QCOMPARE(changes.horizon, CftcHorizon::FourWeeks);
    QCOMPARE(changes.long_leg.value, 50.0);
    QCOMPARE(changes.short_leg.value, -20.0);
    QCOMPARE(changes.net.value, 70.0);
    QCOMPARE(changes.net_pct_oi.value, 7.0);

    // The gross legs are the only evidence this layer offers: increasing longs
    // and decreasing shorts are never collapsed into one number.
    QCOMPARE(changes.long_leg.gap_days, 28);
    QCOMPARE(changes.short_leg.gap_days, 28);

    // A missing participant leg leaves that leg unavailable but keeps the net
    // unavailable too; nothing is filled with zero.
    const CftcObservation missing =
        observation(QStringLiteral("2026-09-08"), 1000.0, {110.0, 0.0, 0.0}, {std::nullopt, 0.0, 0.0});
    QVERIFY(!cftc_position_metrics(missing, 0).has_short);
    QVERIFY(!cftc_position_changes({missing}, 0, CftcHorizon::OneReport).net.has_value);

    // A caller holding the official latest report must get a stale result,
    // not a comparison anchored one report earlier.
    const CftcPositionChanges stale =
        cftc_position_changes(observations, 0, CftcHorizon::FourWeeks, QDate(2026, 10, 6));
    QVERIFY(stale.long_leg.stale);
    QVERIFY(!stale.long_leg.has_value);
    QVERIFY(stale.short_leg.stale);
    QVERIFY(stale.net.stale);
    QVERIFY(stale.net_pct_oi.stale);
}

void TstCftcMetrics::position_changes_default_as_of_uses_official_latest() {
    QVector<CftcObservation> observations;
    observations.append(observation(QStringLiteral("2026-09-01"), 1000.0, {10.0, 1.0, 0.0}, {1.0, 1.0, 0.0}));
    observations.append(observation(QStringLiteral("2026-09-08"), 1100.0, {20.0, 2.0, 0.0}, {2.0, 1.0, 0.0}));
    // The newest official report carries the short leg but not the long leg,
    // the net, or open interest.
    observations.append(
        observation(QStringLiteral("2026-09-15"), std::nullopt, {std::nullopt, 3.0, 0.0}, {3.0, 1.0, 0.0}));

    // Without an explicit as_of the wrapper must still use the official latest
    // report (2026-09-15), so the missing legs are stale rather than an older
    // 1W change silently presented as current.
    const CftcPositionChanges changes = cftc_position_changes(observations, 0, CftcHorizon::OneReport);
    QVERIFY(changes.long_leg.has_anchor);
    QCOMPARE(changes.long_leg.anchor_date, QDate(2026, 9, 8));
    QVERIFY2(changes.long_leg.stale, "the metric series ends before the official latest report");
    QVERIFY(!changes.long_leg.has_value);
    QVERIFY(changes.net.stale);
    QVERIFY(!changes.net.has_value);
    QVERIFY(changes.net_pct_oi.stale);
    QVERIFY2(changes.short_leg.has_value, "a leg present on the latest report still computes");
    QCOMPARE(changes.short_leg.value, 1.0);
    QCOMPARE(changes.short_leg.gap_days, 7);

    const CftcHorizonChange oi = cftc_open_interest_change(observations, CftcHorizon::OneReport);
    QVERIFY(oi.stale);
    QVERIFY(!oi.has_value);

    // Replay of a sliced history that ends on a report which did carry the
    // legs computes normally.
    const CftcPositionChanges replayed =
        cftc_position_changes(QVector<CftcObservation>{observations[0], observations[1]}, 0, CftcHorizon::OneReport);
    QVERIFY(replayed.long_leg.has_value);
    QCOMPARE(replayed.long_leg.value, 10.0);
}

void TstCftcMetrics::position_changes_share_one_official_anchor() {
    // Six official reports; the nominal 4W report (2026-09-01) carries the
    // short leg but not the long leg. Every metric must still be measured
    // against that one shared anchor: Long is unavailable (not silently
    // re-anchored to the 35-day-old 2026-08-25 report), while Short keeps the
    // 28-day span.
    struct Row {
        const char* date;
        std::optional<double> long_leg;
        std::optional<double> short_leg;
        std::optional<double> other_long;
        std::optional<double> other_short;
    };
    const Row rows[] = {
        {"2026-08-25", 5.0, 1.0, 7.0, 1.0},   {"2026-09-01", std::nullopt, 2.0, 8.0, 1.0},
        {"2026-09-08", 30.0, 3.0, 9.0, 1.0},  {"2026-09-15", 35.0, 4.0, 10.0, 1.0},
        {"2026-09-22", 40.0, 5.0, 11.0, 1.0}, {"2026-09-29", 50.0, 6.0, 12.0, 1.0},
    };
    QVector<CftcObservation> observations;
    for (const Row& row : rows) {
        observations.append(observation(QString::fromLatin1(row.date), 1000.0, {row.long_leg, row.other_long, 0.0},
                                        {row.short_leg, row.other_short, 0.0}));
    }

    const CftcPositionChanges mixed = cftc_position_changes(observations, 0, CftcHorizon::FourWeeks);
    QVERIFY(mixed.long_leg.has_anchor);
    QCOMPARE(mixed.long_leg.anchor_date, QDate(2026, 9, 1));
    QVERIFY2(!mixed.long_leg.has_value, "the long leg is absent at the shared anchor");
    QVERIFY(mixed.short_leg.has_value);
    QCOMPARE(mixed.short_leg.gap_days, 28);
    QCOMPARE(mixed.short_leg.value, 4.0);
    QVERIFY(mixed.net.has_anchor);
    QVERIFY2(!mixed.net.has_value, "the net is absent at the shared anchor");
    QCOMPARE(mixed.net.anchor_date, QDate(2026, 9, 1));

    // When every leg is present, the gross legs and the net are measured over
    // the same pair, so ΔLong − ΔShort == ΔNet exactly.
    const CftcPositionChanges consistent = cftc_position_changes(observations, 1, CftcHorizon::FourWeeks);
    QVERIFY(consistent.long_leg.has_value);
    QVERIFY(consistent.short_leg.has_value);
    QVERIFY(consistent.net.has_value);
    QCOMPARE(consistent.long_leg.gap_days, 28);
    QCOMPARE(consistent.short_leg.gap_days, 28);
    QCOMPARE(consistent.net.gap_days, 28);
    QCOMPARE(consistent.net.value, consistent.long_leg.value - consistent.short_leg.value);
}

void TstCftcMetrics::open_interest_change_uses_the_same_anchor_rule() {
    QVector<CftcObservation> observations;
    const QVector<double> oi = {1000, 1100, 1200, 1300, 1400};
    for (int i = 0; i < oi.size(); ++i) {
        const QDate date = QDate(2026, 9, 1).addDays(7LL * i);
        observations.append(observation(date.toString(Qt::ISODate), oi[i], {1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}));
    }
    const CftcHorizonChange four = cftc_open_interest_change(observations, CftcHorizon::FourWeeks);
    QVERIFY(four.has_value);
    QCOMPARE(four.value, 400.0);
    QCOMPARE(four.anchor_date, QDate(2026, 9, 1));

    auto missing_latest = observations;
    missing_latest.last().open_interest = std::nullopt;
    const CftcHorizonChange stale =
        cftc_open_interest_change(missing_latest, CftcHorizon::FourWeeks, QDate(2026, 9, 29));
    QVERIFY2(stale.stale, "the latest report does not carry open interest, so the series is stale");
    QVERIFY(!stale.has_value);
}

// ── Momentum ────────────────────────────────────────────────────────────────

void TstCftcMetrics::moving_average_requires_full_window_and_latest_report() {
    const auto series = weekly_values({10, 20, 30, 40, 50}, QDate(2026, 9, 1));
    const CftcMovingAverage four = cftc_moving_average(series, 4);
    QVERIFY(four.has_value);
    QVERIFY(four.at_latest_report);
    QCOMPARE(four.requested, 4);
    QCOMPARE(four.count, 4);
    QCOMPARE(four.value, 35.0);
    QCOMPARE(four.first_date, QDate(2026, 9, 8));
    QCOMPARE(four.last_date, QDate(2026, 9, 29));
    QVERIFY(!four.gapped);

    const CftcMovingAverage thirteen = cftc_moving_average(series, 13);
    QVERIFY2(!thirteen.has_value, "five reports cannot define a 13-report average");
    QCOMPARE(thirteen.count, 5);

    const CftcMovingAverage stale = cftc_moving_average(series, 4, QDate(2026, 10, 6));
    QVERIFY(!stale.at_latest_report);
    QVERIFY(!stale.has_value);
    QCOMPARE(stale.count, 0);

    const CftcMovingAverage invalid = cftc_moving_average(series, 0);
    QVERIFY(!invalid.has_value);

    // A series exactly as long as the window is the smallest valid average.
    const CftcMovingAverage exact = cftc_moving_average(weekly_values({10, 20, 30, 40}, QDate(2026, 9, 1)), 4);
    QVERIFY(exact.has_value);
    QCOMPARE(exact.count, 4);
    QCOMPARE(exact.value, 25.0);
    QCOMPARE(exact.first_date, QDate(2026, 9, 1));
}

void TstCftcMetrics::moving_average_marks_internal_report_gaps() {
    QVector<CftcDatedValue> series;
    series.append(dated(2026, 9, 1, 10));
    series.append(dated(2026, 9, 8, 20));
    series.append(dated(2026, 9, 15, 30));
    series.append(dated(2026, 10, 13, 40));
    series.append(dated(2026, 10, 20, 50));
    const CftcMovingAverage average = cftc_moving_average(series, 4);
    QVERIFY(average.has_value);
    QVERIFY2(average.gapped, "the window spans a missing report");
    QCOMPARE(average.count, 4);
    QCOMPARE(average.value, 35.0);

    // The derived readings must carry that gap state instead of collapsing to
    // an apparently ordinary scalar.
    const CftcMetricReading minus_average = cftc_net_minus_moving_average(series, 4);
    QVERIFY(minus_average.has_value);
    QVERIFY(minus_average.gapped);
    QCOMPARE(minus_average.value, 15.0);

    const CftcMetricReading slope = cftc_moving_average_slope(series, 4);
    QVERIFY(slope.has_value);
    QVERIFY2(slope.gapped, "the slope's previous window spans the missing report");
    QCOMPARE(slope.value, 10.0);
}

void TstCftcMetrics::net_minus_moving_average() {
    const auto series = weekly_values({10, 20, 30, 40, 50}, QDate(2026, 9, 1));
    const CftcMetricReading reading = cftc_net_minus_moving_average(series, 4);
    QVERIFY(reading.has_value);
    QVERIFY2(!reading.gapped, "a complete weekly window is not gapped");
    QCOMPARE(reading.value, 15.0);
    QCOMPARE(reading.reference_date, QDate(2026, 9, 29));

    const CftcMetricReading unavailable = cftc_net_minus_moving_average(series, 13);
    QVERIFY(!unavailable.has_value);
}

void TstCftcMetrics::moving_average_slope_uses_one_prior_report() {
    const auto series = weekly_values({10, 20, 30, 40, 50}, QDate(2026, 9, 1));
    const CftcMetricReading slope = cftc_moving_average_slope(series, 4);
    QVERIFY(slope.has_value);
    QVERIFY(!slope.gapped);
    // Current 4-report mean (20+30+40+50)/4 = 35; one report earlier
    // (10+20+30+40)/4 = 25.
    QCOMPARE(slope.value, 10.0);
    QCOMPARE(slope.reference_date, QDate(2026, 9, 29));

    const auto short_series = weekly_values({10, 20, 30, 40}, QDate(2026, 9, 1));
    const CftcMetricReading unavailable = cftc_moving_average_slope(short_series, 4);
    QVERIFY2(!unavailable.has_value, "a slope needs one report before the window");

    const CftcMetricReading stale = cftc_moving_average_slope(series, 4, QDate(2026, 10, 6));
    QVERIFY(!stale.has_value);
}

void TstCftcMetrics::positioning_persistence_counts_same_direction_steps() {
    const auto rising = weekly_values({10, 20, 30, 40}, QDate(2026, 9, 1));
    const CftcPositioningPersistence up = cftc_positioning_persistence(rising);
    QVERIFY(up.has_value);
    QCOMPARE(up.direction, CftcDirection::Up);
    QCOMPARE(up.changes, 3);
    QCOMPARE(up.since_date, QDate(2026, 9, 1));

    const auto broken = weekly_values({10, 20, 15, 25}, QDate(2026, 9, 1));
    const CftcPositioningPersistence mixed = cftc_positioning_persistence(broken);
    QVERIFY(mixed.has_value);
    QCOMPARE(mixed.direction, CftcDirection::Up);
    QCOMPARE(mixed.changes, 1);
    QCOMPARE(mixed.since_date, QDate(2026, 9, 15));

    const auto flat = weekly_values({10, 10, 10}, QDate(2026, 9, 1));
    const CftcPositioningPersistence flat_run = cftc_positioning_persistence(flat);
    QCOMPARE(flat_run.direction, CftcDirection::Flat);
    QCOMPARE(flat_run.changes, 2);

    QVector<CftcDatedValue> gapped;
    gapped.append(dated(2026, 9, 1, 10));
    gapped.append(dated(2026, 9, 29, 20));
    const CftcPositioningPersistence gapped_run = cftc_positioning_persistence(gapped);
    QVERIFY2(!gapped_run.has_value, "a 28-day hole is not a weekly step");

    QVERIFY(!cftc_positioning_persistence(rising, QDate(2026, 10, 6)).has_value);
    QVERIFY(!cftc_positioning_persistence(weekly_values({10}, QDate(2026, 9, 1))).has_value);
}

// ── Signal-reference trailing normalization ─────────────────────────────────

void TstCftcMetrics::trailing_stats_known_series() {
    // 27 weekly reports ending 2026-09-29; the first report sits exactly at the
    // 26W window start (2026-03-31), so the window is fully covered.
    const auto series = weekly_values(ramp(1, 27), QDate(2026, 3, 31));
    const CftcTrailingStats stats = cftc_trailing_stats(series, CftcTrailingWindow::Weeks26);
    QCOMPARE(stats.latest_report, QDate(2026, 9, 29));
    QVERIFY(stats.at_latest_report);
    QVERIFY(stats.current_present);
    QCOMPARE(stats.current, 27.0);
    QVERIFY(stats.reference_covered);
    QCOMPARE(stats.reference_count, 26);
    QCOMPARE(stats.reference_first_date, QDate(2026, 3, 31));
    QCOMPARE(stats.reference_last_date, QDate(2026, 9, 22));
    QCOMPARE(stats.min_value, 1.0);
    QCOMPARE(stats.max_value, 26.0);
    QCOMPARE(stats.avg, 13.5);
    QVERIFY(!stats.zero_variance);
    QVERIFY(stats.has_percentile);
    QCOMPARE(stats.percentile, 100.0);
    QVERIFY(stats.has_cot_index);
    // The reference excludes the current report, so a new high is not clamped:
    // (27 − 1) / (26 − 1) × 100 = 104, not 100.
    QCOMPARE(stats.cot_index, 104.0);
    QVERIFY(stats.has_zscore);
    QVERIFY(qAbs(stats.zscore - 13.5 / std::sqrt(58.5)) < 1e-9);
    QVERIFY(stats.has_distance_high);
    QCOMPARE(stats.distance_high, 1.0);
    QCOMPARE(stats.distance_low, 26.0);
}

void TstCftcMetrics::trailing_stats_exclude_the_current_observation() {
    // Current report is a new low (0) against a reference of 1..26. If the
    // current observation leaked into its own reference, (0 − 0) / (26 − 0)
    // would read COT Index 0 and percentile 1/27; the strictly trailing
    // reference gives −4 and 0% instead.
    QVector<double> values = ramp(1, 27);
    values.last() = 0.0;
    const auto series = weekly_values(values, QDate(2026, 3, 31));
    const CftcTrailingStats stats = cftc_trailing_stats(series, CftcTrailingWindow::Weeks26);
    QVERIFY(stats.has_cot_index);
    QCOMPARE(stats.cot_index, -4.0);
    QVERIFY(stats.has_percentile);
    QCOMPARE(stats.percentile, 0.0);
    QCOMPARE(stats.max_value, 26.0);
    QCOMPARE(stats.min_value, 1.0);
    QCOMPARE(stats.distance_low, -1.0);
    QVERIFY(qAbs(stats.zscore - (0.0 - 13.5) / std::sqrt(58.5)) < 1e-9);

    // The reference window is the eligible history before the current report:
    // its last date is the report immediately preceding the latest one.
    QCOMPARE(stats.reference_last_date, QDate(2026, 9, 22));
    QCOMPARE(stats.reference_count, 26);
}

void TstCftcMetrics::trailing_stats_longer_windows_compute_full_references() {
    // Build a weekly series backwards from a fixed latest report so the
    // expected reference counts are exact for each longer window. Values rise
    // from 1 (oldest) to 106 (current), so the current report is also the
    // series maximum and the strictly trailing index exceeds 100.
    const QDate latest(2026, 9, 29);
    QVector<CftcDatedValue> series;
    for (int i = 0; i < 106; ++i) {
        CftcDatedValue point;
        point.date = latest.addDays(-7LL * i);
        point.date_label = point.date.toString(Qt::ISODate);
        point.value = static_cast<double>(106 - i);
        series.prepend(point);
    }

    // 52W: window start 2025-09-30; the report landing exactly on it is the
    // oldest eligible reference, so the reference is reports i = 1..52 with
    // values 54..105. Current 106 is a new high: percentile 100 and a COT
    // Index of (106 − 54) / (105 − 54) × 100 > 100.
    const CftcTrailingStats weeks52 = cftc_trailing_stats(series, CftcTrailingWindow::Weeks52);
    QVERIFY(weeks52.reference_covered);
    QCOMPARE(weeks52.reference_count, 52);
    QCOMPARE(weeks52.reference_first_date, QDate(2025, 9, 30));
    QCOMPARE(weeks52.min_value, 54.0);
    QCOMPARE(weeks52.max_value, 105.0);
    QVERIFY(weeks52.has_percentile);
    QCOMPARE(weeks52.percentile, 100.0);
    QVERIFY(weeks52.has_cot_index);
    QVERIFY(qAbs(weeks52.cot_index - 52.0 / 51.0 * 100.0) < 1e-9);

    // 2Y: window start 2024-09-29; the earliest reports fall before it and are
    // excluded from the reference, leaving 104 eligible reports.
    const CftcTrailingStats years2 = cftc_trailing_stats(series, CftcTrailingWindow::Years2);
    QVERIFY(years2.reference_covered);
    QCOMPARE(years2.reference_count, 104);
    QCOMPARE(years2.reference_last_date, latest.addDays(-7));
    QVERIFY(years2.has_zscore);

    // 5Y cannot be covered by this 106-report history: insufficient history
    // must stay unavailable rather than describe a partial window as full.
    const CftcTrailingStats years5 = cftc_trailing_stats(series, CftcTrailingWindow::Years5);
    QVERIFY(!years5.reference_covered);
    QVERIFY(!years5.has_percentile);
    QVERIFY(!years5.has_cot_index);
    QVERIFY(!years5.has_zscore);
}

void TstCftcMetrics::trailing_stats_minimum_reference_gate() {
    // A history that spans 26W but holds only three reference reports cannot
    // support a default signal-reference statistic: the window is covered but
    // too sparse to describe a distribution.
    QVector<CftcDatedValue> sparse;
    sparse.append(dated(2026, 3, 31, 1));
    sparse.append(dated(2026, 6, 30, 2));
    sparse.append(dated(2026, 9, 15, 3));
    sparse.append(dated(2026, 9, 29, 4));
    const CftcTrailingStats gated = cftc_trailing_stats(sparse, CftcTrailingWindow::Weeks26);
    QVERIFY(gated.reference_covered);
    QCOMPARE(gated.reference_count, 3);
    QVERIFY(!gated.has_min);
    QVERIFY(!gated.has_percentile);
    QVERIFY(!gated.has_cot_index);
    QVERIFY(!gated.has_zscore);

    // A caller that explicitly accepts the sparse reference gets the exact
    // formulas instead.
    const CftcTrailingStats accepted = cftc_trailing_stats(sparse, CftcTrailingWindow::Weeks26, {}, 2);
    QVERIFY(accepted.has_percentile);
    QCOMPARE(accepted.percentile, 100.0);
    QCOMPARE(accepted.cot_index, 150.0);         // (4 − 1) / (3 − 1) × 100
    QVERIFY(qAbs(accepted.zscore - 2.0) < 1e-9); // (4 − 2) / sample SD 1

    // The smallest reference (n = 2) still uses the sample standard deviation.
    QVector<CftcDatedValue> pair_series;
    pair_series.append(dated(2026, 3, 31, 1));
    pair_series.append(dated(2026, 9, 22, 2));
    pair_series.append(dated(2026, 9, 29, 4));
    const CftcTrailingStats pair = cftc_trailing_stats(pair_series, CftcTrailingWindow::Weeks26, {}, 2);
    QCOMPARE(pair.reference_count, 2);
    QVERIFY(pair.has_zscore);
    QVERIFY(qAbs(pair.zscore - 2.5 / std::sqrt(0.5)) < 1e-9);
}

void TstCftcMetrics::trailing_stats_percentile_ties() {
    // The reference holds a tie at the current value: ties count as at or
    // below, so the percentile is 3/4 and the COT Index is 50.
    QVector<CftcDatedValue> series;
    series.append(dated(2026, 3, 31, 1));
    series.append(dated(2026, 4, 7, 2));
    series.append(dated(2026, 4, 14, 2));
    series.append(dated(2026, 4, 21, 3));
    series.append(dated(2026, 9, 29, 2));
    const CftcTrailingStats stats = cftc_trailing_stats(series, CftcTrailingWindow::Weeks26, {}, 2);
    QCOMPARE(stats.reference_count, 4);
    QVERIFY(stats.has_percentile);
    QCOMPARE(stats.percentile, 75.0);
    QCOMPARE(stats.cot_index, 50.0);
    QCOMPARE(stats.distance_high, -1.0);
    QCOMPARE(stats.distance_low, 1.0);
}

void TstCftcMetrics::trailing_stats_insufficient_history_is_unavailable() {
    const auto short_history = weekly_values(ramp(1, 10), QDate(2026, 7, 21));
    const CftcTrailingStats uncovered = cftc_trailing_stats(short_history, CftcTrailingWindow::Years2);
    QVERIFY(!uncovered.reference_covered);
    QVERIFY(!uncovered.has_min);
    QVERIFY(!uncovered.has_percentile);
    QVERIFY(!uncovered.has_cot_index);
    QVERIFY(!uncovered.has_zscore);
    QVERIFY(!uncovered.has_distance_high);
    QVERIFY(!uncovered.has_distance_low);

    // A history that covers the window but holds only one eligible reference
    // report cannot define a distribution either.
    QVector<CftcDatedValue> single_reference;
    single_reference.append(dated(2026, 3, 31, 1));
    single_reference.append(dated(2026, 9, 29, 2));
    const CftcTrailingStats sparse = cftc_trailing_stats(single_reference, CftcTrailingWindow::Weeks26);
    QVERIFY(sparse.reference_covered);
    QCOMPARE(sparse.reference_count, 1);
    QVERIFY(!sparse.has_percentile);
    QVERIFY(!sparse.has_cot_index);
    QVERIFY(!sparse.has_zscore);
}

void TstCftcMetrics::trailing_stats_zero_variance_keeps_percentile_and_descriptives() {
    const auto series = weekly_values(QVector<double>(27, 5.0), QDate(2026, 3, 31));
    const CftcTrailingStats stats = cftc_trailing_stats(series, CftcTrailingWindow::Weeks26);
    QVERIFY(stats.zero_variance);
    QCOMPARE(stats.min_value, 5.0);
    QCOMPARE(stats.max_value, 5.0);
    QCOMPARE(stats.avg, 5.0);
    QVERIFY2(!stats.has_cot_index, "the COT Index divides by the reference range");
    QVERIFY2(!stats.has_zscore, "the z-score divides by the standard deviation");
    // Percentile divides by the reference count, not the range, so it stays
    // defined for a flat reference: every reference ties with the current
    // value at or above the flat level, giving 100%.
    QVERIFY(stats.has_percentile);
    QCOMPARE(stats.percentile, 100.0);
    QCOMPARE(stats.distance_high, 0.0);
    QCOMPARE(stats.distance_low, 0.0);

    // A current value below the flat reference reads 0%.
    QVector<double> below = QVector<double>(27, 5.0);
    below.last() = 4.0;
    const CftcTrailingStats lower =
        cftc_trailing_stats(weekly_values(below, QDate(2026, 3, 31)), CftcTrailingWindow::Weeks26);
    QVERIFY(lower.zero_variance);
    QVERIFY(lower.has_percentile);
    QCOMPARE(lower.percentile, 0.0);
    QVERIFY(!lower.has_cot_index);
    QVERIFY(!lower.has_zscore);
}

void TstCftcMetrics::trailing_stats_stale_or_short_reference() {
    const auto series = weekly_values(ramp(1, 27), QDate(2026, 3, 31));
    const CftcTrailingStats stale = cftc_trailing_stats(series, CftcTrailingWindow::Weeks26, QDate(2026, 10, 6));
    QVERIFY(!stale.at_latest_report);
    QVERIFY(!stale.current_present);
    QVERIFY(!stale.has_min);
    QVERIFY(!stale.has_percentile);

    QVERIFY(!cftc_trailing_stats({}, CftcTrailingWindow::Years2).current_present);
}

void TstCftcMetrics::trailing_window_codes_and_starts() {
    const QDate latest(2026, 9, 29);
    QCOMPARE(cftc_trailing_window_code(CftcTrailingWindow::Weeks26), QStringLiteral("26W"));
    QCOMPARE(cftc_trailing_window_code(CftcTrailingWindow::Weeks52), QStringLiteral("52W"));
    QCOMPARE(cftc_trailing_window_code(CftcTrailingWindow::Years2), QStringLiteral("2Y"));
    QCOMPARE(cftc_trailing_window_code(CftcTrailingWindow::Years5), QStringLiteral("5Y"));
    QCOMPARE(cftc_trailing_window_start(latest, CftcTrailingWindow::Weeks26), QDate(2026, 3, 31));
    QCOMPARE(cftc_trailing_window_start(latest, CftcTrailingWindow::Weeks52), QDate(2025, 9, 30));
    QCOMPARE(cftc_trailing_window_start(latest, CftcTrailingWindow::Years2), QDate(2024, 9, 29));
    QCOMPARE(cftc_trailing_window_start(latest, CftcTrailingWindow::Years5), QDate(2021, 9, 29));

    // One day short of the window start is not full coverage: the difference
    // between "covers the window" and "nearly covers it" must stay visible.
    auto uncovered = weekly_values(ramp(1, 27), QDate(2026, 3, 31));
    uncovered.first().date = QDate(2026, 4, 1);
    const CftcTrailingStats stats = cftc_trailing_stats(uncovered, CftcTrailingWindow::Weeks26);
    QVERIFY(!stats.reference_covered);
    QVERIFY(!stats.has_cot_index);
}

// ── Extreme state ───────────────────────────────────────────────────────────

void TstCftcMetrics::extreme_state_current_and_remaining() {
    const auto series = weekly_values({1, 2, 3, 4, 5}, QDate(2026, 9, 1));
    const CftcExtremeState state = cftc_extreme_state(series, 4.0, 2.5);
    QVERIFY(state.has_value);
    QVERIFY(state.at_upper);
    QVERIFY2(!state.at_lower, "5 is not at or below the lower threshold");
    QCOMPARE(state.weeks_at_upper, 2);
    QVERIFY(state.remained_at_upper);
    QVERIFY(!state.remained_at_lower);
    QVERIFY(!state.left_upper);
    QVERIFY(state.has_prior_upper);
    QCOMPARE(state.prior_upper_date, QDate(2026, 9, 22));
    QCOMPARE(state.prior_upper_value, 4.0);
    QCOMPARE(state.distance_from_upper, 1.0);
    QVERIFY2(!state.away_from_upper, "still at the upper extreme is not moving away");
    QVERIFY(state.has_prior_lower);
    QCOMPARE(state.prior_lower_value, 2.0);
    QVERIFY(state.away_from_lower);
    QCOMPARE(state.distance_from_lower, 3.0);
}

void TstCftcMetrics::extreme_state_exit_and_return_from_prior_extreme() {
    const auto series = weekly_values({1, 2, 3, 5, 4}, QDate(2026, 9, 1));
    const CftcExtremeState upper = cftc_extreme_state(series, 5.0, std::nullopt);
    QVERIFY(!upper.at_upper);
    QCOMPARE(upper.weeks_at_upper, 0);
    QVERIFY(!upper.remained_at_upper);
    QVERIFY2(upper.left_upper, "the previous report was at the upper extreme");
    QVERIFY(upper.has_prior_upper);
    QCOMPARE(upper.prior_upper_date, QDate(2026, 9, 22));
    QVERIFY(upper.away_from_upper);
    QCOMPARE(upper.distance_from_upper, -1.0);
    QVERIFY(!upper.has_prior_lower);
    QVERIFY(!upper.away_from_lower);

    const auto rising_from_low = weekly_values({1, 2, 3, 4, 5}, QDate(2026, 9, 1));
    const CftcExtremeState lower = cftc_extreme_state(rising_from_low, std::nullopt, 1.0);
    QVERIFY2(!lower.at_lower, "5 is above the lower threshold");
    QVERIFY(!lower.left_lower);
    QVERIFY(lower.has_prior_lower);
    QCOMPARE(lower.prior_lower_value, 1.0);
    QVERIFY2(lower.away_from_lower, "rising away from a prior lower extreme");
    QCOMPARE(lower.distance_from_lower, 4.0);
    QVERIFY(!lower.has_prior_upper);
}

void TstCftcMetrics::extreme_state_gap_breaks_persistence() {
    QVector<CftcDatedValue> series;
    series.append(dated(2026, 9, 1, 1));
    series.append(dated(2026, 9, 8, 2));
    series.append(dated(2026, 9, 15, 3));
    series.append(dated(2026, 9, 22, 5));
    series.append(dated(2026, 10, 20, 6));

    const CftcExtremeState state = cftc_extreme_state(series, 5.0, std::nullopt);
    QVERIFY(state.at_upper);
    QCOMPARE(state.weeks_at_upper, 1);
    QVERIFY2(!state.remained_at_upper, "the missing report breaks the streak");
    QVERIFY(!state.has_previous_adjacent);
    QVERIFY2(!state.left_upper, "no adjacent previous report, so no exit evidence");
    QVERIFY(state.has_prior_upper);
    QCOMPARE(state.prior_upper_date, QDate(2026, 9, 22));

    const CftcExtremeState stale = cftc_extreme_state(series, 5.0, std::nullopt, QDate(2026, 11, 3));
    QVERIFY(!stale.at_latest_report);
    QVERIFY(!stale.has_value);
}

void TstCftcMetrics::extreme_state_without_thresholds_or_prior() {
    const auto series = weekly_values({1, 2, 3}, QDate(2026, 9, 1));
    const CftcExtremeState none = cftc_extreme_state(series, std::nullopt, std::nullopt);
    QVERIFY(none.has_value);
    QVERIFY(!none.at_upper);
    QVERIFY(!none.at_lower);
    QVERIFY(!none.has_prior_upper);
    QVERIFY(!none.has_prior_lower);
    QVERIFY(!none.has_distance_from_upper);
    QVERIFY(!none.has_distance_from_lower);
    QVERIFY(!none.away_from_upper);
    QVERIFY(!none.away_from_lower);

    const CftcExtremeState no_prior = cftc_extreme_state(series, 99.0, -99.0);
    QVERIFY(!no_prior.at_upper);
    QVERIFY(!no_prior.at_lower);
    QVERIFY(!no_prior.has_prior_upper);
    QVERIFY(!no_prior.has_prior_lower);
    QVERIFY(!cftc_extreme_state({}, 1.0, 0.0).has_value);

    // Threshold membership is inclusive: at/above the upper bound and at/below
    // the lower bound both qualify.
    const CftcExtremeState at_upper = cftc_extreme_state(weekly_values({1, 2, 3}, QDate(2026, 9, 1)), 3.0, 1.0);
    QVERIFY2(at_upper.at_upper, "current == upper threshold is at the extreme");
    QVERIFY(!at_upper.at_lower);
    const CftcExtremeState at_lower = cftc_extreme_state(weekly_values({1, 2, 1}, QDate(2026, 9, 1)), 3.0, 1.0);
    QVERIFY2(at_lower.at_lower, "current == lower threshold is at the extreme");
    QVERIFY(!at_lower.at_upper);
}

// ── Price / OI primitives ───────────────────────────────────────────────────

void TstCftcMetrics::price_oi_directions_never_substitute_zero() {
    const CftcPriceOiDirections both = cftc_price_oi_directions(5.0, -3.0);
    QVERIFY(both.price_available);
    QVERIFY(both.open_interest_available);
    QCOMPARE(both.price, CftcDirection::Up);
    QCOMPARE(both.open_interest, CftcDirection::Down);

    const CftcPriceOiDirections missing = cftc_price_oi_directions(std::nullopt, 0.0);
    QVERIFY(!missing.price_available);
    QVERIFY(missing.open_interest_available);
    QCOMPARE(missing.price, CftcDirection::Unavailable);
    QCOMPARE(missing.open_interest, CftcDirection::Flat);

    const CftcPriceOiDirections none = cftc_price_oi_directions(std::nullopt, std::nullopt);
    QVERIFY(!none.price_available);
    QVERIFY(!none.open_interest_available);
    QCOMPARE(none.price, CftcDirection::Unavailable);
    QCOMPARE(none.open_interest, CftcDirection::Unavailable);
    QVERIFY(!cftc_changes_same_direction(std::nullopt, std::nullopt));
    QVERIFY(!cftc_changes_opposed(std::nullopt, std::nullopt));

    QVERIFY(cftc_changes_same_direction(5.0, 3.0));
    QVERIFY(cftc_changes_same_direction(0.0, 0.0));
    QVERIFY(!cftc_changes_same_direction(0.0, 3.0));
    QVERIFY(!cftc_changes_same_direction(std::nullopt, 3.0));
    QVERIFY(cftc_changes_opposed(5.0, -3.0));
    QVERIFY(cftc_changes_opposed(-3.0, 5.0));
    QVERIFY(!cftc_changes_opposed(5.0, 0.0));
    QVERIFY(!cftc_changes_opposed(std::nullopt, -3.0));

    QVERIFY(cftc_directions_opposed(CftcDirection::Up, CftcDirection::Down));
    QVERIFY(!cftc_directions_opposed(CftcDirection::Flat, CftcDirection::Down));
    QVERIFY(!cftc_directions_opposed(CftcDirection::Unavailable, CftcDirection::Down));
}

void TstCftcMetrics::price_and_positioning_change_combination() {
    QVector<CftcPricePoint> prices;
    prices.append({QDate(2026, 8, 28), 100.0});
    prices.append({QDate(2026, 9, 1), 110.0});
    prices.append({QDate(2026, 9, 29), 90.0});

    const auto net_series = weekly_values({10, 20, 30, 40, 50}, QDate(2026, 9, 1));
    const QDate report_date(2026, 9, 29);
    const std::optional<double> net_4w = cftc_change_since_days(net_series, 28, report_date);
    const std::optional<double> net_13w = cftc_change_since_days(net_series, 91, report_date);
    const std::optional<double> price_4w = cftc_price_change_since_days(prices, 28);
    const std::optional<double> price_13w = cftc_price_change_since_days(prices, 91);

    QVERIFY(net_4w.has_value());
    QCOMPARE(*net_4w, 40.0);
    QVERIFY(!net_13w.has_value());
    QVERIFY(price_4w.has_value());
    QCOMPARE(*price_4w, -20.0);
    QVERIFY(!price_13w.has_value());

    QVERIFY2(cftc_changes_opposed(price_4w, net_4w), "price fell while positioning rose over 4W");
    QVERIFY(!cftc_changes_opposed(price_13w, net_13w));
    QVERIFY(!cftc_changes_same_direction(price_13w, net_13w));
}

void TstCftcMetrics::price_change_respects_the_report_as_of() {
    // A full history that continues past the CFTC report date. The historical
    // calculation must align its "latest" close to the report, so later rows
    // can never leak into it.
    QVector<CftcPricePoint> prices;
    prices.append({QDate(2026, 8, 28), 100.0});
    prices.append({QDate(2026, 9, 1), 110.0});
    prices.append({QDate(2026, 9, 8), 120.0});
    prices.append({QDate(2026, 9, 15), 130.0});
    prices.append({QDate(2026, 9, 18), 140.0});
    prices.append({QDate(2026, 9, 24), 160.0});
    prices.append({QDate(2026, 10, 1), 180.0});

    const QDate report_date(2026, 9, 15);
    const std::optional<double> aligned = cftc_price_change_since_days(prices, 14, report_date);
    QVERIFY(aligned.has_value());
    // Latest close on or before 9/15 is 130; 14 days earlier is 9/1 at 110.
    QCOMPARE(*aligned, 20.0);

    // Without the as_of constraint the helper anchors at the series end:
    // 180 − 130 = 50. The two must differ, proving the historical path is
    // report-aligned rather than accidentally safe.
    const std::optional<double> unconstrained = cftc_price_change_since_days(prices, 14);
    QVERIFY(unconstrained.has_value());
    QCOMPARE(*unconstrained, 50.0);

    // No close at or before the report date: unavailable, never the first
    // later close.
    QVERIFY(!cftc_price_change_since_days(prices, 14, QDate(2026, 8, 1)).has_value());

    // A report date after the last close simply uses that last close.
    QVERIFY(cftc_price_change_since_days(prices, 7, QDate(2026, 10, 6)).has_value());
}

// ── Provider parsing ────────────────────────────────────────────────────────

void TstCftcMetrics::parse_accepts_cftc_report_date_forms() {
    auto parse_single = [](const QString& date_text) {
        QJsonObject row;
        row[QStringLiteral("report_date_as_yyyy_mm_dd")] = date_text;
        // The provider emits every participant field of the family, with null
        // for a cell the report did not carry; a row without any of them is a
        // report-family mismatch (covered separately).
        row[QStringLiteral("non_commercial_long")] = QJsonValue(QJsonValue::Null);
        row[QStringLiteral("non_commercial_short")] = QJsonValue(QJsonValue::Null);
        QJsonArray rows;
        rows.append(row);
        return cftc_parse_history(rows, CftcFamily::Legacy);
    };

    // Socrata's ISO timestamp is accepted and reduced to its date.
    const CftcHistory timestamped = parse_single(QStringLiteral("2026-09-01T00:00:00.000"));
    QVERIFY2(timestamped.error.isEmpty(), qPrintable(timestamped.error));
    QCOMPARE(timestamped.observations.first().date, QDate(2026, 9, 1));

    const CftcHistory plain = parse_single(QStringLiteral("2026-09-08"));
    QVERIFY(plain.error.isEmpty());
    QCOMPARE(plain.observations.first().date, QDate(2026, 9, 8));

    // The shared chart contract's coarser forms stay accepted so the core's
    // own parser is a behaviour-preserving replacement.
    const CftcHistory month = parse_single(QStringLiteral("2026-09"));
    QVERIFY(month.error.isEmpty());
    QCOMPARE(month.observations.first().date, QDate(2026, 9, 1));
    const CftcHistory year = parse_single(QStringLiteral("2026"));
    QVERIFY(year.error.isEmpty());
    QCOMPARE(year.observations.first().date, QDate(2026, 1, 1));

    const CftcHistory junk = parse_single(QStringLiteral("not-a-date"));
    QVERIFY(!junk.error.isEmpty());
    QVERIFY(junk.observations.isEmpty());
    const CftcHistory empty = parse_single(QString());
    QVERIFY(!empty.error.isEmpty());
}

// ── Provider trader context ─────────────────────────────────────────────────

void TstCftcMetrics::trader_context_fields_parse_and_missing_stays_absent() {
    const CftcHistory history = cftc_parse_history(legacy_rows_with_trader_context(), CftcFamily::Legacy);
    QVERIFY2(history.error.isEmpty(), qPrintable(history.error));
    QCOMPARE(history.observations.size(), 1);
    const CftcObservation& obs = history.observations.first();
    QVERIFY(obs.traders_total.has_value());
    QCOMPARE(*obs.traders_total, 208.0);
    QCOMPARE(*obs.traders_reportable_long, 65.0);
    QCOMPARE(*obs.traders_reportable_short, 70.0);
    QCOMPARE(*obs.concentration_gross_4_long, 12.5);
    QCOMPARE(*obs.concentration_gross_4_short, 19.3);
    QCOMPARE(*obs.concentration_gross_8_long, 23.1);
    QCOMPARE(*obs.concentration_gross_8_short, 30.3);
    QCOMPARE(*obs.concentration_net_4_long, 12.5);
    QCOMPARE(*obs.concentration_net_4_short, 18.6);
    QCOMPARE(*obs.concentration_net_8_long, 21.7);
    QCOMPARE(*obs.concentration_net_8_short, 27.1);

    // A resource row without the trader-context fields leaves them absent;
    // absent is not zero and must not become one.
    QJsonArray bare = legacy_rows_with_trader_context();
    QJsonObject row = bare.first().toObject();
    for (const auto& key : {QStringLiteral("traders_total"), QStringLiteral("traders_reportable_long"),
                            QStringLiteral("traders_reportable_short"), QStringLiteral("concentration_gross_4_long"),
                            QStringLiteral("concentration_gross_4_short"), QStringLiteral("concentration_gross_8_long"),
                            QStringLiteral("concentration_gross_8_short"), QStringLiteral("concentration_net_4_long"),
                            QStringLiteral("concentration_net_4_short"), QStringLiteral("concentration_net_8_long"),
                            QStringLiteral("concentration_net_8_short")})
        row.remove(key);
    bare.replace(0, row);

    const CftcHistory without = cftc_parse_history(bare, CftcFamily::Legacy);
    QVERIFY(without.error.isEmpty());
    const CftcObservation& bare_obs = without.observations.first();
    QVERIFY(!bare_obs.traders_total.has_value());
    QVERIFY(!bare_obs.traders_reportable_long.has_value());
    QVERIFY(!bare_obs.concentration_gross_4_long.has_value());
    QVERIFY(!bare_obs.concentration_net_8_short.has_value());
}

// ── Provider input validation (2026-09-24 audit corrections) ────────────────

void TstCftcMetrics::parse_rejects_non_finite_values() {
    // "nan" / "inf" are corrupt cells, never numbers and never silently missing.
    for (const QString& text : {QStringLiteral("nan"), QStringLiteral("inf"), QStringLiteral("-inf")}) {
        QJsonArray rows = legacy_rows_with_trader_context();
        QJsonObject row = rows.first().toObject();
        row[QStringLiteral("non_commercial_long")] = text;
        rows.replace(0, row);
        const CftcHistory history = cftc_parse_history(rows, CftcFamily::Legacy);
        QVERIFY2(!history.error.isEmpty(), qPrintable(text));
        QVERIFY(history.observations.isEmpty());
        QVERIFY(history.error.contains(QStringLiteral("non-finite")));
    }
    QJsonArray rows = legacy_rows_with_trader_context();
    QJsonObject row = rows.first().toObject();
    row[QStringLiteral("open_interest_all")] = QStringLiteral("inf");
    rows.replace(0, row);
    QVERIFY(!cftc_parse_history(rows, CftcFamily::Legacy).error.isEmpty());

    // Every other retained numeric field too: a non-finite trader count or
    // concentration cell rejects the payload, naming the field, instead of
    // silently becoming a missing reading.
    for (const QString& key :
         {QStringLiteral("traders_total"), QStringLiteral("traders_reportable_long"),
          QStringLiteral("traders_reportable_short"), QStringLiteral("concentration_gross_4_long"),
          QStringLiteral("concentration_gross_4_short"), QStringLiteral("concentration_gross_8_long"),
          QStringLiteral("concentration_gross_8_short"), QStringLiteral("concentration_net_4_long"),
          QStringLiteral("concentration_net_4_short"), QStringLiteral("concentration_net_8_long"),
          QStringLiteral("concentration_net_8_short")}) {
        QJsonArray field_rows = legacy_rows_with_trader_context();
        QJsonObject field_row = field_rows.last().toObject();
        field_row[key] = QStringLiteral("nan");
        field_rows.replace(field_rows.size() - 1, field_row);
        const CftcHistory history = cftc_parse_history(field_rows, CftcFamily::Legacy);
        QVERIFY2(history.observations.isEmpty(), qPrintable(key));
        QVERIFY2(history.error.contains(QStringLiteral("non-finite")) && history.error.contains(key),
                 qPrintable(key + QStringLiteral(": ") + history.error));
    }

    QJsonObject bare;
    bare[QStringLiteral("x")] = QStringLiteral("nan");
    QVERIFY(!cftc_number(bare, QStringLiteral("x")).has_value());
}

void TstCftcMetrics::parse_rejects_impossible_positions() {
    auto parse_with = [](const QString& key, const QJsonValue& value) {
        QJsonArray rows = legacy_rows_with_trader_context();
        QJsonObject row = rows.first().toObject();
        row[key] = value;
        rows.replace(0, row);
        return cftc_parse_history(rows, CftcFamily::Legacy);
    };
    // A leg larger than the whole market's Open Interest (1000).
    CftcHistory history = parse_with(QStringLiteral("non_commercial_long"), 24329.0);
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.error.contains(QStringLiteral("larger than the market's Open Interest")));
    // A negative leg and a negative Open Interest.
    history = parse_with(QStringLiteral("commercial_short"), -5.0);
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.error.contains(QStringLiteral("negative")));
    history = parse_with(QStringLiteral("open_interest_all"), -1.0);
    QVERIFY(!history.error.isEmpty());
    // Combined reports round delta-adjusted options: two contracts of rounding
    // above Open Interest are tolerated, three are not.
    history = parse_with(QStringLiteral("non_commercial_long"), 1002.0);
    QVERIFY2(history.error.isEmpty(), qPrintable(history.error));
    history = parse_with(QStringLiteral("non_commercial_long"), 1003.0);
    QVERIFY(!history.error.isEmpty());
    // A missing Open Interest cannot be checked against and stays accepted.
    QJsonArray rows = legacy_rows_with_trader_context();
    QJsonObject row = rows.first().toObject();
    row.remove(QStringLiteral("open_interest_all"));
    rows.replace(0, row);
    QVERIFY(cftc_parse_history(rows, CftcFamily::Legacy).error.isEmpty());
}

void TstCftcMetrics::parse_rejects_rows_of_another_family() {
    // TFF rows parsed as Legacy carry none of the Legacy participant fields:
    // the payload is refused with a family reason instead of reading as
    // "every leg is missing".
    QJsonObject row;
    row[QStringLiteral("report_date_as_yyyy_mm_dd")] = QStringLiteral("2026-09-15");
    row[QStringLiteral("open_interest_all")] = 5000.0;
    row[QStringLiteral("leveraged_funds_long")] = 800.0;
    row[QStringLiteral("leveraged_funds_short")] = 1400.0;
    QJsonArray rows;
    rows.append(row);
    const CftcHistory history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.error.contains(QStringLiteral("report family")));
    QVERIFY(!history.error.contains(QStringLiteral("missing")));
    // The same row is a valid TFF row.
    QVERIFY(cftc_parse_history(rows, CftcFamily::Tff).error.isEmpty());

    // Other Reportables and Non-Reportable exist in several families, so they
    // cannot identify one: Disaggregated rows parsed as TFF are refused even
    // though they carry those shared fields.
    QJsonObject disaggregated;
    disaggregated[QStringLiteral("report_date_as_yyyy_mm_dd")] = QStringLiteral("2026-09-15");
    disaggregated[QStringLiteral("open_interest_all")] = 2000.0;
    for (const QString& key :
         {QStringLiteral("producer_merchant"), QStringLiteral("swap_dealer"), QStringLiteral("managed_money"),
          QStringLiteral("other_reportable"), QStringLiteral("non_reportable")}) {
        disaggregated[key + QStringLiteral("_long")] = 100.0;
        disaggregated[key + QStringLiteral("_short")] = 100.0;
    }
    QJsonArray disaggregated_rows;
    disaggregated_rows.append(disaggregated);
    QVERIFY(!cftc_parse_history(disaggregated_rows, CftcFamily::Tff).error.isEmpty());
    QVERIFY(cftc_parse_history(disaggregated_rows, CftcFamily::Disaggregated).error.isEmpty());
    // Legacy rows (shared Non-Reportable field) parsed as Disaggregated.
    QVERIFY(!cftc_parse_history(legacy_rows_with_trader_context(), CftcFamily::Disaggregated).error.isEmpty());
    // A row mixing two families' distinctive fields is refused as well.
    QJsonArray mixed = legacy_rows_with_trader_context();
    QJsonObject mixed_row = mixed.first().toObject();
    mixed_row[QStringLiteral("leveraged_funds_long")] = 10.0;
    mixed.replace(0, mixed_row);
    QVERIFY(!cftc_parse_history(mixed, CftcFamily::Legacy).error.isEmpty());
    QCOMPARE(
        cftc_family_distinctive_participant_keys(CftcFamily::Tff),
        (QStringList{QStringLiteral("dealer"), QStringLiteral("asset_manager"), QStringLiteral("leveraged_funds")}));
}

void TstCftcMetrics::parse_reads_and_checks_the_report_basis() {
    QJsonArray rows = legacy_rows_with_trader_context();
    QJsonObject row = rows.first().toObject();
    row[QStringLiteral("futonly_or_combined")] = QStringLiteral("FutOnly");
    rows.replace(0, row);
    CftcHistory history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY2(history.error.isEmpty(), qPrintable(history.error));
    QCOMPARE(history.observations.first().report_basis, QStringLiteral("FutOnly"));
    QCOMPARE(cftc_normalized_report_basis(QStringLiteral("FutOnly")), QStringLiteral("futures_only"));
    QCOMPARE(cftc_normalized_report_basis(QStringLiteral("Combined")), QStringLiteral("futures_and_options_combined"));
    QVERIFY(cftc_normalized_report_basis(QStringLiteral("Supplemental")).isEmpty());
    QVERIFY(cftc_history_basis_error(history.observations, /*futures_only=*/true).isEmpty());
    // Futures Only rows requested as Combined, and Combined rows requested as
    // Futures Only, are both refused.
    QVERIFY(!cftc_history_basis_error(history.observations, /*futures_only=*/false).isEmpty());
    row[QStringLiteral("futonly_or_combined")] = QStringLiteral("Combined");
    rows.replace(0, row);
    history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY(cftc_history_basis_error(history.observations, /*futures_only=*/false).isEmpty());
    const QString mislabelled = cftc_history_basis_error(history.observations, /*futures_only=*/true);
    QVERIFY(!mislabelled.isEmpty());
    QVERIFY(mislabelled.contains(QStringLiteral("Combined")));
    // A row that does not state its basis cannot be verified.
    row.remove(QStringLiteral("futonly_or_combined"));
    rows.replace(0, row);
    history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY(history.observations.first().report_basis.isEmpty());
    QVERIFY(!cftc_history_basis_error(history.observations, /*futures_only=*/true).isEmpty());
}

QTEST_GUILESS_MAIN(TstCftcMetrics)
#include "tst_cftc_metrics.moc"
