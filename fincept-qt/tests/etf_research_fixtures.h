// Deterministic synthetic fixtures for the ETF research engine tests. Nothing
// here is a market observation: every series is generated from a stated rule
// so expected values can be derived by hand.
#pragma once
#include "services/etf/EtfSessionCalendar.h"
#include "services/etf/research/EtfResearchInputs.h"

#include <QJsonObject>

#include <cmath>

namespace etfr_fixture {

using namespace fincept::services::etf;
using namespace fincept::services::etf::research;

/// NYSE sessions ending at `last` (inclusive), oldest first.
inline QVector<QDate> nyse_sessions(const QDate& last, int n) {
    QVector<QDate> out;
    for (QDate d = last; out.size() < n && d.isValid(); d = d.addDays(-1)) {
        if (UsEquityCalendar::covers(d)) {
            if (UsEquityCalendar::day(d).is_session())
                out.prepend(d);
        } else if (d.dayOfWeek() < 6) {
            out.prepend(d); // before 2019: plain weekdays
        }
    }
    return out;
}

/// close_t = start * exp(drift * t + amp * sin(t / period)), volume = base * (1 + 0.1 sin(t/7)).
inline BarSeries series(const QString& sym, const QVector<QDate>& dates, double start, double drift, double amp = 0.0,
                        double period = 20.0, double base_volume = 1e6) {
    BarSeries s;
    s.symbol = sym;
    for (int t = 0; t < dates.size(); ++t) {
        DailyBar b;
        b.date = dates[t];
        b.close = start * std::exp(drift * t + amp * std::sin(t / period));
        b.volume = base_volume * (1.0 + 0.1 * std::sin(t / 7.0));
        s.bars.append(b);
    }
    return s;
}

/// Bars with the given closes on the given sessions, one per session.
inline BarSeries closes(const QString& sym, const QVector<QDate>& dates, const QVector<double>& values) {
    BarSeries s;
    s.symbol = sym;
    for (int i = 0; i < dates.size(); ++i) {
        DailyBar b;
        b.date = dates[i];
        b.close = values[i];
        b.volume = 1e6;
        s.bars.append(b);
    }
    return s;
}

/// The NYSE session after `d` (plain weekdays outside the calendar's coverage).
inline QDate next_session(const QDate& d) {
    for (QDate x = d.addDays(1);; x = x.addDays(1)) {
        if (UsEquityCalendar::covers(x) ? UsEquityCalendar::day(x).is_session() : x.dayOfWeek() < 6)
            return x;
    }
}

/// A Yahoo fund capture whose NAV is `nav_session`'s NAV, as Yahoo serves it: the
/// store records the next session (the last one completed at capture time) and
/// the capture is taken the morning after that session, before any close.
inline FundCapture capture(const QDate& nav_session, double aum, double nav,
                           std::optional<double> shares = std::nullopt, const QDateTime& at = QDateTime()) {
    FundCapture c;
    c.effective_session = next_session(nav_session);
    c.effective_rule = QStringLiteral("prior_completed_session_v1");
    c.captured_at = at.isValid() ? at : QDateTime(c.effective_session.addDays(1), QTime(13, 0), QTimeZone::UTC);
    c.total_assets = aum;
    c.nav = nav;
    c.shares_outstanding = shares;
    c.fields = QJsonObject{{QStringLiteral("totalAssets"), aum}, {QStringLiteral("navPrice"), nav}};
    return c;
}

inline MacroSeries macro(const QString& id, const QVector<QPair<QDate, double>>& pts) {
    MacroSeries m;
    m.source = QStringLiteral("fred");
    m.id = id;
    for (const auto& p : pts)
        m.points.append({p.first, p.second, false});
    return m;
}

} // namespace etfr_fixture
