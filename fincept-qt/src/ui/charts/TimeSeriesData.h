// src/ui/charts/TimeSeriesData.h
//
// Minimum shared contract for an ordinary historical numeric series plus the
// pure ordering/range/gap rules the chart-first economics path relies on.
// Header-only over Qt Core so the rules are unit-testable without a widget
// tree (see the HARD RULE at the top of tests/CMakeLists.txt).
//
// Truthfulness rules encoded here:
//   * one TimeSeriesPoint is one real provider observation;
//   * missing periods are absent, never zero and never interpolated;
//   * a range filter is anchored at the latest observation actually returned,
//     never at "today", and never synthesises history the source did not send;
//   * gap splitting is derived from the provider frequency (or the returned
//     spacing when the provider states none), so a break in the line means an
//     expected observation is missing rather than an ordinary market closure.
#pragma once

#include <QDate>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace fincept::ui {

struct TimeSeriesPoint {
    QDate date;         // parsed observation date (a year-only period maps to Jan 1)
    QString date_label; // provider's original period text, e.g. "2024" or "2025-09-15"
    double value = 0.0;
};

struct TimeSeriesMeta {
    QString title;
    QString source;       // provider display name, e.g. "World Bank"
    QString unit;         // provider unit where stated; may be empty
    QString frequency;    // provider frequency where stated; may be empty
    QString last_updated; // provider freshness stamp where stated; may be empty
};

struct TimeSeries {
    QVector<TimeSeriesPoint> points; // ascending by date
    TimeSeriesMeta meta;
};

/// Practical history windows offered by the shared chart.
enum class TimeRange { OneYear, ThreeYears, FiveYears, TenYears, Max };

inline int time_range_years(TimeRange range) {
    switch (range) {
        case TimeRange::OneYear:
            return 1;
        case TimeRange::ThreeYears:
            return 3;
        case TimeRange::FiveYears:
            return 5;
        case TimeRange::TenYears:
            return 10;
        case TimeRange::Max:
            break;
    }
    return 0;
}

/// Parse "YYYY", "YYYY-MM" or "YYYY-MM-DD" (a trailing "T…" or " …" time part
/// is ignored). Returns an invalid QDate for anything else; providers must not
/// be guessed at.
inline QDate parse_time_series_date(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return {};
    int cut = trimmed.indexOf(QLatin1Char('T'));
    const int space = trimmed.indexOf(QLatin1Char(' '));
    if (space >= 0 && (cut < 0 || space < cut))
        cut = space;
    const QString date_part = cut > 0 ? trimmed.left(cut) : trimmed;
    const QStringList parts = date_part.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.size() == 3)
        return QDate(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
    if (parts.size() == 2)
        return QDate(parts[0].toInt(), parts[1].toInt(), 1);
    if (parts.size() == 1) {
        bool ok = false;
        const int year = parts[0].toInt(&ok);
        if (ok && year > 0)
            return QDate(year, 1, 1);
    }
    return {};
}

/// Chronological (ascending) order. Equal dates keep their incoming order.
inline QVector<TimeSeriesPoint> sorted_time_series(QVector<TimeSeriesPoint> points) {
    std::stable_sort(points.begin(), points.end(),
                     [](const TimeSeriesPoint& a, const TimeSeriesPoint& b) { return a.date < b.date; });
    return points;
}

/// Start of the requested window, anchored at the latest returned observation.
inline QDate time_range_start(const QVector<TimeSeriesPoint>& points, TimeRange range) {
    if (points.isEmpty() || range == TimeRange::Max)
        return {};
    return points.last().date.addYears(-time_range_years(range));
}

/// A window is offered only when the returned history actually spans it.
inline bool time_range_available(const QVector<TimeSeriesPoint>& points, TimeRange range) {
    if (range == TimeRange::Max)
        return !points.isEmpty();
    if (points.isEmpty())
        return false;
    return points.first().date <= time_range_start(points, range);
}

/// Window filter over the real observations; never fabricates a boundary point.
inline QVector<TimeSeriesPoint> filter_time_range(const QVector<TimeSeriesPoint>& points, TimeRange range) {
    if (range == TimeRange::Max || points.isEmpty())
        return points;
    const QDate start = time_range_start(points, range);
    QVector<TimeSeriesPoint> out;
    out.reserve(points.size());
    for (const auto& p : points) {
        if (p.date.isValid() && p.date >= start)
            out << p;
    }
    return out;
}

/// Nominal spacing implied by a provider frequency label; 0 when unknown.
/// Longer/ambiguous prefixes ("semi-annual", "biweekly") are matched before the
/// coarse contains() checks so they do not inherit the wrong cadence.
inline int frequency_step_days(const QString& frequency) {
    const QString f = frequency.toLower().trimmed();
    if (f.isEmpty())
        return 0;
    if (f.startsWith(QLatin1String("semi")) || f.contains(QLatin1String("half")))
        return 182;
    if (f.contains(QLatin1String("biweekly")) || f.contains(QLatin1String("fortnight")))
        return 14;
    if (f.contains(QLatin1String("daily")) || f == QLatin1String("d"))
        return 1;
    if (f.contains(QLatin1String("weekly")) || f == QLatin1String("w"))
        return 7;
    if (f.contains(QLatin1String("month")) || f == QLatin1String("m"))
        return 30;
    if (f.contains(QLatin1String("quarter")) || f == QLatin1String("q"))
        return 91;
    if (f.contains(QLatin1String("annual")) || f.contains(QLatin1String("year")) || f == QLatin1String("a"))
        return 365;
    return 0;
}

/// Closest positive spacing between consecutive returned observations (0 when
/// there are fewer than two). A single missing period doubles one gap, so the
/// closest spacing still reflects the provider's normal cadence while a median
/// would be dragged upward by the very gap being detected.
inline int inferred_step_days(const QVector<TimeSeriesPoint>& points) {
    int closest = 0;
    for (int i = 1; i < points.size(); ++i) {
        const qint64 gap = points[i - 1].date.daysTo(points[i].date);
        if (gap > 0 && (closest == 0 || gap < closest))
            closest = static_cast<int>(gap);
    }
    return closest;
}

/// "Annual" only when every returned period is a bare year; otherwise empty.
/// Used for display when the provider states no frequency of its own.
inline QString inferred_frequency_label(const QVector<TimeSeriesPoint>& points) {
    if (points.isEmpty())
        return {};
    for (const auto& p : points) {
        bool year_only = false;
        p.date_label.toInt(&year_only);
        if (!year_only || p.date_label.size() != 4)
            return {};
    }
    return QStringLiteral("Annual");
}

/// Gap length above which at least one expected observation is missing.
/// Daily data tolerates a week so weekends/holidays do not break the line;
/// coarser frequencies break at 1.5 x the nominal spacing (one missing month,
/// quarter or year). 0 means "no frequency information".
inline int gap_break_days(const QString& frequency) {
    const int step = frequency_step_days(frequency);
    if (step <= 0)
        return 0;
    if (step == 1)
        return 7;
    const int threshold = (step * 3) / 2;
    return threshold < 1 ? 1 : threshold;
}

/// Split observations into line segments at genuine gaps. A single observation
/// still produces one segment (it renders as a marker, never as a zero).
inline QVector<QVector<TimeSeriesPoint>> split_time_series_gaps(const QVector<TimeSeriesPoint>& points,
                                                                const QString& frequency) {
    QVector<QVector<TimeSeriesPoint>> segments;
    if (points.isEmpty())
        return segments;
    int threshold = gap_break_days(frequency);
    if (threshold <= 0) {
        const int inferred = inferred_step_days(points);
        threshold = inferred <= 1 ? 7 : (inferred * 3) / 2;
        if (threshold < 1)
            threshold = 1;
    }
    QVector<TimeSeriesPoint> current;
    current << points.first();
    for (int i = 1; i < points.size(); ++i) {
        if (points[i - 1].date.daysTo(points[i].date) > threshold) {
            segments << current;
            current.clear();
        }
        current << points[i];
    }
    segments << current;
    return segments;
}

/// Readout text for a tooltip: the same precision the Raw Data table shows, so
/// hover cannot disagree with the underlying observation. Non-finite values are
/// an explicit dash, never a number.
inline QString format_series_value(double value) {
    if (!std::isfinite(value))
        return QStringLiteral("—");
    if (value == std::floor(value) && std::abs(value) < 1e15)
        return QString::number(static_cast<qint64>(value));
    return QString::number(value, 'g', 10);
}

} // namespace fincept::ui
