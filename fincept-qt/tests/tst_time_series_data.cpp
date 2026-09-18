// tests/tst_time_series_data.cpp
//
// The shared historical time-series rules used by the chart-first economics
// path: date parsing, chronological ordering, history windows anchored at the
// latest returned observation, and gap splitting that breaks the line only
// when an expected observation is missing. Header-only over Qt Core; no app
// sources (tests/ HARD RULE).
#include "ui/charts/TimeSeriesData.h"

#include <QtTest>

#include <limits>

using namespace fincept::ui;

namespace {

TimeSeriesPoint point(const QString& label, double value) {
    return TimeSeriesPoint{parse_time_series_date(label), label, value};
}

QVector<TimeSeriesPoint> years(int first, int last) {
    QVector<TimeSeriesPoint> pts;
    for (int y = first; y <= last; ++y)
        pts << point(QString::number(y), static_cast<double>(y));
    return pts;
}

int segment_count(const QVector<QVector<TimeSeriesPoint>>& segments) {
    return static_cast<int>(segments.size());
}

} // namespace

class TstTimeSeriesData : public QObject {
    Q_OBJECT
  private slots:
    void parses_year_month_and_day_periods();
    void parses_space_separated_timestamps();
    void rejects_unparseable_periods();
    void sorts_chronologically_and_keeps_equal_dates_stable();
    void range_filter_is_anchored_at_latest_observation();
    void range_availability_requires_returned_history();
    void annual_missing_year_splits_line();
    void daily_weekend_gap_does_not_split_line();
    void monthly_missing_month_splits_line();
    void inferred_frequency_is_annual_only_for_year_labels();
    void frequency_labels_map_to_cadence();
    void infers_closest_positive_spacing();
    void gap_split_keeps_single_point_as_one_segment();
    void formats_values_like_the_raw_table();
};

void TstTimeSeriesData::parses_year_month_and_day_periods() {
    QCOMPARE(parse_time_series_date(QStringLiteral("2024")), QDate(2024, 1, 1));
    QCOMPARE(parse_time_series_date(QStringLiteral("2024-03")), QDate(2024, 3, 1));
    QCOMPARE(parse_time_series_date(QStringLiteral("2025-09-15")), QDate(2025, 9, 15));
    QCOMPARE(parse_time_series_date(QStringLiteral("2025-09-15T00:00:00")), QDate(2025, 9, 15));
    QCOMPARE(parse_time_series_date(QStringLiteral("  2024  ")), QDate(2024, 1, 1));
}

void TstTimeSeriesData::rejects_unparseable_periods() {
    QVERIFY(!parse_time_series_date(QString()).isValid());
    QVERIFY(!parse_time_series_date(QStringLiteral("not-a-date")).isValid());
    QVERIFY(!parse_time_series_date(QStringLiteral("2024-13-01")).isValid());
    QVERIFY(!parse_time_series_date(QStringLiteral("abc")).isValid());
}

void TstTimeSeriesData::sorts_chronologically_and_keeps_equal_dates_stable() {
    QVector<TimeSeriesPoint> input = {point(QStringLiteral("2022"), 3), point(QStringLiteral("2020"), 1),
                                      point(QStringLiteral("2021"), 2), point(QStringLiteral("2021"), 9)};
    const auto sorted = sorted_time_series(input);
    QCOMPARE(sorted.size(), 4);
    QCOMPARE(sorted[0].date, QDate(2020, 1, 1));
    QCOMPARE(sorted[1].date, QDate(2021, 1, 1));
    QCOMPARE(sorted[1].value, 2.0);
    QCOMPARE(sorted[2].value, 9.0); // equal dates keep their incoming order
    QCOMPARE(sorted[3].date, QDate(2022, 1, 1));
}

void TstTimeSeriesData::range_filter_is_anchored_at_latest_observation() {
    QVector<TimeSeriesPoint> two_decades = years(2010, 2020);
    const auto one_year = filter_time_range(two_decades, TimeRange::OneYear);
    QCOMPARE(one_year.size(), 2);
    QCOMPARE(one_year.first().date_label, QStringLiteral("2019"));
    QCOMPARE(one_year.last().date_label, QStringLiteral("2020"));

    // Anchored on the provider's last returned date, not on "today"; the
    // boundary observation is inclusive and no point is fabricated.
    QVector<TimeSeriesPoint> midyear = {point(QStringLiteral("2019-06-14"), 1.0),
                                        point(QStringLiteral("2019-06-15"), 2.0),
                                        point(QStringLiteral("2020-06-15"), 3.0)};
    const auto boundary = filter_time_range(midyear, TimeRange::OneYear);
    QCOMPARE(boundary.size(), 2);
    QCOMPARE(boundary.first().date, QDate(2019, 6, 15));
    QCOMPARE(boundary.last().date, QDate(2020, 6, 15));
    QCOMPARE(filter_time_range(midyear, TimeRange::Max).size(), 3);
}

void TstTimeSeriesData::range_availability_requires_returned_history() {
    const auto ten_years = years(2016, 2025);
    QVERIFY(time_range_available(ten_years, TimeRange::OneYear));
    QVERIFY(time_range_available(ten_years, TimeRange::FiveYears));
    QVERIFY(!time_range_available(ten_years, TimeRange::TenYears)); // only 9 years of span
    QVERIFY(time_range_available(ten_years, TimeRange::Max));

    const QVector<TimeSeriesPoint> empty;
    QVERIFY(!time_range_available(empty, TimeRange::OneYear));
    QVERIFY(!time_range_available(empty, TimeRange::Max));
}

void TstTimeSeriesData::annual_missing_year_splits_line() {
    QVector<TimeSeriesPoint> pts = {point(QStringLiteral("2018"), 1.0), point(QStringLiteral("2019"), 2.0),
                                    point(QStringLiteral("2021"), 4.0), point(QStringLiteral("2022"), 5.0)};
    const auto segments = split_time_series_gaps(pts, QStringLiteral("Annual"));
    QCOMPARE(segment_count(segments), 2);
    QCOMPARE(segments[0].size(), 2);
    QCOMPARE(segments[1].size(), 2);

    // With no frequency label the inferred cadence must reach the same verdict.
    QCOMPARE(segment_count(split_time_series_gaps(pts, QString())), 2);
}

void TstTimeSeriesData::daily_weekend_gap_does_not_split_line() {
    QVector<TimeSeriesPoint> pts = {point(QStringLiteral("2026-09-10"), 1.0), point(QStringLiteral("2026-09-11"), 1.1),
                                    point(QStringLiteral("2026-09-14"), 1.2), point(QStringLiteral("2026-09-15"), 1.3)};
    QCOMPARE(segment_count(split_time_series_gaps(pts, QStringLiteral("Daily"))), 1);

    // A genuinely missing week does break the line.
    QVector<TimeSeriesPoint> gapped = {point(QStringLiteral("2026-09-01"), 1.0),
                                       point(QStringLiteral("2026-09-15"), 1.2)};
    QCOMPARE(segment_count(split_time_series_gaps(gapped, QStringLiteral("Daily"))), 2);
}

void TstTimeSeriesData::monthly_missing_month_splits_line() {
    QVector<TimeSeriesPoint> pts = {point(QStringLiteral("2024-01-01"), 1.0), point(QStringLiteral("2024-02-01"), 2.0),
                                    point(QStringLiteral("2024-04-01"), 4.0)};
    QCOMPARE(segment_count(split_time_series_gaps(pts, QStringLiteral("Monthly"))), 2);
    QCOMPARE(segment_count(split_time_series_gaps(pts, QString())), 2);
}

void TstTimeSeriesData::inferred_frequency_is_annual_only_for_year_labels() {
    QCOMPARE(inferred_frequency_label(years(2018, 2020)), QStringLiteral("Annual"));
    QVERIFY(inferred_frequency_label({point(QStringLiteral("2024-01"), 1.0), point(QStringLiteral("2024-02"), 2.0)})
                .isEmpty());
    QVERIFY(inferred_frequency_label({}).isEmpty());
}

void TstTimeSeriesData::parses_space_separated_timestamps() {
    QCOMPARE(parse_time_series_date(QStringLiteral("2024-01-01 00:00:00")), QDate(2024, 1, 1));
    QCOMPARE(parse_time_series_date(QStringLiteral("2024-03-05 23:59")), QDate(2024, 3, 5));
}

void TstTimeSeriesData::frequency_labels_map_to_cadence() {
    QCOMPARE(frequency_step_days(QStringLiteral("Daily")), 1);
    QCOMPARE(frequency_step_days(QStringLiteral("Weekly")), 7);
    QCOMPARE(frequency_step_days(QStringLiteral("Biweekly")), 14);
    QCOMPARE(frequency_step_days(QStringLiteral("Monthly")), 30);
    QCOMPARE(frequency_step_days(QStringLiteral("Quarterly")), 91);
    QCOMPARE(frequency_step_days(QStringLiteral("Semi-annual")), 182);
    QCOMPARE(frequency_step_days(QStringLiteral("Annual")), 365);
    QCOMPARE(frequency_step_days(QStringLiteral("M")), 30);
    QCOMPARE(frequency_step_days(QStringLiteral("Q")), 91);
    QCOMPARE(frequency_step_days(QStringLiteral("A")), 365);
    QCOMPARE(frequency_step_days(QString()), 0);
}

void TstTimeSeriesData::infers_closest_positive_spacing() {
    // The closest spacing survives a missing period; a median would not.
    const QVector<TimeSeriesPoint> annual = {point(QStringLiteral("2018"), 1.0), point(QStringLiteral("2019"), 2.0),
                                             point(QStringLiteral("2021"), 4.0)};
    QCOMPARE(inferred_step_days(annual), 365);
    QCOMPARE(inferred_step_days({point(QStringLiteral("2024"), 1.0)}), 0);
    QCOMPARE(inferred_step_days({}), 0);
}

void TstTimeSeriesData::gap_split_keeps_single_point_as_one_segment() {
    const auto segments = split_time_series_gaps({point(QStringLiteral("2024"), 7.0)}, QStringLiteral("Annual"));
    QCOMPARE(segment_count(segments), 1);
    QCOMPARE(segments[0].size(), 1);
    QCOMPARE(split_time_series_gaps({}, QStringLiteral("Annual")).size(), 0);
}

void TstTimeSeriesData::formats_values_like_the_raw_table() {
    // Hover must not disagree with the Raw Data cell for the same observation.
    QCOMPARE(format_series_value(90026.5163), QStringLiteral("90026.5163"));
    QCOMPARE(format_series_value(2.4), QStringLiteral("2.4"));
    QCOMPARE(format_series_value(0.0), QStringLiteral("0"));
    QCOMPARE(format_series_value(-57976.628204291), QStringLiteral("-57976.6282"));
    QCOMPARE(format_series_value(std::numeric_limits<double>::quiet_NaN()), QStringLiteral("—"));
    QCOMPARE(format_series_value(std::numeric_limits<double>::infinity()), QStringLiteral("—"));
}

QTEST_GUILESS_MAIN(TstTimeSeriesData)
#include "tst_time_series_data.moc"
