// tests/tst_cftc_sync_chart.cpp
//
// Batch 4B of the CFTC work: the pure data contract behind the synchronized
// Price + Positioning chart (screens/economics/panels/CftcSyncChartData.h).
// It proves that price is aligned to official CFTC report dates without using
// a later observation, that unavailable price dates remain gaps and are never
// interpolated, that price context from a previous market cannot populate a
// new market, that proxy/source labels stay truthful, that the positioning
// pane carries the report-family-appropriate primary participant's reported
// net position, that the visible range filters the drawn points only, and that
// the 1/4/13-report interpretation context is exposed. Header-only over Qt
// Core; no app sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcSyncChartData.h"

#include <QtTest>

using namespace fincept::screens;
using namespace fincept::services;
using namespace fincept::ui;

namespace {

const QDate kLatest(2026, 9, 15);

/// Hard-coded expected principal observation slot per family, matching the
/// frozen Batch 1 participant tables (Legacy non_commercial = 1, Disaggregated
/// managed_money = 2, TFF leveraged_funds = 2). Tests never derive expectations
/// through the helper under test or through the generic speculative flag.
int expected_principal_slot(CftcFamily family) {
    switch (family) {
        case CftcFamily::Disaggregated:
            return 2;
        case CftcFamily::Tff:
            return 2;
        case CftcFamily::Legacy:
            break;
    }
    return 1;
}

CftcObservation family_observation(CftcFamily family, const QDate& date, double open_interest, double long_leg,
                                   double short_leg) {
    const QVector<CftcParticipant> participants = cftc_family_participants(family);
    const int target = expected_principal_slot(family);
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
    for (int i = 0; i < participants.size(); ++i) {
        longs.append(100.0);
        shorts.append(100.0);
    }
    if (target >= 0 && target < participants.size()) {
        longs[target] = long_leg;
        shorts[target] = short_leg;
    }
    CftcObservation observation;
    observation.date = date;
    observation.date_label = date.toString(Qt::ISODate);
    observation.market = QStringLiteral("TEST - EXCHANGE");
    observation.contract_code = QStringLiteral("000000");
    observation.units = QStringLiteral("Test units");
    observation.open_interest = open_interest;
    observation.longs = longs;
    observation.shorts = shorts;
    return observation;
}

QVector<CftcObservation> make_series(CftcFamily family, int count, double long_leg, double short_leg,
                                     double open_interest = 1000.0) {
    QVector<CftcObservation> out;
    out.reserve(count);
    for (int i = 0; i < count; ++i)
        out.append(
            family_observation(family, kLatest.addDays(-7LL * (count - 1 - i)), open_interest, long_leg, short_leg));
    return out;
}

CftcInterpretationState make_state(const QString& state_id, const QString& participant_key, int horizon = -1) {
    CftcInterpretationState state;
    state.state_id = state_id;
    state.participant_key = participant_key;
    if (horizon > 0) {
        state.has_horizon = true;
        state.horizon_reports = horizon;
    }
    return state;
}

CftcInterpretationResult make_result(CftcFamily family) {
    CftcInterpretationResult result;
    result.rule_set_version = cftc_interpretation_rule_set_version();
    result.family = family;
    result.family_code = cftc_family_code(family);
    result.report_date = kLatest;
    result.report_date_available = true;
    result.latest_observation_date = kLatest;
    result.open_interest_available = true;
    result.open_interest = 1000.0;
    for (const auto& candidate : cftc_family_participants(family)) {
        CftcParticipantInterpretation participant;
        participant.participant_key = candidate.key;
        participant.label = candidate.label;
        participant.terminology = cftc_participant_terminology(family, candidate.key);
        participant.terminology_code = cftc_terminology_code(participant.terminology);
        participant.crowding_terminology_allowed = cftc_crowding_terminology_allowed(family, candidate.key);
        result.participants.append(participant);
    }
    return result;
}

/// The expected principal participant per family, hard-coded rather than
/// derived through the helper under test or the generic speculative flag.
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

CftcParticipantInterpretation* primary_participant(CftcInterpretationResult& result) {
    const QString key = expected_principal_key(result.family);
    for (auto& participant : result.participants) {
        if (participant.participant_key == key)
            return &participant;
    }
    return result.participants.isEmpty() ? nullptr : &result.participants.first();
}

QVector<CftcPricePoint> report_prices(const QVector<CftcObservation>& observations,
                                      const QVector<int>& skip_indices = {}) {
    QVector<CftcPricePoint> prices;
    for (int i = 0; i < observations.size(); ++i) {
        if (skip_indices.contains(i))
            continue;
        prices.append({observations[i].date.addDays(-2), 100.0 + i});
    }
    return prices;
}

CftcSyncChartData build(const CftcInterpretationResult& interpretation, const QVector<CftcObservation>& observations,
                        const QVector<CftcPricePoint>& prices, const QString& requested_market,
                        const QString& price_market, CftcRange range, const QString& source, bool continuous,
                        bool spot) {
    return cftc_build_sync_chart_data(interpretation, observations, prices, requested_market, price_market, range,
                                      CftcPriceContextState::Ready, QString(), source, continuous, spot);
}

CftcSyncChartData build_with_price_state(const CftcInterpretationResult& interpretation,
                                         const QVector<CftcObservation>& observations,
                                         const QVector<CftcPricePoint>& prices, const QString& requested_market,
                                         const QString& price_market, CftcRange range,
                                         CftcPriceContextState price_state, const QString& price_reason,
                                         const QString& source) {
    return cftc_build_sync_chart_data(interpretation, observations, prices, requested_market, price_market, range,
                                      price_state, price_reason, source, true, false);
}

int segment_count(const QVector<fincept::ui::TimeSeriesPoint>& points) {
    return fincept::ui::split_time_series_gaps(points, QStringLiteral("Weekly")).size();
}

} // namespace

class TstCftcSyncChart : public QObject {
    Q_OBJECT
  private slots:
    void price_aligns_to_report_dates_without_future_observation();
    void missing_price_dates_remain_gaps();
    void stale_price_from_previous_market_is_rejected();
    void proxy_and_source_labels_are_truthful();
    void positioning_pane_uses_primary_participant_net();
    void positioning_uses_terminology_principal_per_family();
    void missing_principal_leaves_context_empty();
    void range_filters_only_the_visible_points();
    void horizon_context_exposes_interpretation_states();
    void price_unavailable_keeps_positioning_pane();
    void empty_price_source_is_unavailable();
    void pending_price_reports_pending_not_unspecified();
    void provider_failure_reason_is_carried_into_the_chart();
    void no_mapped_source_reason_is_carried_into_the_chart();
    void hover_values_share_the_snapped_report_date();
    void hover_snap_dates_are_shared_between_panes();
    void deterministic_repeatability();
};

void TstCftcSyncChart::price_aligns_to_report_dates_without_future_observation() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    QVector<CftcPricePoint> prices = report_prices(observations);
    // A later observation must never stand in for an earlier report date.
    prices.append({observations[3].date.addDays(3), 999.0});
    std::stable_sort(prices.begin(), prices.end(),
                     [](const CftcPricePoint& a, const CftcPricePoint& b) { return a.date < b.date; });

    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data =
        build(interpretation, observations, prices, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::Max,
              QStringLiteral("TEST source"), true, false);

    QVERIFY(data.price.available);
    QCOMPARE(data.price.points.size(), observations.size());
    for (int i = 0; i < data.price.points.size(); ++i) {
        QCOMPARE(data.price.points[i].date, observations[i].date);
        QCOMPARE(data.price.points[i].date_label, observations[i].date_label);
        QVERIFY(data.price.points[i].date <= observations[i].date);
        QCOMPARE(data.price.points[i].value, 100.0 + i);
    }
    QCOMPARE(data.price.points[3].value, 103.0);
    QVERIFY(data.price.points[3].value != 999.0);
}

void TstCftcSyncChart::missing_price_dates_remain_gaps() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    const QVector<CftcPricePoint> prices = report_prices(observations, {2, 3});

    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data =
        build(interpretation, observations, prices, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::Max,
              QStringLiteral("TEST source"), true, false);

    QVERIFY(data.price.available);
    QCOMPARE(data.price.points.size(), 5);
    for (const auto& point : data.price.points)
        QVERIFY(point.date != observations[3].date);
    QCOMPARE(segment_count(data.price.points), 2);
}

void TstCftcSyncChart::stale_price_from_previous_market_is_rejected() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    const QVector<CftcPricePoint> prices = report_prices(observations);

    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData stale =
        build(interpretation, observations, prices, QStringLiteral("gold"), QStringLiteral("silver"), CftcRange::Max,
              QStringLiteral("TEST source"), true, false);
    QVERIFY(!stale.price.available);
    QVERIFY(stale.price.points.isEmpty());
    QVERIFY(!stale.price.unavailable_reason.isEmpty());

    const CftcSyncChartData current =
        build(interpretation, observations, prices, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::Max,
              QStringLiteral("TEST source"), true, false);
    QVERIFY(current.price.available);
    QVERIFY(!current.price.points.isEmpty());
}

void TstCftcSyncChart::proxy_and_source_labels_are_truthful() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    const QVector<CftcPricePoint> prices = report_prices(observations);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);

    const CftcSyncChartData futures =
        build(interpretation, observations, prices, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::Max,
              QStringLiteral("Yahoo Finance — GC=F front-month continuous futures"), true, false);
    QCOMPARE(futures.price_source, QStringLiteral("Yahoo Finance — GC=F front-month continuous futures"));
    QVERIFY(futures.price_continuous_proxy);
    QVERIFY(!futures.price_spot_index);

    const CftcSyncChartData spot =
        build(interpretation, observations, prices, QStringLiteral("vix"), QStringLiteral("vix"), CftcRange::Max,
              QStringLiteral("Yahoo Finance — ^VIX spot index"), false, true);
    QCOMPARE(spot.price_source, QStringLiteral("Yahoo Finance — ^VIX spot index"));
    QVERIFY(!spot.price_continuous_proxy);
    QVERIFY(spot.price_spot_index);
}

void TstCftcSyncChart::positioning_pane_uses_primary_participant_net() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data =
        build(interpretation, observations, report_prices(observations), QStringLiteral("gold"), QStringLiteral("gold"),
              CftcRange::Max, QStringLiteral("TEST source"), true, false);

    QVERIFY(data.positioning.available);
    QCOMPARE(data.positioning.points.size(), observations.size());
    for (int i = 0; i < data.positioning.points.size(); ++i) {
        QCOMPARE(data.positioning.points[i].date, observations[i].date);
        QCOMPARE(data.positioning.points[i].value, 200.0); // 600 long - 400 short
    }
    QCOMPARE(data.positioning_label, QStringLiteral("Non-Commercial"));
}

void TstCftcSyncChart::range_filters_only_the_visible_points() {
    QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 190, 400.0, 600.0);
    for (int i = 170; i < observations.size(); ++i) {
        const int step = i - 169;
        observations[i].longs[1] = 400.0 + 300.0 * step;
        observations[i].shorts[1] = 600.0 - 20.0 * step;
    }
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    interpretation.participants[1].states
        << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), QStringLiteral("non_commercial"), 4);
    interpretation.participants[1].states
        << make_state(QStringLiteral("LONG_ACCUMULATION"), QStringLiteral("non_commercial"), 4);

    const CftcSyncChartData max =
        build(interpretation, observations, report_prices(observations), QStringLiteral("gold"), QStringLiteral("gold"),
              CftcRange::Max, QStringLiteral("TEST source"), true, false);
    const CftcSyncChartData one_year =
        build(interpretation, observations, report_prices(observations), QStringLiteral("gold"), QStringLiteral("gold"),
              CftcRange::OneYear, QStringLiteral("TEST source"), true, false);

    QCOMPARE(max.positioning.points.size(), observations.size());
    QVERIFY(one_year.positioning.points.size() < max.positioning.points.size());
    QCOMPARE(one_year.horizon_context, max.horizon_context);
    QCOMPARE(one_year.positioning_label, max.positioning_label);
    QCOMPARE(one_year.report_date, max.report_date);
}

void TstCftcSyncChart::horizon_context_exposes_interpretation_states() {
    CftcInterpretationResult interpretation = make_result(CftcFamily::Disaggregated);
    CftcParticipantInterpretation* primary = primary_participant(interpretation);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 1);
    primary->states << make_state(QStringLiteral("NET_LONGWARD_SHIFT"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("LONG_ACCUMULATION"), primary->participant_key, 4);
    primary->states << make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 13);

    const QVector<CftcObservation> observations = make_series(CftcFamily::Disaggregated, 6, 600.0, 400.0);
    const CftcSyncChartData data =
        build(interpretation, observations, report_prices(observations), QStringLiteral("gold"), QStringLiteral("gold"),
              CftcRange::Max, QStringLiteral("TEST source"), true, false);

    QCOMPARE(data.positioning_label, QStringLiteral("Managed Money"));
    QCOMPARE(data.horizon_context.size(), 3);
    QVERIFY(data.horizon_context[0].startsWith(QStringLiteral("one report:")));
    QVERIFY(data.horizon_context[0].contains(QStringLiteral("longward shift")));
    QVERIFY(data.horizon_context[1].startsWith(QStringLiteral("four reports:")));
    QVERIFY(data.horizon_context[1].contains(QStringLiteral("longward shift")));
    QVERIFY(data.horizon_context[1].contains(QStringLiteral("long accumulation")));
    QVERIFY(data.horizon_context[2].startsWith(QStringLiteral("thirteen reports:")));
    QVERIFY(data.horizon_context[2].contains(QStringLiteral("shortward shift")));
}

void TstCftcSyncChart::price_unavailable_keeps_positioning_pane() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data =
        build(interpretation, observations, {}, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::Max,
              QStringLiteral("TEST source"), true, false);

    QVERIFY(!data.price.available);
    QVERIFY(!data.price.unavailable_reason.isEmpty());
    QVERIFY(data.positioning.available);
    QCOMPARE(data.positioning.points.size(), observations.size());
}

void TstCftcSyncChart::empty_price_source_is_unavailable() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data =
        build(interpretation, observations, report_prices(observations), QStringLiteral("gold"), QStringLiteral("gold"),
              CftcRange::Max, QString(), true, false);
    QVERIFY(!data.price.available);
    QVERIFY(!data.price.unavailable_reason.isEmpty());
    QVERIFY(data.positioning.available);
}

void TstCftcSyncChart::positioning_uses_terminology_principal_per_family() {
    const CftcFamily families[] = {CftcFamily::Legacy, CftcFamily::Disaggregated, CftcFamily::Tff};
    const QStringList labels = {QStringLiteral("Non-Commercial"), QStringLiteral("Managed Money"),
                                QStringLiteral("Leveraged Funds")};
    for (int i = 0; i < 3; ++i) {
        const CftcFamily family = families[i];
        const QVector<CftcObservation> observations = make_series(family, 6, 600.0, 400.0);
        CftcInterpretationResult interpretation = make_result(family);
        const CftcSyncChartData data =
            build(interpretation, observations, report_prices(observations), QStringLiteral("gold"),
                  QStringLiteral("gold"), CftcRange::Max, QStringLiteral("TEST source"), true, false);
        QCOMPARE(data.positioning_label, labels[i]);
        QVERIFY(data.positioning.available);
        QCOMPARE(data.positioning.points.size(), observations.size());
        for (const auto& point : data.positioning.points)
            QCOMPARE(point.value, 200.0); // the expected principal slot's 600 - 400
        QCOMPARE(data.positioning_palette_index, expected_principal_slot(family));
    }
}

void TstCftcSyncChart::missing_principal_leaves_context_empty() {
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    for (int i = interpretation.participants.size() - 1; i >= 0; --i) {
        if (interpretation.participants[i].participant_key == QStringLiteral("non_commercial"))
            interpretation.participants.removeAt(i);
    }
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    const CftcSyncChartData data =
        build(interpretation, observations, report_prices(observations), QStringLiteral("gold"), QStringLiteral("gold"),
              CftcRange::Max, QStringLiteral("TEST source"), true, false);
    QVERIFY(!data.positioning.available);
    QVERIFY(!data.positioning.unavailable_reason.isEmpty());
    // A participant that was never resolved must not produce "no material
    // state" context lines.
    QVERIFY(data.horizon_context.isEmpty());
}

void TstCftcSyncChart::pending_price_reports_pending_not_unspecified() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data =
        build_with_price_state(interpretation, observations, {}, QStringLiteral("gold"), QStringLiteral("gold"),
                               CftcRange::Max, CftcPriceContextState::Pending, QString(), QString());
    QVERIFY(!data.price.available);
    QCOMPARE(data.price.unavailable_reason, QStringLiteral("price context is pending"));
    QVERIFY(!data.price.unavailable_reason.contains(QStringLiteral("unspecified")));
    QVERIFY(data.positioning.available);
}

void TstCftcSyncChart::provider_failure_reason_is_carried_into_the_chart() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data = build_with_price_state(
        interpretation, observations, {}, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::Max,
        CftcPriceContextState::Unavailable, QStringLiteral("the Yahoo Finance history request failed"), QString());
    QVERIFY(!data.price.available);
    QCOMPARE(data.price.unavailable_reason, QStringLiteral("the Yahoo Finance history request failed"));
    QVERIFY(!data.price.unavailable_reason.contains(QStringLiteral("unspecified")));
    QVERIFY(data.positioning.available);
}

void TstCftcSyncChart::no_mapped_source_reason_is_carried_into_the_chart() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    const CftcSyncChartData data =
        build_with_price_state(interpretation, observations, {}, QStringLiteral("gold"), QStringLiteral("gold"),
                               CftcRange::Max, CftcPriceContextState::Unavailable,
                               QStringLiteral("no retained free price source is mapped for this market"), QString());
    QVERIFY(!data.price.available);
    QCOMPARE(data.price.unavailable_reason, QStringLiteral("no retained free price source is mapped for this market"));
    QVERIFY(data.positioning.available);
}

void TstCftcSyncChart::hover_values_share_the_snapped_report_date() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    // A gap: report 3 carries no qualifying price.
    const CftcSyncChartData data =
        build(interpretation, observations, report_prices(observations, {2, 3}), QStringLiteral("gold"),
              QStringLiteral("gold"), CftcRange::Max, QStringLiteral("TEST source"), true, false);
    QVERIFY(data.price.available);
    QVERIFY(data.positioning.available);

    const CftcSyncHoverValue both = cftc_sync_hover_values(data, observations[1].date);
    QCOMPARE(both.date, observations[1].date);
    QVERIFY(both.has_price);
    QVERIFY(both.has_positioning);
    QCOMPARE(both.positioning, 200.0);

    const CftcSyncHoverValue gap = cftc_sync_hover_values(data, observations[3].date);
    QVERIFY(!gap.has_price);
    QVERIFY(gap.has_positioning);
    QCOMPARE(gap.positioning, 200.0);

    const CftcSyncHoverValue absent = cftc_sync_hover_values(data, observations[0].date.addDays(-1));
    QVERIFY(!absent.has_price);
    QVERIFY(!absent.has_positioning);
    QCOMPARE(absent.price, 0.0);
    QCOMPARE(absent.positioning, 0.0);
}

void TstCftcSyncChart::hover_snap_dates_are_shared_between_panes() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 6, 600.0, 400.0);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    // Reports 3 and 4 carry no qualifying price: a deliberate price gap.
    const CftcSyncChartData data =
        build(interpretation, observations, report_prices(observations, {2, 3}), QStringLiteral("gold"),
              QStringLiteral("gold"), CftcRange::Max, QStringLiteral("TEST source"), true, false);
    QVERIFY(data.positioning.available);
    QVERIFY(!data.price.points.isEmpty());
    for (const auto& point : data.price.points)
        QVERIFY(point.date != observations[3].date);

    // The shared snap set is the sorted union of both series' report dates, so
    // the missing-price report date is selectable even though the price pane
    // itself has no point there.
    const QVector<QDate> snap_dates = cftc_sync_snap_dates(data);
    QCOMPARE(snap_dates.size(), observations.size());
    for (const auto& observation : observations)
        QVERIFY(snap_dates.contains(observation.date));
    for (int i = 1; i < snap_dates.size(); ++i)
        QVERIFY(snap_dates[i - 1] < snap_dates[i]);

    const qint64 min_ms = QDateTime(observations.first().date, QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 max_ms = QDateTime(observations.last().date, QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 gap_ms = QDateTime(observations[3].date, QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 near_gap_ms = gap_ms - 2LL * 86400000LL;
    QCOMPARE(cftc_sync_nearest_snap_date(snap_dates, gap_ms, min_ms, max_ms), observations[3].date);
    // A position nearer the gap than any surviving price date snaps to the gap
    // date itself; nothing is interpolated toward the neighbouring prices.
    QCOMPARE(cftc_sync_nearest_snap_date(snap_dates, near_gap_ms, min_ms, max_ms), observations[3].date);
    // Both panes call the shared helper with the same set, target and window,
    // so they cannot resolve to different dates.
    QCOMPARE(cftc_sync_nearest_snap_date(snap_dates, near_gap_ms, min_ms, max_ms),
             cftc_sync_nearest_snap_date(snap_dates, near_gap_ms, min_ms, max_ms));

    // Hovering the shared snap date exposes the truthful positioning-without-
    // price readout, which the old per-pane price snapping could never reach.
    const CftcSyncHoverValue gap = cftc_sync_hover_values(data, observations[3].date);
    QVERIFY(!gap.has_price);
    QVERIFY(gap.has_positioning);

    // A window that contains no snap date resolves to an invalid date, and an
    // empty snap set never snaps.
    const qint64 before_first_ms = QDateTime(observations[0].date, QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 after_first_ms = QDateTime(observations[1].date, QTime(0, 0)).toMSecsSinceEpoch();
    QVERIFY(!cftc_sync_nearest_snap_date(snap_dates, min_ms, before_first_ms + 86400000LL, after_first_ms - 86400000LL)
                 .isValid());
    QVERIFY(!cftc_sync_nearest_snap_date({}, gap_ms, min_ms, max_ms).isValid());
    QCOMPARE(cftc_sync_nearest_snap_date(snap_dates, gap_ms, min_ms, max_ms), observations[3].date);
}

void TstCftcSyncChart::deterministic_repeatability() {
    const QVector<CftcObservation> observations = make_series(CftcFamily::Legacy, 12, 700.0, 300.0);
    const QVector<CftcPricePoint> prices = report_prices(observations);
    CftcInterpretationResult interpretation = make_result(CftcFamily::Legacy);
    CftcParticipantInterpretation* primary = primary_participant(interpretation);
    primary->states << make_state(QStringLiteral("NET_SHORTWARD_SHIFT"), primary->participant_key, 4);

    const CftcSyncChartData first =
        build(interpretation, observations, prices, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::TwoYears,
              QStringLiteral("TEST source"), true, false);
    const CftcSyncChartData second =
        build(interpretation, observations, prices, QStringLiteral("gold"), QStringLiteral("gold"), CftcRange::TwoYears,
              QStringLiteral("TEST source"), true, false);

    QCOMPARE(first.positioning.points.size(), second.positioning.points.size());
    QCOMPARE(first.price.points.size(), second.price.points.size());
    QCOMPARE(first.horizon_context, second.horizon_context);
    for (int i = 0; i < first.price.points.size(); ++i) {
        QCOMPARE(first.price.points[i].date, second.price.points[i].date);
        QCOMPARE(first.price.points[i].value, second.price.points[i].value);
    }
}

QTEST_GUILESS_MAIN(TstCftcSyncChart)
#include "tst_cftc_sync_chart.moc"
