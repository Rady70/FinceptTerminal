// src/services/etf/research/EtfResearchMath.h
//
// Small, dependency-free numerics for the ETF research engine: sample
// statistics, symmetric eigen-decomposition (cyclic Jacobi), Cholesky,
// least squares and a Gaussian hidden Markov model (diagonal-free full
// covariance, Baum-Welch with scaling, Viterbi, filtered posteriors).
//
// Every function is deterministic. Nothing here substitutes a neutral value for
// an undefined one: an undefined statistic is std::nullopt.
//
// Header-only over the C++ standard library.
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <vector>

namespace fincept::services::etf::research::math {

using Vec = std::vector<double>;
using Mat = std::vector<Vec>; ///< row-major, Mat[i][j]

inline bool finite(double v) {
    return std::isfinite(v);
}

inline std::optional<double> mean(const Vec& v) {
    if (v.empty())
        return std::nullopt;
    double s = 0.0;
    for (double x : v)
        s += x;
    return s / static_cast<double>(v.size());
}

/// Sample standard deviation (n-1). Undefined below two observations.
inline std::optional<double> stdev(const Vec& v) {
    if (v.size() < 2)
        return std::nullopt;
    const double m = *mean(v);
    double ss = 0.0;
    for (double x : v)
        ss += (x - m) * (x - m);
    return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

inline std::optional<double> median(Vec v) {
    if (v.empty())
        return std::nullopt;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// z-score of the last element against the trailing `lookback` elements
/// (inclusive). Needs at least `min_n`. A flat series (sd below a scale-relative
/// floor) has no z-score rather than an enormous one.
inline std::optional<double> zscore_last(const Vec& series, size_t lookback, size_t min_n) {
    if (series.empty())
        return std::nullopt;
    const size_t start = series.size() > lookback ? series.size() - lookback : 0;
    Vec w(series.begin() + static_cast<std::ptrdiff_t>(start), series.end());
    if (w.size() < min_n || w.size() < 2)
        return std::nullopt;
    const double m = *mean(w);
    const double sd = *stdev(w);
    if (!(sd > std::max(std::abs(m), 1e-12) * 1e-9))
        return std::nullopt;
    return (w.back() - m) / sd;
}

/// Cross-sectional standardisation of the defined values. Undefined inputs stay
/// undefined; fewer than three defined values, or zero dispersion, gives no z.
inline std::vector<std::optional<double>> cross_section_z(const std::vector<std::optional<double>>& xs,
                                                          double clamp = 0.0) {
    Vec defined;
    for (const auto& x : xs)
        if (x)
            defined.push_back(*x);
    std::vector<std::optional<double>> out(xs.size());
    if (defined.size() < 3)
        return out;
    const double m = *mean(defined);
    const double sd = *stdev(defined);
    if (!(sd > 1e-12))
        return out;
    for (size_t i = 0; i < xs.size(); ++i) {
        if (!xs[i])
            continue;
        double z = (*xs[i] - m) / sd;
        if (clamp > 0.0)
            z = std::clamp(z, -clamp, clamp);
        out[i] = z;
    }
    return out;
}

inline std::optional<double> pearson(const Vec& a, const Vec& b) {
    if (a.size() != b.size() || a.size() < 3)
        return std::nullopt;
    const double ma = *mean(a), mb = *mean(b);
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    if (saa <= 0 || sbb <= 0)
        return std::nullopt;
    return sab / std::sqrt(saa * sbb);
}

/// Correlation matrix of column series (each Vec in `cols` is one variable,
/// all the same length). Returns empty on any undefined pair.
inline Mat correlation_matrix(const std::vector<Vec>& cols) {
    const size_t n = cols.size();
    Mat c(n, Vec(n, 0.0));
    for (size_t i = 0; i < n; ++i) {
        c[i][i] = 1.0;
        for (size_t j = i + 1; j < n; ++j) {
            const auto r = pearson(cols[i], cols[j]);
            if (!r)
                return {};
            c[i][j] = c[j][i] = *r;
        }
    }
    return c;
}

/// Symmetric eigen-decomposition by cyclic Jacobi rotations. Eigenvalues are
/// returned in descending order with eigenvectors as COLUMNS of `vectors`.
struct Eigen {
    Vec values;
    Mat vectors;
};

inline Eigen jacobi_eigen(Mat a, int max_sweeps = 100) {
    const size_t n = a.size();
    Mat v(n, Vec(n, 0.0));
    for (size_t i = 0; i < n; ++i)
        v[i][i] = 1.0;
    for (int sweep = 0; sweep < max_sweeps; ++sweep) {
        double off = 0.0;
        for (size_t p = 0; p < n; ++p)
            for (size_t q = p + 1; q < n; ++q)
                off += a[p][q] * a[p][q];
        if (off < 1e-22)
            break;
        for (size_t p = 0; p < n; ++p) {
            for (size_t q = p + 1; q < n; ++q) {
                if (std::abs(a[p][q]) < 1e-300)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (size_t k = 0; k < n; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (size_t k = 0; k < n; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (size_t k = 0; k < n; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t x, size_t y) { return a[x][x] > a[y][y]; });
    Eigen e;
    e.values.resize(n);
    e.vectors.assign(n, Vec(n, 0.0));
    for (size_t k = 0; k < n; ++k) {
        e.values[k] = a[idx[k]][idx[k]];
        // Deterministic sign: the largest-magnitude component is positive.
        double big = 0.0;
        for (size_t i = 0; i < n; ++i)
            if (std::abs(v[i][idx[k]]) > std::abs(big))
                big = v[i][idx[k]];
        const double sign = big < 0 ? -1.0 : 1.0;
        for (size_t i = 0; i < n; ++i)
            e.vectors[i][k] = sign * v[i][idx[k]];
    }
    return e;
}

/// Cholesky factor L (lower) of a symmetric positive-definite matrix, or empty.
inline Mat cholesky(const Mat& a) {
    const size_t n = a.size();
    Mat l(n, Vec(n, 0.0));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j <= i; ++j) {
            double s = a[i][j];
            for (size_t k = 0; k < j; ++k)
                s -= l[i][k] * l[j][k];
            if (i == j) {
                if (!(s > 0.0))
                    return {};
                l[i][i] = std::sqrt(s);
            } else {
                l[i][j] = s / l[j][j];
            }
        }
    }
    return l;
}

/// log(det A) of an SPD matrix via Cholesky; nullopt when not positive definite.
inline std::optional<double> log_det_spd(const Mat& a) {
    const Mat l = cholesky(a);
    if (l.empty())
        return std::nullopt;
    double s = 0.0;
    for (size_t i = 0; i < l.size(); ++i)
        s += std::log(l[i][i]);
    return 2.0 * s;
}

/// Solve L L^T x = b given Cholesky factor L.
inline Vec cholesky_solve(const Mat& l, const Vec& b) {
    const size_t n = l.size();
    Vec y(n, 0.0), x(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        double s = b[i];
        for (size_t k = 0; k < i; ++k)
            s -= l[i][k] * y[k];
        y[i] = s / l[i][i];
    }
    for (size_t ii = n; ii-- > 0;) {
        double s = y[ii];
        for (size_t k = ii + 1; k < n; ++k)
            s -= l[k][ii] * x[k];
        x[ii] = s / l[ii][ii];
    }
    return x;
}

/// Ordinary least squares with an intercept. X rows are observations.
struct OlsFit {
    Vec beta;    ///< [intercept, b1..bk]
    Vec t_stats; ///< per coefficient
    double r2 = 0.0;
    size_t n = 0;
};

inline std::optional<OlsFit> ols(const Mat& x, const Vec& y) {
    const size_t n = y.size();
    if (n == 0 || x.size() != n)
        return std::nullopt;
    const size_t k = x[0].size() + 1;
    if (n <= k + 2)
        return std::nullopt;
    Mat xtx(k, Vec(k, 0.0));
    Vec xty(k, 0.0);
    for (size_t r = 0; r < n; ++r) {
        Vec row(k);
        row[0] = 1.0;
        for (size_t j = 1; j < k; ++j)
            row[j] = x[r][j - 1];
        for (size_t i = 0; i < k; ++i) {
            xty[i] += row[i] * y[r];
            for (size_t j = 0; j < k; ++j)
                xtx[i][j] += row[i] * row[j];
        }
    }
    const Mat l = cholesky(xtx);
    if (l.empty())
        return std::nullopt;
    OlsFit f;
    f.n = n;
    f.beta = cholesky_solve(l, xty);
    double sse = 0.0, sst = 0.0;
    const double ym = *mean(y);
    for (size_t r = 0; r < n; ++r) {
        double fit = f.beta[0];
        for (size_t j = 1; j < k; ++j)
            fit += f.beta[j] * x[r][j - 1];
        sse += (y[r] - fit) * (y[r] - fit);
        sst += (y[r] - ym) * (y[r] - ym);
    }
    f.r2 = sst > 0 ? 1.0 - sse / sst : 0.0;
    const double sigma2 = sse / static_cast<double>(n - k);
    f.t_stats.resize(k);
    for (size_t j = 0; j < k; ++j) {
        Vec e(k, 0.0);
        e[j] = 1.0;
        const Vec col = cholesky_solve(l, e); // column j of (X'X)^-1
        const double se = std::sqrt(std::max(0.0, sigma2 * col[j]));
        f.t_stats[j] = se > 0 ? f.beta[j] / se : 0.0;
    }
    return f;
}

// ── Gaussian hidden Markov model ─────────────────────────────────────────────
// States are ordered by the caller-chosen dimension after fitting, so state 0
// is the "calmest" by that dimension. Covariances are full and regularised by
// `reg` on the diagonal (hmmlearn's min_covar plays the same role).
struct GaussianHmm {
    size_t k = 0;
    size_t d = 0;
    Vec start;             ///< k
    Mat trans;             ///< k x k
    Mat means;             ///< k x d
    std::vector<Mat> covs; ///< k x (d x d)
    double log_likelihood = -std::numeric_limits<double>::infinity();
    int iterations = 0;
    bool converged = false;
};

namespace hmm_detail {
struct Density {
    Mat chol;
    double log_norm = 0.0;
};

inline std::vector<Density> densities(const GaussianHmm& m) {
    std::vector<Density> out(m.k);
    for (size_t s = 0; s < m.k; ++s) {
        out[s].chol = cholesky(m.covs[s]);
        if (out[s].chol.empty())
            return {};
        double ld = 0.0;
        for (size_t i = 0; i < m.d; ++i)
            ld += std::log(out[s].chol[i][i]);
        out[s].log_norm = -0.5 * static_cast<double>(m.d) * std::log(2.0 * 3.14159265358979323846) - ld;
    }
    return out;
}

inline double log_pdf(const Density& den, const Vec& mu, const Vec& x) {
    const size_t d = x.size();
    Vec z(d);
    for (size_t i = 0; i < d; ++i) {
        double s = x[i] - mu[i];
        for (size_t k = 0; k < i; ++k)
            s -= den.chol[i][k] * z[k];
        z[i] = s / den.chol[i][i];
    }
    double q = 0.0;
    for (double v : z)
        q += v * v;
    return den.log_norm - 0.5 * q;
}

inline Mat emission_log(const GaussianHmm& m, const Mat& x, bool* ok) {
    const auto den = densities(m);
    *ok = !den.empty();
    Mat le(x.size(), Vec(m.k, 0.0));
    if (!*ok)
        return le;
    for (size_t t = 0; t < x.size(); ++t)
        for (size_t s = 0; s < m.k; ++s)
            le[t][s] = log_pdf(den[s], m.means[s], x[t]);
    return le;
}
} // namespace hmm_detail

/// Forward pass with scaling. Returns per-time filtered posteriors P(s_t | x_1..t)
/// and the sequence log-likelihood.
inline std::optional<std::pair<Mat, double>> hmm_filter(const GaussianHmm& m, const Mat& x) {
    if (x.empty())
        return std::nullopt;
    bool ok = false;
    const Mat le = hmm_detail::emission_log(m, x, &ok);
    if (!ok)
        return std::nullopt;
    Mat alpha(x.size(), Vec(m.k, 0.0));
    double ll = 0.0;
    for (size_t t = 0; t < x.size(); ++t) {
        double mx = -std::numeric_limits<double>::infinity();
        for (size_t s = 0; s < m.k; ++s)
            mx = std::max(mx, le[t][s]);
        double norm = 0.0;
        for (size_t s = 0; s < m.k; ++s) {
            double prior = 0.0;
            if (t == 0) {
                prior = m.start[s];
            } else {
                for (size_t r = 0; r < m.k; ++r)
                    prior += alpha[t - 1][r] * m.trans[r][s];
            }
            alpha[t][s] = prior * std::exp(le[t][s] - mx);
            norm += alpha[t][s];
        }
        if (!(norm > 0.0))
            return std::nullopt;
        for (size_t s = 0; s < m.k; ++s)
            alpha[t][s] /= norm;
        ll += std::log(norm) + mx;
    }
    return std::make_pair(alpha, ll);
}

/// Most likely state path (Viterbi, log space).
inline std::vector<int> hmm_viterbi(const GaussianHmm& m, const Mat& x) {
    bool ok = false;
    const Mat le = hmm_detail::emission_log(m, x, &ok);
    if (!ok || x.empty())
        return {};
    const size_t T = x.size();
    Mat delta(T, Vec(m.k, 0.0));
    std::vector<std::vector<int>> psi(T, std::vector<int>(m.k, 0));
    for (size_t s = 0; s < m.k; ++s)
        delta[0][s] = std::log(std::max(m.start[s], 1e-300)) + le[0][s];
    for (size_t t = 1; t < T; ++t) {
        for (size_t s = 0; s < m.k; ++s) {
            double best = -std::numeric_limits<double>::infinity();
            int arg = 0;
            for (size_t r = 0; r < m.k; ++r) {
                const double v = delta[t - 1][r] + std::log(std::max(m.trans[r][s], 1e-300));
                if (v > best) {
                    best = v;
                    arg = static_cast<int>(r);
                }
            }
            delta[t][s] = best + le[t][s];
            psi[t][s] = arg;
        }
    }
    std::vector<int> path(T, 0);
    double best = -std::numeric_limits<double>::infinity();
    for (size_t s = 0; s < m.k; ++s)
        if (delta[T - 1][s] > best) {
            best = delta[T - 1][s];
            path[T - 1] = static_cast<int>(s);
        }
    for (size_t t = T - 1; t > 0; --t)
        path[t - 1] = psi[t][static_cast<size_t>(path[t])];
    return path;
}

/// Fit a k-state Gaussian HMM by Baum-Welch. Deterministic initialisation:
/// observations are sorted by `order_dim` and split into k equal quantile
/// groups; each group's mean/covariance seeds a state, transitions start at
/// 0.9 persistence. After fitting, states are re-ordered ascending by the mean
/// of `order_dim`.
inline std::optional<GaussianHmm> hmm_fit(const Mat& x, size_t k, size_t order_dim, int max_iter = 200,
                                          double tol = 1e-4, double reg = 1e-3) {
    const size_t T = x.size();
    if (T < k * 10 || k < 2)
        return std::nullopt;
    const size_t d = x[0].size();
    if (order_dim >= d)
        return std::nullopt;
    GaussianHmm m;
    m.k = k;
    m.d = d;
    m.start.assign(k, 1.0 / static_cast<double>(k));
    m.trans.assign(k, Vec(k, 0.1 / static_cast<double>(k - 1)));
    for (size_t s = 0; s < k; ++s)
        m.trans[s][s] = 0.9;
    std::vector<size_t> idx(T);
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return x[a][order_dim] < x[b][order_dim]; });
    m.means.assign(k, Vec(d, 0.0));
    m.covs.assign(k, Mat(d, Vec(d, 0.0)));
    for (size_t s = 0; s < k; ++s) {
        const size_t lo = s * T / k, hi = (s + 1) * T / k;
        const double n = static_cast<double>(hi - lo);
        for (size_t i = lo; i < hi; ++i)
            for (size_t a = 0; a < d; ++a)
                m.means[s][a] += x[idx[i]][a] / n;
        for (size_t i = lo; i < hi; ++i)
            for (size_t a = 0; a < d; ++a)
                for (size_t b = 0; b < d; ++b)
                    m.covs[s][a][b] += (x[idx[i]][a] - m.means[s][a]) * (x[idx[i]][b] - m.means[s][b]) / n;
        for (size_t a = 0; a < d; ++a)
            m.covs[s][a][a] += reg;
    }

    double prev = -std::numeric_limits<double>::infinity();
    for (int it = 0; it < max_iter; ++it) {
        bool ok = false;
        const Mat le = hmm_detail::emission_log(m, x, &ok);
        if (!ok)
            return std::nullopt;
        Mat alpha(T, Vec(k)), beta(T, Vec(k, 1.0));
        Vec scale(T, 0.0);
        Vec emx(T, 0.0);
        Mat em(T, Vec(k));
        for (size_t t = 0; t < T; ++t) {
            double mx = -std::numeric_limits<double>::infinity();
            for (size_t s = 0; s < k; ++s)
                mx = std::max(mx, le[t][s]);
            emx[t] = mx;
            for (size_t s = 0; s < k; ++s)
                em[t][s] = std::exp(le[t][s] - mx);
        }
        double ll = 0.0;
        for (size_t t = 0; t < T; ++t) {
            double norm = 0.0;
            for (size_t s = 0; s < k; ++s) {
                double prior = 0.0;
                if (t == 0)
                    prior = m.start[s];
                else
                    for (size_t r = 0; r < k; ++r)
                        prior += alpha[t - 1][r] * m.trans[r][s];
                alpha[t][s] = prior * em[t][s];
                norm += alpha[t][s];
            }
            if (!(norm > 0.0))
                return std::nullopt;
            scale[t] = norm;
            for (size_t s = 0; s < k; ++s)
                alpha[t][s] /= norm;
            ll += std::log(norm) + emx[t];
        }
        for (size_t t = T - 1; t > 0; --t) {
            for (size_t r = 0; r < k; ++r) {
                double v = 0.0;
                for (size_t s = 0; s < k; ++s)
                    v += m.trans[r][s] * em[t][s] * beta[t][s];
                beta[t - 1][r] = v / scale[t];
            }
        }
        // E-step aggregates.
        Mat gamma(T, Vec(k));
        Mat xi_sum(k, Vec(k, 0.0));
        for (size_t t = 0; t < T; ++t) {
            double g = 0.0;
            for (size_t s = 0; s < k; ++s) {
                gamma[t][s] = alpha[t][s] * beta[t][s];
                g += gamma[t][s];
            }
            for (size_t s = 0; s < k; ++s)
                gamma[t][s] /= g;
            if (t + 1 < T) {
                double z = 0.0;
                Mat xi(k, Vec(k));
                for (size_t r = 0; r < k; ++r)
                    for (size_t s = 0; s < k; ++s) {
                        xi[r][s] = alpha[t][r] * m.trans[r][s] * em[t + 1][s] * beta[t + 1][s];
                        z += xi[r][s];
                    }
                for (size_t r = 0; r < k; ++r)
                    for (size_t s = 0; s < k; ++s)
                        xi_sum[r][s] += xi[r][s] / z;
            }
        }
        // M-step.
        for (size_t s = 0; s < k; ++s)
            m.start[s] = gamma[0][s];
        for (size_t r = 0; r < k; ++r) {
            double row = 0.0;
            for (size_t s = 0; s < k; ++s)
                row += xi_sum[r][s];
            for (size_t s = 0; s < k; ++s)
                m.trans[r][s] = row > 0 ? xi_sum[r][s] / row : 1.0 / static_cast<double>(k);
        }
        for (size_t s = 0; s < k; ++s) {
            double w = 0.0;
            Vec mu(d, 0.0);
            for (size_t t = 0; t < T; ++t) {
                w += gamma[t][s];
                for (size_t a = 0; a < d; ++a)
                    mu[a] += gamma[t][s] * x[t][a];
            }
            if (!(w > 1e-9))
                return std::nullopt;
            for (size_t a = 0; a < d; ++a)
                mu[a] /= w;
            Mat cv(d, Vec(d, 0.0));
            for (size_t t = 0; t < T; ++t)
                for (size_t a = 0; a < d; ++a)
                    for (size_t b = 0; b < d; ++b)
                        cv[a][b] += gamma[t][s] * (x[t][a] - mu[a]) * (x[t][b] - mu[b]);
            for (size_t a = 0; a < d; ++a) {
                for (size_t b = 0; b < d; ++b)
                    cv[a][b] /= w;
                cv[a][a] += reg;
            }
            m.means[s] = mu;
            m.covs[s] = cv;
        }
        m.log_likelihood = ll;
        m.iterations = it + 1;
        if (std::abs(ll - prev) < tol) {
            m.converged = true;
            break;
        }
        prev = ll;
    }
    // Order states by the mean of order_dim.
    std::vector<size_t> order(k);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return m.means[a][order_dim] < m.means[b][order_dim]; });
    GaussianHmm o = m;
    for (size_t i = 0; i < k; ++i) {
        o.start[i] = m.start[order[i]];
        o.means[i] = m.means[order[i]];
        o.covs[i] = m.covs[order[i]];
        for (size_t j = 0; j < k; ++j)
            o.trans[i][j] = m.trans[order[i]][order[j]];
    }
    return o;
}

} // namespace fincept::services::etf::research::math
