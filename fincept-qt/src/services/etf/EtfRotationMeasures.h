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
// sessions of the U.S. equity calendar (EtfSessionCalendar.h; a holiday is
// not a session, an early close is):
//   price_return_k             C(t) / C(t-k) - 1, k = 5, 21, 63, 126, 252
//   trend_efficiency_n         (C(t) - C(t-n)) / sum |C(i) - C(i-1)| over the
//                              n steps, n = 21, 63: +1 a straight rise, -1 a
//                              straight fall, near 0 no net progress
//   return_acceleration_21     [C(t)/C(t-21) - 1] - [C(t-21)/C(t-42) - 1]
//   volume_ratio_a_b           mean volume of the last a full sessions / mean
//                              volume of the b full sessions before them,
//                              (a, b) = (5, 63), (21, 126)
//   relative_price_return_k    (C(t)/C(t-k)) / (R(t)/R(t-k)) - 1 against a
//                              reference the CALLER declares; only when both
//                              closes are on the same units and basis
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
//   * a calendar session inside the stored history without an available bar
//     is session_bar_missing: never filled, never skipped (a skipped session
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
                          "relative=caller_declared_reference,same_units_and_basis;freshness=%6")
        .arg(QLatin1String(kRotationReturnBasis), horizons.join(QLatin1Char(',')), efficiency.join(QLatin1Char(',')))
        .arg(kReturnAccelerationWindow)
        .arg(volume.join(QLatin1Char(',')), QLatin1String(kRotationFreshnessRule));
}

struct RotationSessionInputs {
    QDate session;
    SessionDayType session_type = SessionDayType::Regular;
    SelectedInput close;
    SelectedInput volume;
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
    QDate latest_session;   ///< the latest session with an available close
    QDate expected_session; ///< the session the freshness rule expects at as_of (invalid: cannot be judged)
    RotationFreshness freshness = RotationFreshness::Unknown;
    RotationSessionResult values; ///< the latest session's values; usable ones become STALE when stale
};

struct RotationMeasures {
    DerivedTimeFrame frame;
    bool has_reference = false;
    QVector<RotationSessionInputs> inputs;   ///< every calendar session from the first to the latest available close
    QVector<RotationSessionResult> sessions; ///< same order
    RotationSnapshot snapshot;
};

/// The session the ibkr_next_session_v1 freshness rule expects at `as_of`: the
/// latest session whose next session has opened by then. nullopt when the
/// calendar cannot answer.
inline std::optional<QDate> rotation_expected_session(const QDateTime& as_of) {
    const QDate today = UsEquityCalendar::exchange_date(as_of);
    if (!today.isValid())
        return std::nullopt;
    std::optional<MarketSessionDay> s;
    const MarketSessionDay d = UsEquityCalendar::day(today);
    if (d.type == SessionDayType::OutsideCoverage)
        return std::nullopt;
    if (d.is_session())
        s = d;
    else
        s = UsEquityCalendar::last_session_before(today);
    for (int guard = 0; s && guard < 10; ++guard) {
        const auto next = UsEquityCalendar::next_session_after(s->date);
        if (!next)
            return std::nullopt;
        if (next->open_utc <= as_of)
            return s->date;
        s = UsEquityCalendar::last_session_before(s->date);
    }
    return std::nullopt;
}

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

/// A price input of the series, or why it cannot be used.
struct PriceAt {
    std::optional<double> price;
    DerivedReason reason = DerivedReason::None;
    const SelectedInput* input = nullptr;
};

inline PriceAt price_at(const QHash<QDate, SelectedInput>& closes, const QDate& d, DerivedReason absent) {
    PriceAt p;
    const auto it = closes.constFind(d);
    if (it == closes.cend() || !it->present) {
        p.reason = absent;
        return p;
    }
    p.input = &it.value();
    if (!it->value.reported()) {
        p.reason = DerivedReason::ValueMissing;
        return p;
    }
    if (!(it->value.value > 0.0) || !std::isfinite(it->value.value)) {
        p.reason = DerivedReason::InvalidInput;
        return p;
    }
    p.price = it->value.value;
    return p;
}

} // namespace rotation_detail

/// Compute every rotation measure of one listed instrument. `observations`
/// are its stored vintages (other sources are ignored); `reference`, when not
/// null, are the stored vintages of the instrument the caller declares as the
/// reference for relative measures (`subject_is_reference` when they are the
/// same instrument).
inline RotationMeasures compute_rotation_measures(const QVector<StoredObservation>& observations,
                                                  const DerivedTimeFrame& tf,
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

    if (s.first.isValid()) {
        bool complete = false;
        for (const MarketSessionDay& d : UsEquityCalendar::weekdays_in(s.first, s.last, &complete)) {
            if (!d.is_session())
                continue;
            RotationSessionInputs in;
            in.session = d.date;
            in.session_type = d.type;
            in.close = s.close.value(d.date);
            in.volume = s.volume.value(d.date);
            out.inputs.append(in);
        }
    }
    const int n = static_cast<int>(out.inputs.size());

    auto close_at = [&](int i) { return price_at(s.close, out.inputs[i].session, DerivedReason::SessionBarMissing); };

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
            const PriceAt then = close_at(i - k);
            if (!then.price) {
                r.price_return[h] = DerivedValue::unavailable(then.reason);
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
            } else {
                const PriceAt mid = close_at(i - k);
                const PriceAt old = close_at(i - 2 * k);
                if (!mid.price) {
                    r.return_acceleration = DerivedValue::unavailable(mid.reason);
                } else if (!old.price) {
                    r.return_acceleration = DerivedValue::unavailable(old.reason);
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
            if (r.session_type == SessionDayType::EarlyClose) {
                r.volume_ratio[w] = DerivedValue::unavailable(DerivedReason::EarlyCloseSessionExcluded);
                continue;
            }
            QVector<int> full;
            for (int q = i; q >= 0 && full.size() < vw.recent + vw.baseline; --q) {
                if (out.inputs[q].session_type == SessionDayType::Regular)
                    full.append(q);
            }
            if (full.size() < vw.recent + vw.baseline) {
                r.volume_ratio[w] = DerivedValue::unavailable(DerivedReason::InsufficientHistory);
                continue;
            }
            InputAccumulator acc;
            double recent = 0.0;
            double baseline = 0.0;
            DerivedReason failure = DerivedReason::None;
            // Oldest first, so the sums run in session order.
            for (int j = static_cast<int>(full.size()) - 1; j >= 0; --j) {
                const int q = full[j];
                const SelectedInput& v = out.inputs[q].volume;
                if (!v.present) {
                    failure = DerivedReason::SessionBarMissing;
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
                const PriceAt then = close_at(i - k);
                if (!then.price) {
                    r.relative_price_return[h] = DerivedValue::unavailable(then.reason);
                    continue;
                }
                const PriceAt ref_now = price_at(ref.close, r.session, DerivedReason::ReferenceUnavailable);
                const PriceAt ref_then =
                    price_at(ref.close, out.inputs[i - k].session, DerivedReason::ReferenceUnavailable);
                if (!ref_now.price || !ref_then.price) {
                    // Any unusable reference bar makes the relation unavailable.
                    r.relative_price_return[h] = DerivedValue::unavailable(DerivedReason::ReferenceUnavailable);
                    continue;
                }
                const bool same_basis =
                    now.input->units == ref_now.input->units && then.input->units == ref_then.input->units &&
                    now.input->basis == ref_now.input->basis && then.input->basis == ref_then.input->basis;
                if (!same_basis) {
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

    // The latest snapshot and its freshness.
    if (n > 0) {
        out.snapshot.has_data = true;
        out.snapshot.latest_session = s.last;
        out.snapshot.values = out.sessions.last();
        const auto expected = rotation_expected_session(tf.as_of);
        if (!expected) {
            out.snapshot.freshness = RotationFreshness::Unknown;
        } else {
            out.snapshot.expected_session = *expected;
            out.snapshot.freshness = s.last < *expected ? RotationFreshness::Stale : RotationFreshness::Fresh;
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
    o.insert(QStringLiteral("vintages_known"), s.vintages_known);
    return o;
}

/// JSON of the measures. Sessions outside [from, to] (either may be invalid:
/// unbounded) are left out of the series; the calculation itself always used
/// the full stored history, so the lookback of the first listed session is
/// complete.
inline QJsonObject rotation_measures_json(const RotationMeasures& m, const QDate& from = QDate(),
                                          const QDate& to = QDate()) {
    QJsonArray sessions;
    for (int i = 0; i < m.sessions.size(); ++i) {
        const RotationSessionResult& r = m.sessions[i];
        if ((from.isValid() && r.session < from) || (to.isValid() && r.session > to))
            continue;
        const RotationSessionInputs& in = m.inputs[i];
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
    o.insert(QStringLiteral("sessions"), sessions);
    o.insert(QStringLiteral("latest"), snapshot);
    return o;
}

} // namespace fincept::services::etf
