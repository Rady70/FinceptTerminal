// tests/tst_cftc_workspace_contract.cpp
//
// The pure CFTC presentation contracts
// (screens/economics/panels/CftcWorkspaceContract.h): the 1W | 4W | 13W
// interpretation-horizon vocabulary and the label of the strict
// 156-prior-report Net %OI percentile. Header-only over Qt Core; no app
// sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcWorkspaceContract.h"

#include <QtTest>

using namespace fincept::screens;

class TstCftcWorkspaceContract : public QObject {
    Q_OBJECT
  private slots:
    void horizon_vocabulary_is_fixed();
    void percentile_label_names_the_measure();
};

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

void TstCftcWorkspaceContract::percentile_label_names_the_measure() {
    const QString interpretation = cftc_interpretation_percentile_label();
    QVERIFY(interpretation.contains(QStringLiteral("156 prior reports")));
    QVERIFY(interpretation.contains(QStringLiteral("Net %OI")));
}

QTEST_GUILESS_MAIN(TstCftcWorkspaceContract)
#include "tst_cftc_workspace_contract.moc"
