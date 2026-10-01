#include "screens/economics/panels/FedWatchViewModel.h"

#include <QJsonArray>
#include <QtTest>

using namespace fincept::screens::fedwatch;

class TestFedWatchViewModel : public QObject {
    Q_OBJECT
  private slots:
    void missingIsNotZero() {
        QCOMPARE(number(QJsonValue()), QString("Unavailable"));
        QCOMPARE(number(QJsonValue(QJsonValue::Null)), QString("Unavailable"));
        QCOMPARE(number(QJsonValue("0")), QString("Unavailable"));
        QCOMPARE(number(0.0, "%"), QString("0.00%"));
        QCOMPARE(number(-8.8, " pp"), QString("-8.80 pp"));
    }
    void outcomesKeepExactAndTailIdentity() {
        QCOMPARE(outcome_label(-50, true), QString("-50 bp or less"));
        QCOMPARE(outcome_label(50, true), QString("+50 bp or more"));
        QCOMPARE(outcome_label(-25, false), QString("-25 bp (exact)"));
        QCOMPARE(outcome_label(0, false), QString("0 bp (exact)"));
    }
    void historicalPointsRetainGapsZeroAndOrder() {
        const QJsonArray rows{QJsonObject{{"observed_at", "2026-09-28T12:00:00Z"}, {"probability_pct", 60}},
                              QJsonObject{{"observed_at", "2026-09-26T12:00:00Z"}, {"probability_pct", 0}},
                              QJsonObject{{"observed_at", "2026-09-27T12:00:00Z"}, {"probability_pct", QJsonValue()}},
                              QJsonObject{{"observed_at", "bad date"}, {"probability_pct", 40}},
                              QJsonObject{{"observed_at", "2026-09-27T12:00:00Z"}, {"probability_pct", "40"}}};
        const auto result = points(rows, "observed_at", "probability_pct");
        QCOMPARE(result.size(), 2);
        QCOMPARE(result[0].value, 0.0);
        QCOMPARE(result[0].instant.date(), QDate(2026, 9, 26));
        QCOMPARE(result[1].value, 60.0);
        QCOMPARE(result[1].instant.time(), QTime(12, 0));
    }
    void divergenceUsesBackendValues() {
        const auto result = points(QJsonArray{QJsonObject{{"date", "2026-09-26"}, {"probability_diff_pp", -8.8}},
                                              QJsonObject{{"date", "2026-09-28"}, {"probability_diff_pp", 0.0}}},
                                   "date", "probability_diff_pp");
        QCOMPARE(result.size(), 2);
        QCOMPARE(result[0].value, -8.8);
        QCOMPARE(result[1].value, 0.0);
        QCOMPARE(result[0].instant.date(), QDate(2026, 9, 26));
        QCOMPARE(result[0].instant.time(), QTime(0, 0));
        QCOMPARE(result[0].instant.offsetFromUtc(), 0);
    }
    void chartSegmentsCannotBridgeMissingUtcDays() {
        const auto retained =
            points(QJsonArray{QJsonObject{{"observed_at", "2026-09-01T12:00:00Z"}, {"probability_pct", 10}},
                              QJsonObject{{"observed_at", "2026-09-02T12:00:00Z"}, {"probability_pct", 20}},
                              QJsonObject{{"observed_at", "2026-09-03T12:00:00Z"}, {"probability_pct", QJsonValue()}},
                              QJsonObject{{"observed_at", "2026-09-04T12:00:00Z"}, {"probability_pct", 40}},
                              QJsonObject{{"observed_at", "2026-09-04T18:00:00Z"}, {"probability_pct", 45}}},
                   "observed_at", "probability_pct");
        QCOMPARE(retained.size(), 4);
        QVERIFY(adjacent_observations(retained[0], retained[1]));
        QVERIFY(!adjacent_observations(retained[1], retained[2]));
        QVERIFY(adjacent_observations(retained[2], retained[3]));
        QVERIFY(!adjacent_observations(retained[2], retained[0]));
        QCOMPARE(retained[2].value, 40.0);
    }
    void rangeIsDisplayOnlyAndCommonAcrossMethods() {
        const QVector<Series> retained{
            {"LIVE_INVESTING_DERIVED",
             {{QDateTime::fromString("2026-08-01T00:00:00Z", Qt::ISODate), 30, {}},
              {QDateTime::fromString("2026-09-28T00:00:00Z", Qt::ISODate), 60, {}}}},
            {"POLYMARKET_CLOB", {{QDateTime::fromString("2026-09-01T00:00:00Z", Qt::ISODate), 50, {}}}},
            {"HISTORICAL_ZQ_RECONSTRUCTED", {{QDateTime::fromString("2026-08-01T00:00:00Z", Qt::ISODate), 45, {}}}}};
        const auto shortRange = filter_range(retained, 30);
        QCOMPARE(shortRange.size(), 3);
        QCOMPARE(shortRange[0].points.size(), 1);
        QCOMPARE(shortRange[1].points.size(), 1);
        QCOMPARE(shortRange[2].points.size(), 0);
        QCOMPARE(retained[0].points.size(), 2);
        QCOMPARE(filter_range(retained, 0)[2].points[0].value, 45.0);
    }
};
QTEST_GUILESS_MAIN(TestFedWatchViewModel)
#include "tst_fedwatch_view_model.moc"
