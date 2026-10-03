// src/services/etf/research/EtfResearchRegime.h
//
// Sector co-movement / regime MODEL views. Adapted from
// triphopp/bloomberg-terminal@3387072 backend/routers/regime.py,
// backend/analytics/regime_calibration.py and backend/analytics/regime_v2.py.
//
// Retained as descriptive geometry: correlation matrix, wedge magnitude
// |sin θ| = sqrt(1-ρ²), PCA projection (√λ·v), Gram-determinant score
// det(C)^(1/N), the Marchenko-Pastur bound (1+sqrt(N/T))² and k_signal.
// Corrections:
//  * MRS thresholds (midpoints of a 4-state Gaussian HMM's sorted means, fitted
//    on rolling 63-session mean |ρ|) are applied ONLY to the 63-session view;
//    the reference applied them to 21-, 126- and 252-session windows too,
//    whose |ρ| distributions differ. Other windows use the fixed heuristic
//    thresholds and say so (capped LOW).
//  * the reference's "known regime" spot-check labelled its expected answers
//    from the same thresholds it tested, so it is not kept as validation;
//    threshold stability across expanding refits is reported instead.
//  * the 6-feature HMM uses FRED VIXCLS instead of Yahoo ^VIX, a deterministic
//    initialisation (hmmlearn's random k-means init is not reproducible), and
//    is graded EXPERIMENTAL: its parameters see the whole sample (in-sample)
//    and MarketLab has not reproduced the reference's walk-forward study. The
//    reference's posture/headline text ("Reduce risk…") is not carried.
//  * trend labels say what moved — CORRELATION RISING/FALLING/STABLE — instead
//    of CONTRACTING/EXPANDING.
//
// Header-only over Qt Core.
#pragma once
#include "services/etf/research/EtfResearchMath.h"
#include "services/etf/research/EtfResearchModels.h"
#include "services/etf/research/EtfResearchRotation.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <cmath>

namespace fincept::services::etf::research {

inline constexpr const char* kRegimeMethod = "etfr_sector_regime_geometry_v1";
inline constexpr const char* kMrsMethod = "etfr_mrs_hmm4_avgabsrho63_v1";
inline constexpr const char* kRegimeV2Method = "etfr_regime_hmm6_v1";

/// Daily total returns of `series` on their common sessions; the last `n`
/// returns (all when n <= 0). `dates_out` receives the return dates.
inline std::vector<math::Vec> common_returns(const QVector<const TrIndex*>& series, int n, QVector<QDate>* dates_out,
                                             bool weekly = false) {
    std::vector<math::Vec> cols(static_cast<size_t>(series.size()));
    if (series.isEmpty())
        return cols;
    QVector<QDate> common = series[0]->dates;
    for (int s = 1; s < series.size(); ++s) {
        QVector<QDate> next;
        const auto& d = series[s]->dates;
        std::set_intersection(common.begin(), common.end(), d.begin(), d.end(), std::back_inserter(next));
        common = next;
    }
    if (weekly) {
        QVector<QDate> w;
        for (const QDate& d : common) {
            if (!w.isEmpty() && week_ending_friday(w.last()) == week_ending_friday(d))
                w.last() = d;
            else
                w.append(d);
        }
        common = w;
    }
    const int total = static_cast<int>(common.size()) - 1;
    if (total < 1)
        return cols;
    const int take = n > 0 ? std::min(n, total) : total;
    const int first = static_cast<int>(common.size()) - take;
    QVector<QDate> dates;
    for (int i = first; i < common.size(); ++i)
        dates.append(common[i]);
    for (int s = 0; s < series.size(); ++s) {
        const TrIndex* t = series[s];
        auto& col = cols[static_cast<size_t>(s)];
        for (int i = first; i < common.size(); ++i) {
            const int a = t->index_of(common[i - 1]);
            const int b = t->index_of(common[i]);
            col.push_back(t->tr[b] / t->tr[a] - 1.0);
        }
    }
    if (dates_out)
        *dates_out = dates;
    return cols;
}

inline double mean_abs_offdiag(const math::Mat& c) {
    double s = 0;
    int n = 0;
    for (size_t i = 0; i < c.size(); ++i)
        for (size_t j = i + 1; j < c.size(); ++j) {
            s += std::abs(c[i][j]);
            ++n;
        }
    return n ? s / n : 0.0;
}

inline QString heuristic_corr_label(double avg) {
    if (avg > 0.70)
        return QStringLiteral("CRISIS");
    if (avg > 0.55)
        return QStringLiteral("RISK-OFF");
    if (avg > 0.40)
        return QStringLiteral("TRENDING");
    return QStringLiteral("DIVERGENT");
}

inline QString threshold_corr_label(double avg, const QVector<double>& t) {
    if (avg > t[2])
        return QStringLiteral("CRISIS");
    if (avg > t[1])
        return QStringLiteral("RISK-OFF");
    if (avg > t[0])
        return QStringLiteral("TRENDING");
    return QStringLiteral("DIVERGENT");
}

inline QString geom_label(double score) {
    if (score < 0.25)
        return QStringLiteral("CORRELATED");
    if (score < 0.45)
        return QStringLiteral("TRENDING");
    if (score < 0.65)
        return QStringLiteral("MIXED");
    return QStringLiteral("DIVERGENT");
}

inline QString factor_regime(int k, double lambda1, int n) {
    if (k == 0)
        return QStringLiteral("NOISE");
    const double dom = lambda1 / n;
    if (k == 1)
        return dom > 0.75
                   ? QStringLiteral("SINGLE-FACTOR-DOMINANT")
                   : (dom > 0.45 ? QStringLiteral("SINGLE-FACTOR-MODERATE") : QStringLiteral("SINGLE-FACTOR-WEAK"));
    return k <= 3 ? QStringLiteral("MULTI-FACTOR") : QStringLiteral("MANY-FACTORS");
}

inline ResearchValue model_label(const QString& label, double value, Credibility base,
                                 const QVector<CredCondition>& conds, const QString& method, const QDate& eff) {
    ResearchValue v = graded(value, EvidenceClass::Model, base, conds, QStringLiteral("score"), method,
                             QLatin1String(kSourceYahoo), eff);
    v.label = label;
    v.flags.removeAll(QLatin1String(flag::kActualZero));
    return v;
}

inline CorrelationView correlation_view(const QString& period, const QVector<const TrIndex*>& sectors, int n,
                                        bool weekly, const QVector<double>* mrs_thresholds) {
    CorrelationView v;
    v.period = period;
    QVector<QDate> dates;
    const auto cols = common_returns(sectors, n, &dates, weekly);
    for (const TrIndex* t : sectors)
        v.symbols.append(t->symbol);
    v.observations = cols.empty() ? 0 : static_cast<int>(cols[0].size());
    if (v.observations < 10 || sectors.size() < 3) {
        v.avg_abs_corr = ResearchValue::unavailable(QStringLiteral("insufficient_common_history"), kRegimeMethod);
        v.corr_label = v.avg_abs_corr;
        v.geom_score = v.avg_abs_corr;
        v.geom_label = v.avg_abs_corr;
        return v;
    }
    const math::Mat c = math::correlation_matrix(cols);
    if (c.empty()) {
        v.avg_abs_corr = ResearchValue::unavailable(QStringLiteral("flat_return_series"), kRegimeMethod);
        v.corr_label = v.avg_abs_corr;
        v.geom_score = v.avg_abs_corr;
        v.geom_label = v.avg_abs_corr;
        return v;
    }
    const int N = static_cast<int>(c.size());
    for (int i = 0; i < N; ++i) {
        QVector<double> row, wrow;
        for (int j = 0; j < N; ++j) {
            row.append(c[i][j]);
            wrow.append(std::sqrt(std::max(0.0, 1.0 - c[i][j] * c[i][j])));
        }
        v.corr.append(row);
        v.wedge.append(wrow);
    }
    const math::Eigen e = math::jacobi_eigen(c);
    double pos = 0;
    for (double l : e.values)
        if (l > 0)
            pos += l;
    v.var_pc1 = pos > 0 ? e.values[0] / pos : 0;
    v.var_pc2 = pos > 0 && N > 1 ? e.values[1] / pos : 0;
    double scale = 1e-9;
    QVector<QPair<double, double>> raw;
    for (int i = 0; i < N; ++i) {
        const double x = e.vectors[i][0] * std::sqrt(std::max(0.0, e.values[0]));
        const double y = N > 1 ? e.vectors[i][1] * std::sqrt(std::max(0.0, e.values[1])) : 0.0;
        raw.append({x, y});
        scale = std::max({scale, std::abs(x), std::abs(y)});
    }
    for (const auto& p : raw)
        v.pca.append({p.first / scale, p.second / scale});
    const QDate eff = dates.isEmpty() ? QDate() : dates.last();
    const double avg = mean_abs_offdiag(c);
    const bool use_mrs = mrs_thresholds && mrs_thresholds->size() == 3 && n == 63 && !weekly;
    v.calibration = use_mrs ? QStringLiteral("MRS") : QStringLiteral("heuristic");
    v.avg_abs_corr = graded(avg, EvidenceClass::Model, Credibility::High, {}, QStringLiteral("score"),
                            QLatin1String(kRegimeMethod), QLatin1String(kSourceYahoo), eff);
    v.avg_abs_corr.flags.removeAll(QLatin1String(flag::kActualZero));
    v.corr_label = model_label(use_mrs ? threshold_corr_label(avg, *mrs_thresholds) : heuristic_corr_label(avg), avg,
                               Credibility::Medium,
                               use_mrs ? QVector<CredCondition>{CredCondition::InSampleModel}
                                       : QVector<CredCondition>{CredCondition::HeuristicThreshold},
                               use_mrs ? QLatin1String(kMrsMethod) : QLatin1String(kRegimeMethod), eff);
    const auto ld = math::log_det_spd(c);
    v.lambda_max = std::pow(1.0 + std::sqrt(static_cast<double>(N) / v.observations), 2.0);
    v.lambda_1 = e.values[0];
    for (double l : e.values)
        if (l > v.lambda_max)
            ++v.k_signal;
    v.factor_regime = factor_regime(v.k_signal, v.lambda_1, N);
    if (ld) {
        const double score = std::exp(*ld / N);
        v.geom_score = graded(score, EvidenceClass::Model, Credibility::High, {}, QStringLiteral("score"),
                              QLatin1String(kRegimeMethod), QLatin1String(kSourceYahoo), eff);
        v.geom_label = model_label(geom_label(score), score, Credibility::Medium, {CredCondition::HeuristicThreshold},
                                   QLatin1String(kRegimeMethod), eff);
    } else {
        v.geom_score = ResearchValue::unavailable(QStringLiteral("singular_correlation_matrix"), kRegimeMethod);
        v.geom_label = v.geom_score;
    }
    return v;
}

/// Rolling 63-session mean |ρ| over the common history of `sectors`.
inline QVector<QPair<QDate, double>> rolling_avg_abs_corr(const QVector<const TrIndex*>& sectors, int window = 63) {
    QVector<QDate> dates;
    const auto cols = common_returns(sectors, 0, &dates);
    QVector<QPair<QDate, double>> out;
    if (cols.empty())
        return out;
    const int T = static_cast<int>(cols[0].size());
    for (int t = window; t <= T; ++t) {
        std::vector<math::Vec> w;
        for (const auto& c : cols)
            w.emplace_back(c.begin() + (t - window), c.begin() + t);
        const math::Mat m = math::correlation_matrix(w);
        if (m.empty())
            continue;
        out.append({dates[t - 1], mean_abs_offdiag(m)});
    }
    return out;
}

struct MrsFit {
    bool ready = false;
    QVector<double> thresholds;
    QVector<double> means;
    QVector<double> threshold_std;
    std::optional<math::GaussianHmm> hmm;
};

inline QVector<double> thresholds_from(const math::GaussianHmm& h) {
    return {(h.means[0][0] + h.means[1][0]) / 2, (h.means[1][0] + h.means[2][0]) / 2,
            (h.means[2][0] + h.means[3][0]) / 2};
}

/// 4-state HMM on the score series; threshold stability over up to eight
/// expanding refits (train [0, t), t stepping 252 observations).
inline MrsFit fit_mrs(const QVector<QPair<QDate, double>>& scores) {
    MrsFit f;
    math::Mat x;
    for (const auto& p : scores)
        x.push_back({p.second});
    if (x.size() < 600)
        return f;
    f.hmm = math::hmm_fit(x, 4, 0, 300, 1e-5, 1e-6);
    if (!f.hmm)
        return f;
    f.thresholds = thresholds_from(*f.hmm);
    for (const auto& m : f.hmm->means)
        f.means.append(m[0]);
    f.ready = true;
    QVector<QVector<double>> folds;
    const size_t min_train = std::max<size_t>(500, x.size() * 4 / 10);
    for (size_t t = min_train; t + 252 <= x.size() && folds.size() < 8; t += 252) {
        const math::Mat train(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(t));
        if (const auto h = math::hmm_fit(train, 4, 0, 300, 1e-5, 1e-6))
            folds.append(thresholds_from(*h));
    }
    for (int k = 0; k < 3; ++k) {
        math::Vec v;
        for (const auto& fo : folds)
            v.push_back(fo[k]);
        const auto sd = math::stdev(v);
        f.threshold_std.append(sd ? *sd : -1.0);
    }
    return f;
}

/// 6-feature regime HMM (corr, realized vol, VIX, credit, breadth, trend).
inline RegimeV2 compute_regime_v2(const QVector<const TrIndex*>& core_sectors, const TrIndex& spy, const TrIndex& hyg,
                                  const TrIndex& ief, const MacroSeries* vix) {
    RegimeV2 r;
    const QString method = QLatin1String(kRegimeV2Method);
    if (core_sectors.size() < 9 || spy.empty() || hyg.empty() || ief.empty() || !vix || vix->points.isEmpty()) {
        r.label = ResearchValue::unavailable(QStringLiteral("regime_inputs_missing"), method);
        return r;
    }
    QVector<const TrIndex*> all = core_sectors;
    all.append(&spy);
    all.append(&hyg);
    all.append(&ief);
    QVector<QDate> dates;
    const auto rets = common_returns(all, 0, &dates); // returns on common sessions
    const int T = dates.size();
    if (T < 1300) {
        r.label = ResearchValue::unavailable(QStringLiteral("regime_history_insufficient"), method);
        return r;
    }
    const int ns = core_sectors.size();
    // Levels on the common sessions.
    QVector<QVector<double>> lvl(all.size());
    for (int s = 0; s < all.size(); ++s)
        for (const QDate& d : dates)
            lvl[s].append(all[s]->tr[all[s]->index_of(d)]);
    QHash<QDate, double> vix_by;
    for (const auto& p : vix->points)
        vix_by[p.date] = p.value;
    math::Mat x;
    QVector<QDate> xd;
    double last_vix = std::numeric_limits<double>::quiet_NaN();
    int vix_age = 99;
    for (int t = 0; t < T; ++t) {
        if (vix_by.contains(dates[t])) {
            last_vix = vix_by[dates[t]];
            vix_age = 0;
        } else {
            ++vix_age;
        }
        if (t < 252 || vix_age > 3 || !std::isfinite(last_vix))
            continue;
        std::vector<math::Vec> w;
        for (int s = 0; s < ns; ++s)
            w.emplace_back(rets[static_cast<size_t>(s)].begin() + (t - 62),
                           rets[static_cast<size_t>(s)].begin() + t + 1);
        const math::Mat c = math::correlation_matrix(w);
        if (c.empty())
            continue;
        math::Vec spy_r(rets[static_cast<size_t>(ns)].begin() + (t - 20),
                        rets[static_cast<size_t>(ns)].begin() + t + 1);
        const double rvol = *math::stdev(spy_r) * std::sqrt(252.0);
        const double ratio_now = lvl[ns + 1][t] / lvl[ns + 2][t];
        const double ratio_then = lvl[ns + 1][t - 63] / lvl[ns + 2][t - 63];
        int above = 0;
        for (int s = 0; s < ns; ++s) {
            double sum = 0;
            for (int k = t - 199; k <= t; ++k)
                sum += lvl[s][k];
            if (lvl[s][t] > sum / 200)
                ++above;
        }
        double ssum = 0;
        for (int k = t - 199; k <= t; ++k)
            ssum += lvl[ns][k];
        x.push_back({mean_abs_offdiag(c), rvol, last_vix, ratio_now / ratio_then - 1.0, static_cast<double>(above) / ns,
                     lvl[ns][t] / (ssum / 200) - 1.0});
        xd.append(dates[t]);
    }
    if (x.size() < 1000) {
        r.label = ResearchValue::unavailable(QStringLiteral("regime_history_insufficient"), method);
        return r;
    }
    // In-sample standardisation (the reference's scaler does the same).
    const size_t D = 6;
    math::Vec mu(D, 0), sd(D, 0);
    for (const auto& row : x)
        for (size_t j = 0; j < D; ++j)
            mu[j] += row[j] / x.size();
    for (const auto& row : x)
        for (size_t j = 0; j < D; ++j)
            sd[j] += (row[j] - mu[j]) * (row[j] - mu[j]) / x.size();
    for (auto& s : sd)
        s = std::sqrt(s);
    math::Mat z = x;
    for (auto& row : z)
        for (size_t j = 0; j < D; ++j)
            row[j] = sd[j] > 0 ? (row[j] - mu[j]) / sd[j] : 0.0;
    const auto hmm = math::hmm_fit(z, 4, 1, 200, 1e-4, 1e-3);
    if (!hmm) {
        r.label = ResearchValue::unavailable(QStringLiteral("regime_model_fit_failed"), method);
        return r;
    }
    r.observations = static_cast<int>(x.size());
    r.converged = hmm->converged;
    // Causal labels: prefix Viterbi over a 252-observation context, 5-day hysteresis.
    const int Tz = static_cast<int>(z.size());
    const int start = std::max(0, Tz - 300);
    QVector<int> raw;
    for (int t = start; t < Tz; ++t) {
        const int lo = std::max(0, t - 251);
        const math::Mat ctx(z.begin() + lo, z.begin() + t + 1);
        const auto path = math::hmm_viterbi(*hmm, ctx);
        raw.append(path.empty() ? 0 : path.back());
    }
    QVector<int> smooth;
    int cur = raw.first(), streak = 0, cand = -1;
    for (int s : raw) {
        if (s == cur) {
            cand = -1;
            streak = 0;
        } else if (s == cand) {
            if (++streak >= 5) {
                cur = s;
                cand = -1;
                streak = 0;
            }
        } else {
            cand = s;
            streak = 1;
        }
        smooth.append(cur);
    }
    static const char* kLabels[4] = {"DIVERGENT", "TRENDING", "RISK-OFF", "CRISIS"};
    const int state = smooth.last();
    int since = smooth.size() - 1;
    while (since > 0 && smooth[since - 1] == state)
        --since;
    r.days_in_state = smooth.size() - since;
    r.since = xd[start + since];
    const int hist0 = std::max(0, static_cast<int>(smooth.size()) - 90);
    for (int i = hist0; i < smooth.size(); ++i)
        r.history.append({xd[start + i], smooth[i]});
    const math::Mat ctx(z.end() - std::min<std::ptrdiff_t>(252, static_cast<std::ptrdiff_t>(z.size())), z.end());
    if (const auto filt = math::hmm_filter(*hmm, ctx))
        for (double p : filt->first.back())
            r.probabilities.append(p);
    r.label =
        model_label(QLatin1String(kLabels[state]), r.probabilities.isEmpty() ? 0.0 : r.probabilities[state],
                    Credibility::Medium, {CredCondition::InSampleModel, CredCondition::Unvalidated}, method, xd.last());
    r.label.units = QStringLiteral("probability");
    r.label.source = QStringLiteral("yahoo_chart+fred_VIXCLS");
    static const char* kF[6] = {"corr", "rvol", "vix", "credit", "breadth", "trend"};
    for (size_t j = 0; j < D; ++j)
        r.features.insert(QLatin1String(kF[j]), x.back()[j]);
    return r;
}

} // namespace fincept::services::etf::research
