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

CftcObservation family_observation(CftcFamily family, const QDate& date, double open_interest, double long_leg,
                                   double short_leg) {
    const QVector<CftcParticipant> participants = cftc_family_participants(family);
    const int target = cftc_speculative_index(participants);
    QVector<std::optional<double>> longs;
    QVector<std::optional<double>> shorts;
    for (int i = 0; i < participants.size(); ++i) {
        longs.append(100.0);
        shorts.append(100.0);
    }
    if (target >= 0) {
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
                                      source, continuous, spot);
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
    void range_filters_only_the_visible_points();
    void horizon_context_exposes_interpretation_states();
    void price_unavailable_keeps_positioning_pane();
    void empty_price_source_is_unavailable();
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
