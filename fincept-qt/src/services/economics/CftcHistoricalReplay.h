// src/services/economics/CftcHistoricalReplay.h
//
// Batch 3 of the CFTC research engine: the historically causal replay and
// outcome-measurement layer used to validate the Batch 2 research state
// (CFTC_RESEARCH_ENGINE_PLAN.md stages 3 and 4).
//
// This layer never re-implements a research rule. Every state it produces comes
// from a call to the one engine used by the future live UI integration,
// `cftc_evaluate_research_state` (services/economics/CftcResearchState.h). The
// replay layer only:
//
//   * chooses what history is visible at a historical decision point and feeds
//     it to that engine, respecting the engine's documented "history truncated
//     at the as-of report" input contract (CftcResearchState.h line ~1779);
//   * applies the predeclared publication/effective-time convention, so a
//     Tuesday-dated report is never treated as available on Tuesday;
//   * resolves the predeclared price-entry date and measures 4W/13W forward
//     price outcomes strictly after the state became available;
//   * marks observations that are outside the predeclared evaluation universe
//     (minimum trailing history, timing-exclusion windows) instead of silently
//     dropping them.
//
// Publication/effective convention (predeclared before any outcome was seen)
// ---------------------------------------------------------------------------
// The CFTC Release Schedule states the reports are released at 3:30 p.m.
// Eastern time, "usually released on Friday", with "federal holidays may delay
// release by one or two days". The CFTC also states there is no historical
// release-date list. The retained public metadata (Socrata `:created_at`) only
// records genuine weekly publication times from the report dated 2022-09-13
// onward; every earlier row carries the 2022-09-13 bulk-migration timestamp.
//
// Therefore, for a report dated D:
//
//   nominal_release(D) = the Friday of the reporting week (the next Friday
//                        strictly after D);
//   effective(D)       = max( nominal_release(D) + 5 calendar days,
//                             first weekday strictly after a genuine recorded
//                             publication date )
//
// The +5-day term is the documented "one or two days" holiday-delay allowance
// expressed as weekdays (Friday +1 = Monday, +2 = Tuesday, first weekday
// strictly after = Wednesday), so the state is treated as public before that
// session opens even under the slowest documented normal schedule. A genuine
// recorded publication later than that (catch-up releases) always wins. No
// federal holiday calendar is encoded; an unlisted holiday can only move the
// effective date later, never earlier, because entry waits for the first
// observed price at or after the effective date.
//
// A Socrata `:created_at` counts as a genuine publication timestamp only when
// it is at least three processing days after the report date (the CFTC receives
// data Wednesday for release Friday), at most 120 days after it (larger lags
// are the historical bulk load, not a release), and not before 2022-09-14 (the
// observed migration date of the whole pre-PRE history).
//
// Known pre-PRE publication interruptions are excluded, not guessed:
//   * report dates 1995-12-15 .. 1996-01-30 (1995-96 federal appropriations
//     lapse and the first post-lapse reports, whose catch-up timing is not
//     retained);
//   * report dates 2001-09-10 .. 2001-09-28 (the September 11, 2001
//     interruption; the retained CFTC announcement establishes that the
//     2001-09-10 reports were released 2001-09-21, after the conservative
//     effective date, and the following releases' timing is not retained);
//   * report dates 2013-09-27 .. 2013-11-01 (October 2013 federal
//     appropriations lapse; retained announcements do not establish timing);
//   * report dates 2018-12-21 .. 2019-03-01 (December 2018-January 2019 lapse;
//     the CFTC documented that the backlog was republished in chronological
//     order at two reports per week, but the exact recovered schedule per
//     report is not retained).
// The 2023 ION backlog and the 2025 lapse are not hand-excluded: genuine
// `:created_at` metadata records their actual catch-up publication dates.
//
// Price-entry and outcome convention
// ----------------------------------
//   * entry price = the first observed close on or after effective(D), with no
//     price preprocessing, interpolation or roll simulation; the retained
//     Yahoo continuous/front-month proxies remain rolling proxies and spot
//     indices remain spot indices, as labelled by the caller;
//   * if no price exists within 10 calendar days of effective(D), the outcome
//     is invalid ("entry window"), never substituted;
//   * 4W/13W target = entry date + 28 / 91 calendar days; exit = the first
//     observed close on or after the target; if that close is more than 7
//     calendar days after the target, or no price reaches the target, the
//     outcome is invalid;
//   * the outcome is the simple close-to-close percent change, reported with
//     the actual entry/exit dates and elapsed days. No sizing, leverage,
//     stop-loss, take-profit, cost or portfolio construction is applied.
//
// Minimum-history gate (predeclared)
// ----------------------------------
// An observation is eligible for the primary evaluation only when all of the
// following hold, using only information available for that state:
//   * report date >= 1994-10-01 (the CFTC weekly report era with a full 2Y of
//     weekly trailing history; before 1992-09-30 the source contains only
//     mid-month and month-end observations);
//   * the report date is not inside a timing-exclusion window;
//   * the engine reports the current report as available;
//   * the 26W and 2Y strictly-trailing references are fully covered;
//   * the 4W and 13W positioning anchors and the 13W trend window are defined.
// Ineligible observations are counted and reported with their reason; they are
// never silently dropped and their trailing requirements are never weakened.

#pragma once

#include "services/economics/CftcResearchState.h"

#include <QDate>
#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>

#include <algorithm>
#include <optional>

namespace fincept::services {

// ── Publication / effective timing ──────────────────────────────────────────

/// Lowest date at which Socrata `:created_at` is a genuine publication time:
/// every earlier row carries the observed 2022-09-13 bulk-migration timestamp.
inline QDate cftc_replay_publication_metadata_floor() {
    return QDate(2022, 9, 14);
}

/// A created-at date is a publication timestamp only inside the genuine window.
inline bool cftc_replay_publication_metadata_is_genuine(const QDate& report_date, const QDate& created_at_date) {
    if (!report_date.isValid() || !created_at_date.isValid())
        return false;
    if (created_at_date < cftc_replay_publication_metadata_floor())
        return false;
    const qint64 lag = report_date.daysTo(created_at_date);
    return lag >= 3 && lag <= 120;
}

/// The Friday of the reporting week (the CFTC's usual release day).
inline QDate cftc_replay_nominal_release_date(const QDate& report_date) {
    if (!report_date.isValid())
        return {};
    int days = (5 - report_date.dayOfWeek() + 7) % 7;
    if (days == 0)
        days = 7; // a report dated Friday is released the following Friday
    return report_date.addDays(days);
}

/// First weekday strictly after a date (used for recorded publication dates).
inline QDate cftc_replay_next_weekday(const QDate& date) {
    if (!date.isValid())
        return {};
    QDate next = date.addDays(1);
    while (next.dayOfWeek() > 5)
        next = next.addDays(1);
    return next;
}

/// The conservative effective date of a report. See the header comment for the
/// derivation; `genuine_publication_date` is invalid when no genuine retained
/// publication timestamp exists.
inline QDate cftc_replay_effective_date(const QDate& report_date, const QDate& genuine_publication_date = {}) {
    if (!report_date.isValid())
        return {};
    const QDate nominal = cftc_replay_nominal_release_date(report_date);
    QDate effective = nominal.addDays(5);
    if (genuine_publication_date.isValid()) {
        const QDate recorded = cftc_replay_next_weekday(genuine_publication_date);
        if (recorded > effective)
            effective = recorded;
    }
    return effective;
}

/// Pre-PRE report-date windows whose publication timing cannot be reconstructed
/// from retained metadata and is therefore excluded rather than assumed.
inline QVector<QPair<QDate, QDate>> cftc_replay_timing_exclusion_windows() {
    return {
        {QDate(1995, 12, 15), QDate(1996, 1, 30)}, // 1995-96 lapse and first post-lapse reports
        {QDate(2001, 9, 10), QDate(2001, 9, 28)},  // September 11, 2001 interruption
        {QDate(2013, 9, 27), QDate(2013, 11, 1)},  // October 2013 federal appropriations lapse
        {QDate(2018, 12, 21), QDate(2019, 3, 1)},  // Dec 2018-Jan 2019 lapse and its ambiguous catch-up
    };
}

inline QString cftc_replay_timing_exclusion_reason() {
    return QStringLiteral("Report date falls in a pre-PRE publication-interruption window whose recovered "
                          "release dates are not retained; excluded under the conservative timing rule.");
}

// ── Predeclared configuration ───────────────────────────────────────────────

struct CftcReplayOptions {
    /// First report date eligible for the primary evaluation (weekly era with a
    /// full 2Y weekly trailing reference).
    QDate evaluation_start = QDate(1994, 10, 1);
    /// Development period is every eligible report dated on or before this day;
    /// the holdout is strictly after it. Declared before outcomes were seen.
    QDate development_end = QDate(2018, 12, 31);
    int four_week_days = 28;
    int thirteen_week_days = 91;
    int outcome_tolerance_days = 7;
    int entry_window_days = 10;
    int trailing_min_reference = kCftcTrailingMinObservations;
    QVector<QPair<QDate, QDate>> timing_exclusions = cftc_replay_timing_exclusion_windows();
};

// ── Outcome measurement ─────────────────────────────────────────────────────

struct CftcForwardOutcome {
    bool valid = false;
    QString invalid_reason;
    QDate entry_date;
    double entry_price = 0.0;
    QDate exit_date;
    double exit_price = 0.0;
    double return_pct = 0.0;
    int elapsed_days = 0;
    int target_days = 0;
};

/// Close-to-close forward outcome starting strictly after the state's effective
/// date. Missing or too-short price history yields an invalid outcome with a
/// reason; it is never replaced by zero or a nearby stale price.
inline CftcForwardOutcome cftc_replay_forward_outcome(const QVector<CftcPricePoint>& prices,
                                                      const QDate& effective_date,
                                                      int horizon_days,
                                                      const CftcReplayOptions& options) {
    CftcForwardOutcome outcome;
    outcome.target_days = horizon_days;
    if (!effective_date.isValid()) {
        outcome.invalid_reason = QStringLiteral("No effective date was established for the report.");
        return outcome;
    }
    const CftcPricePoint* entry = nullptr;
    for (const auto& point : prices) {
        if (point.date >= effective_date) {
            entry = &point;
            break;
        }
    }
    if (entry == nullptr) {
        outcome.invalid_reason = QStringLiteral("No price observation at or after the effective date; the forward "
                                                "outcome cannot be measured.");
        return outcome;
    }
    if (entry->date > effective_date.addDays(options.entry_window_days)) {
        outcome.invalid_reason = QStringLiteral("The first price at or after the effective date is outside the %1-day "
                                                "entry window.")
                                     .arg(options.entry_window_days);
        return outcome;
    }
    outcome.entry_date = entry->date;
    outcome.entry_price = entry->close;
    const QDate target = entry->date.addDays(horizon_days);
    const CftcPricePoint* exit = nullptr;
    for (const auto& point : prices) {
        if (point.date >= target) {
            exit = &point;
            break;
        }
    }
    if (exit == nullptr) {
        outcome.invalid_reason = QStringLiteral("Price history ends before the %1-day target can be observed.")
                                     .arg(horizon_days);
        return outcome;
    }
    if (exit->date > target.addDays(options.outcome_tolerance_days)) {
        outcome.invalid_reason = QStringLiteral("The first price at or after the %1-day target is more than %2 days "
                                                "past it.")
                                     .arg(horizon_days)
                                     .arg(options.outcome_tolerance_days);
        return outcome;
    }
    outcome.exit_date = exit->date;
    outcome.exit_price = exit->close;
    outcome.elapsed_days = static_cast<int>(entry->date.daysTo(exit->date));
    outcome.return_pct = (exit->close / entry->close - 1.0) * 100.0;
    outcome.valid = true;
    return outcome;
}

// ── Market replay ───────────────────────────────────────────────────────────

/// One market/family with the exact inputs the replay may see. The replay
/// slices this complete data internally; callers never decide visibility.
struct CftcReplayMarket {
    CftcFamily family = CftcFamily::Legacy;
    QString market_key;
    QString label;
    QString asset_class;
    QVector<CftcObservation> observations; // ascending official reports
    QVector<CftcPricePoint> prices;        // ascending qualified closes
    QString price_source;
    bool price_continuous_proxy = false;
    bool price_spot_index = false;
    /// Genuine publication dates keyed by report date (Socrata `:created_at`),
    /// raw; the genuine-window rule is applied by the replay layer.
    QHash<QDate, QDate> created_at_dates;
};

struct CftcReplayObservation {
    QDate report_date;
    QDate nominal_release_date;
    QDate effective_date;
    bool publication_timestamp_known = false;
    QDate publication_date; // recorded genuine publication date when known
    bool timing_excluded = false;
    QString timing_exclusion_reason;
    bool development = false;
    bool eligible = false;
    QString ineligibility_reason;
    CftcResearchResult result;
    CftcForwardOutcome outcome_4w;
    CftcForwardOutcome outcome_13w;
};

/// The observations visible at a decision point: the report at `report_date`
/// and everything before it. The engine's documented contract requires exactly
/// this truncated slice; future reports are never supplied.
inline QVector<CftcObservation> cftc_replay_observation_slice(const QVector<CftcObservation>& observations,
                                                              const QDate& report_date) {
    QVector<CftcObservation> slice;
    for (const auto& observation : observations) {
        if (observation.date <= report_date)
            slice.append(observation);
    }
    return slice;
}

/// The prices visible at the same decision point. The Batch 2 price evidence is
/// aligned to the official report date, so a state can never see a close after
/// its own report date even if one is supplied.
inline QVector<CftcPricePoint> cftc_replay_price_slice(const QVector<CftcPricePoint>& prices,
                                                       const QDate& report_date) {
    QVector<CftcPricePoint> slice;
    for (const auto& point : prices) {
        if (point.date <= report_date)
            slice.append(point);
    }
    return slice;
}

/// Replay exactly one report date through the Batch 2 engine.
inline CftcReplayObservation cftc_replay_at(const CftcReplayMarket& market,
                                            const QDate& report_date,
                                            const CftcReplayOptions& options) {
    CftcReplayObservation observation;
    observation.report_date = report_date;
    observation.nominal_release_date = cftc_replay_nominal_release_date(report_date);
    observation.development = report_date <= options.development_end;

    for (const auto& window : options.timing_exclusions) {
        if (report_date >= window.first && report_date <= window.second) {
            observation.timing_excluded = true;
            observation.timing_exclusion_reason = cftc_replay_timing_exclusion_reason();
            break;
        }
    }

    const auto created = market.created_at_dates.constFind(report_date);
    if (created != market.created_at_dates.constEnd() &&
        cftc_replay_publication_metadata_is_genuine(report_date, created.value())) {
        observation.publication_timestamp_known = true;
        observation.publication_date = created.value();
    }
    observation.effective_date =
        cftc_replay_effective_date(report_date, observation.publication_timestamp_known ? observation.publication_date
                                                                                       : QDate());

    CftcResearchInput input;
    input.family = market.family;
    input.observations = cftc_replay_observation_slice(market.observations, report_date);
    input.as_of = report_date;
    input.effective_date = observation.effective_date;
    input.prices = cftc_replay_price_slice(market.prices, report_date);
    input.price_source = market.price_source;
    input.price_continuous_proxy = market.price_continuous_proxy;
    input.price_spot_index = market.price_spot_index;
    input.trailing_min_reference = options.trailing_min_reference;
    observation.result = cftc_evaluate_research_state(input);

    const CftcStateReadings& readings = observation.result.readings;
    if (report_date < options.evaluation_start) {
        observation.ineligibility_reason =
            QStringLiteral("Report predates the predeclared weekly-era evaluation start %1.")
                .arg(options.evaluation_start.toString(Qt::ISODate));
    } else if (observation.timing_excluded) {
        observation.ineligibility_reason = observation.timing_exclusion_reason;
    } else if (!observation.result.data_available) {
        observation.ineligibility_reason =
            observation.result.data_stale
                ? QStringLiteral("The speculative series ends before the as-of report; the prior report is not used.")
                : QStringLiteral("The as-of report has no usable speculative net position.");
    } else if (!readings.stats_26w.reference_covered) {
        observation.ineligibility_reason = QStringLiteral("The 26W strictly-trailing reference is not covered.");
    } else if (!readings.stats_2y.reference_covered) {
        observation.ineligibility_reason = QStringLiteral("The 2Y strictly-trailing reference is not covered.");
    } else if (!readings.changes_4w.net.has_value || !readings.changes_13w.net.has_value) {
        observation.ineligibility_reason = QStringLiteral("The 4W or 13W positioning anchor is unavailable.");
    } else if (!readings.net_minus_ma_13w.has_value || !readings.ma_slope_13w.has_value) {
        observation.ineligibility_reason = QStringLiteral("The 13W trend window is unavailable.");
    } else {
        observation.eligible = true;
    }

    if (observation.eligible) {
        observation.outcome_4w =
            cftc_replay_forward_outcome(market.prices, observation.effective_date, options.four_week_days, options);
        observation.outcome_13w =
            cftc_replay_forward_outcome(market.prices, observation.effective_date, options.thirteen_week_days,
                                        options);
    }
    return observation;
}

/// Replay every official report date of a market. The result is ordered by
/// report date; ineligible observations are retained with their reason.
inline QVector<CftcReplayObservation> cftc_replay_market(const CftcReplayMarket& market,
                                                        const CftcReplayOptions& options) {
    QVector<CftcReplayObservation> replayed;
    replayed.reserve(market.observations.size());
    for (const auto& observation : market.observations) {
        if (!observation.date.isValid())
            continue;
        replayed.append(cftc_replay_at(market, observation.date, options));
    }
    std::stable_sort(replayed.begin(), replayed.end(),
                     [](const CftcReplayObservation& left, const CftcReplayObservation& right) {
                         return left.report_date < right.report_date;
                     });
    return replayed;
}

// ── Outcome statistics ──────────────────────────────────────────────────────

struct CftcReturnStats {
    int n = 0;
    double mean = 0.0;
    double median = 0.0;
    double positive_rate = 0.0;
    double negative_rate = 0.0;
};

/// Descriptive statistics over a set of percent returns. Median of an even
/// count is the mean of the two central values. Empty input yields n = 0.
inline CftcReturnStats cftc_return_stats(QVector<double> values) {
    CftcReturnStats stats;
    stats.n = values.size();
    if (values.isEmpty())
        return stats;
    std::stable_sort(values.begin(), values.end());
    double sum = 0.0;
    int positive = 0;
    int negative = 0;
    for (double value : values) {
        sum += value;
        if (value > 0.0)
            ++positive;
        if (value < 0.0)
            ++negative;
    }
    stats.mean = sum / static_cast<double>(values.size());
    const int count = values.size();
    stats.median = (count % 2 == 1) ? values[count / 2]
                                    : (values[count / 2 - 1] + values[count / 2]) / 2.0;
    stats.positive_rate = static_cast<double>(positive) / static_cast<double>(count);
    stats.negative_rate = static_cast<double>(negative) / static_cast<double>(count);
    return stats;
}

} // namespace fincept::services
