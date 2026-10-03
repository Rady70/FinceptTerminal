// src/services/etf/research/EtfResearchModels.h
//
// Sector and country MODEL layers. Adapted from triphopp/bloomberg-terminal
// @3387072 backend/analytics/{sector_bc,sector_mom,sector_val,sector_factor,
// sector_confluence,country_rotation}.py with these corrections:
//
//  BUSINESS CYCLE
//   * level and direction are separate axes (yield-curve slope, CFNAI activity
//     and HY credit spread give the level; the 3-month CFNAI change gives the
//     direction) and the four phase probabilities are their product, so they
//     sum to one by construction. The reference mapped one combined score onto
//     four ordered labels and called its HIGHEST band "EARLY_RECOVERY";
//   * a missing input is UNAVAILABLE, never a z of 0; the HYG-LQD fallback for
//     the credit spread is z-scored over a year and flagged FALLBACK;
//   * no breadth/VIX "veto"; both are shown as context;
//   * the Stovall (1996) favourability matrix is kept as the documented
//     stylised mapping; its use is graded EXPERIMENTAL (not validated here).
//  VALUATION
//   * the hard-coded P/E means/standard deviations (unsourced) and the 4 %
//     10-year fallback are removed. A sector's P/E is z-scored against
//     MarketLab's own captured P/E history once 20 capture sessions exist;
//     until then the layer uses the cross-sectional earnings-yield spread over
//     the FRED 10-year yield, graded EXPERIMENTAL because sector P/E levels
//     differ structurally.
//  MACRO FACTOR
//   * the static 11x5 beta table (literature-sourced, one column unsourced) is
//     replaced by betas estimated by OLS of weekly sector-minus-SPY total
//     returns on weekly factor changes over three years: Δ 10y-2y slope,
//     Δ 10y breakeven inflation (daily; replaces monthly CPI), Δ HY OAS, log
//     change of the broad dollar index and of WTI (FRED, replacing Yahoo
//     DX-Y.NYB and CL=F). The reference mixed z-scores with raw 20-day returns
//     and its credit factor was always 0 (a z-score of a one-element list).
//  CONFLUENCE
//   * weights and the VIX>35 brake are kept; a missing layer is dropped and the
//     remaining weights renormalised (PARTIAL) only when three layers remain;
//     bands are neutral research labels (no OVERWEIGHT/UNDERWEIGHT posture).
//  COUNTRY
//   * momentum uses true 12-1/6/3-month components; World Bank GDP growth
//     (3 latest years) and current account need both inputs and recent years;
//     Taiwan is UNAVAILABLE (no World Bank series), never 0; carry is the
//     trailing 12-month distribution yield from the same price history
//     (replacing Yahoo dividendYield with a >0.5 unit guess).
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/research/EtfResearchMath.h"
#include "services/etf/research/EtfResearchRotation.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <QHash>
#include <QStringList>

#include <array>
#include <cmath>

namespace fincept::services::etf::research {

inline constexpr const char* kCycleMethod = "etfr_business_cycle_clock_v1";
inline constexpr const char* kValuationMethod = "etfr_sector_valuation_v1";
inline constexpr const char* kFactorMethod = "etfr_macro_factor_ols_v1";
inline constexpr const char* kConfluenceMethod = "etfr_sector_confluence_v1";
inline constexpr const char* kCountryMethod = "etfr_country_composite_v1";
inline constexpr const char* kSourceFred = "fred";
inline constexpr const char* kSourceWorldBank = "world_bank";

/// Stovall (1996) favourability: [recovery, expansion, slowdown, contraction].
inline const QHash<QString, std::array<int, 4>>& stovall_matrix() {
    static const QHash<QString, std::array<int, 4>> m = {
        {QStringLiteral("XLY"), {2, 1, -1, -2}}, {QStringLiteral("XLF"), {2, 1, 0, -1}},
        {QStringLiteral("XLI"), {1, 2, -1, -1}}, {QStringLiteral("XLB"), {1, 1, 2, -2}},
        {QStringLiteral("XLE"), {0, 1, 2, -1}},  {QStringLiteral("XLK"), {1, 2, 0, -1}},
        {QStringLiteral("XLC"), {1, 1, 0, -1}},  {QStringLiteral("XLV"), {-1, 0, 1, 2}},
        {QStringLiteral("XLP"), {-1, -1, 1, 2}}, {QStringLiteral("XLRE"), {1, 0, -1, 1}},
        {QStringLiteral("XLU"), {0, -1, 0, 2}},
    };
    return m;
}

inline const MacroSeries* series_ptr(const QHash<QString, MacroSeries>& h, const QString& id) {
    const auto it = h.constFind(id);
    return it == h.constEnd() ? nullptr : &it.value();
}

inline const MacroSeries* series_ptr(const QHash<QString, MacroSeries>& h, const char* id) {
    return series_ptr(h, QString::fromLatin1(id));
}

/// Values of a FRED series observed at or before `as_of`, ascending.
inline math::Vec macro_values(const MacroSeries* s, const QDate& as_of, QDate* last = nullptr) {
    math::Vec out;
    if (!s)
        return out;
    for (const auto& p : s->points) {
        if (p.date > as_of)
            break;
        out.push_back(p.value);
        if (last)
            *last = p.date;
    }
    return out;
}

inline bool macro_input_stale(const QDate& last, const QDate& as_of, int max_days) {
    return !last.isValid() || last.daysTo(as_of) > max_days;
}

inline double logistic(double x, double k = 2.0) {
    return 1.0 / (1.0 + std::exp(-k * x));
}

inline ResearchValue model_value(std::optional<double> v, Credibility base, const QVector<CredCondition>& conds,
                                 const QString& units, const QString& method, const QString& source, const QDate& eff,
                                 const QString& missing_reason) {
    if (!v)
        return ResearchValue::unavailable(missing_reason, method);
    return graded(*v, EvidenceClass::Model, base, conds, units, method, source, eff);
}

/// Business-cycle clock and per-sector BC layer z-scores.
inline BusinessCycle compute_business_cycle(const QHash<QString, MacroSeries>& fred, const TrIndex* hyg,
                                            const TrIndex* lqd, const QDate& as_of,
                                            QHash<QString, ResearchValue>* sector_bc_z) {
    BusinessCycle bc;
    const QString method = QLatin1String(kCycleMethod);
    QDate d_slope, d_act, d_oas, d_vix;
    const math::Vec slope = macro_values(series_ptr(fred, "T10Y2Y"), as_of, &d_slope);
    const math::Vec act = macro_values(series_ptr(fred, "CFNAI"), as_of, &d_act);
    const math::Vec oas = macro_values(series_ptr(fred, "BAMLH0A0HYM2"), as_of, &d_oas);
    const math::Vec vix = macro_values(series_ptr(fred, "VIXCLS"), as_of, &d_vix);
    const QString fred_src = QLatin1String(kSourceFred);
    auto conds_for = [&](const QDate& last, int max_days) {
        QVector<CredCondition> c{CredCondition::Unvalidated};
        if (macro_input_stale(last, as_of, max_days))
            c.append(CredCondition::MacroStale);
        return c;
    };
    const auto z_slope = math::zscore_last(slope, 252, 200);
    const auto z_act = math::zscore_last(act, 36, 30);
    std::optional<double> z_credit;
    if (const auto z = math::zscore_last(oas, 252, 200))
        z_credit = -*z;
    QDate credit_date = d_oas;
    if (!z_credit && hyg && lqd) {
        // Fallback: HYG minus LQD 20-session total return, z over a year (inverted sense
        // already: HYG outperforming = tighter credit = positive).
        const Aligned al = align(*hyg, *lqd);
        math::Vec spread;
        for (int i = 20; i < al.dates.size(); ++i)
            spread.push_back((al.a[i] / al.a[i - 20]) - (al.b[i] / al.b[i - 20]));
        z_credit = math::zscore_last(spread, 252, 120);
        bc.credit_fallback = z_credit.has_value();
        if (!al.dates.isEmpty())
            credit_date = al.dates.last();
    }
    // Direction: 3-month change of CFNAI, z against the trailing 120 months.
    std::optional<double> z_mom;
    if (act.size() >= 40) {
        math::Vec chg;
        for (size_t i = 3; i < act.size(); ++i)
            chg.push_back(act[i] - act[i - 3]);
        z_mom = math::zscore_last(chg, 120, 36);
    }
    bc.z_slope = model_value(z_slope, Credibility::Medium, conds_for(d_slope, 7), QStringLiteral("z"), method, fred_src,
                             d_slope, QStringLiteral("fred_T10Y2Y_unavailable"));
    bc.z_activity = model_value(z_act, Credibility::Medium, conds_for(d_act, 75), QStringLiteral("z"), method, fred_src,
                                d_act, QStringLiteral("fred_CFNAI_unavailable"));
    {
        QVector<CredCondition> c = conds_for(credit_date, 7);
        if (bc.credit_fallback)
            c.append(CredCondition::Fallback);
        bc.z_credit = model_value(z_credit, Credibility::Medium, c, QStringLiteral("z"), method,
                                  bc.credit_fallback ? QStringLiteral("yahoo_chart(HYG,LQD)") : fred_src, credit_date,
                                  QStringLiteral("credit_spread_unavailable"));
    }
    bc.z_momentum = model_value(z_mom, Credibility::Medium, conds_for(d_act, 75), QStringLiteral("z"), method, fred_src,
                                d_act, QStringLiteral("fred_CFNAI_history_insufficient"));
    if (!vix.empty()) {
        bc.vix = graded(vix.back(), EvidenceClass::Measured, Credibility::NotGraded, {}, QStringLiteral("index"),
                        QStringLiteral("fred_VIXCLS"), fred_src, d_vix);
        bc.vix.credibility = Credibility::NotGraded;
        bc.vix.credibility_reasons.clear();
        if (macro_input_stale(d_vix, as_of, 7))
            bc.vix.add_flag(flag::kStale);
    } else {
        bc.vix = ResearchValue::unavailable(QStringLiteral("fred_VIXCLS_unavailable"));
    }
    if (!z_slope || !z_act || !z_credit || !z_mom) {
        bc.level = ResearchValue::unavailable(QStringLiteral("cycle_inputs_incomplete"), method);
        bc.direction = bc.level;
        bc.phase = bc.level;
        if (sector_bc_z)
            for (auto it = stovall_matrix().begin(); it != stovall_matrix().end(); ++it)
                sector_bc_z->insert(it.key(), bc.level);
        return bc;
    }
    QVector<CredCondition> conds{CredCondition::Unvalidated};
    if (macro_input_stale(d_act, as_of, 75) || macro_input_stale(d_slope, as_of, 7))
        conds.append(CredCondition::MacroStale);
    if (bc.credit_fallback)
        conds.append(CredCondition::Fallback);
    const double level = (0.30 * *z_slope + 0.30 * *z_act + 0.25 * *z_credit) / 0.85;
    const QDate eff = std::max(d_slope, d_act);
    bc.level =
        graded(level, EvidenceClass::Model, Credibility::Medium, conds, QStringLiteral("z"), method, fred_src, eff);
    bc.direction =
        graded(*z_mom, EvidenceClass::Model, Credibility::Medium, conds, QStringLiteral("z"), method, fred_src, d_act);
    const double pl = logistic(level), pd = logistic(*z_mom);
    bc.p_recovery = (1 - pl) * pd;
    bc.p_expansion = pl * pd;
    bc.p_slowdown = pl * (1 - pd);
    bc.p_contraction = (1 - pl) * (1 - pd);
    const std::array<double, 4> p{bc.p_recovery, bc.p_expansion, bc.p_slowdown, bc.p_contraction};
    static const char* kPhase[4] = {"RECOVERY", "EXPANSION", "SLOWDOWN", "CONTRACTION"};
    const int arg = static_cast<int>(std::max_element(p.begin(), p.end()) - p.begin());
    bc.phase = bc.level;
    bc.phase.value = p[arg];
    bc.phase.units = QStringLiteral("probability");
    bc.phase.label = QLatin1String(kPhase[arg]);
    if (sector_bc_z) {
        QStringList keys = stovall_matrix().keys();
        std::sort(keys.begin(), keys.end()); // fixed order: summation order must not vary by process
        std::vector<std::optional<double>> raw;
        for (const QString& key : keys) {
            const auto& f = stovall_matrix()[key];
            raw.push_back(p[0] * f[0] + p[1] * f[1] + p[2] * f[2] + p[3] * f[3]);
        }
        const auto z = math::cross_section_z(raw, 2.5);
        for (int i = 0; i < keys.size(); ++i)
            sector_bc_z->insert(keys[i],
                                model_value(z[static_cast<size_t>(i)], Credibility::Medium, conds, QStringLiteral("z"),
                                            method, fred_src, eff, QStringLiteral("cross_section_undefined")));
    }
    return bc;
}

/// Weekly (Friday-ending) last observation of a daily series up to as_of.
inline QHash<QDate, double> weekly_last(const MacroSeries* s, const QDate& as_of) {
    QHash<QDate, double> out;
    if (!s)
        return out;
    for (const auto& p : s->points) {
        if (p.date > as_of)
            break;
        out[week_ending_friday(p.date)] = p.value;
    }
    return out;
}

inline QHash<QDate, double> weekly_last_index(const TrIndex& t) {
    QHash<QDate, double> out;
    for (int i = 0; i < t.size(); ++i)
        out[week_ending_friday(t.dates[i])] = t.tr[i];
    return out;
}

struct FactorModelResult {
    QHash<QString, ResearchValue> sector_z;
    QHash<QString, QVector<double>> betas;
    QHash<QString, QVector<double>> tstats;
    QHash<QString, double> r2;
    QVector<ResearchValue> factor_state;
    QStringList factor_names;
};

/// Macro-factor layer: OLS sensitivities of weekly sector-minus-SPY returns to
/// weekly factor changes (156 weeks), applied to the latest 4-week factor change.
inline FactorModelResult compute_factor_model(const QHash<QString, MacroSeries>& fred,
                                              const QHash<QString, TrIndex>& sectors, const TrIndex& spy,
                                              const QDate& as_of) {
    FactorModelResult r;
    const QString method = QLatin1String(kFactorMethod);
    r.factor_names = {QStringLiteral("T10Y2Y"), QStringLiteral("T10YIE"), QStringLiteral("BAMLH0A0HYM2"),
                      QStringLiteral("DTWEXBGS"), QStringLiteral("DCOILWTICO")};
    const bool log_change[5] = {false, false, false, true, true};
    QVector<QHash<QDate, double>> fw;
    QDate latest_factor;
    bool stale = false;
    for (int f = 0; f < 5; ++f) {
        const MacroSeries* s = series_ptr(fred, r.factor_names[f]);
        fw.append(weekly_last(s, as_of));
        QDate last;
        macro_values(s, as_of, &last);
        if (macro_input_stale(last, as_of, 14))
            stale = true;
        if (last.isValid() && (!latest_factor.isValid() || last > latest_factor))
            latest_factor = last;
    }
    const QHash<QDate, double> spy_w = weekly_last_index(spy);
    // Weeks with every factor and SPY, ascending.
    QVector<QDate> weeks;
    for (auto it = spy_w.begin(); it != spy_w.end(); ++it) {
        bool all = true;
        for (const auto& h : fw)
            if (!h.contains(it.key()))
                all = false;
        if (all)
            weeks.append(it.key());
    }
    std::sort(weeks.begin(), weeks.end());
    auto change = [&](int f, const QDate& a, const QDate& b) {
        const double x0 = fw[f].value(a), x1 = fw[f].value(b);
        return log_change[f] ? std::log(x1 / x0) : x1 - x0;
    };
    QVector<CredCondition> conds{CredCondition::Unvalidated, CredCondition::InSampleModel};
    if (stale)
        conds.append(CredCondition::MacroStale);
    if (weeks.size() < 60) {
        for (auto it = sectors.begin(); it != sectors.end(); ++it)
            r.sector_z[it.key()] = ResearchValue::unavailable(QStringLiteral("factor_history_insufficient"), method);
        for (int f = 0; f < 5; ++f)
            r.factor_state.append(ResearchValue::unavailable(QStringLiteral("factor_history_insufficient"), method));
        return r;
    }
    // Latest 4-week factor change and its z against trailing 3-year 4-week changes.
    math::Vec latest_change(5, 0.0);
    for (int f = 0; f < 5; ++f) {
        math::Vec hist;
        for (int i = 4; i < weeks.size(); ++i)
            hist.push_back(change(f, weeks[i - 4], weeks[i]));
        latest_change[static_cast<size_t>(f)] = hist.back();
        const auto z = math::zscore_last(hist, 156, 52);
        r.factor_state.append(model_value(
            z, Credibility::Medium,
            stale ? QVector<CredCondition>{CredCondition::MacroStale} : QVector<CredCondition>{}, QStringLiteral("z"),
            method, QLatin1String(kSourceFred), latest_factor, QStringLiteral("factor_history_insufficient")));
    }
    QStringList keys;
    std::vector<std::optional<double>> raw;
    const int start = std::max(1, static_cast<int>(weeks.size()) - 156);
    QStringList sector_keys = sectors.keys();
    std::sort(sector_keys.begin(), sector_keys.end());
    for (const QString& key : sector_keys) {
        const QHash<QDate, double> sw = weekly_last_index(sectors[key]);
        math::Mat x;
        math::Vec y;
        for (int i = start; i < weeks.size(); ++i) {
            const QDate a = weeks[i - 1], b = weeks[i];
            if (!sw.contains(a) || !sw.contains(b))
                continue;
            const double ry = (sw[b] / sw[a] - 1.0) - (spy_w[b] / spy_w[a] - 1.0);
            math::Vec row;
            for (int f = 0; f < 5; ++f)
                row.push_back(change(f, a, b));
            x.push_back(row);
            y.push_back(ry);
        }
        keys.append(key);
        const auto fit = math::ols(x, y);
        if (!fit || fit->n < 100) {
            raw.push_back(std::nullopt);
            continue;
        }
        QVector<double> b, t;
        double score = 0.0;
        for (int f = 0; f < 5; ++f) {
            b.append(fit->beta[static_cast<size_t>(f + 1)]);
            t.append(fit->t_stats[static_cast<size_t>(f + 1)]);
            score += fit->beta[static_cast<size_t>(f + 1)] * latest_change[static_cast<size_t>(f)];
        }
        r.betas[key] = b;
        r.tstats[key] = t;
        r.r2[key] = fit->r2;
        raw.push_back(score);
    }
    const auto z = math::cross_section_z(raw, 2.5);
    for (int i = 0; i < keys.size(); ++i)
        r.sector_z[keys[i]] = model_value(z[static_cast<size_t>(i)], Credibility::Low, conds, QStringLiteral("z"),
                                          method, QStringLiteral("fred+yahoo_chart"), latest_factor,
                                          QStringLiteral("factor_regression_unavailable"));
    return r;
}

inline QString model_band(double score) {
    if (score >= 0.60)
        return QStringLiteral("HIGH");
    if (score >= 0.25)
        return QStringLiteral("ABOVE");
    if (score >= -0.25)
        return QStringLiteral("NEUTRAL");
    if (score >= -0.60)
        return QStringLiteral("BELOW");
    return QStringLiteral("LOW");
}

/// Confluence over the four layers with renormalised weights; needs three.
inline ResearchValue confluence_score(const QVector<std::pair<QString, ResearchValue>>& layers,
                                      const QHash<QString, double>& weights, double* dispersion) {
    double wsum = 0, s = 0;
    int n = 0;
    math::Vec zs;
    QVector<CredCondition> conds{CredCondition::Unvalidated};
    for (const auto& [name, v] : layers) {
        if (!v.value)
            continue;
        wsum += weights.value(name);
        s += weights.value(name) * *v.value;
        zs.push_back(*v.value);
        ++n;
        if (v.has_flag(flag::kStale))
            conds.append(CredCondition::Stale);
    }
    if (dispersion)
        *dispersion = n >= 2 ? *math::stdev(zs) : 0.0;
    if (n < 3 || !(wsum > 0))
        return ResearchValue::unavailable(QStringLiteral("fewer_than_three_layers"), QLatin1String(kConfluenceMethod));
    if (n < layers.size())
        conds.append(CredCondition::Partial);
    return graded(s / wsum, EvidenceClass::Model, Credibility::Low, conds, QStringLiteral("z"),
                  QLatin1String(kConfluenceMethod), QStringLiteral("model_layers"), QDate());
}

} // namespace fincept::services::etf::research
