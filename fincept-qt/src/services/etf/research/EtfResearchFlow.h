// src/services/etf/research/EtfResearchFlow.h
//
// ESTIMATED daily ETF creation/redemption from MarketLab's own manual captures
// of Yahoo's fund snapshot (total assets, NAV, reported shares). Adapted from
// triphopp/bloomberg-terminal@3387072 backend/etf_aum.py, corrected:
//
//  * The reference's two "independent" estimators are algebraically the SAME
//    number: with implied shares s = A/N, (s1 - s0)·N1 = A1 - A0·(N1/N0) for any
//    inputs, so they can never disagree and their agreement is no evidence.
//    MarketLab keeps that quantity once (E1, implied-shares) and compares it with
//    estimators that use different inputs:
//      E2 = Δ(reported shares outstanding) × NAV_cur  (a separate Yahoo field)
//      E3 = AUM_cur − AUM_prev × close_cur/close_prev (market close, not NAV)
//    Disagreement between E1 and E2 beyond tolerance is shown, not averaged.
//  * A return-adjusted AUM flow must use the NAV PRICE return: AUM already falls
//    by the distribution on the ex-date, so a total-return adjustment (as the
//    reference's docstring describes) would book every distribution as an
//    inflow of shares × distribution.
//  * Yahoo does not date AUM or NAV. Each capture is assigned the last NYSE
//    session completed before its New York calendar date (rule
//    prior_completed_session_v1) and flagged ASSUMED_EFFECTIVE_DATE; the NAV is
//    checked against that session's close and a > 0.5 % gap lowers credibility.
//  * Captures several sessions apart give ONE interval estimate, never a value
//    smeared over the missing days; a missed capture stays a gap.
//  * An interval containing a split has no E1 or E2. Where AUM did not change
//    while NAV did (Yahoo re-served a stale snapshot) no estimator is computed,
//    and that capture never anchors the next interval: the next fresh capture
//    spans the whole gap from the last fresh one.
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/EtfSessionCalendar.h"
#include "services/etf/research/EtfResearchInputs.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <cmath>

namespace fincept::services::etf::research {

inline constexpr const char* kFlowMethod = "etfr_estimated_flow_v1";
inline constexpr const char* kSourceYahooFund = "yahoo_fund_snapshot";
inline constexpr double kFlowAgreementTolerance = 0.002; ///< of prior AUM
inline constexpr double kNavDateTolerance = 0.005;       ///< NAV vs assumed-session close

/// Sessions in (from, to] on the NYSE calendar; -1 when outside coverage.
inline int nyse_sessions_between(const QDate& from, const QDate& to) {
    bool complete = false;
    if (!from.isValid() || !to.isValid() || to <= from)
        return to == from ? 0 : -1;
    const auto days = UsEquityCalendar::weekdays_in(from.addDays(1), to, &complete);
    if (!complete)
        return -1;
    int n = 0;
    for (const auto& d : days)
        if (d.is_session())
            ++n;
    return n;
}

/// The latest capture of each effective session, ascending.
inline QVector<FundCapture> captures_by_session(const QVector<FundCapture>& caps, int* duplicates) {
    QVector<FundCapture> out;
    int dup = 0;
    for (const FundCapture& c : caps) {
        if (!c.effective_session.isValid())
            continue;
        if (!out.isEmpty() && out.last().effective_session == c.effective_session) {
            ++dup;
            if (c.captured_at >= out.last().captured_at)
                out.last() = c;
            continue;
        }
        out.append(c);
    }
    std::sort(out.begin(), out.end(),
              [](const FundCapture& a, const FundCapture& b) { return a.effective_session < b.effective_session; });
    if (duplicates)
        *duplicates = dup;
    return out;
}

inline std::optional<double> close_on(const BarSeries& bars, const QDate& d) {
    for (int i = bars.bars.size() - 1; i >= 0; --i) {
        if (bars.bars[i].date == d)
            return bars.bars[i].close;
        if (bars.bars[i].date < d)
            break;
    }
    return std::nullopt;
}

inline bool split_between(const BarSeries& bars, const QDate& from, const QDate& to) {
    for (const auto& b : bars.bars)
        if (b.date > from && b.date <= to && b.split > 0.0)
            return true;
    return false;
}

inline FlowInterval flow_interval(const FundCapture& p, const FundCapture& c, const BarSeries& bars) {
    FlowInterval f;
    f.from = p.effective_session;
    f.to = c.effective_session;
    f.sessions = nyse_sessions_between(f.from, f.to);
    f.aum_prev = p.total_assets;
    f.aum_cur = c.total_assets;
    f.nav_prev = p.nav;
    f.nav_cur = c.nav;
    if (f.sessions > 1)
        f.flags.append(QLatin1String(flag::kGap));
    if (f.sessions < 0)
        f.flags.append(QStringLiteral("CALENDAR_NOT_COVERED"));
    const bool split = split_between(bars, f.from, f.to);
    if (split)
        f.flags.append(QStringLiteral("SPLIT_IN_INTERVAL"));
    const bool have_an = f.aum_prev && f.aum_cur && f.nav_prev && f.nav_cur && *f.nav_prev > 0 && *f.nav_cur > 0;
    if (have_an) {
        f.implied_shares_prev = *f.aum_prev / *f.nav_prev;
        f.implied_shares_cur = *f.aum_cur / *f.nav_cur;
    }
    if (split) {
        f.reason = QStringLiteral("split_in_interval");
    } else if (!have_an) {
        f.reason = QStringLiteral("aum_or_nav_missing");
    } else if (*f.aum_cur == *f.aum_prev && *f.nav_cur != *f.nav_prev) {
        // The provider re-served the previous AUM: the capture does not describe
        // this session, so no estimator may use it (E2's share count and E3's AUM
        // come from the same stale snapshot; E3 would book -AUM x return as flow).
        f.reason = QStringLiteral("aum_not_updated");
        f.flags.append(QStringLiteral("AUM_UNCHANGED_NAV_CHANGED"));
        return f;
    } else {
        f.e1_implied_shares = (*f.implied_shares_cur - *f.implied_shares_prev) * *f.nav_cur;
        if (*f.nav_cur == *f.nav_prev)
            f.flags.append(QStringLiteral("NAV_UNCHANGED"));
    }
    if (!split && p.shares_outstanding && c.shares_outstanding && f.nav_cur) {
        f.e2_reported_shares = (*c.shares_outstanding - *p.shares_outstanding) * *f.nav_cur;
        if (*c.shares_outstanding == *p.shares_outstanding)
            f.flags.append(QStringLiteral("REPORTED_SHARES_UNCHANGED"));
    }
    const auto c0 = close_on(bars, f.from);
    const auto c1 = close_on(bars, f.to);
    if (!split && f.aum_prev && f.aum_cur && c0 && c1 && *c0 > 0)
        f.e3_price_adjusted = *f.aum_cur - *f.aum_prev * (*c1 / *c0);
    return f;
}

/// Estimate flow for one fund. `expected_session` is the latest NYSE session
/// completed at the frame's decision time.
inline EstimatedFlow estimate_flow(const QVector<FundCapture>& raw_captures, const BarSeries& bars,
                                   const QVector<MeasuredMonth>& measured, const QDate& expected_session) {
    EstimatedFlow e;
    int dup = 0;
    const QVector<FundCapture> caps = captures_by_session(raw_captures, &dup);
    e.captures = raw_captures.size();
    e.capture_sessions = caps.size();
    const QString method = QLatin1String(kFlowMethod);
    auto unavailable_all = [&](const QString& why) {
        e.latest = ResearchValue::unavailable(why, method);
        e.latest_e2 = e.latest;
        e.latest_e3 = e.latest;
        e.sum_5 = e.latest;
        e.sum_20 = e.latest;
        e.pct_aum_20 = e.latest;
        e.coverage_20 = e.latest;
        e.validation_vs_measured = e.latest;
    };
    if (caps.isEmpty()) {
        unavailable_all(QStringLiteral("no_capture"));
        e.latest.add_flag(flag::kNoObservation);
        return e;
    }
    e.first_session = caps.first().effective_session;
    e.last_session = caps.last().effective_session;
    if (caps.size() < 2) {
        unavailable_all(QStringLiteral("record_too_young_one_capture_session"));
        return e;
    }
    // Intervals run between fresh captures. A capture whose AUM was re-served
    // unchanged (NAV moved) is recorded as an unusable interval and does not become
    // the next anchor: the following fresh capture spans the whole gap instead of
    // pairing with a stale AUM.
    int anchor = 0;
    for (int i = 1; i < caps.size(); ++i) {
        FlowInterval f = flow_interval(caps[anchor], caps[i], bars);
        const bool stale_aum = f.reason == QLatin1String("aum_not_updated");
        e.intervals.append(std::move(f));
        if (!stale_aum)
            anchor = i;
    }

    const FlowInterval& last = e.intervals.last();
    const FundCapture& lc = caps.last();
    QVector<CredCondition> conds;
    // Corroboration by the independent reported-shares field.
    bool corroborated = false;
    if (last.e1_implied_shares && last.e2_reported_shares && last.aum_prev) {
        const double tol = kFlowAgreementTolerance * std::abs(*last.aum_prev);
        if (std::abs(*last.e1_implied_shares - *last.e2_reported_shares) <= tol) {
            corroborated = true;
            e.agreement = QStringLiteral("agree");
        } else {
            e.agreement = QStringLiteral("disagree");
            conds.append(CredCondition::EstimatorDisagreement);
        }
    } else {
        e.agreement = QStringLiteral("uncorroborated");
    }
    // NAV date check against the assumed session's close.
    const auto c = close_on(bars, lc.effective_session);
    if (!c || !lc.nav || std::abs(*lc.nav / *c - 1.0) > kNavDateTolerance)
        conds.append(CredCondition::SourceTimingAmbiguous);
    if (expected_session.isValid()) {
        const int lag = nyse_sessions_between(lc.effective_session, expected_session);
        if (lag < 0 || lag > 1)
            conds.append(CredCondition::Stale);
    }
    const Credibility base = corroborated ? Credibility::Medium : Credibility::Low;
    if (last.e1_implied_shares) {
        e.latest = graded(*last.e1_implied_shares, EvidenceClass::Estimated, base, conds, QStringLiteral("usd"),
                          method + QStringLiteral(":E1_implied_shares"), QLatin1String(kSourceYahooFund), last.to);
        e.latest.add_flag(flag::kAssumedDate);
        if (last.sessions > 1)
            e.latest.add_flag(flag::kGap);
    } else {
        e.latest = ResearchValue::unavailable(last.reason, method);
    }
    auto side = [&](const std::optional<double>& v, const QString& tag, const QString& why) {
        if (!v)
            return ResearchValue::unavailable(why, method + tag);
        ResearchValue r = graded(*v, EvidenceClass::Estimated, Credibility::Low, {CredCondition::SourceTimingAmbiguous},
                                 QStringLiteral("usd"), method + tag, QLatin1String(kSourceYahooFund), last.to);
        return r;
    };
    e.latest_e2 = side(last.e2_reported_shares, QStringLiteral(":E2_reported_shares"),
                       QStringLiteral("reported_shares_missing_or_split"));
    e.latest_e3 =
        side(last.e3_price_adjusted, QStringLiteral(":E3_close_adjusted_aum"), QStringLiteral("close_or_aum_missing"));

    // Windows over the last N NYSE sessions ending at the expected session.
    auto window = [&](int n, ResearchValue* sum, ResearchValue* coverage, ResearchValue* pct) {
        const QDate end = expected_session.isValid() ? expected_session : e.last_session;
        QDate start = end;
        for (int k = 0; k < n; ++k) {
            const auto prev = UsEquityCalendar::last_session_before(start);
            if (!prev)
                break;
            start = prev->date; // the window is (start, end]
        }
        double s = 0;
        int covered = 0;
        int used = 0;
        std::optional<double> aum0;
        bool any_flag_gap = false;
        for (const FlowInterval& f : e.intervals) {
            if (f.from < start || f.to > end || !f.e1_implied_shares)
                continue;
            if (!aum0)
                aum0 = f.aum_prev;
            s += *f.e1_implied_shares;
            covered += std::max(0, f.sessions);
            ++used;
            if (f.sessions > 1)
                any_flag_gap = true;
        }
        if (!used) {
            *sum = ResearchValue::unavailable(QStringLiteral("no_captured_interval_in_window"), method);
            *coverage = ResearchValue::unavailable(QStringLiteral("no_captured_interval_in_window"), method);
            if (pct)
                *pct = *sum;
            return;
        }
        QVector<CredCondition> wc = conds;
        if (covered < n)
            wc.append(CredCondition::Partial);
        *sum = graded(s, EvidenceClass::Estimated, base, wc, QStringLiteral("usd"), method,
                      QLatin1String(kSourceYahooFund), end);
        sum->add_flag(flag::kAssumedDate);
        if (any_flag_gap)
            sum->add_flag(flag::kGap);
        *coverage = graded(static_cast<double>(covered) / n * 100.0, EvidenceClass::Estimated, base, {},
                           QStringLiteral("pct"), method, QLatin1String(kSourceYahooFund), end);
        if (pct) {
            if (aum0 && *aum0 > 0) {
                *pct = graded(s / *aum0 * 100.0, EvidenceClass::Estimated, base, wc, QStringLiteral("pct"), method,
                              QLatin1String(kSourceYahooFund), end);
            } else {
                *pct = ResearchValue::unavailable(QStringLiteral("prior_aum_missing"), method);
            }
        }
    };
    ResearchValue cov5;
    window(5, &e.sum_5, &cov5, nullptr);
    window(20, &e.sum_20, &e.coverage_20, &e.pct_aum_20);

    // Validation: a calendar month fully covered by E1 intervals vs SEC measured flow.
    e.validation_vs_measured = ResearchValue::unavailable(QStringLiteral("no_fully_covered_measured_month"), method);
    for (int m = measured.size() - 1; m >= 0; --m) {
        const MeasuredMonth& mm = measured[m];
        if (!mm.flow_usd)
            continue;
        const QDate first = mm.month;
        const QDate lastd = mm.month.addMonths(1).addDays(-1);
        const auto before = UsEquityCalendar::last_session_before(first);
        const auto last_session = UsEquityCalendar::last_session_before(lastd.addDays(1));
        if (!before || !last_session)
            continue;
        double s = 0;
        int covered = 0;
        const int need = nyse_sessions_between(before->date, last_session->date);
        for (const FlowInterval& f : e.intervals)
            if (f.from >= before->date && f.to <= last_session->date && f.e1_implied_shares) {
                s += *f.e1_implied_shares;
                covered += f.sessions;
            }
        if (need > 0 && covered == need) {
            e.validation_vs_measured = graded(s - *mm.flow_usd, EvidenceClass::Estimated, Credibility::Medium, {},
                                              QStringLiteral("usd"), method + QStringLiteral(":E1_minus_sec_nport"),
                                              QStringLiteral("yahoo_fund_snapshot+sec_nport"), lastd);
            break;
        }
    }
    return e;
}

} // namespace fincept::services::etf::research
