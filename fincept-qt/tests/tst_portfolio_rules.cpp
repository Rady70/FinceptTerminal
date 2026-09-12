// tests/tst_portfolio_rules.cpp
//
// The portfolio presentation rules that keep missing, genuine zero and
// directional states distinct: trend/colour direction, contribution
// availability, and presence-aware ordering. Header-only over plain types so
// this suite needs no widgets, no services and no DataHub (tests/ HARD RULE).
#include "screens/portfolio/PortfolioDisplayRules.h"

#include <QtTest>

using namespace fincept::screens;

class TstPortfolioRules : public QObject {
    Q_OBJECT
  private slots:
    void trend_two_points_distinguishes_direction();
    void trend_falls_back_to_observed_day_change();
    void trend_without_observation_is_unavailable();
    void contribution_is_unavailable_with_zero_denominator();
    void sort_keeps_missing_last_in_both_directions();
};

void TstPortfolioRules::trend_two_points_distinguishes_direction() {
    QCOMPARE(portfolio_trend_direction(2, 1.0, 2.0, false, 0.0), 1);
    QCOMPARE(portfolio_trend_direction(2, 2.0, 1.0, false, 0.0), -1);
    // Equal first/last is flat, not positive.
    QCOMPARE(portfolio_trend_direction(2, 5.0, 5.0, false, 0.0), 0);
    QCOMPARE(portfolio_trend_direction(5, 3.0, 3.0, false, 0.0), 0);
}

void TstPortfolioRules::trend_falls_back_to_observed_day_change() {
    // A single point cannot show a trend; the observed day change decides.
    QCOMPARE(portfolio_trend_direction(1, 0.0, 0.0, true, 1.5), 1);
    QCOMPARE(portfolio_trend_direction(1, 0.0, 0.0, true, -2.0), -1);
    // An observed flat day is neutral, not positive.
    QCOMPARE(portfolio_trend_direction(1, 0.0, 0.0, true, 0.0), 0);
}

void TstPortfolioRules::trend_without_observation_is_unavailable() {
    // No two-point history and no observed change: unavailable (2), never up.
    QCOMPARE(portfolio_trend_direction(1, 0.0, 0.0, false, 0.0), 2);
    QCOMPARE(portfolio_trend_direction(0, 0.0, 0.0, false, 0.0), 2);
}

void TstPortfolioRules::contribution_is_unavailable_with_zero_denominator() {
    // Gains and losses that cancel leave no meaningful per-holding share.
    QVERIFY(!portfolio_contribution_available(0.0));
    QVERIFY(portfolio_contribution_available(0.01));
    QVERIFY(portfolio_contribution_available(-1000.0));
}

void TstPortfolioRules::sort_keeps_missing_last_in_both_directions() {
    // Present readings sort before missing ones in ascending and descending
    // order; a missing row never ranks by its hidden fallback value.
    QVERIFY(portfolio_sort_before(true, 5.0, false, 0.0, true));
    QVERIFY(portfolio_sort_before(true, 5.0, false, 0.0, false));
    QVERIFY(!portfolio_sort_before(false, 0.0, true, 5.0, true));
    QVERIFY(!portfolio_sort_before(false, 0.0, true, 5.0, false));

    // A genuine zero is a present reading.
    QVERIFY(portfolio_sort_before(true, 0.0, false, 0.0, true));
    QVERIFY(portfolio_sort_before(true, 0.0, true, 5.0, true));
    QVERIFY(portfolio_sort_before(true, 5.0, true, 0.0, false));

    // Two missing rows are equivalent in both directions.
    QVERIFY(!portfolio_sort_before(false, 0.0, false, 0.0, true));
    QVERIFY(!portfolio_sort_before(false, 0.0, false, 0.0, false));
}

QTEST_GUILESS_MAIN(TstPortfolioRules)
#include "tst_portfolio_rules.moc"
