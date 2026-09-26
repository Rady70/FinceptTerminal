// src/services/etf/EtfSessionCalendar.h
//
// The U.S. equity exchange session calendar the ETF data foundation stores
// with every IBKR bar and uses for the SEC availability rule (ETF Capital
// Flows Batch B; A2 section 7.4 and review finding 8).
//
// IBKR daily bars carry no early-close flag and no session hours, so an early
// close would otherwise read as an ordinary low-volume day, a holiday could not
// be told apart from a missing bar, and the SEC rule
// sec_next_session_after_acceptance_v1 could not be evaluated. This calendar
// answers those questions for a bounded, source-verified range and refuses to
// guess outside it (OutsideCoverage): a date it does not cover has no session
// type, which is a different state from a holiday.
//
// Calendar version `nyse_us_equities_v1`. Every holiday, closure and early
// close below was read from NYSE Group's own published schedules:
//   * 2019-2021: "NYSE Group Announces 2019, 2020 and 2021 Holiday and Early
//     Closings Calendar" (ir.theice.com, 2018-12-04);
//   * 2022-2024: "NYSE Group Announces 2022, 2023 and 2024 Holiday and Early
//     Closings Calendar" (ir.theice.com, 2021-12-27);
//   * 2025-2027: "NYSE Group Announces 2025, 2026 and 2027 Holiday and Early
//     Closings Calendar" (ir.theice.com, 2024);
//   * 2028: NYSE "Holidays & Trading Hours" page (nyse.com, read 2026-09-25);
//   * the unscheduled closure of 2025-01-09 (National Day of Mourning for
//     former President Carter): ICE press release of 2024-12.
// Early closes are at 1:00 p.m. ET; regular sessions are 9:30 a.m. to 4:00 p.m.
// ET. The NYSE schedules state that all NYSE cash equity markets (NYSE, NYSE
// Arca, NYSE American, NYSE National, NYSE Chicago) observe them. A2 found the
// same 501 session dates for 62 ETFs listed on ARCA, NASDAQ, BATS and NYSE
// (2024-09-25 to 2026-09-24), which is why one U.S. equity calendar serves the
// listed-ETF universe; tst_etf_calendar_timing re-checks that window.
//
// Header-only over Qt Core.
#pragma once
#include <QDate>
#include <QDateTime>
#include <QTime>
#include <QTimeZone>
#include <QVector>

#include <iterator>
#include <optional>

namespace fincept::services::etf {

inline constexpr const char* kUsEquityCalendarId = "XNYS";
inline constexpr const char* kUsEquityCalendarVersion = "nyse_us_equities_v1";

enum class SessionDayType {
    Regular,         ///< 9:30 a.m. - 4:00 p.m. ET
    EarlyClose,      ///< 9:30 a.m. - 1:00 p.m. ET
    Holiday,         ///< a weekday without a session (holiday or unscheduled closure)
    Weekend,         ///< Saturday or Sunday
    OutsideCoverage, ///< the calendar does not know this date; never guessed
};

inline const char* session_day_type_id(SessionDayType t) {
    switch (t) {
        case SessionDayType::Regular:
            return "regular";
        case SessionDayType::EarlyClose:
            return "early_close";
        case SessionDayType::Holiday:
            return "holiday";
        case SessionDayType::Weekend:
            return "weekend";
        case SessionDayType::OutsideCoverage:
            return "outside_coverage";
    }
    return "";
}

struct MarketSessionDay {
    QDate date;
    SessionDayType type = SessionDayType::OutsideCoverage;
    QTime open_local;    ///< exchange time; invalid for a non-session
    QTime close_local;   ///< exchange time; invalid for a non-session
    QDateTime open_utc;  ///< invalid for a non-session
    QDateTime close_utc; ///< invalid for a non-session

    bool is_session() const { return type == SessionDayType::Regular || type == SessionDayType::EarlyClose; }
    bool is_early_close() const { return type == SessionDayType::EarlyClose; }
};

namespace calendar_detail {

struct Ymd {
    int y;
    int m;
    int d;
};

// Weekdays without a session. Includes observed holidays (a Saturday holiday is
// observed on the Friday, a Sunday one on the Monday) exactly as NYSE listed
// them; New Year's Day 2022 and 2028 fell on a Saturday and NYSE observed no
// holiday (NYSE Rule 7.2), so neither year has a New Year entry.
inline constexpr Ymd kNoSessionWeekdays[] = {
    // 2019
    {2019, 1, 1},
    {2019, 1, 21},
    {2019, 2, 18},
    {2019, 4, 19},
    {2019, 5, 27},
    {2019, 7, 4},
    {2019, 9, 2},
    {2019, 11, 28},
    {2019, 12, 25},
    // 2020
    {2020, 1, 1},
    {2020, 1, 20},
    {2020, 2, 17},
    {2020, 4, 10},
    {2020, 5, 25},
    {2020, 7, 3},
    {2020, 9, 7},
    {2020, 11, 26},
    {2020, 12, 25},
    // 2021
    {2021, 1, 1},
    {2021, 1, 18},
    {2021, 2, 15},
    {2021, 4, 2},
    {2021, 5, 31},
    {2021, 7, 5},
    {2021, 9, 6},
    {2021, 11, 25},
    {2021, 12, 24},
    // 2022 (first year with Juneteenth; no New Year holiday)
    {2022, 1, 17},
    {2022, 2, 21},
    {2022, 4, 15},
    {2022, 5, 30},
    {2022, 6, 20},
    {2022, 7, 4},
    {2022, 9, 5},
    {2022, 11, 24},
    {2022, 12, 26},
    // 2023
    {2023, 1, 2},
    {2023, 1, 16},
    {2023, 2, 20},
    {2023, 4, 7},
    {2023, 5, 29},
    {2023, 6, 19},
    {2023, 7, 4},
    {2023, 9, 4},
    {2023, 11, 23},
    {2023, 12, 25},
    // 2024
    {2024, 1, 1},
    {2024, 1, 15},
    {2024, 2, 19},
    {2024, 3, 29},
    {2024, 5, 27},
    {2024, 6, 19},
    {2024, 7, 4},
    {2024, 9, 2},
    {2024, 11, 28},
    {2024, 12, 25},
    // 2025 (2025-01-09: unscheduled closure, National Day of Mourning)
    {2025, 1, 1},
    {2025, 1, 9},
    {2025, 1, 20},
    {2025, 2, 17},
    {2025, 4, 18},
    {2025, 5, 26},
    {2025, 6, 19},
    {2025, 7, 4},
    {2025, 9, 1},
    {2025, 11, 27},
    {2025, 12, 25},
    // 2026
    {2026, 1, 1},
    {2026, 1, 19},
    {2026, 2, 16},
    {2026, 4, 3},
    {2026, 5, 25},
    {2026, 6, 19},
    {2026, 7, 3},
    {2026, 9, 7},
    {2026, 11, 26},
    {2026, 12, 25},
    // 2027
    {2027, 1, 1},
    {2027, 1, 18},
    {2027, 2, 15},
    {2027, 3, 26},
    {2027, 5, 31},
    {2027, 6, 18},
    {2027, 7, 5},
    {2027, 9, 6},
    {2027, 11, 25},
    {2027, 12, 24},
    // 2028 (no New Year holiday)
    {2028, 1, 17},
    {2028, 2, 21},
    {2028, 4, 14},
    {2028, 5, 29},
    {2028, 6, 19},
    {2028, 7, 4},
    {2028, 9, 4},
    {2028, 11, 23},
    {2028, 12, 25},
};

// Sessions that close early at 1:00 p.m. ET.
inline constexpr Ymd kEarlyCloses[] = {
    {2019, 7, 3},   {2019, 11, 29}, {2019, 12, 24}, {2020, 11, 27}, {2020, 12, 24}, {2021, 11, 26}, {2022, 11, 25},
    {2023, 7, 3},   {2023, 11, 24}, {2024, 7, 3},   {2024, 11, 29}, {2024, 12, 24}, {2025, 7, 3},   {2025, 11, 28},
    {2025, 12, 24}, {2026, 11, 27}, {2026, 12, 24}, {2027, 11, 26}, {2028, 7, 3},   {2028, 11, 24},
};

inline bool contains(const Ymd* first, const Ymd* last, const QDate& d) {
    for (const Ymd* it = first; it != last; ++it) {
        if (it->y == d.year() && it->m == d.month() && it->d == d.day())
            return true;
    }
    return false;
}

} // namespace calendar_detail

class UsEquityCalendar {
  public:
    static QDate coverage_first() { return QDate(2019, 1, 1); }
    static QDate coverage_last() { return QDate(2028, 12, 31); }

    static bool covers(const QDate& d) { return d.isValid() && d >= coverage_first() && d <= coverage_last(); }

    /// The exchange time zone. Resolved by IANA id; an unresolvable zone makes
    /// every time conversion invalid, which callers treat as not computable.
    static QTimeZone exchange_zone() { return QTimeZone(QByteArrayLiteral("America/New_York")); }

    /// The exchange (U.S. Eastern) calendar date of an instant.
    static QDate exchange_date(const QDateTime& instant) {
        const QTimeZone zone = exchange_zone();
        if (!instant.isValid() || !zone.isValid())
            return {};
        return instant.toTimeZone(zone).date();
    }

    static MarketSessionDay day(const QDate& d) {
        using namespace calendar_detail;
        MarketSessionDay out;
        out.date = d;
        if (!covers(d)) {
            out.type = SessionDayType::OutsideCoverage;
            return out;
        }
        if (d.dayOfWeek() >= 6) {
            out.type = SessionDayType::Weekend;
            return out;
        }
        if (contains(std::begin(kNoSessionWeekdays), std::end(kNoSessionWeekdays), d)) {
            out.type = SessionDayType::Holiday;
            return out;
        }
        const QTimeZone zone = exchange_zone();
        if (!zone.isValid()) {
            // Without the exchange zone no session time can be stated; refuse to
            // label the day rather than attach wrong UTC times to it.
            out.type = SessionDayType::OutsideCoverage;
            return out;
        }
        const bool early = contains(std::begin(kEarlyCloses), std::end(kEarlyCloses), d);
        out.type = early ? SessionDayType::EarlyClose : SessionDayType::Regular;
        out.open_local = QTime(9, 30);
        out.close_local = early ? QTime(13, 0) : QTime(16, 0);
        out.open_utc = QDateTime(d, out.open_local, zone).toUTC();
        out.close_utc = QDateTime(d, out.close_local, zone).toUTC();
        return out;
    }

    /// The first session on a calendar date strictly after `d`, or nullopt when
    /// it would lie outside the covered range.
    static std::optional<MarketSessionDay> next_session_after(const QDate& d) {
        if (!d.isValid())
            return std::nullopt;
        for (QDate c = d.addDays(1); covers(c); c = c.addDays(1)) {
            const MarketSessionDay s = day(c);
            if (s.is_session())
                return s;
            if (s.type == SessionDayType::OutsideCoverage)
                return std::nullopt;
        }
        return std::nullopt;
    }

    /// The last session on a calendar date strictly before `d`, or nullopt when
    /// it would lie outside the covered range.
    static std::optional<MarketSessionDay> last_session_before(const QDate& d) {
        if (!d.isValid())
            return std::nullopt;
        for (QDate c = d.addDays(-1); covers(c); c = c.addDays(-1)) {
            const MarketSessionDay s = day(c);
            if (s.is_session())
                return s;
            if (s.type == SessionDayType::OutsideCoverage)
                return std::nullopt;
        }
        return std::nullopt;
    }

    /// Every weekday in [first, last] with its type (sessions and holidays), in
    /// date order. Weekends are omitted; dates outside coverage are omitted and
    /// reported through `complete` (false when part of the range is not covered).
    static QVector<MarketSessionDay> weekdays_in(const QDate& first, const QDate& last, bool* complete = nullptr) {
        QVector<MarketSessionDay> out;
        bool all_covered = first.isValid() && last.isValid();
        for (QDate c = first; all_covered && c <= last; c = c.addDays(1)) {
            const MarketSessionDay s = day(c);
            if (s.type == SessionDayType::OutsideCoverage) {
                all_covered = false;
                continue;
            }
            if (s.type != SessionDayType::Weekend)
                out.append(s);
        }
        if (complete)
            *complete = all_covered;
        return out;
    }
};

} // namespace fincept::services::etf
