// tests/tst_cftc_workspace_contract.cpp
//
// Batch 4B correction pass: the pure CFTC Analysis-page contract
// (screens/economics/panels/CftcWorkspaceContract.h). It pins the information
// hierarchy (header, current snapshot, interpretation, then the analytical
// charts and deeper tables), the sections the history-display range may
// refresh, the 1W | 4W | 13W interpretation-horizon vocabulary, and the
// percentile labelling that keeps the strict 156-prior-report Net %OI measure
// distinct from the selected-window raw-net statistic. Header-only over Qt
// Core; no app sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcWorkspaceContract.h"

#include <QtTest>

using namespace fincept::screens;

class TstCftcWorkspaceContract : public QObject {
    Q_OBJECT
  private slots:
    void section_order_is_the_information_hierarchy();
    void visible_range_refreshes_only_range_driven_sections();
    void section_order_guard_rejects_a_step_backwards();
    void horizon_vocabulary_is_fixed();
    void percentile_labels_separate_the_two_measures();
};

void TstCftcWorkspaceContract::section_order_is_the_information_hierarchy() {
    const QVector<CftcWorkspaceSection> order = cftc_workspace_section_order();
    const QVector<CftcWorkspaceSection> expected = {CftcWorkspaceSection::Header,
                                                    CftcWorkspaceSection::CurrentSnapshot,
                                                    CftcWorkspaceSection::Interpretation,
                                                    CftcWorkspaceSection::Positioning,
                                                    CftcWorkspaceSection::WeeklyChanges,
                                                    CftcWorkspaceSection::Statistics,
                                                    CftcWorkspaceSection::PricePositioningEvidence,
                                                    CftcWorkspaceSection::SyncChart,
                                                    CftcWorkspaceSection::HistoricalPositioning,
                                                    CftcWorkspaceSection::Heatmap};
    QCOMPARE(order, expected);

    // The snapshot must display before the interpretation and the participant
    // detail tables; the interpretation and those tables precede every chart.
    QVERIFY(order.indexOf(CftcWorkspaceSection::CurrentSnapshot) < order.indexOf(CftcWorkspaceSection::Interpretation));
    QVERIFY(order.indexOf(CftcWorkspaceSection::Interpretation) < order.indexOf(CftcWorkspaceSection::Positioning));
    QVERIFY(order.indexOf(CftcWorkspaceSection::Positioning) < order.indexOf(CftcWorkspaceSection::SyncChart));
    QVERIFY(order.indexOf(CftcWorkspaceSection::PricePositioningEvidence) <
            order.indexOf(CftcWorkspaceSection::SyncChart));
    QVERIFY(order.indexOf(CftcWorkspaceSection::Interpretation) <
            order.indexOf(CftcWorkspaceSection::HistoricalPositioning));
}

void TstCftcWorkspaceContract::visible_range_refreshes_only_range_driven_sections() {
    // The interpretation always evaluates the full validated history and the
    // 156-prior-report reference, so the history-display range must never
    // refresh (and never re-evaluate) it.
    QVERIFY(!cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::Interpretation));
    QVERIFY(!cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::WeeklyChanges));
    // The positioning table is latest-report legs and shares only after the
    // correction pass; it carries no selected-window statistic any more.
    QVERIFY(!cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::Positioning));

    QVERIFY(cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::SyncChart));
    QVERIFY(cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::HistoricalPositioning));
    QVERIFY(cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::Statistics));
    QVERIFY(cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::PricePositioningEvidence));
    QVERIFY(cftc_section_refreshed_by_visible_range(CftcWorkspaceSection::Heatmap));
}

void TstCftcWorkspaceContract::section_order_guard_rejects_a_step_backwards() {
    QVERIFY(cftc_section_order_stays_monotonic(-1, 0));
    QVERIFY(cftc_section_order_stays_monotonic(0, 1));
    QVERIFY(!cftc_section_order_stays_monotonic(1, 0));
    QVERIFY(!cftc_section_order_stays_monotonic(1, 1));
}

void TstCftcWorkspaceContract::horizon_vocabulary_is_fixed() {
    QCOMPARE(cftc_interpretation_horizons(), QVector<int>({1, 4, 13}));
    QVERIFY(cftc_is_interpretation_horizon(1));
    QVERIFY(cftc_is_interpretation_horizon(4));
    QVERIFY(cftc_is_interpretation_horizon(13));
    QVERIFY(!cftc_is_interpretation_horizon(0));
    QVERIFY(!cftc_is_interpretation_horizon(2));
    QVERIFY(!cftc_is_interpretation_horizon(156));
    QCOMPARE(cftc_default_interpretation_horizon(), 4);
    QCOMPARE(cftc_horizon_button_label(1), QStringLiteral("1W"));
    QCOMPARE(cftc_horizon_button_label(4), QStringLiteral("4W"));
    QCOMPARE(cftc_horizon_button_label(13), QStringLiteral("13W"));
    QVERIFY(cftc_horizon_button_label(2).isEmpty());
    QCOMPARE(cftc_horizon_evidence_header(1), QStringLiteral("1 REPORT"));
    QCOMPARE(cftc_horizon_evidence_header(4), QStringLiteral("4 REPORTS"));
    QCOMPARE(cftc_horizon_evidence_header(13), QStringLiteral("13 REPORTS"));
}

void TstCftcWorkspaceContract::percentile_labels_separate_the_two_measures() {
    const QString interpretation = cftc_interpretation_percentile_label();
    const QString window = cftc_window_percentile_label(QStringLiteral("2Y"));
    QVERIFY(interpretation.contains(QStringLiteral("156 prior reports")));
    QVERIFY(interpretation.contains(QStringLiteral("Net %OI")));
    QCOMPARE(window, QStringLiteral("WINDOW PERCENTILE (2Y)"));
    QVERIFY(window.contains(QStringLiteral("2Y")));
    QVERIFY(interpretation != window);

    const QString tooltip = cftc_window_percentile_tooltip();
    QVERIFY(tooltip.contains(QStringLiteral("different measure")));
    QVERIFY(tooltip.contains(QStringLiteral("156 prior reports")));
    QVERIFY(tooltip.contains(QStringLiteral("raw net position")));
    QVERIFY(tooltip.contains(QStringLiteral("latest report included")));
    for (const QString& verdict :
         {QStringLiteral("UPPER RANGE"), QStringLiteral("MIDDLE RANGE"), QStringLiteral("LOWER RANGE")}) {
        QVERIFY(!window.contains(verdict));
        QVERIFY(!tooltip.contains(verdict));
    }
}

QTEST_GUILESS_MAIN(TstCftcWorkspaceContract)
#include "tst_cftc_workspace_contract.moc"
