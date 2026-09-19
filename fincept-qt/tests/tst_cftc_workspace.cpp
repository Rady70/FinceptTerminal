// tests/tst_cftc_workspace.cpp
//
// The R3 CFTC workspace's data model and analytics: report-family participant
// semantic mapping, null-preserving parsing, range windows anchored at the
// latest returned report, window statistics with explicit formulas and
// degenerate-case handling, weekly-neighbour changes, heatmap trailing
// statistics and price alignment. Header-only over Qt Core; no app sources
// (tests/ HARD RULE).
#include "screens/economics/panels/CftcWorkspaceData.h"

#include <QtTest>

using namespace fincept::screens;

namespace {

CftcObservation observation(const QString& date, double oi, const QVector<std::optional<double>>& longs,
                            const QVector<std::optional<double>>& shorts) {
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

QJsonArray legacy_rows() {
    QJsonArray rows;
    rows.append(QJsonObject{{QStringLiteral("report_date_as_yyyy_mm_dd"), QStringLiteral("2026-09-01")},
                            {QStringLiteral("market_and_exchange_names"), QStringLiteral("GOLD - COMEX")},
                            {QStringLiteral("cftc_contract_market_code"), QStringLiteral("088691")},
                            {QStringLiteral("contract_units"), QStringLiteral("TROY OUNCES")},
                            {QStringLiteral("open_interest_all"), 1000.0},
                            {QStringLiteral("commercial_long"), 300.0},
                            {QStringLiteral("commercial_short"), QStringLiteral("100")},
                            {QStringLiteral("non_commercial_long"), 400.0},
                            {QStringLiteral("non_commercial_short"), 150.0},
                            {QStringLiteral("non_reportable_long"), 200.0},
                            {QStringLiteral("non_reportable_short"), 250.0}});
    rows.append(QJsonObject{{QStringLiteral("report_date_as_yyyy_mm_dd"), QStringLiteral("2026-09-08")},
                            {QStringLiteral("market_and_exchange_names"), QStringLiteral("GOLD - COMEX")},
                            {QStringLiteral("cftc_contract_market_code"), QStringLiteral("088691")},
                            {QStringLiteral("contract_units"), QStringLiteral("TROY OUNCES")},
                            {QStringLiteral("open_interest_all"), 1100.0},
                            {QStringLiteral("commercial_long"), 320.0},
                            {QStringLiteral("commercial_short"), 90.0},
                            {QStringLiteral("non_commercial_long"), 430.0},
                            {QStringLiteral("non_commercial_short"), 160.0},
                            {QStringLiteral("non_reportable_long"), 210.0},
                            {QStringLiteral("non_reportable_short"), 240.0}});
    return rows;
}

} // namespace

class TstCftcWorkspace : public QObject {
    Q_OBJECT
  private slots:
    // Participant semantics
    void legacy_participant_classes();
    void disaggregated_participant_classes();
    void tff_participant_classes();
    void speculative_index_per_family();
    void family_code_round_trip();

    // Parsing
    void parse_preserves_missing_legs_and_numeric_strings();
    void parse_sorts_ascending();
    void parse_rejects_unusable_date();
    void parse_rejects_duplicate_report_dates();
    void parse_rejects_empty_payload();

    // Analytics
    void net_requires_both_legs();
    void range_availability_is_anchored_at_latest_report();
    void range_filter_never_synthesises_observations();
    void weekly_change_requires_a_weekly_neighbour();
    void change_since_days_uses_the_last_report_at_or_before_the_target();
    void window_stats_known_series();
    void window_stats_degenerate_cases();
    void heatmap_series_gates_short_prefixes();
    void price_helpers_align_to_report_dates();
    void direction_alignment_rules();
    void extreme_dates_follow_values();
};

// ── Participant semantics ───────────────────────────────────────────────────

void TstCftcWorkspace::legacy_participant_classes() {
    const auto participants = cftc_family_participants(CftcFamily::Legacy);
    QCOMPARE(participants.size(), 3);
    QCOMPARE(participants[0].key, QStringLiteral("commercial"));
    QCOMPARE(participants[1].key, QStringLiteral("non_commercial"));
    QCOMPARE(participants[2].key, QStringLiteral("non_reportable"));
    QVERIFY(participants[1].speculative);
    QVERIFY(!participants[0].key.contains(QStringLiteral("dealer")));
}

void TstCftcWorkspace::disaggregated_participant_classes() {
    const auto participants = cftc_family_participants(CftcFamily::Disaggregated);
    QCOMPARE(participants.size(), 5);
    QCOMPARE(participants[0].key, QStringLiteral("producer_merchant"));
    QCOMPARE(participants[1].key, QStringLiteral("swap_dealer"));
    QCOMPARE(participants[2].key, QStringLiteral("managed_money"));
    QCOMPARE(participants[3].key, QStringLiteral("other_reportable"));
    QCOMPARE(participants[4].key, QStringLiteral("non_reportable"));
    QVERIFY(participants[2].speculative);
    QVERIFY(!participants[0].label.contains(QStringLiteral("Commercial")));
}

void TstCftcWorkspace::tff_participant_classes() {
    const auto participants = cftc_family_participants(CftcFamily::Tff);
    QCOMPARE(participants.size(), 5);
    QCOMPARE(participants[0].key, QStringLiteral("dealer"));
    QCOMPARE(participants[1].key, QStringLiteral("asset_manager"));
    QCOMPARE(participants[2].key, QStringLiteral("leveraged_funds"));
    QCOMPARE(participants[3].key, QStringLiteral("other_reportable"));
    QCOMPARE(participants[4].key, QStringLiteral("non_reportable"));
    QVERIFY(participants[2].speculative);
    for (const auto& participant : participants) {
        QVERIFY(!participant.key.contains(QStringLiteral("commercial")));
        QVERIFY(!participant.key.contains(QStringLiteral("managed_money")));
    }
}

void TstCftcWorkspace::speculative_index_per_family() {
    QCOMPARE(cftc_speculative_index(cftc_family_participants(CftcFamily::Legacy)), 1);
    QCOMPARE(cftc_speculative_index(cftc_family_participants(CftcFamily::Disaggregated)), 2);
    QCOMPARE(cftc_speculative_index(cftc_family_participants(CftcFamily::Tff)), 2);
}

void TstCftcWorkspace::family_code_round_trip() {
    QCOMPARE(cftc_family_code(CftcFamily::Legacy), QStringLiteral("legacy"));
    QCOMPARE(cftc_family_code(CftcFamily::Disaggregated), QStringLiteral("disaggregated"));
    QCOMPARE(cftc_family_code(CftcFamily::Tff), QStringLiteral("tff"));
    QCOMPARE(cftc_family_from_code(QStringLiteral("financial")), CftcFamily::Tff);
    QCOMPARE(cftc_family_from_code(QStringLiteral("disaggregated")), CftcFamily::Disaggregated);
    QCOMPARE(cftc_family_from_code(QStringLiteral("legacy")), CftcFamily::Legacy);
}

// ── Parsing ─────────────────────────────────────────────────────────────────

void TstCftcWorkspace::parse_preserves_missing_legs_and_numeric_strings() {
    QJsonArray rows = legacy_rows();
    QJsonObject first = rows[0].toObject();
    first.remove(QStringLiteral("non_reportable_short"));
    rows.replace(0, first);

    const CftcHistory history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY2(history.error.isEmpty(), qPrintable(history.error));
    QCOMPARE(history.observations.size(), 2);
    const CftcObservation& obs = history.observations.first();
    QCOMPARE(obs.market, QStringLiteral("GOLD - COMEX"));
    QCOMPARE(obs.contract_code, QStringLiteral("088691"));
    QCOMPARE(obs.units, QStringLiteral("TROY OUNCES"));
    QVERIFY(obs.open_interest.has_value());
    QCOMPARE(*obs.open_interest, 1000.0);
    QCOMPARE(*obs.longs[0], 300.0);
    QVERIFY2(*obs.shorts[0] == 100.0, "a numeric string cell is a real observation");
    QVERIFY2(!obs.shorts[2].has_value(), "a missing cell must stay absent");
    QVERIFY2(!cftc_participant_net(obs, 2).has_value(), "a net with a missing leg stays unavailable");
}

void TstCftcWorkspace::parse_sorts_ascending() {
    QJsonArray rows = legacy_rows();
    rows.replace(0, legacy_rows()[1]);
    rows.replace(1, legacy_rows()[0]);

    const CftcHistory history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY(history.error.isEmpty());
    QCOMPARE(history.observations.first().date, QDate(2026, 9, 1));
    QCOMPARE(history.observations.last().date, QDate(2026, 9, 8));
}

void TstCftcWorkspace::parse_rejects_unusable_date() {
    QJsonArray rows = legacy_rows();
    QJsonObject broken = rows[0].toObject();
    broken[QStringLiteral("report_date_as_yyyy_mm_dd")] = QStringLiteral("not-a-date");
    rows.replace(0, broken);

    const CftcHistory history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.observations.isEmpty());
}

void TstCftcWorkspace::parse_rejects_duplicate_report_dates() {
    QJsonArray rows = legacy_rows();
    rows.append(legacy_rows()[0]);

    const CftcHistory history = cftc_parse_history(rows, CftcFamily::Legacy);
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.observations.isEmpty());
}

void TstCftcWorkspace::parse_rejects_empty_payload() {
    const CftcHistory history = cftc_parse_history(QJsonArray(), CftcFamily::Legacy);
    QVERIFY(!history.error.isEmpty());
    QVERIFY(history.observations.isEmpty());
}

// ── Analytics ───────────────────────────────────────────────────────────────

void TstCftcWorkspace::net_requires_both_legs() {
    CftcObservation obs;
    obs.longs = {10.0, 20.0, std::nullopt};
    obs.shorts = {4.0, std::nullopt, 5.0};
    QCOMPARE(*cftc_participant_net(obs, 0), 6.0);
    QVERIFY(!cftc_participant_net(obs, 1).has_value());
    QVERIFY(!cftc_participant_net(obs, 2).has_value());
    QVERIFY(!cftc_participant_net(obs, 9).has_value());
}

void TstCftcWorkspace::range_availability_is_anchored_at_latest_report() {
    QVector<CftcObservation> observations;
    observations.append(observation(QStringLiteral("2006-06-13"), 100, {1, 1, 1}, {1, 1, 1}));
    observations.append(observation(QStringLiteral("2016-06-14"), 100, {1, 1, 1}, {1, 1, 1}));
    observations.append(observation(QStringLiteral("2026-06-16"), 100, {1, 1, 1}, {1, 1, 1}));

    QVERIFY(cftc_range_available(observations, CftcRange::OneYear));
    QVERIFY(cftc_range_available(observations, CftcRange::TwentyYears));
    // 20 years before 2026-06-16 is 2006-06-16; the first report is one day
    // earlier, so 20Y is offered. 21Y would not be, which the enum cannot ask.
    QVERIFY(cftc_range_available(observations, CftcRange::Max));

    // A window is offered only when the returned history actually spans it,
    // exactly like the shared R1 series contract: one observation is a MAX-only
    // history, never a one-year history with a single point.
    QVector<CftcObservation> short_history;
    short_history.append(observation(QStringLiteral("2026-06-16"), 100, {1, 1, 1}, {1, 1, 1}));
    QVERIFY(!cftc_range_available(short_history, CftcRange::OneYear));
    QVERIFY(!cftc_range_available(short_history, CftcRange::TwoYears));
    QVERIFY(cftc_range_available(short_history, CftcRange::Max));

    QVector<CftcObservation> empty;
    QVERIFY(!cftc_range_available(empty, CftcRange::Max));
    QVERIFY(!cftc_range_available(empty, CftcRange::OneYear));
}

void TstCftcWorkspace::range_filter_never_synthesises_observations() {
    QVector<CftcObservation> observations;
    observations.append(observation(QStringLiteral("2024-01-02"), 1, {1, 1, 1}, {1, 1, 1}));
    observations.append(observation(QStringLiteral("2025-06-03"), 2, {1, 1, 1}, {1, 1, 1}));
    observations.append(observation(QStringLiteral("2026-06-16"), 3, {1, 1, 1}, {1, 1, 1}));

    // Latest is 2026-06-16, so the 2Y window starts at 2024-06-16; the
    // 2024-01-02 observation is outside it and the filter must not invent a
    // boundary point.
    const auto filtered = cftc_filter_range(observations, CftcRange::TwoYears);
    QCOMPARE(filtered.size(), 2);
    QCOMPARE(filtered.first().date, QDate(2025, 6, 3));
    QCOMPARE(filtered.last().date, QDate(2026, 6, 16));
    QCOMPARE(cftc_filter_range(observations, CftcRange::Max).size(), 3);
}

void TstCftcWorkspace::weekly_change_requires_a_weekly_neighbour() {
    const auto weekly = weekly_values({10, 20, 30}, QDate(2026, 9, 1));
    const CftcChange change = cftc_weekly_change(weekly);
    QVERIFY(change.has_pair);
    QVERIFY(change.has_value);
    QCOMPARE(change.gap_days, 7);
    QCOMPARE(change.value, 10.0);
    QCOMPARE(change.previous_date, QDate(2026, 9, 8));

    auto gapped = weekly;
    gapped.last().date = QDate(2026, 10, 6); // 28 days after the previous report
    const CftcChange stale = cftc_weekly_change(gapped);
    QVERIFY(stale.has_pair);
    QVERIFY(!stale.has_value);
    QCOMPARE(stale.gap_days, 28);

    const CftcChange single = cftc_weekly_change(weekly_values({10}, QDate(2026, 9, 1)));
    QVERIFY(!single.has_pair);
    QVERIFY(!single.has_value);
}

void TstCftcWorkspace::change_since_days_uses_the_last_report_at_or_before_the_target() {
    const auto series = weekly_values({10, 20, 30, 40, 50}, QDate(2026, 9, 1));
    // Latest 2026-09-29; 28 days back = 2026-09-01.
    QCOMPARE(*cftc_change_since_days(series, 28), 40.0);
    // 13 weeks back reaches before the series start: unavailable.
    QVERIFY(!cftc_change_since_days(series, 91).has_value());

    // With exactly two weekly observations, a 7-day change reaches the first
    // one; a 14-day change reaches past the series start and stays unavailable.
    const auto short_series = weekly_values({10, 12}, QDate(2026, 9, 1));
    QCOMPARE(*cftc_change_since_days(short_series, 7), 2.0);
    QVERIFY(!cftc_change_since_days(short_series, 14).has_value());

    // A long report gap must not be labelled a 4-week change: the only
    // observation at least 28 days back is far outside the 28(+10)-day bound.
    QVector<CftcDatedValue> gapped;
    gapped.append(dated(2026, 1, 6, 10));
    gapped.append(dated(2026, 4, 7, 20));
    gapped.append(dated(2026, 4, 14, 30));
    QVERIFY(!cftc_change_since_days(gapped, 28).has_value());
    QCOMPARE(*cftc_change_since_days(gapped, 7), 10.0);
}

void TstCftcWorkspace::window_stats_known_series() {
    const auto window = weekly_values({10, 20, 30, 40, 50}, QDate(2026, 9, 1));
    const CftcWindowStats stats = cftc_window_stats(window);

    QCOMPARE(stats.count, 5);
    QCOMPARE(stats.latest, 50.0);
    QCOMPARE(stats.min_value, 10.0);
    QCOMPARE(stats.max_value, 50.0);
    QCOMPARE(stats.avg, 30.0);
    QCOMPARE(stats.cot_index, 100.0);
    QCOMPARE(stats.percentile, 100.0);
    QVERIFY(stats.has_zscore);
    QVERIFY(qAbs(stats.zscore - 1.2649110640673518) < 1e-9);
    QCOMPARE(stats.distance_high, 0.0);
    QCOMPARE(stats.distance_low, 40.0);
    QVERIFY(stats.has_change_4w);
    QCOMPARE(stats.change_4w, 40.0);
    QVERIFY(!stats.has_change_13w);
    QVERIFY(!stats.zero_variance);

    const CftcWindowStats min_at_latest = cftc_window_stats(weekly_values({50, 40, 30, 20, 10}, QDate(2026, 9, 1)));
    QCOMPARE(min_at_latest.cot_index, 0.0);
    QCOMPARE(min_at_latest.percentile, 20.0);
    QCOMPARE(min_at_latest.distance_high, -40.0);
    QCOMPARE(min_at_latest.distance_low, 0.0);
}

void TstCftcWorkspace::window_stats_degenerate_cases() {
    const CftcWindowStats empty = cftc_window_stats({});
    QCOMPARE(empty.count, 0);
    QVERIFY(!empty.has_latest);
    QVERIFY(!empty.has_cot_index);
    QVERIFY(!empty.has_zscore);

    const CftcWindowStats single = cftc_window_stats(weekly_values({42}, QDate(2026, 9, 1)));
    QCOMPARE(single.count, 1);
    QVERIFY(single.has_latest);
    QCOMPARE(single.latest, 42.0);
    QVERIFY2(!single.has_cot_index, "one observation cannot define a range");
    QVERIFY(!single.has_percentile);
    QVERIFY(!single.has_zscore);
    QVERIFY2(!single.zero_variance, "a single observation is not a zero-variance window");
    QCOMPARE(single.distance_high, 0.0);
    QCOMPARE(single.distance_low, 0.0);

    const CftcWindowStats flat = cftc_window_stats(weekly_values({5, 5, 5, 5}, QDate(2026, 9, 1)));
    QVERIFY(flat.zero_variance);
    QVERIFY2(!flat.has_cot_index, "a zero-variance window must not divide by zero");
    QVERIFY(!flat.has_percentile);
    QVERIFY(!flat.has_zscore);
    QCOMPARE(flat.distance_high, 0.0);
    QCOMPARE(flat.distance_low, 0.0);
}

void TstCftcWorkspace::heatmap_series_gates_short_prefixes() {
    const auto window = weekly_values({10, 20, 30, 40, 50, 60, 70, 80, 90, 100}, QDate(2026, 1, 6));
    const auto points = cftc_heatmap_series(window, 4);

    QCOMPARE(points.size(), 4);
    QCOMPARE(points.first().net, 70.0);
    QCOMPARE(points.last().net, 100.0);
    QVERIFY2(!points.first().has_cot_index, "the first column has only 7 trailing observations");
    QVERIFY2(points.last().has_cot_index, "the last column has the full 10");
    QCOMPARE(points.first().has_change, true);
    QCOMPARE(points.first().change, 10.0);

    const auto short_window = weekly_values({10, 20, 30}, QDate(2026, 1, 6));
    const auto gated = cftc_heatmap_series(short_window, 3);
    QCOMPARE(gated.size(), 3);
    for (const auto& point : gated) {
        QVERIFY(!point.has_cot_index);
        QVERIFY(!point.has_percentile);
        QVERIFY(!point.has_zscore);
    }
    QVERIFY(gated.last().has_change);

    QVERIFY(cftc_heatmap_series({}, 10).isEmpty());
    QCOMPARE(cftc_heatmap_series(window, 999).size(), 10);
}

void TstCftcWorkspace::price_helpers_align_to_report_dates() {
    QVector<CftcPricePoint> prices;
    prices.append({QDate(2026, 8, 28), 100.0});
    prices.append({QDate(2026, 9, 1), 110.0});
    prices.append({QDate(2026, 9, 8), 120.0});
    prices.append({QDate(2026, 9, 15), 130.0});

    QCOMPARE(*cftc_price_on_or_before(prices, QDate(2026, 9, 3)), 110.0);
    QCOMPARE(*cftc_price_on_or_before(prices, QDate(2026, 9, 1)), 110.0);
    QVERIFY(!cftc_price_on_or_before(prices, QDate(2026, 8, 1)).has_value());

    QCOMPARE(*cftc_price_change_since_days(prices, 14), 20.0);
    QCOMPARE(*cftc_price_change_since_days(prices, 7), 10.0);
    QVERIFY(!cftc_price_change_since_days(prices, 365).has_value());

    // A price series with a long hole must not present a spring-to-autumn span
    // as a 4-week change.
    QVector<CftcPricePoint> gapped;
    gapped.append({QDate(2026, 1, 5), 100.0});
    gapped.append({QDate(2026, 9, 1), 110.0});
    gapped.append({QDate(2026, 9, 15), 130.0});
    QVERIFY(!cftc_price_change_since_days(gapped, 28).has_value());
}

void TstCftcWorkspace::direction_alignment_rules() {
    QCOMPARE(cftc_direction(std::nullopt), CftcDirection::Unavailable);
    QCOMPARE(cftc_direction(0.0), CftcDirection::Flat);
    QCOMPARE(cftc_direction(1.0), CftcDirection::Up);
    QCOMPARE(cftc_direction(-1.0), CftcDirection::Down);

    QVERIFY(cftc_directions_aligned(CftcDirection::Up, CftcDirection::Up));
    QVERIFY(cftc_directions_aligned(CftcDirection::Down, CftcDirection::Down));
    QVERIFY(cftc_directions_aligned(CftcDirection::Flat, CftcDirection::Flat));
    QVERIFY(!cftc_directions_aligned(CftcDirection::Flat, CftcDirection::Up));
    QVERIFY(!cftc_directions_aligned(CftcDirection::Unavailable, CftcDirection::Up));
    QVERIFY(!cftc_directions_aligned(CftcDirection::Up, CftcDirection::Down));
}

void TstCftcWorkspace::extreme_dates_follow_values() {
    QDate high;
    QDate low;
    QVERIFY(cftc_net_extreme_dates(weekly_values({10, 60, 20, -5, 30}, QDate(2026, 1, 6)), high, low));
    QCOMPARE(high, QDate(2026, 1, 13));
    QCOMPARE(low, QDate(2026, 1, 27));
    QVERIFY(!cftc_net_extreme_dates({}, high, low));
}

QTEST_GUILESS_MAIN(TstCftcWorkspace)
#include "tst_cftc_workspace.moc"
