// tests/tst_cftc_net_format.cpp
//
// The CFTC net-position sign rule: only a strictly positive net carries a "+",
// an exact zero is neutral and must not render as "+0", and a negative net
// keeps its sign. Header-only over Qt Core; no app sources (tests/ HARD RULE).
#include "screens/economics/panels/CftcNetFormat.h"

#include <QtTest>

class TstCftcNetFormat : public QObject {
    Q_OBJECT
  private slots:
    void positive_net_gets_plus();
    void exact_zero_net_is_neutral();
    void negative_net_keeps_minus();
};

void TstCftcNetFormat::positive_net_gets_plus() {
    QCOMPARE(fincept::screens::cftc_signed_net(261098), QStringLiteral("+261098"));
}

void TstCftcNetFormat::exact_zero_net_is_neutral() {
    QCOMPARE(fincept::screens::cftc_signed_net(0), QStringLiteral("0"));
    QVERIFY(!fincept::screens::cftc_signed_net(0).startsWith(QLatin1Char('+')));
}

void TstCftcNetFormat::negative_net_keeps_minus() {
    QCOMPARE(fincept::screens::cftc_signed_net(-261098), QStringLiteral("-261098"));
}

QTEST_GUILESS_MAIN(TstCftcNetFormat)
#include "tst_cftc_net_format.moc"
