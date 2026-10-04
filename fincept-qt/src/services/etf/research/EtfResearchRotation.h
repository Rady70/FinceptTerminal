// src/services/etf/research/EtfResearchRotation.h
//
// Market-behaviour calculations of the research engine (PROXY and MODEL
// evidence). Adapted from triphopp/bloomberg-terminal@3387072
// backend/routers/rotation.py, backend/analytics/sector_mom.py and
// backend/analytics/country_rotation.py, with these corrections (recorded in
// the control repository's consolidated Batch E record):
//
//  * returns are total returns computed by MarketLab from the split-adjusted
//    close plus cash and capital-gain distributions (Yahoo's dividend-adjusted
//    close is restated at every distribution and is not stored);
//  * turnover is close x volume of the SAME unadjusted-for-dividends close,
//    and a day counts only when every member has volume (the reference summed
//    whatever was present, so a missing fund raised everyone else's share);
//  * momentum components that lack history are UNAVAILABLE and leave the
//    cross-section, never 0.0 (the reference's silent neutral);
//  * the country "12M-1M" component is the 12-month return skipping the last
//    month (the reference computed a one-month return under that name and
//    never used its 13-month price);
//  * equal-weight baskets are daily-rebalanced from member returns with a
//    minimum member fraction (the reference averaged buy-and-hold levels, each
//    rebased at its own first date, so late-listed members distorted the path).
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfSessionCalendar.h"
#include "services/etf/research/EtfResearchInputs.h"
#include "services/etf/research/EtfResearchMath.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <QHash>
#include <QSet>

#include <cmath>

namespace fincept::services::etf::research {

inline constexpr const char* kRotationMethod = "etfr_rotation_v1";
inline constexpr const char* kRrgMethod = "etfr_rrg_jdk_style_v1";
inline constexpr const char* kTurnoverMethod = "etfr_turnover_share_v1";
inline constexpr const char* kMomentumMethod = "etfr_momentum_composite_v1";
inline constexpr const char* kSourceYahoo = "yahoo_chart";

inline constexpr int kD1 = 1;
inline constexpr int kW1 = 5;
inline constexpr int kM1 = 21;
inline constexpr int kM3 = 63;

/// A total-return index on the series' own sessions.
struct TrIndex {
    QString symbol;
    QVector<QDate> dates;
    QVector<double> tr; ///< 1.0 at the first session
    QVector<double> close;
    QVector<std::optional<double>> volume;
    QVector<double> distribution; ///< cash + capital gain per share, ex-date that session
    bool revised = false;
    /// Session at which the index restarted after a suspected unadjusted corporate
    /// action (close ratio outside [1/3, 3] with no recorded distribution); invalid
    /// when the history is continuous. Earlier sessions are discarded, never bridged.
    QDate restarted_at;
    double restart_ratio = 0.0;

    int size() const { return static_cast<int>(dates.size()); }
    bool empty() const { return dates.isEmpty(); }
    QDate last() const { return dates.isEmpty() ? QDate() : dates.last(); }

    int index_of(const QDate& d) const {
        const auto it = std::lower_bound(dates.begin(), dates.end(), d);
        if (it == dates.end() || *it != d)
            return -1;
        return static_cast<int>(it - dates.begin());
    }
};

/// Close ratios outside [1/kDiscontinuityRatio, kDiscontinuityRatio] in one
/// session, with no recorded distribution, are treated as an unadjusted corporate
/// action (spin-off, unadjusted split, re-denomination), not as a return.
inline constexpr double kDiscontinuityRatio = 3.0;

inline TrIndex make_tr_index(const BarSeries& s) {
    TrIndex t;
    t.symbol = s.symbol;
    t.revised = s.any_revised;
    double level = 1.0;
    for (int i = 0; i < s.bars.size(); ++i) {
        const DailyBar& b = s.bars[i];
        if (!(b.close > 0.0))
            continue;
        if (!t.close.isEmpty()) {
            const double ratio = b.close / t.close.last();
            if (b.dividend + b.capital_gain == 0.0 &&
                (ratio > kDiscontinuityRatio || ratio < 1.0 / kDiscontinuityRatio)) {
                const QString symbol = t.symbol;
                const bool revised = t.revised;
                t = TrIndex();
                t.symbol = symbol;
                t.revised = revised;
                t.restarted_at = b.date;
                t.restart_ratio = ratio;
                level = 1.0;
            }
        }
        if (!t.close.isEmpty())
            level *= (b.close + b.dividend + b.capital_gain) / t.close.last();
        t.dates.append(b.date);
        t.tr.append(level);
        t.close.append(b.close);
        t.volume.append(b.volume);
        t.distribution.append(b.dividend + b.capital_gain);
    }
    return t;
}

/// Percent change of `level` over `lag` sessions ending at index `end`.
inline std::optional<double> pct_change(const QVector<double>& level, int end, int lag) {
    if (end < 0 || end - lag < 0 || end >= level.size())
        return std::nullopt;
    const double prev = level[end - lag];
    if (!(prev > 0.0))
        return std::nullopt;
    return (level[end] / prev - 1.0) * 100.0;
}

/// Two indices on their common sessions.
struct Aligned {
    QVector<QDate> dates;
    QVector<double> a;
    QVector<double> b;
};

inline Aligned align(const TrIndex& x, const TrIndex& y) {
    Aligned out;
    int i = 0, j = 0;
    while (i < x.size() && j < y.size()) {
        if (x.dates[i] == y.dates[j]) {
            out.dates.append(x.dates[i]);
            out.a.append(x.tr[i]);
            out.b.append(y.tr[j]);
            ++i;
            ++j;
        } else if (x.dates[i] < y.dates[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return out;
}

/// Freshness of a series against the frame. U.S.-listed symbols must reach the
/// latest completed NYSE session; other markets may lag up to five calendar
/// days (holidays differ and are not modelled for them).
inline bool series_is_stale(const QString& symbol, const QDate& last, const QDate& expected_us, const QDate& as_of) {
    if (!last.isValid())
        return true;
    const bool us = !symbol.contains(QLatin1Char('.')) && !symbol.startsWith(QLatin1Char('^'));
    if (us && expected_us.isValid())
        return last < expected_us;
    return last.daysTo(as_of) > 5;
}

/// The unavailable value for a window the index cannot cover: plain short history,
/// or history cut at a suspected unadjusted corporate action.
inline ResearchValue short_history_value(const TrIndex& t, const QString& method) {
    if (!t.restarted_at.isValid())
        return ResearchValue::unavailable(QStringLiteral("insufficient_history"), method);
    ResearchValue v = ResearchValue::unavailable(
        QStringLiteral("history_restarted_%1_close_ratio_%2_suspect_unadjusted_corporate_action")
            .arg(t.restarted_at.toString(Qt::ISODate))
            .arg(t.restart_ratio, 0, 'f', 3),
        method);
    v.add_flag(flag::kHistoryRestarted);
    return v;
}

inline ResearchValue proxy_return(const TrIndex& t, int lag, const QVector<CredCondition>& conds) {
    const auto v = pct_change(t.tr, t.size() - 1, lag);
    if (!v)
        return short_history_value(t, QLatin1String(kRotationMethod));
    return graded(*v, EvidenceClass::Proxy, Credibility::High, conds, QStringLiteral("pct"),
                  QLatin1String(kRotationMethod), QLatin1String(kSourceYahoo), t.last());
}

inline ReturnSet compute_returns(const TrIndex& t, const TrIndex* bench, const QString& bench_symbol,
                                 const QVector<CredCondition>& conds) {
    ReturnSet r;
    r.benchmark = bench_symbol;
    r.last_session = t.last();
    if (t.empty()) {
        r.d1 = ResearchValue::unavailable(QStringLiteral("no_observation"));
        r.d1.add_flag(flag::kNoObservation);
        r.w1 = r.m1 = r.m3 = r.d1;
        r.rel_m1 = ResearchValue::unavailable(QStringLiteral("no_observation"));
        return r;
    }
    r.d1 = proxy_return(t, kD1, conds);
    r.w1 = proxy_return(t, kW1, conds);
    r.m1 = proxy_return(t, kM1, conds);
    r.m3 = proxy_return(t, kM3, conds);
    if (!bench || bench->empty() || bench->symbol == t.symbol) {
        r.rel_m1 =
            ResearchValue::unavailable(bench && bench->symbol == t.symbol ? QStringLiteral("subject_is_benchmark")
                                                                          : QStringLiteral("no_benchmark"));
        return r;
    }
    const Aligned al = align(t, *bench);
    const int n = static_cast<int>(al.dates.size());
    const auto own = pct_change(al.a, n - 1, kM1);
    const auto ben = pct_change(al.b, n - 1, kM1);
    if (!own || !ben) {
        r.rel_m1 = t.restarted_at.isValid() || bench->restarted_at.isValid()
                       ? short_history_value(t.restarted_at.isValid() ? t : *bench, QLatin1String(kRotationMethod))
                       : ResearchValue::unavailable(QStringLiteral("insufficient_common_history"));
        return r;
    }
    QVector<CredCondition> c = conds;
    if (al.dates.last() != t.last())
        c.append(CredCondition::Partial); // benchmark ends earlier than the subject
    r.rel_m1 = graded(*own - *ben, EvidenceClass::Proxy, Credibility::High, c, QStringLiteral("pp"),
                      QLatin1String(kRotationMethod), QLatin1String(kSourceYahoo), al.dates.last());
    return r;
}

// ── RRG (JdK-style approximation, weekly) ────────────────────────────────────
inline QDate week_ending_friday(const QDate& d) {
    const int dow = d.dayOfWeek(); // Mon=1..Sun=7
    return d.addDays((5 - dow + 7) % 7);
}

inline QString rrg_quadrant(double ratio, double mom) {
    if (ratio >= 100 && mom >= 100)
        return QStringLiteral("Leading");
    if (ratio < 100 && mom >= 100)
        return QStringLiteral("Improving");
    if (ratio >= 100)
        return QStringLiteral("Weakening");
    return QStringLiteral("Lagging");
}

/// True when a weekday session could still follow `last` within its
/// Friday-ending week: NYSE calendar for U.S. listings (holiday Fridays end a
/// week on Thursday), any weekday elsewhere.
inline bool week_in_progress(const QDate& last, bool us_calendar) {
    const QDate friday = week_ending_friday(last);
    for (QDate d = last.addDays(1); d <= friday; d = d.addDays(1)) {
        if (d.dayOfWeek() >= 6)
            continue;
        if (us_calendar && UsEquityCalendar::covers(d) && !UsEquityCalendar::day(d).is_session())
            continue;
        return true;
    }
    return false;
}

/// Weekly RS = subject / benchmark at the last common session of each
/// COMPLETED Friday-ending week, labelled by that session. RS-Ratio =
/// 100·RS/SMA8(RS), RS-Momentum = 100·Ratio/SMA4(Ratio). Needs 70 common
/// sessions (reference). Correction: the reference also plotted the week still
/// in progress as if it were a full week, so a Wednesday point compared three
/// sessions of relative progress with full weeks and dragged momentum down.
inline QVector<RrgPoint> rrg_frame(const TrIndex& t, const TrIndex& bench) {
    const Aligned al = align(t, bench);
    QVector<RrgPoint> out;
    if (al.dates.size() < 70)
        return out;
    QVector<QDate> wd;
    QVector<double> rs;
    for (int i = 0; i < al.dates.size(); ++i) {
        const QDate key = week_ending_friday(al.dates[i]);
        const double v = al.a[i] / al.b[i];
        if (!wd.isEmpty() && week_ending_friday(wd.last()) == key) {
            wd.last() = al.dates[i];
            rs.last() = v;
        } else {
            wd.append(al.dates[i]);
            rs.append(v);
        }
    }
    const bool us = !t.symbol.contains(QLatin1Char('.')) && !t.symbol.startsWith(QLatin1Char('^'));
    if (!wd.isEmpty() && week_in_progress(wd.last(), us)) {
        wd.removeLast();
        rs.removeLast();
    }
    QVector<std::optional<double>> ratio(rs.size());
    for (int i = 7; i < rs.size(); ++i) {
        double s = 0;
        for (int k = i - 7; k <= i; ++k)
            s += rs[k];
        ratio[i] = 100.0 * rs[i] / (s / 8.0);
    }
    for (int i = 10; i < rs.size(); ++i) {
        if (!ratio[i] || !ratio[i - 3])
            continue;
        double s = 0;
        bool ok = true;
        for (int k = i - 3; k <= i; ++k) {
            if (!ratio[k]) {
                ok = false;
                break;
            }
            s += *ratio[k];
        }
        if (!ok)
            continue;
        out.append({wd[i], *ratio[i], 100.0 * *ratio[i] / (s / 4.0)});
    }
    return out;
}

inline RrgResult compute_rrg(const TrIndex& t, const TrIndex* bench, const QString& bench_symbol,
                             const QVector<CredCondition>& conds, int tail = 8) {
    RrgResult r;
    r.benchmark = bench_symbol;
    if (!bench || bench->empty() || bench->symbol == t.symbol) {
        r.quadrant =
            ResearchValue::unavailable(bench && bench->symbol == t.symbol ? QStringLiteral("subject_is_benchmark")
                                                                          : QStringLiteral("no_benchmark"),
                                       QLatin1String(kRrgMethod));
        return r;
    }
    const QVector<RrgPoint> frame = rrg_frame(t, *bench);
    if (frame.isEmpty()) {
        r.quadrant = short_history_value(t, QLatin1String(kRrgMethod));
        r.quadrant.add_flag(flag::kInsufficientHistory);
        return r;
    }
    const RrgPoint& last = frame.last();
    r.quadrant.evidence = EvidenceClass::Model;
    r.quadrant.label = rrg_quadrant(last.ratio, last.mom);
    r.quadrant.value = last.ratio;
    r.quadrant.units = QStringLiteral("index");
    r.quadrant.method = QLatin1String(kRrgMethod);
    r.quadrant.source = QLatin1String(kSourceYahoo);
    r.quadrant.effective = last.date;
    r.quadrant.credibility = grade_credibility(Credibility::Medium, conds, &r.quadrant.credibility_reasons);
    if (frame.size() >= 2)
        r.direction = last.mom >= frame[frame.size() - 2].mom ? QStringLiteral("up") : QStringLiteral("down");
    const int start = std::max(0, static_cast<int>(frame.size()) - tail);
    for (int i = start; i < frame.size(); ++i)
        r.trail.append(frame[i]);
    return r;
}

// ── Momentum composite ───────────────────────────────────────────────────────
struct MomentumParts {
    std::optional<double> m12_1, m6, m3, r1;
    std::optional<double> composite;
    std::optional<double> realized_vol;
};

/// Sector form (reference sector_mom): 0.5·(12M skip 1M) + 0.3·(6M skip 1M) +
/// 0.2·(3M skip 1W) − 0.3·(1M). Country form: the same three components, no
/// reversal term. Every component must exist, or there is no composite.
inline MomentumParts momentum_parts(const TrIndex& t, bool reversal) {
    MomentumParts p;
    const auto& v = t.tr;
    const int n = v.size();
    auto ratio = [&](int a, int b) -> std::optional<double> {
        if (n < b || !(v[n - b] > 0))
            return std::nullopt;
        return v[n - a] / v[n - b] - 1.0;
    };
    p.m12_1 = ratio(22, 253);
    p.m6 = ratio(22, 127);
    p.m3 = ratio(6, 64);
    p.r1 = ratio(1, 22);
    if (p.m12_1 && p.m6 && p.m3 && p.r1) {
        double c = 0.5 * *p.m12_1 + 0.3 * *p.m6 + 0.2 * *p.m3;
        if (reversal)
            c -= 0.3 * *p.r1;
        p.composite = c;
    }
    if (n >= 23) {
        math::Vec rets;
        for (int i = n - 22; i < n; ++i)
            rets.push_back(v[i] / v[i - 1] - 1.0);
        if (const auto sd = math::stdev(rets))
            p.realized_vol = *sd * std::sqrt(252.0);
    }
    return p;
}

/// Above its own 200-session moving average (total-return index).
inline std::optional<bool> above_moving_average(const TrIndex& t, int window = 200) {
    if (t.size() < window)
        return std::nullopt;
    double s = 0;
    for (int i = t.size() - window; i < t.size(); ++i)
        s += t.tr[i];
    return t.tr.last() > s / window;
}

/// Trailing-12-month distributions per share divided by the latest close.
inline std::optional<double> trailing_distribution_yield(const TrIndex& t) {
    if (t.size() < 252)
        return std::nullopt;
    double d = 0;
    for (int i = t.size() - 252; i < t.size(); ++i)
        d += t.distribution[i];
    if (!(t.close.last() > 0))
        return std::nullopt;
    return d / t.close.last() * 100.0;
}

// ── Turnover share (U.S. sector complex) ─────────────────────────────────────
struct TurnoverResult {
    QHash<QString, ResearchValue> share;    ///< percent of complex, window mean
    QHash<QString, ResearchValue> delta_bp; ///< change vs the previous window, bp
    QHash<QString, ResearchValue> z;        ///< z of that change vs trailing year
    ResearchValue tilt_bp;
    ResearchValue tilt_z;
    QString tilt_state;
    int days_used = 0;
    int days_skipped = 0;
};

inline TurnoverResult compute_turnover(const QHash<QString, TrIndex>& idx, const QStringList& members,
                                       const QStringList& defensive, const QStringList& cyclical, int window,
                                       double band_bp, const QVector<CredCondition>& conds) {
    TurnoverResult r;
    // Union of member dates; a day is used only if every member has close and volume.
    QSet<QDate> all;
    for (const auto& m : members)
        for (const QDate& d : idx.value(m).dates)
            all.insert(d);
    QVector<QDate> days(all.begin(), all.end());
    std::sort(days.begin(), days.end());
    QHash<QString, QVector<double>> share;
    QVector<QDate> used;
    for (const QDate& d : days) {
        double total = 0;
        QHash<QString, double> dv;
        bool complete = true;
        for (const auto& m : members) {
            const TrIndex& t = idx[m];
            const int i = t.index_of(d);
            if (i < 0 || !t.volume[i]) {
                complete = false;
                break;
            }
            dv[m] = t.close[i] * *t.volume[i];
            total += dv[m];
        }
        if (!complete || !(total > 0)) {
            ++r.days_skipped;
            continue;
        }
        used.append(d);
        for (const auto& m : members)
            share[m].append(dv[m] / total);
    }
    r.days_used = used.size();
    QVector<CredCondition> c = conds;
    const bool partial_days =
        r.days_skipped > 0 && used.size() > 0 && used.last() != days.last(); // the latest session was incomplete
    if (partial_days)
        c.append(CredCondition::Partial);
    auto rolling = [&](const QVector<double>& s) {
        QVector<double> out;
        for (int i = window - 1; i < s.size(); ++i) {
            double a = 0;
            for (int k = i - window + 1; k <= i; ++k)
                a += s[k];
            out.append(a / window);
        }
        return out;
    };
    auto delta_z = [&](const QVector<double>& rolled, ResearchValue* dbp, ResearchValue* z, const QString& method) {
        const int n = rolled.size();
        if (n < window + 1) {
            *dbp = ResearchValue::unavailable(QStringLiteral("insufficient_history"), method);
            *z = ResearchValue::unavailable(QStringLiteral("insufficient_history"), method);
            return;
        }
        *dbp = graded((rolled[n - 1] - rolled[n - 1 - window]) * 10000.0, EvidenceClass::Proxy, Credibility::Medium, c,
                      QStringLiteral("bp"), method, QLatin1String(kSourceYahoo), used.last());
        math::Vec diffs;
        for (int i = window; i < n; ++i)
            diffs.push_back(rolled[i] - rolled[i - window]);
        // Overlapping window changes are autocorrelated, so the z is descriptive only.
        const auto zz = math::zscore_last(diffs, 252, 60);
        if (!zz) {
            *z = ResearchValue::unavailable(QStringLiteral("insufficient_or_flat_history"), method);
            return;
        }
        QVector<CredCondition> cz = c;
        cz.append(CredCondition::ShortHistory); // overlapping-window z: not an independent-sample statistic
        *z = graded(*zz, EvidenceClass::Proxy, Credibility::Medium, cz, QStringLiteral("z"), method,
                    QLatin1String(kSourceYahoo), used.last());
    };
    for (const auto& m : members) {
        const QVector<double> rolled = rolling(share.value(m));
        if (rolled.isEmpty()) {
            r.share[m] =
                ResearchValue::unavailable(QStringLiteral("insufficient_history"), QLatin1String(kTurnoverMethod));
            r.delta_bp[m] = r.share[m];
            r.z[m] = r.share[m];
            continue;
        }
        r.share[m] = graded(rolled.last() * 100.0, EvidenceClass::Proxy, Credibility::Medium, c, QStringLiteral("pct"),
                            QLatin1String(kTurnoverMethod), QLatin1String(kSourceYahoo), used.last());
        delta_z(rolled, &r.delta_bp[m], &r.z[m], QLatin1String(kTurnoverMethod));
    }
    QVector<double> tilt_series;
    for (int i = 0; i < used.size(); ++i) {
        double d = 0, cy = 0;
        for (const auto& m : defensive)
            d += share.value(m).value(i);
        for (const auto& m : cyclical)
            cy += share.value(m).value(i);
        tilt_series.append(d - cy);
    }
    const QVector<double> tilt_rolled = rolling(tilt_series);
    delta_z(tilt_rolled, &r.tilt_bp, &r.tilt_z, QStringLiteral("etfr_defensive_cyclical_tilt_v1"));
    if (r.tilt_bp.value) {
        const double bp = *r.tilt_bp.value;
        r.tilt_state = bp > band_bp ? QStringLiteral("DEFENSIVE")
                                    : (bp < -band_bp ? QStringLiteral("CYCLICAL") : QStringLiteral("BALANCED"));
    }
    return r;
}

// ── Equal-weight basket (daily rebalanced) ───────────────────────────────────
inline TrIndex equal_weight_basket(const QString& id, const QVector<const TrIndex*>& members, double min_fraction,
                                   int* members_with_data) {
    TrIndex out;
    out.symbol = id;
    int with_data = 0;
    QSet<QDate> all;
    for (const TrIndex* m : members) {
        if (!m || m->size() < 2)
            continue;
        ++with_data;
        for (const QDate& d : m->dates)
            all.insert(d);
    }
    if (members_with_data)
        *members_with_data = with_data;
    QVector<QDate> days(all.begin(), all.end());
    std::sort(days.begin(), days.end());
    const int need = std::max(1, static_cast<int>(std::ceil(min_fraction * members.size())));
    double level = 1.0;
    for (const QDate& d : days) {
        double s = 0;
        int n = 0;
        for (const TrIndex* m : members) {
            if (!m)
                continue;
            const int i = m->index_of(d);
            if (i <= 0)
                continue;
            s += m->tr[i] / m->tr[i - 1] - 1.0;
            ++n;
        }
        if (n < need) {
            if (out.empty())
                continue; // the basket starts once enough members trade
            continue;     // a session without enough members is not a basket session
        }
        level *= 1.0 + s / n;
        out.dates.append(d);
        out.tr.append(level);
        out.close.append(level);
        out.volume.append(std::nullopt);
        out.distribution.append(0.0);
    }
    return out;
}

} // namespace fincept::services::etf::research
