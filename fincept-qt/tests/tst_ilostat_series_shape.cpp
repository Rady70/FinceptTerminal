// tests/tst_ilostat_series_shape.cpp
//
// The ILOSTAT result-shape rule behind the chart-first path: a long-format
// SDMX result is chartable only as one country's series with unique periods,
// and numeric TIME_PERIOD values are normalised to exact text before the
// shared series contract sees them. Header-only over Qt Core; no app sources
// (tests/ HARD RULE).
#include "screens/economics/panels/IlostatSeriesData.h"

#include <QtTest>

using namespace fincept::screens;

namespace {

QJsonObject row(const QString& area, const QString& period, const QJsonValue& value = 1.0) {
    QJsonObject obj;
    obj["REF_AREA"] = area;
    obj["TIME_PERIOD"] = period;
    obj["OBS_VALUE"] = value;
    return obj;
}

} // namespace

class TstIlostatSeriesShape : public QObject {
    Q_OBJECT
  private slots:
    void single_area_with_unique_periods_is_chartable();
    void multiple_areas_are_not_chartable();
    void missing_area_is_not_chartable();
    void repeated_period_is_not_chartable();
    void empty_result_is_not_chartable();
    void numeric_periods_become_exact_text();
    void string_periods_and_other_fields_are_untouched();
    void mixed_numeric_and_string_periods_are_not_chartable();
    void missing_period_is_not_chartable();
};

void TstIlostatSeriesShape::single_area_with_unique_periods_is_chartable() {
    const QJsonArray rows = {row(QStringLiteral("USA"), QStringLiteral("2010")),
                             row(QStringLiteral("USA"), QStringLiteral("2011")),
                             row(QStringLiteral("USA"), QStringLiteral("2012"))};
    QCOMPARE(ilostat_single_series_area(rows), QStringLiteral("USA"));
}

void TstIlostatSeriesShape::multiple_areas_are_not_chartable() {
    const QJsonArray rows = {row(QStringLiteral("CAN"), QStringLiteral("2010")),
                             row(QStringLiteral("USA"), QStringLiteral("2010"))};
    QVERIFY(ilostat_single_series_area(rows).isEmpty());
}

void TstIlostatSeriesShape::missing_area_is_not_chartable() {
    const QJsonArray rows = {row(QStringLiteral("USA"), QStringLiteral("2010")),
                             row(QString(), QStringLiteral("2011"))};
    QVERIFY(ilostat_single_series_area(rows).isEmpty());
}

void TstIlostatSeriesShape::repeated_period_is_not_chartable() {
    const QJsonArray rows = {row(QStringLiteral("USA"), QStringLiteral("2010")),
                             row(QStringLiteral("USA"), QStringLiteral("2010"))};
    QVERIFY(ilostat_single_series_area(rows).isEmpty());
}

void TstIlostatSeriesShape::empty_result_is_not_chartable() {
    QVERIFY(ilostat_single_series_area({}).isEmpty());
}

void TstIlostatSeriesShape::numeric_periods_become_exact_text() {
    // ILOSTAT's SDMX CSV parser emits TIME_PERIOD as a JSON number; the shared
    // series contract names periods as text.
    QJsonObject raw;
    raw["REF_AREA"] = QStringLiteral("USA");
    raw["TIME_PERIOD"] = 2010;
    raw["OBS_VALUE"] = 9.633;
    const QJsonArray normalized = ilostat_normalize_periods(QJsonArray{raw});

    QCOMPARE(normalized.first().toObject()["TIME_PERIOD"].toString(), QStringLiteral("2010"));
    QCOMPARE(normalized.first().toObject()["OBS_VALUE"].toDouble(), 9.633);
    QCOMPARE(ilostat_single_series_area(normalized), QStringLiteral("USA"));
}

void TstIlostatSeriesShape::string_periods_and_other_fields_are_untouched() {
    const QJsonArray normalized = ilostat_normalize_periods(
        QJsonArray{row(QStringLiteral("USA"), QStringLiteral("2010-Q1"), QStringLiteral("x"))});
    QCOMPARE(normalized.first().toObject()["TIME_PERIOD"].toString(), QStringLiteral("2010-Q1"));
    QCOMPARE(normalized.first().toObject()["OBS_VALUE"].toString(), QStringLiteral("x"));
}

void TstIlostatSeriesShape::mixed_numeric_and_string_periods_are_not_chartable() {
    QJsonObject numeric;
    numeric["REF_AREA"] = QStringLiteral("USA");
    numeric["TIME_PERIOD"] = 2010;
    numeric["OBS_VALUE"] = 1.0;
    // Both rows name the same real period after normalisation; two observations
    // on one date are not one ordinary series.
    const QJsonArray normalized =
        ilostat_normalize_periods(QJsonArray{numeric, row(QStringLiteral("USA"), QStringLiteral("2010"))});
    QVERIFY(ilostat_single_series_area(normalized).isEmpty());
}

void TstIlostatSeriesShape::missing_period_is_not_chartable() {
    QJsonObject without_period;
    without_period["REF_AREA"] = QStringLiteral("USA");
    without_period["OBS_VALUE"] = 1.0;
    QVERIFY(ilostat_single_series_area(QJsonArray{without_period}).isEmpty());
}

QTEST_GUILESS_MAIN(TstIlostatSeriesShape)
#include "tst_ilostat_series_shape.moc"
