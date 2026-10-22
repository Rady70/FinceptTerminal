#pragma once
// Axis helpers of the dashboard's painted charts (HighlightCharts.h): which
// calendar dates get a tick on a time axis and which round values get a line
// on a value axis. Qt Core only, so the rules are unit-tested
// (tests/tst_dashboard_widget_rules.cpp).
#include <QDate>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace fincept::screens::widgets::charts {

struct DateTick {
    QDate date;
    QString label;
};

/// Calendar ticks strictly between d0 and d1 for a time axis `width_px`
/// wide: Mondays, or month, two-month, quarter, half-year or year starts —
/// the finest step whose ticks stay about 55 px apart.
inline QVector<DateTick> date_ticks(const QDate& d0, const QDate& d1, double width_px) {
    QVector<DateTick> out;
    if (!d0.isValid() || !d1.isValid() || d0.daysTo(d1) < 2 || width_px < 80)
        return out;
    const int max_ticks = std::max(1, int(width_px / 55));
    struct Step {
        int days;
        int months;
    };
    for (const Step s : {Step{7, 0}, Step{0, 1}, Step{0, 2}, Step{0, 3}, Step{0, 6}, Step{0, 12}}) {
        QVector<DateTick> t;
        if (s.days > 0) {
            QDate d = d0.addDays(8 - d0.dayOfWeek()); // the next Monday
            for (; d < d1; d = d.addDays(s.days))
                t.append({d, d.toString(QStringLiteral("d MMM"))});
        } else {
            QDate d(d0.year(), d0.month(), 1);
            d = d.addMonths(1);
            while ((d.month() - 1) % s.months != 0)
                d = d.addMonths(1);
            for (; d < d1; d = d.addMonths(s.months))
                t.append({d, s.months == 12                    ? d.toString(QStringLiteral("yyyy"))
                             : d.month() == 1 || s.months >= 3 ? d.toString(QStringLiteral("MMM yy"))
                                                               : d.toString(QStringLiteral("MMM"))});
        }
        if (t.size() <= max_ticks) {
            out = t;
            break;
        }
    }
    return out;
}

/// Round axis values covering [lo, hi]: steps of 1, 2, 2.5 or 5 × 10^k, about
/// `count` of them.
inline QVector<double> nice_ticks(double lo, double hi, int count = 4) {
    QVector<double> out;
    if (!(hi > lo))
        return out;
    const double raw = (hi - lo) / std::max(1, count);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double step = mag * 10;
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (raw <= m * mag) {
            step = m * mag;
            break;
        }
    for (double v = std::ceil(lo / step) * step; v <= hi + step * 1e-9; v += step)
        out.append(std::abs(v) < step * 1e-9 ? 0.0 : v);
    return out;
}

} // namespace fincept::screens::widgets::charts
