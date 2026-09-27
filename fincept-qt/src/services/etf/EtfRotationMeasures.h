// src/services/etf/EtfRotationMeasures.h
//
// ETF Capital Flows, Batch C: market-rotation proxy measures from the IBKR
// completed-session bars Batch B stores (docs/ETF_FLOW_BATCH_C_IMPLEMENTATION.md
// in the control repository).
//
// These are secondary-market measures. They are not ETF creation/redemption
// flow, have no dollar units, and are labelled `rotation_proxy` everywhere.
// Only the two inputs Batch A2 qualified are read (A2 section 7.2):
//   * `bar_close`, the regular-hours TRADES close: split-adjusted, NOT
//     dividend-adjusted, so every return below is a PRICE return. No total
//     return basis is qualified (D5): distributions are missing from these
//     returns, by up to 7-8 % over two years for T-bill funds and 15-22 % for
//     option-income funds (A2 section 7.6). No measure here claims that
//     returns of different exposures are comparable;
//   * `bar_volume`, IBKR's filtered regular-hours volume: used only against
//     its own history, never as consolidated volume and never as flow.
// Open, high and low are stored but not qualified, and are not read.
//
// Method `rotation_proxy_measures_v1`, for session t, horizons counted in
// sessions of the persisted session record (below; a holiday is not a
// session, an early close is):
//   price_return_k             C(t) / C(t-k) - 1, k = 5, 21, 63, 126, 252
//   trend_efficiency_n         (C(t) - C(t-n)) / sum |C(i) - C(i-1)| over the
//                              n steps, n = 21, 63: +1 a straight rise, -1 a
//                              straight fall, near 0 no net progress
//   return_acceleration_21     [C(t)/C(t-21) - 1] - [C(t-21)/C(t-42) - 1]
//   volume_ratio_a_b           mean volume of the last a full sessions / mean
//                              volume of the b full sessions before them,
//                              (a, b) = (5, 63), (21, 126)
//   relative_price_return_k    (C(t)/C(t-k)) / (R(t)/R(t-k)) - 1 against a
//                              reference the CALLER declares
// Every input of one value must be on one units and one basis: a value whose
// closes (or volumes) change units or basis between them has no number
// (input_basis_not_uniform), and a relative return needs all four closes, the
// subject's and the reference's, on one units and basis
// (return_basis_not_comparable otherwise). Nothing is converted.
//
// Sessions. The session record is what Batch B persisted in
// etf_market_sessions with the bars, for the pinned calendar
// (kUsEquityCalendarId, kUsEquityCalendarVersion):
//   * a persisted row governs its date, whatever the compiled calendar says:
//     its day type decides whether the date is a session and whether it is an
//     early close. A row that differs from the compiled rule is listed;
//   * a bar needs its session's row (Batch B writes one with every bar it
//     stores): a close or volume without one is session_record_missing;
//   * a weekday inside the history that has no row is a day no stored
//     retrieval covered. It takes the pinned version's compiled rule, the rule
//     Batch B writes every row of that version from, and is not failed
//     closed: session rows carry no recording time, so a later retrieval that
//     writes the row would otherwise change a result recomputed for an
//     earlier (as_of, known_at). A weekday neither covers is
//     session_calendar_not_covered for every window that contains it (the
//     number of sessions across it is unknown);
//   * the freshness rule needs dates after the latest stored session; those,
//     and only those, are projected from the compiled rule, and the snapshot
//     says so (expected_session_calendar).
//
// Early closes: a 1 p.m. session's volume is structurally low (0.39-0.49 of a
// symbol's median in A2 section 7.4), and scaling it by session length would
// still leave it low, because trading is not uniform through the day. Volume
// windows therefore count full (regular) sessions only, and a volume measure
// ON an early-close session is NOT_APPLICABLE. Its close is a genuine
// completed-session close, so price measures use it.
//
// Missing and stale:
//   * a window that reaches before the first available bar is
//     insufficient_history (a launch, or history not ingested);
//   * a session inside the stored history without an available bar is
//     session_bar_missing: never filled, never skipped (a skipped session
//     would silently lengthen the horizon);
//   * the in-progress session is never stored (Batch B), and the latest
//     snapshot is judged by the ibkr_next_session_v1 freshness rule: the
//     session t is expected once the session after t has opened by as_of. A
//     latest bar older than that is STALE.
//
// Nothing is combined into a composite score: price and volume measures stay
// separate (see the Batch C record for why no weighting is justified).
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfDerivedModel.h"
#include "services/etf/EtfSessionCalendar.h"

#include <QDate>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

#include <array>
#include <cmath>
#include <optional>

namespace fincept::services::etf {

inline constexpr const char* kRotationProxyMeasuresVersion = "rotation_proxy_measures_v1";
inline constexpr const char* kRotationReturnBasis = "price_return_ibkr_trades_rth_close_split_adjusted";
inline constexpr const char* kRotationVolumeBasis = "ibkr_filtered_rth_volume_self_relative";
inline constexpr const char* kRotationFreshnessRule = "ibkr_next_session_v1";
inline constexpr const char* kRotationSessionSource = "etf_market_sessions";

inline constexpr std::array<int, 5> kRotationReturnHorizons = {5, 21, 63, 126, 252};
inline constexpr std::array<int, 2> kTrendEfficiencyWindows = {21, 63};
inline constexpr int kReturnAccelerationWindow = 21;
struct VolumeWindow {
    int recent;
    int baseline;
};
inline constexpr std::array<VolumeWindow, 2> kVolumeWindows = {{{5, 63}, {21, 126}}};

inline constexpr const char* kBarClose = "bar_close";
inline constexpr const char* kBarVolume = "bar_volume";

/// The parameters of rotation_proxy_measures_v1. Changing any of them is a new
/// method version (tst_etf_derived_calc pins the pair).
inline QString rotation_parameters_text() {
    QStringList horizons;
    for (int k : kRotationReturnHorizons)
        horizons << QString::number(k);
    QStringList efficiency;
    for (int n : kTrendEfficiencyWindows)
        efficiency << QString::number(n);
    QStringList volume;
    for (const VolumeWindow& w : kVolumeWindows)
        volume << QStringLiteral("%1/%2").arg(w.recent).arg(w.baseline);
    return QStringLiteral("inputs=bar_close,bar_volume;return_basis=%1;horizons=%2;trend_efficiency=%3;"
                          "acceleration=%4;volume=%5,full_sessions_only,early_close_not_applicable;"
                          "relative=caller_declared_reference,same_units_and_basis;"
                          "inputs_of_one_value=one_units_and_basis;"
                          "sessions=persisted_record_governs,bar_needs_its_record,unrecorded_weekday_calendar_rule,"
                          "uncovered_weekday_fails_closed;freshness=%6,calendar_rule_after_latest_session")
        .arg(QLatin1String(kRotationReturnBasis), horizons.join(QLatin1Char(',')), efficiency.join(QLatin1Char(',')))
        .arg(kReturnAccelerationWindow)
        .arg(volume.join(QLatin1Char(',')), QLatin1String(kRotationFreshnessRule));
}

/// The session record a rotation calculation follows: the etf_market_sessions
/// rows Batch B persisted for the pinned calendar, by date.
struct RotationSessionRecord {
    QHash<QDate, MarketSessionDay> days;

    static RotationSessionRecord from_rows(const QVector<MarketSessionDay>& rows) {
        RotationSessionRecord r;
        for (const MarketSessionDay& d : rows)
            r.days.insert(d.date, d);
        return r;
    }
};

struct RotationSessionInputs {
    QDate session;
    SessionDayType session_type = SessionDayType::Regular; ///< OutsideCoverage: a weekday nothing covers
    bool recorded = true;                                  ///< the session has its persisted row
    SelectedInput close;
    SelectedInput volume;

    bool covered() const { return session_type != SessionDayType::OutsideCoverage; }
};

struct RotationSessionResult {
    QDate session;
    SessionDayType session_type = SessionDayType::Regular;
    std::array<DerivedValue, kRotationReturnHorizons.size()> price_return;
    std::array<DerivedValue, kTrendEfficiencyWindows.size()> trend_efficiency;
    DerivedValue return_acceleration;
    std::array<DerivedValue, kVolumeWindows.size()> volume_ratio;
    bool has_reference = false;
    std::array<DerivedValue, kRotationReturnHorizons.size()> relative_price_return;
};

enum class RotationFreshness { Fresh, Stale, Unknown };

inline const char* rotation_freshness_id(RotationFreshness f) {
    switch (f) {
        case RotationFreshness::Fresh:
            return "fresh";
        case RotationFreshness::Stale:
            return "stale";
        case RotationFreshness::Unknown:
            return "unknown";
    }
    return "";
}

struct RotationSnapshot {
    bool has_data = false;
    QDate latest_session;            ///< the latest listed session
    QDate expected_session;          ///< the session the freshness rule expects at as_of (invalid: cannot be judged)
    bool expected_projected = false; ///< a date after the latest session was read from the compiled rule
    RotationFreshness freshness = RotationFreshness::Unknown;
    RotationSessionResult values; ///< the latest session's values; usable ones become STALE when stale
};

/// A date where the session record, the compiled rule and the stored bars do
/// not line up. Listed, never repaired.
struct RotationCalendarException {
    QDate date;
    QString kind;                   ///< record_differs_from_calendar_rule | close_without_session_record |
                                    ///< close_on_non_session_day | weekday_not_covered
    QString recorded_day_type;      ///< empty: no persisted row
    QString calendar_rule_day_type; ///< the compiled rule's day type
    bool close_known = false;       ///< an available close of the instrument is on the date
};

struct RotationMeasures {
    DerivedTimeFrame frame;
    bool has_reference = false;
    QVector<RotationSessionInputs> inputs; ///< every session, and uncovered weekday, from the first to the latest close
    QVector<RotationSessionResult> sessions; ///< same order
    QVector<RotationCalendarException> calendar_exceptions;
    RotationSnapshot snapshot;
};

namespace rotation_detail {

using DateMap = QMap<QDate, QVector<StoredObservation>>;

struct Series {
    QHash<QDate, SelectedInput> close;
    QHash<QDate, SelectedInput> volume;
    QDate first;
    QDate last;
};

inline Series select_series(const QVector<StoredObservation>& observations, const DerivedTimeFrame& tf) {
    DateMap closes;
    DateMap volumes;
    for (const StoredObservation& o : observations) {
        if (o.source_type != QLatin1String(source_type_id(SourceType::IbkrTwsReadonly)))
            continue;
        if (o.measure == QLatin1String(kBarClose))
            closes[o.effective_date].append(o);
        else if (o.measure == QLatin1String(kBarVolume))
            volumes[o.effective_date].append(o);
    }
    Series s;
    for (auto it = closes.cbegin(); it != closes.cend(); ++it) {
        const SelectedInput in = select_input(it.value(), tf);
        if (!in.present)
            continue;
        s.close.insert(it.key(), in);
        if (!s.first.isValid() || it.key() < s.first)
            s.first = it.key();
        if (!s.last.isValid() || it.key() > s.last)
            s.last = it.key();
    }
    for (auto it = volumes.cbegin(); it != volumes.cend(); ++it) {
        const SelectedInput in = select_input(it.value(), tf);
        if (in.present)
            s.volume.insert(it.key(), in);
    }
    return s;
}

/// One weekday as the calculation reads it: the persisted row when there is
/// one, the pinned version's compiled rule otherwise (OutsideCoverage when
/// neither covers the date).
struct GoverningDay {
    MarketSessionDay day;
    bool recorded = false;
};

inline GoverningDay governing_day(const RotationSessionRecord& record, const QDate& d) {
    const auto it = record.days.constFind(d);
    if (it != record.days.cend())
        return {it.value(), true};
    return {UsEquityCalendar::day(d), false};
}

/// A price input, or why it cannot be used.
struct PriceAt {
    std::optional<double> price;
    DerivedReason reason = DerivedReason::None;
    const SelectedInput* input = nullptr;
};

inline PriceAt checked_price(const SelectedInput& in, PriceAt p) {
    p.input = &in;
    if (!in.value.reported()) {
        p.reason = DerivedReason::ValueMissing;
        return p;
    }
    if (!(in.value.value > 0.0) || !std::isfinite(in.value.value)) {
        p.reason = DerivedReason::InvalidInput;
        return p;
    }
    p.price = in.value.value;
    return p;
}

/// The subject's close at one sequence entry.
inline PriceAt close_of(const RotationSessionInputs& in) {
    PriceAt p;
    if (!in.covered()) {
        p.reason = DerivedReason::SessionCalendarNotCovered;
        return p;
    }
    if (!in.close.present) {
        p.reason = DerivedReason::SessionBarMissing;
        return p;
    }
    if (!in.recorded) {
        p.reason = DerivedReason::SessionRecordMissing;
        return p;
    }
    return checked_price(in.close, p);
}

/// The reference's close on a session of the subject's sequence.
inline PriceAt reference_close(const QHash<QDate, SelectedInput>& closes, const QDate& d) {
    PriceAt p;
    const auto it = closes.constFind(d);
    if (it == closes.cend() || !it->present) {
        p.reason = DerivedReason::ReferenceUnavailable;
        return p;
    }
    return checked_price(it.value(), p);
}

struct ExpectedSession {
    std::optional<QDate> session;
    bool projected = false;
};

} // namespace rotation_detail

/// The session the ibkr_next_session_v1 freshness rule expects at `as_of`: the
/// latest session whose next session has opened by then. Dates up to
/// `latest_session` are read as the calculation reads them (the session
/// record, the compiled rule for a weekday without a row); later dates are a
/// projection of the compiled rule, flagged in `projected`. No session when a
/// date needed is covered by neither.
inline rotation_detail::ExpectedSession
rotation_expected_session(const QDateTime& as_of, const RotationSessionRecord& record, const QDate& latest_session) {
    using namespace rotation_detail;
    ExpectedSession out;
    auto day_at = [&](const QDate& d) {
        if (latest_session.isValid() && d <= latest_session)
            return governing_day(record, d).day;
        out.projected = true;
        return UsEquityCalendar::day(d);
    };
    // No run of non-session days is anywhere near a month long.
    constexpr int kMaxScanDays = 31;
    auto next_session_after = [&](const QDate& d) -> std::optional<MarketSessionDay> {
        QDate c = d.addDays(1);
        for (int i = 0; i < kMaxScanDays; ++i, c = c.addDays(1)) {
            const MarketSessionDay s = day_at(c);
            if (s.is_session())
                return s;
            if (s.type == SessionDayType::OutsideCoverage)
                return std::nullopt;
        }
        return std::nullopt;
    };
    auto last_session_before = [&](const QDate& d) -> std::optional<MarketSessionDay> {
        QDate c = d.addDays(-1);
        for (int i = 0; i < kMaxScanDays; ++i, c = c.addDays(-1)) {
            const MarketSessionDay s = day_at(c);
            if (s.is_session())
                return s;
            if (s.type == SessionDayType::OutsideCoverage)
                return std::nullopt;
        }
        return std::nullopt;
    };
    const QDate today = UsEquityCalendar::exchange_date(as_of);
    if (!today.isValid())
        return out;
    const MarketSessionDay d = day_at(today);
    if (d.type == SessionDayType::OutsideCoverage)
        return out;
    std::optional<MarketSessionDay> s =
        d.is_session() ? std::optional<MarketSessionDay>(d) : last_session_before(today);
    for (int guard = 0; s && guard < 10; ++guard) {
        const auto next = next_session_after(s->date);
        if (!next)
            return out;
        if (next->open_utc <= as_of) {
            out.session = s->date;
            return out;
        }
        s = last_session_before(s->date);
    }
    return out;
}

/// Compute every rotation measure of one listed instrument. `observations`
/// are its stored vintages (other sources are ignored); `sessions` the session
/// record; `reference`, when not null, the stored vintages of the instrument
/// the caller declares as the reference for relative measures
/// (`subject_is_reference` when they are the same instrument).
inline RotationMeasures compute_rotation_measures(const QVector<StoredObservation>& observations,
                                                  const DerivedTimeFrame& tf, const RotationSessionRecord& sessions,
                                                  const QVector<StoredObservation>* reference = nullptr,
                                                  bool subject_is_reference = false) {
    using namespace rotation_detail;
    RotationMeasures out;
    out.frame = tf;
    out.has_reference = reference != nullptr;
    if (!tf.valid())
        return out;
    const Series s = select_series(observations, tf);
    Series ref;
    if (reference)
        ref = select_series(*reference, tf);

    // The sequence: every weekday from the first to the latest available
    // close that is a session, or that nothing covers.
    if (s.first.isValid()) {
        for (QDate d = s.first; d <= s.last; d = d.addDays(1)) {
            if (d.dayOfWeek() >= 6)
                continue;
            const GoverningDay g = governing_day(sessions, d);
            const MarketSessionDay rule = UsEquityCalendar::day(d);
            const bool close_known = s.close.contains(d);
            auto exception = [&](const char* kind) {
                out.calendar_exceptions.append(
                    {d, QLatin1String(kind),
                     g.recorded ? QString::fromLatin1(session_day_type_id(g.day.type)) : QString(),
                     QLatin1String(session_day_type_id(rule.type)), close_known});
            };
            if (g.recorded &&
                (g.day.type != rule.type || g.day.open_utc != rule.open_utc || g.day.close_utc != rule.close_utc))
                exception("record_differs_from_calendar_rule");
            RotationSessionInputs in;
            in.session = d;
            if (g.day.type == SessionDayType::OutsideCoverage) {
                exception("weekday_not_covered");
                in.session_type = SessionDayType::OutsideCoverage;
                in.recorded = false;
                out.inputs.append(in);
                continue;
            }
            if (!g.day.is_session()) {
                if (close_known)
                    exception("close_on_non_session_day");
                continue;
            }
            if (close_known && !g.recorded)
                exception("close_without_session_record");
            in.session_type = g.day.type;
            in.recorded = g.recorded;
            in.close = s.close.value(d);
            in.volume = s.volume.value(d);
            out.inputs.append(in);
        }
    }
    const int n = static_cast<int>(out.inputs.size());

    // The latest uncovered entry at or before each index: a window [a, b]
    // holds one exactly when last_uncovered[b] >= a.
    QVector<int> last_uncovered(n, -1);
    for (int i = 0; i < n; ++i)
        last_uncovered[i] = !out.inputs[i].covered() ? i : (i > 0 ? last_uncovered[i - 1] : -1);
    auto covered_window = [&](int a, int b) { return last_uncovered[b] < a; };
    auto close_at = [&](int i) { return close_of(out.inputs[i]); };

    out.sessions.reserve(n);
    for (int i = 0; i < n; ++i) {
        RotationSessionResult r;
        r.session = out.inputs[i].session;
        r.session_type = out.inputs[i].session_type;
        const PriceAt now = close_at(i);

        // Point-to-point price returns over k sessions.
        for (std::size_t h = 0; h < kRotationReturnHorizons.size(); ++h) {
            const int k = kRotationReturnHorizons[h];
            if (!now.price) {
                r.price_return[h] = DerivedValue::unavailable(now.reason);
                continue;
            }
            if (i - k < 0) {
                r.price_return[h] = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
                continue;
            }
            if (!covered_window(i - k, i)) {
                r.price_return[h] = DerivedValue::unavailable(DerivedReason::SessionCalendarNotCovered);
                continue;
            }
            const PriceAt then = close_at(i - k);
            if (!then.price) {
                r.price_return[h] = DerivedValue::unavailable(then.reason);
                continue;
            }
            if (!now.input->same_basis(*then.input)) {
                r.price_return[h] = DerivedValue::unavailable(DerivedReason::InputBasisNotUniform);
                continue;
            }
            InputAccumulator acc;
            acc.add(*then.input, out.inputs[i - k].session);
            acc.add(*now.input, r.session);
            r.price_return[h] = DerivedValue::computed(*now.price / *then.price - 1.0, acc, QualityState::Proxy);
        }

        // Trend efficiency: net move over the path length of the n steps.
        for (std::size_t w = 0; w < kTrendEfficiencyWindows.size(); ++w) {
            const int win = kTrendEfficiencyWindows[w];
            if (!now.price) {
                r.trend_efficiency[w] = DerivedValue::unavailable(now.reason);
                continue;
            }
            if (i - win < 0) {
                r.trend_efficiency[w] = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
                continue;
            }
            if (!covered_window(i - win, i)) {
                r.trend_efficiency[w] = DerivedValue::unavailable(DerivedReason::SessionCalendarNotCovered);
                continue;
            }
            InputAccumulator acc;
            double path = 0.0;
            std::optional<double> previous;
            DerivedReason failure = DerivedReason::None;
            for (int q = i - win; q <= i; ++q) {
                const PriceAt p = close_at(q);
                if (!p.price) {
                    failure = p.reason;
                    break;
                }
                if (!p.input->same_basis(*now.input)) {
                    failure = DerivedReason::InputBasisNotUniform;
                    break;
                }
                acc.add(*p.input, out.inputs[q].session);
                if (previous)
                    path += std::fabs(*p.price - *previous);
                previous = p.price;
            }
            if (failure != DerivedReason::None) {
                r.trend_efficiency[w] = DerivedValue::unavailable(failure);
                continue;
            }
            if (!(path > 0.0)) {
                r.trend_efficiency[w] = DerivedValue::unavailable(DerivedReason::InvalidDenominator);
                continue;
            }
            const double start = *close_at(i - win).price;
            r.trend_efficiency[w] = DerivedValue::computed((*now.price - start) / path, acc, QualityState::Proxy);
        }

        // Acceleration: the latest 21-session return minus the one before it.
        {
            const int k = kReturnAccelerationWindow;
            if (!now.price) {
                r.return_acceleration = DerivedValue::unavailable(now.reason);
            } else if (i - 2 * k < 0) {
                r.return_acceleration = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
            } else if (!covered_window(i - 2 * k, i)) {
                r.return_acceleration = DerivedValue::unavailable(DerivedReason::SessionCalendarNotCovered);
            } else {
                const PriceAt mid = close_at(i - k);
                const PriceAt old = close_at(i - 2 * k);
                if (!mid.price) {
                    r.return_acceleration = DerivedValue::unavailable(mid.reason);
                } else if (!old.price) {
                    r.return_acceleration = DerivedValue::unavailable(old.reason);
                } else if (!old.input->same_basis(*now.input) || !mid.input->same_basis(*now.input)) {
                    r.return_acceleration = DerivedValue::unavailable(DerivedReason::InputBasisNotUniform);
                } else {
                    InputAccumulator acc;
                    acc.add(*old.input, out.inputs[i - 2 * k].session);
                    acc.add(*mid.input, out.inputs[i - k].session);
                    acc.add(*now.input, r.session);
                    r.return_acceleration = DerivedValue::computed(
                        (*now.price / *mid.price - 1.0) - (*mid.price / *old.price - 1.0), acc, QualityState::Proxy);
                }
            }
        }

        // Self-relative volume over full sessions only.
        for (std::size_t w = 0; w < kVolumeWindows.size(); ++w) {
            const VolumeWindow vw = kVolumeWindows[w];
            if (!out.inputs[i].covered()) {
                r.volume_ratio[w] = DerivedValue::unavailable(DerivedReason::SessionCalendarNotCovered);
                continue;
            }
            if (r.session_type == SessionDayType::EarlyClose) {
                r.volume_ratio[w] = DerivedValue::unavailable(DerivedReason::EarlyCloseSessionExcluded);
                continue;
            }
            QVector<int> full;
            bool uncovered = false;
            for (int q = i; q >= 0 && full.size() < vw.recent + vw.baseline; --q) {
                if (!out.inputs[q].covered()) {
                    uncovered = true;
                    break;
                }
                if (out.inputs[q].session_type == SessionDayType::Regular)
                    full.append(q);
            }
            if (uncovered) {
                r.volume_ratio[w] = DerivedValue::unavailable(DerivedReason::SessionCalendarNotCovered);
                continue;
            }
            if (full.size() < vw.recent + vw.baseline) {
                r.volume_ratio[w] = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
                continue;
            }
            InputAccumulator acc;
            double recent = 0.0;
            double baseline = 0.0;
            DerivedReason failure = DerivedReason::None;
            const SelectedInput* first_volume = nullptr;
            // Oldest first, so the sums run in session order.
            for (int j = static_cast<int>(full.size()) - 1; j >= 0; --j) {
                const int q = full[j];
                const SelectedInput& v = out.inputs[q].volume;
                if (!v.present) {
                    failure = DerivedReason::SessionBarMissing;
                    break;
                }
                if (!out.inputs[q].recorded) {
                    failure = DerivedReason::SessionRecordMissing;
                    break;
                }
                if (!v.value.reported()) {
                    failure = DerivedReason::ValueMissing;
                    break;
                }
                if (v.value.value < 0.0 || !std::isfinite(v.value.value)) {
                    failure = DerivedReason::InvalidInput;
                    break;
                }
                if (first_volume && !v.same_basis(*first_volume)) {
                    failure = DerivedReason::InputBasisNotUniform;
                    break;
                }
                if (!first_volume)
                    first_volume = &v;
                acc.add(v, out.inputs[q].session);
                if (j < vw.recent)
                    recent += v.value.value;
                else
                    baseline += v.value.value;
            }
            if (failure != DerivedReason::None) {
                r.volume_ratio[w] = DerivedValue::unavailable(failure);
                continue;
            }
            const double base_mean = baseline / vw.baseline;
            if (!(base_mean > 0.0)) {
                r.volume_ratio[w] = DerivedValue::unavailable(DerivedReason::InvalidDenominator);
                continue;
            }
            r.volume_ratio[w] = DerivedValue::computed((recent / vw.recent) / base_mean, acc, QualityState::Proxy);
        }

        // Relative price return against the declared reference.
        r.has_reference = out.has_reference;
        if (out.has_reference) {
            for (std::size_t h = 0; h < kRotationReturnHorizons.size(); ++h) {
                const int k = kRotationReturnHorizons[h];
                if (subject_is_reference) {
                    r.relative_price_return[h] = DerivedValue::unavailable(DerivedReason::SubjectIsReference);
                    continue;
                }
                if (!now.price) {
                    r.relative_price_return[h] = DerivedValue::unavailable(now.reason);
                    continue;
                }
                if (i - k < 0) {
                    r.relative_price_return[h] = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
                    continue;
                }
                if (!covered_window(i - k, i)) {
                    r.relative_price_return[h] = DerivedValue::unavailable(DerivedReason::SessionCalendarNotCovered);
                    continue;
                }
                const PriceAt then = close_at(i - k);
                if (!then.price) {
                    r.relative_price_return[h] = DerivedValue::unavailable(then.reason);
                    continue;
                }
                const PriceAt ref_now = reference_close(ref.close, r.session);
                const PriceAt ref_then = reference_close(ref.close, out.inputs[i - k].session);
                if (!ref_now.price || !ref_then.price) {
                    // Any unusable reference bar makes the relation unavailable.
                    r.relative_price_return[h] = DerivedValue::unavailable(DerivedReason::ReferenceUnavailable);
                    continue;
                }
                // All four closes on one units and basis: the subject's own
                // pair first, then the reference's against it.
                if (!now.input->same_basis(*then.input)) {
                    r.relative_price_return[h] = DerivedValue::unavailable(DerivedReason::InputBasisNotUniform);
                    continue;
                }
                if (!ref_now.input->same_basis(*now.input) || !ref_then.input->same_basis(*now.input)) {
                    r.relative_price_return[h] = DerivedValue::unavailable(DerivedReason::ReturnBasisNotComparable);
                    continue;
                }
                InputAccumulator acc;
                acc.add(*then.input, out.inputs[i - k].session);
                acc.add(*now.input, r.session);
                acc.add(*ref_then.input, out.inputs[i - k].session);
                acc.add(*ref_now.input, r.session);
                const double value = (*now.price / *then.price) / (*ref_now.price / *ref_then.price) - 1.0;
                r.relative_price_return[h] = DerivedValue::computed(value, acc, QualityState::Proxy);
            }
        }
        out.sessions.append(r);
    }

    // The latest snapshot and its freshness: the latest listed session.
    int latest = n - 1;
    while (latest >= 0 && !out.inputs[latest].covered())
        --latest;
    if (latest >= 0) {
        out.snapshot.has_data = true;
        out.snapshot.latest_session = out.inputs[latest].session;
        out.snapshot.values = out.sessions[latest];
        const ExpectedSession expected = rotation_expected_session(tf.as_of, sessions, out.snapshot.latest_session);
        out.snapshot.expected_projected = expected.projected;
        if (!expected.session) {
            out.snapshot.freshness = RotationFreshness::Unknown;
        } else {
            out.snapshot.expected_session = *expected.session;
            out.snapshot.freshness =
                out.snapshot.latest_session < *expected.session ? RotationFreshness::Stale : RotationFreshness::Fresh;
        }
        if (out.snapshot.freshness == RotationFreshness::Stale) {
            auto age = [](DerivedValue& v) {
                if (v.usable()) {
                    v.state = QualityState::Stale;
                    v.reason = DerivedReason::LatestSessionBeforeExpected;
                }
            };
            RotationSessionResult& v = out.snapshot.values;
            for (DerivedValue& d : v.price_return)
                age(d);
            for (DerivedValue& d : v.trend_efficiency)
                age(d);
            age(v.return_acceleration);
            for (DerivedValue& d : v.volume_ratio)
                age(d);
            for (DerivedValue& d : v.relative_price_return)
                age(d);
        }
    }
    return out;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

inline QString rotation_measure_id(const char* stem, int n) {
    return QStringLiteral("%1_%2").arg(QLatin1String(stem)).arg(n);
}

inline QJsonObject rotation_values_json(const RotationSessionResult& r) {
    const QString kind = QLatin1String(measurement_kind_id(MeasurementKind::RotationProxy));
    QJsonObject values;
    for (std::size_t h = 0; h < kRotationReturnHorizons.size(); ++h)
        values.insert(rotation_measure_id("price_return", kRotationReturnHorizons[h]),
                      derived_value_json(r.price_return[h], kind, QStringLiteral("price_return_ratio")));
    for (std::size_t w = 0; w < kTrendEfficiencyWindows.size(); ++w)
        values.insert(rotation_measure_id("trend_efficiency", kTrendEfficiencyWindows[w]),
                      derived_value_json(r.trend_efficiency[w], kind, QStringLiteral("efficiency_ratio_minus1_1")));
    values.insert(rotation_measure_id("return_acceleration", kReturnAccelerationWindow),
                  derived_value_json(r.return_acceleration, kind, QStringLiteral("price_return_difference")));
    for (std::size_t w = 0; w < kVolumeWindows.size(); ++w)
        values.insert(
            QStringLiteral("volume_ratio_%1_%2").arg(kVolumeWindows[w].recent).arg(kVolumeWindows[w].baseline),
            derived_value_json(r.volume_ratio[w], kind, QStringLiteral("self_relative_volume_ratio")));
    if (r.has_reference) {
        for (std::size_t h = 0; h < kRotationReturnHorizons.size(); ++h)
            values.insert(
                rotation_measure_id("relative_price_return", kRotationReturnHorizons[h]),
                derived_value_json(r.relative_price_return[h], kind, QStringLiteral("relative_price_return_ratio")));
    }
    return values;
}

inline QJsonObject rotation_bar_input_json(const SelectedInput& s) {
    QJsonObject o;
    o.insert(QStringLiteral("present"), s.present);
    if (!s.present)
        return o;
    o.insert(QStringLiteral("observation_id"), s.observation_id);
    o.insert(QStringLiteral("value"), s.value.reported() ? QJsonValue(s.value.value) : QJsonValue(QJsonValue::Null));
    o.insert(QStringLiteral("quality"), QLatin1String(quality_state_id(s.quality)));
    o.insert(QStringLiteral("available_from"), derived_time_text(s.available_from));
    o.insert(QStringLiteral("point_in_time_status"), QLatin1String(point_in_time_status_id(s.point_in_time_status)));
    o.insert(QStringLiteral("revision_state"), s.revision_state);
    o.insert(QStringLiteral("units"), s.units);
    o.insert(QStringLiteral("basis"), s.basis);
    o.insert(QStringLiteral("vintages_known"), s.vintages_known);
    return o;
}

inline QJsonObject rotation_calendar_json(const RotationMeasures& m) {
    QJsonArray exceptions;
    for (const RotationCalendarException& e : m.calendar_exceptions) {
        exceptions.append(QJsonObject{{QStringLiteral("date"), e.date.toString(Qt::ISODate)},
                                      {QStringLiteral("kind"), e.kind},
                                      {QStringLiteral("recorded_day_type"), e.recorded_day_type.isEmpty()
                                                                                ? QJsonValue(QJsonValue::Null)
                                                                                : QJsonValue(e.recorded_day_type)},
                                      {QStringLiteral("calendar_rule_day_type"), e.calendar_rule_day_type},
                                      {QStringLiteral("close_known"), e.close_known}});
    }
    return QJsonObject{{QStringLiteral("calendar_id"), QLatin1String(kUsEquityCalendarId)},
                       {QStringLiteral("calendar_version"), QLatin1String(kUsEquityCalendarVersion)},
                       {QStringLiteral("session_source"), QLatin1String(kRotationSessionSource)},
                       {QStringLiteral("exceptions"), exceptions}};
}

/// JSON of the measures. Sessions outside [from, to] (either may be invalid:
/// unbounded) are left out of the series; the calculation itself always used
/// the full stored history, so the lookback of the first listed session is
/// complete. A weekday nothing covers is not a session and is not listed: it
/// is in the calendar exceptions, and every window across it says so.
inline QJsonObject rotation_measures_json(const RotationMeasures& m, const QDate& from = QDate(),
                                          const QDate& to = QDate()) {
    QJsonArray sessions;
    for (int i = 0; i < m.sessions.size(); ++i) {
        const RotationSessionInputs& in = m.inputs[i];
        if (!in.covered())
            continue;
        const RotationSessionResult& r = m.sessions[i];
        if ((from.isValid() && r.session < from) || (to.isValid() && r.session > to))
            continue;
        sessions.append(QJsonObject{
            {QStringLiteral("session"), r.session.toString(Qt::ISODate)},
            {QStringLiteral("session_type"), QLatin1String(session_day_type_id(r.session_type))},
            {QStringLiteral("inputs"), QJsonObject{{QLatin1String(kBarClose), rotation_bar_input_json(in.close)},
                                                   {QLatin1String(kBarVolume), rotation_bar_input_json(in.volume)}}},
            {QStringLiteral("values"), rotation_values_json(r)}});
    }
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("has_data"), m.snapshot.has_data);
    if (m.snapshot.has_data) {
        snapshot.insert(QStringLiteral("latest_session"), m.snapshot.latest_session.toString(Qt::ISODate));
        snapshot.insert(QStringLiteral("expected_session"),
                        m.snapshot.expected_session.isValid()
                            ? QJsonValue(m.snapshot.expected_session.toString(Qt::ISODate))
                            : QJsonValue(QJsonValue::Null));
        snapshot.insert(QStringLiteral("expected_session_calendar"), m.snapshot.expected_projected
                                                                         ? QStringLiteral("calendar_rule_projection")
                                                                         : QStringLiteral("session_record"));
        snapshot.insert(QStringLiteral("freshness"), QLatin1String(rotation_freshness_id(m.snapshot.freshness)));
        snapshot.insert(QStringLiteral("freshness_rule"), QLatin1String(kRotationFreshnessRule));
        snapshot.insert(QStringLiteral("values"), rotation_values_json(m.snapshot.values));
    }
    QJsonObject o;
    o.insert(QStringLiteral("method"), QLatin1String(kRotationProxyMeasuresVersion));
    o.insert(QStringLiteral("return_basis"), QLatin1String(kRotationReturnBasis));
    o.insert(QStringLiteral("total_return"), false);
    o.insert(QStringLiteral("volume_basis"), QLatin1String(kRotationVolumeBasis));
    o.insert(QStringLiteral("calendar_version"), QLatin1String(kUsEquityCalendarVersion));
    o.insert(QStringLiteral("calendar"), rotation_calendar_json(m));
    o.insert(QStringLiteral("sessions"), sessions);
    o.insert(QStringLiteral("latest"), snapshot);
    return o;
}

} // namespace fincept::services::etf
