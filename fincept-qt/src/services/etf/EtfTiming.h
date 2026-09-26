// src/services/etf/EtfTiming.h
//
// The timing rules of the ETF data foundation (ETF Capital Flows Batch B).
// They encode the Batch A2 policy (ETF_FLOW_BATCH_A2_QUALIFICATION.md sections
// 5.1-5.4 and 10) without inventing a public-availability time:
//
//   * `accepted_at` (SEC) is exact but is only a lower bound for public
//     availability; it is stored as it is and never relabelled.
//   * `first_seen_at` is the retrieval time of the vintage that first held a
//     value; it is an upper bound for public availability.
//   * `available_from` is always derived from a named basis: the recorded
//     sighting, a versioned rule, or an assumption. When no basis can be
//     evaluated it stays unknown (invalid) and the status is not_point_in_time.
//
// There is deliberately no `publicly_available_at`: no qualified source
// publishes one (A2 section 5.1).
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfDataModel.h"
#include "services/etf/EtfSessionCalendar.h"

#include <QDate>
#include <QDateTime>
#include <QString>

#include <optional>

namespace fincept::services::etf {

/// Rule ids stored with the retrievals that apply them.
inline constexpr const char* kSecAvailabilityRule = "sec_next_session_after_acceptance_v1";
inline constexpr const char* kIbkrAvailabilityRule = "ibkr_next_session_v1";
/// The IBKR request end rule: only sessions on an exchange date before the
/// request's own exchange date are requested, so the session still trading
/// (and today's session in the delayed feed, A2 section 7.7) is never asked for.
inline constexpr const char* kIbkrRequestEndRule = "ibkr_prior_exchange_date_sessions_v1";

struct TimingAssessment {
    HistoryType history_type = HistoryType::RegulatoryFiling;
    PointInTimeStatus point_in_time_status = PointInTimeStatus::NotPointInTime;
    AvailabilityBasis availability_basis = AvailabilityBasis::None;
    QDateTime available_from; ///< invalid when no basis could be evaluated
};

/// sec_next_session_after_acceptance_v1: the opening of the first NYSE session
/// on a calendar date after the acceptance date in U.S. Eastern time. SPY's
/// June 2026 filing, accepted 2026-08-28T12:25:47Z (08:25 ET, a Friday), is
/// usable from the Monday 2026-08-31 open. nullopt when the calendar cannot
/// answer (outside its coverage).
inline std::optional<QDateTime> sec_next_session_available_from(const QDateTime& accepted_at_utc) {
    const QDate acceptance_date = UsEquityCalendar::exchange_date(accepted_at_utc);
    if (!acceptance_date.isValid())
        return std::nullopt;
    const auto next = UsEquityCalendar::next_session_after(acceptance_date);
    if (!next)
        return std::nullopt;
    return next->open_utc;
}

/// One SEC filing vintage (one accession). Filings accepted at or after
/// MarketLab's observation start for the reporting entity are forward
/// observations, available from their first sighting. Earlier filings are
/// regulatory_filing vintages dated by the conservative rule, which is a
/// MarketLab policy and never `observed`.
inline TimingAssessment classify_sec_vintage(const QDateTime& accepted_at_utc, const QDateTime& observation_start_utc,
                                             const QDateTime& first_seen_utc) {
    TimingAssessment t;
    if (accepted_at_utc.isValid() && observation_start_utc.isValid() && accepted_at_utc >= observation_start_utc) {
        t.history_type = HistoryType::ForwardObserved;
        t.point_in_time_status = PointInTimeStatus::Observed;
        t.availability_basis = AvailabilityBasis::RecordedFirstSeen;
        t.available_from = first_seen_utc;
        return t;
    }
    t.history_type = HistoryType::RegulatoryFiling;
    const auto rule = accepted_at_utc.isValid() ? sec_next_session_available_from(accepted_at_utc) : std::nullopt;
    if (!rule) {
        t.point_in_time_status = PointInTimeStatus::NotPointInTime;
        t.availability_basis = AvailabilityBasis::None;
        t.available_from = QDateTime();
        return t;
    }
    t.point_in_time_status = PointInTimeStatus::ConservativeRule;
    t.availability_basis = AvailabilityBasis::SecNextSessionAfterAcceptance;
    t.available_from = *rule;
    return t;
}

/// One IBKR bar vintage. A bar of a session that closed before MarketLab began
/// observing the instrument is market_backfill: a historical_assumption, never
/// observed (A2 review finding 9). Its first vintage is available from the
/// session close under session_close_assumption; a later, revised vintage
/// cannot be dated before MarketLab saw it, so it is available from its first
/// sighting. A bar of a session that closed after the observation start is a
/// forward observation under ibkr_next_session_v1: available from the later of
/// its first sighting and the next session's open.
inline TimingAssessment classify_ibkr_bar_vintage(const QDate& session_date, const QDateTime& observation_start_utc,
                                                  const QDateTime& first_seen_utc, bool first_vintage_of_key) {
    TimingAssessment t;
    const MarketSessionDay session = UsEquityCalendar::day(session_date);
    const auto next = UsEquityCalendar::next_session_after(session_date);
    if (!session.is_session() || !observation_start_utc.isValid() || !first_seen_utc.isValid()) {
        t.history_type = HistoryType::MarketBackfill;
        t.point_in_time_status = PointInTimeStatus::NotPointInTime;
        t.availability_basis = AvailabilityBasis::None;
        return t;
    }
    const bool closed_before_start = session.close_utc <= observation_start_utc;
    if (closed_before_start) {
        t.history_type = HistoryType::MarketBackfill;
        t.point_in_time_status = PointInTimeStatus::HistoricalAssumption;
        if (first_vintage_of_key) {
            t.availability_basis = AvailabilityBasis::SessionCloseAssumption;
            t.available_from = session.close_utc;
        } else {
            t.availability_basis = AvailabilityBasis::RecordedFirstSeen;
            t.available_from = first_seen_utc;
        }
        return t;
    }
    t.history_type = HistoryType::ForwardObserved;
    if (!next) {
        // The next session is outside the calendar: the rule cannot be applied.
        t.point_in_time_status = PointInTimeStatus::NotPointInTime;
        t.availability_basis = AvailabilityBasis::None;
        return t;
    }
    t.point_in_time_status = PointInTimeStatus::Observed;
    t.availability_basis = AvailabilityBasis::RecordedFirstSeenAndIbkrNextSession;
    t.available_from = first_seen_utc > next->open_utc ? first_seen_utc : next->open_utc;
    return t;
}

/// The last session an IBKR daily request made at `request_utc` may ask for
/// (kIbkrRequestEndRule): the last session on an exchange date strictly before
/// the request's exchange date. nullopt when the calendar cannot answer.
inline std::optional<MarketSessionDay> ibkr_last_requestable_session(const QDateTime& request_utc) {
    const QDate today = UsEquityCalendar::exchange_date(request_utc);
    if (!today.isValid())
        return std::nullopt;
    return UsEquityCalendar::last_session_before(today);
}

/// The wrapper's `--end-date-time` for a session: the end of that exchange
/// date, in the format and zone Batch A2 used ("20260924 23:59:59 US/Eastern").
inline QString ibkr_end_date_time_for(const QDate& session_date) {
    return QStringLiteral("%1 23:59:59 US/Eastern").arg(session_date.toString(QStringLiteral("yyyyMMdd")));
}

} // namespace fincept::services::etf
