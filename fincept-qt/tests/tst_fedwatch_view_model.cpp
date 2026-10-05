#include "screens/economics/panels/FedWatchViewModel.h"

#include <QJsonArray>
#include <QtTest>

using namespace fincept::screens::fedwatch;

class TestFedWatchViewModel : public QObject {
    Q_OBJECT
  private slots:
    void missingValuesStayMissing() {
        QVERIFY(!value(QJsonValue()).has_value());
        QVERIFY(!value(QJsonValue(QJsonValue::Null)).has_value());
        QVERIFY(!value(QJsonValue("0")).has_value());
        QCOMPARE(*value(QJsonValue(0.0)), 0.0);
        QCOMPARE(pct(std::nullopt), QString::fromUtf8("—"));
        QCOMPARE(pct(0.0), QString("0.0%"));
        QCOMPARE(pp(std::nullopt), QString::fromUtf8("—"));
    }
    void signedChangesNeverShowSignedZero() {
        QCOMPARE(pp(3.25), QString("+3.3 pp"));
        QCOMPARE(pp(-0.04), QString("0.0 pp"));
        QCOMPARE(pp(-1.5), QString::fromUtf8("−1.5 pp"));
        QCOMPARE(bp_change(0.119), QString("+12 bp"));
        QCOMPARE(bp_change(-0.0001), QString("0 bp"));
        QCOMPARE(rate(3.9325), QString("3.93%"));
    }
    void labelsArePlainDecisionWords() {
        QCOMPARE(outcome_label(0, false), QString("Hold"));
        QCOMPARE(outcome_label(-25, false), QString("Cut 25 bp"));
        QCOMPARE(outcome_label(-50, true), QString("Cut 50+ bp"));
        QCOMPARE(outcome_label(50, true), QString("Hike 50+ bp"));
        QCOMPARE(outcome_key(50, true), QString("50:tail"));
        QCOMPARE(band_label(3.75, 4.0), QString::fromUtf8("3.75–4.00%"));
    }
    void workspaceParsingKeepsStatesAndOrder() {
        const QJsonObject data{
            {"generated_at", "2026-10-05T12:00:00Z"},
            {"next_meeting_date", "2026-10-28"},
            {"current_target_range", QJsonObject{{"lower", 3.75},
                                                 {"upper", 4.0},
                                                 {"carried_forward", true},
                                                 {"latest_observation_date", "2026-10-04"}}},
            {"meetings",
             QJsonArray{
                 QJsonObject{{"meeting_date", "2026-12-09"},
                             {"status", "UPCOMING"},
                             {"days_until", 65},
                             {"fed", QJsonObject{{"state", "STALE"},
                                                 {"distribution", QJsonArray{QJsonObject{{"rate_low", 4.0},
                                                                                         {"rate_high", 4.25},
                                                                                         {"probability_pct", 60.0}},
                                                                             QJsonObject{{"rate_low", 3.75},
                                                                                         {"rate_high", 4.0},
                                                                                         {"probability_pct", 40.0},
                                                                                         {"previous_week_pct", 35.0}}}},
                                                 {"expected_rate", 4.025}}},
                             {"polymarket", QJsonObject{{"state", "UNAVAILABLE"}}}},
                 QJsonObject{{"meeting_date", "2026-10-28"},
                             {"status", "UPCOMING"},
                             {"days_until", 23},
                             {"fed", QJsonObject{{"state", "CURRENT"}}},
                             {"polymarket", QJsonObject{{"state", "CURRENT"}}},
                             {"comparison", QJsonArray{QJsonObject{{"outcome_bp", 0},
                                                                   {"open_ended", false},
                                                                   {"fed_probability_pct", 80.0},
                                                                   {"polymarket_probability_pct", QJsonValue()}}}}}}}};
        const auto ws = parse_workspace(data);
        QVERIFY(ws.loaded);
        QCOMPARE(ws.meetings.size(), 2);
        QCOMPARE(ws.meetings[0].id, QString("2026-10-28")); // chronological
        QCOMPARE(*ws.target_mid(), 3.875);
        const auto* dec = ws.find("2026-12-09");
        QVERIFY(dec);
        QCOMPARE(dec->fed.state, QString("STALE"));
        QCOMPARE(dec->fed.bands.size(), 2);
        QCOMPARE(dec->fed.bands[0].low, 3.75); // ascending by rate
        QCOMPARE(*dec->fed.bands[0].previous_week, 35.0);
        QVERIFY(!dec->fed.bands[1].previous_week.has_value());
        QVERIFY(!dec->fed.bands[0].previous_day.has_value());
        const auto* oct = ws.find("2026-10-28");
        QCOMPARE(oct->comparison.size(), 1);
        QVERIFY(oct->comparison[0].fed.has_value());
        QVERIFY(!oct->comparison[0].poly.has_value());
        QCOMPARE(ws.upcoming().size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestFedWatchViewModel)
#include "tst_fedwatch_view_model.moc"
