// src/services/etf/research/EtfResearchSnapshot.h
//
// The research engine's output for one time frame: every value the ETF Flow &
// Sector Rotation workspace displays, each a ResearchValue with its evidence
// class, credibility and reasons. The snapshot is computed on demand from the
// stored observations and is never persisted (the existing Batch C/D pattern).
#pragma once
#include "services/etf/research/EtfResearchInputs.h"
#include "services/etf/research/EtfResearchModel.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace fincept::services::etf::research {

struct RrgPoint {
    QDate date;
    double ratio = 0.0;
    double mom = 0.0;
};

struct RrgResult {
    ResearchValue quadrant; ///< MODEL label: Leading | Improving | Weakening | Lagging
    QString direction;      ///< up | down | '' (momentum vs prior week)
    QVector<RrgPoint> trail;
    QString benchmark;
};

struct ReturnSet {
    ResearchValue d1, w1, m1, m3; ///< PROXY, total return (distributions reinvested), percent
    ResearchValue rel_m1;         ///< PROXY, 1M total return minus benchmark, percentage points
    QString benchmark;
    QDate last_session;
};

struct FlowInterval {
    QDate from;
    QDate to;
    int sessions = 0; ///< NYSE sessions in (from, to]
    std::optional<double> aum_prev, aum_cur, nav_prev, nav_cur;
    std::optional<double> implied_shares_prev, implied_shares_cur;
    std::optional<double> e1_implied_shares;  ///< Δ(AUM/NAV) × NAV_cur
    std::optional<double> e2_reported_shares; ///< Δ(reported shares) × NAV_cur
    std::optional<double> e3_price_adjusted;  ///< AUM_cur - AUM_prev × close_cur/close_prev
    /// Largest |reported shares × NAV / AUM − 1| over the captures whose AUM is
    /// fresh (the anchor; the current capture too unless its AUM was re-served).
    std::optional<double> shares_level_gap;
    QStringList flags;
    QString reason; ///< why E1 is unavailable
};

struct EstimatedFlow {
    ResearchValue latest; ///< ESTIMATED, primary E1 over the latest interval, USD
    ResearchValue best;   ///< the best available estimate: E1, else E2 when E1 is refused
    ResearchValue latest_e2;
    ResearchValue latest_e3;
    ResearchValue sum_5; ///< ESTIMATED, E1 over intervals ending in the last 5 sessions
    ResearchValue sum_20;
    ResearchValue pct_aum_20;  ///< sum_20 / AUM at the start of the window, percent
    ResearchValue coverage_20; ///< share of the last 20 sessions covered by captured intervals
    QString agreement;         ///< agree | disagree | uncorroborated | shares_inconsistent | ''
    int captures = 0;
    int capture_sessions = 0; ///< distinct NAV-dated sessions
    int undated_captures = 0; ///< captures whose NAV matched no session unambiguously
    QString undated_reason;   ///< reason of the latest undated capture
    QDate first_session;
    QDate last_session;
    QVector<FlowInterval> intervals;
    ResearchValue validation_vs_measured; ///< latest full month E1 sum minus SEC measured, USD
};

struct MeasuredFlow {
    ResearchValue latest; ///< MEASURED, latest available SEC N-PORT month, USD
    ResearchValue sum_3m;
    QVector<MeasuredMonth> months;
    QString reporting_key;
};

struct FundFacts {
    ResearchValue aum;             ///< MEASURED (provider-reported), USD
    ResearchValue nav;             ///< MEASURED (provider-reported), per share
    ResearchValue implied_shares;  ///< ESTIMATED, AUM/NAV
    ResearchValue reported_shares; ///< MEASURED (provider field)
    ResearchValue shares_gap_pct;  ///< ESTIMATED, implied vs reported shares, percent
    QDate assumed_session;         ///< session of the latest capture (NAV-dated when possible)
    QString session_rule;          ///< nav_close_match_v1, or the stored rule plus why NAV dating failed
    QDateTime captured_at;
    QString name, family, category, exchange, legal_type, currency;
    ResearchValue expense_pct, yield_pct, beta3y, ytd_pct, ret3y_pct, ret5y_pct, pe, nav_premium_pct;
    QDate inception;
    QVector<Holding> holdings;
    QHash<QString, double> sector_weights;
    QDateTime holdings_captured;
    int captures = 0;
};

/// CFTC futures positioning of the market an ETF tracks (context, never flow).
struct CftcContext {
    QString market;              ///< MarketLab CFTC market key ('' when the ETF has none)
    QDate report_date;           ///< positions as of this Tuesday
    ResearchValue net_pct_oi;    ///< PROXY: (non-commercial long - short) / open interest, %
    ResearchValue percentile_3y; ///< PROXY: rank of that share among the last 156 reports, %
    ResearchValue change_4w_pp;  ///< PROXY: change over four reports, percentage points
    int reports = 0;
};

struct UniverseRow {
    UniverseInstrument inst;
    ReturnSet ret;
    RrgResult rrg;
    CftcContext cftc;
    ResearchValue momentum_z;   ///< MODEL, cross-sectional within the row's peer group
    ResearchValue momentum_raw; ///< MODEL, composite return
    QString peer_group;         ///< us_sector | theme | country | cross_asset
    ResearchValue model_score;  ///< MODEL, sector confluence or country composite
    QString model_band;
    ResearchValue turnover_share, turnover_delta_bp, turnover_z; ///< PROXY, U.S. sectors only
    ResearchValue above_200dma;                                  ///< PROXY, 1/0
    ResearchValue carry_12m;                                     ///< PROXY, trailing 12M distributions / close, percent
    ResearchValue cross_check;                                   ///< IBKR vs Yahoo 1M price return difference, pp
    FundFacts fund;
    EstimatedFlow est;
    MeasuredFlow measured;
    QDate last_bar;
    bool stale = false;
    QString freshness; ///< fresh | stale | no_history
    QString history_status;
    /// Class of best_flow_evidence() for a fund; UNAVAILABLE for non-funds.
    EvidenceClass flow_evidence = EvidenceClass::Unavailable;
    /// Total-return index of the row and of its benchmark on common sessions
    /// (last 260), rebased to 100 at the first point, for the detail chart.
    QVector<QDate> chart_dates;
    QVector<double> chart_tr;
    QVector<double> chart_bench;
};

/// The best available flow evidence of a fund, one rule for the FLOW view, the
/// universe scan column and the summary counts: SEC N-PORT measured flow, else
/// the best estimate (E1, else E2), else the market-behaviour proxy (1M total
/// return vs the fund's benchmark, pp), else the measured value's unavailability.
inline const ResearchValue& best_flow_evidence(const UniverseRow& r) {
    if (r.measured.latest.usable())
        return r.measured.latest;
    if (r.est.best.usable())
        return r.est.best;
    if (r.ret.rel_m1.usable())
        return r.ret.rel_m1;
    return r.measured.latest;
}

struct SectorModelRow {
    QString symbol;
    QString name;
    ResearchValue bc_z, mom_z, val_z, factor_z;
    ResearchValue confluence; ///< MODEL
    QString band;
    int rank = 0;
    ResearchValue layer_dispersion;
    QStringList conflicts;
    ResearchValue pe, earnings_yield_spread, pe_own_z;
    QVector<double> factor_betas; ///< order: slope, breakeven, hy_oas, dollar, oil
    QVector<double> factor_tstats;
    double factor_r2 = 0.0;
};

struct BusinessCycle {
    ResearchValue level;     ///< MODEL, composite level z
    ResearchValue direction; ///< MODEL, activity momentum z
    ResearchValue phase;     ///< MODEL label
    double p_recovery = 0, p_expansion = 0, p_slowdown = 0, p_contraction = 0;
    ResearchValue z_slope, z_activity, z_credit, z_momentum; ///< components
    bool credit_fallback = false;
    ResearchValue vix;                  ///< MEASURED (FRED VIXCLS), context
    ResearchValue sectors_above_200dma; ///< PROXY fraction
};

struct SectorModels {
    BusinessCycle cycle;
    QVector<SectorModelRow> rows;
    QHash<QString, double> weights_used;
    bool volatility_brake = false;
    QString top_momentum, top_valuation;
    bool mom_val_divergence = false;
    QStringList bc_mom_conflicts;
    QVector<ResearchValue> factor_state; ///< latest 4-week factor change z, by factor
    QStringList factor_names;
    QStringList notes;
};

struct CountryRow {
    QString symbol;
    QString country;
    QString region;
    QString type;
    ResearchValue m_z, q_z, c_z, composite;
    QString band;
    int rank = 0;
    ResearchValue gdp_3y, current_account, carry;
    int gdp_year = 0, ca_year = 0;
};

struct TiltResult {
    ResearchValue tilt_bp; ///< PROXY, defensive minus cyclical turnover-share change
    ResearchValue tilt_z;
    QString state; ///< DEFENSIVE | CYCLICAL | BALANCED | ''
    int window = 20;
    double band_bp = 10.0;
    QHash<QString, int> quadrants;
    int above_bench = 0;
    int breadth_total = 0;
};

struct CorrelationView {
    QString period;
    QStringList symbols;
    QVector<QVector<double>> corr;
    QVector<QVector<double>> wedge;
    QVector<QPair<double, double>> pca;
    double var_pc1 = 0, var_pc2 = 0;
    ResearchValue avg_abs_corr; ///< MODEL score
    ResearchValue corr_label;   ///< MODEL label
    ResearchValue geom_score;   ///< det^(1/N)
    ResearchValue geom_label;
    int k_signal = 0;
    double lambda_max = 0, lambda_1 = 0;
    QString factor_regime;
    int observations = 0;
    QString calibration; ///< MRS | heuristic
};

struct RegimeV2 {
    ResearchValue label;
    QVector<double> probabilities; ///< DIVERGENT..CRISIS
    int days_in_state = 0;
    QDate since;
    QVector<QPair<QDate, int>> history;
    QHash<QString, double> features;
    int observations = 0;
    bool converged = false;
};

struct RegimeResult {
    QVector<CorrelationView> views; ///< 1m, 3m, 6m, 1y, 2y
    ResearchValue trend_corr;       ///< label: RISING | FALLING | STABLE (correlation)
    double risk_delta_corr = 0;
    ResearchValue trend_geom;
    double risk_delta_geom = 0;
    bool corr_geom_conflict = false;
    double risk_gap = 0;
    bool mrs_ready = false;
    QVector<double> mrs_thresholds;
    QVector<double> mrs_threshold_std;
    QVector<double> mrs_means;
    ResearchValue mrs_label;
    QVector<double> mrs_transition_20d;
    RegimeV2 v2;
};

struct BasketRow {
    Basket basket;
    ReturnSet ret;
    RrgResult rrg;
    int members_available = 0;
    QString benchmark_used;
    bool benchmark_fallback = false;
};

struct ConstituentRow {
    QString research_symbol; ///< the Yahoo symbol used for its data (mapped when reviewed)
    QString parent;
    QString symbol;
    QString name;
    std::optional<double> weight;
    ReturnSet ret;
    QString quadrant;
    QString currency;
    ResearchValue pe, forward_pe, market_cap;
};

struct ConstituentAggregate {
    QString parent;
    int count = 0;
    double weight_covered = 0;
    ResearchValue median_pe, cap_weighted_pe, median_forward_pe, median_debt_equity;
    QString currency_basis; ///< single currency or "mixed"
};

struct IntlSector {
    QString market;
    QString sector;
    ResearchValue ew_d1, ew_m1, ew_m3; ///< PROXY, equal-weight of members with data
    int members = 0;
    int members_with_data = 0;
    ConstituentAggregate fundamentals;
    QVector<ConstituentRow> stocks;
};

struct ResearchSnapshot {
    QString engine_version;
    QString universe_version;
    QString reference_commit;
    QDateTime as_of;
    QDateTime known_at;
    QDateTime computed_at;
    QDate expected_us_session;
    QVector<UniverseRow> rows;
    SectorModels sectors;
    QVector<CountryRow> countries;
    TiltResult tilt;
    RegimeResult regime;
    QVector<BasketRow> baskets;
    QString th_benchmark_used;
    bool th_benchmark_fallback = false;
    QVector<ConstituentRow> constituents;
    QVector<ConstituentAggregate> constituent_aggregates;
    QVector<IntlSector> intl;
    QVector<GroupFlowRow> group_flows;
    bool group_flows_loaded = true; ///< false: the workspace loads them when the FLOW view opens
    QVector<SourceStageStatus> sources;
    QString last_refresh_run_id;
    QDateTime last_refresh_finished;
    QStringList warnings;
    QHash<QString, int> counts;

    const UniverseRow* row(const QString& symbol) const {
        for (const auto& r : rows)
            if (r.inst.symbol == symbol)
                return &r;
        return nullptr;
    }
};

} // namespace fincept::services::etf::research
