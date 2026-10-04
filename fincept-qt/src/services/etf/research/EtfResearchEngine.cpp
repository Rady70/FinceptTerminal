#include "services/etf/research/EtfResearchEngine.h"

#include "services/etf/research/EtfResearchFlow.h"
#include "services/etf/research/EtfResearchMath.h"
#include "services/etf/research/EtfResearchModels.h"
#include "services/etf/research/EtfResearchRegime.h"
#include "services/etf/research/EtfResearchRotation.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace fincept::services::etf::research {

namespace etfr_engine_detail {

const QString kFundSource = QLatin1String(kSourceYahooFund);

std::optional<double> json_num(const QJsonObject& o, const char* key) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble())
        return std::nullopt;
    const double d = v.toDouble();
    return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
}

ResearchValue observed(std::optional<double> v, const QString& units, const QString& method, const QDate& eff,
                       bool stale, const QString& missing) {
    if (!v)
        return ResearchValue::unavailable(missing, method);
    ResearchValue r;
    r.value = v;
    r.evidence = EvidenceClass::Measured;
    r.units = units;
    r.method = method;
    r.source = kFundSource;
    r.source_quality = QLatin1String(quality::kProviderUndated);
    r.effective = eff;
    r.add_flag(flag::kAssumedDate);
    if (stale)
        r.add_flag(flag::kStale);
    if (*v == 0.0)
        r.add_flag(flag::kActualZero);
    return r;
}

QString peer_group_of(const UniverseInstrument& i) {
    if (i.has_role("us_sector"))
        return QStringLiteral("us_sector");
    if (i.has_role("theme"))
        return QStringLiteral("theme");
    if (i.has_role("country"))
        return QStringLiteral("country");
    if (i.has_role("cross_asset"))
        return QStringLiteral("cross_asset");
    return QStringLiteral("benchmark");
}

QVector<CredCondition> base_conditions(bool stale, bool revised) {
    QVector<CredCondition> c;
    if (stale)
        c.append(CredCondition::Stale);
    if (revised)
        c.append(CredCondition::InputRevised);
    return c;
}

} // namespace etfr_engine_detail

using namespace etfr_engine_detail;

FundFacts fund_facts_from_captures(const QVector<FundCapture>& caps, const BarSeries* bars,
                                   const HoldingsCapture* holdings, const QDate& expected_session) {
    FundFacts f;
    f.captures = caps.size();
    if (holdings) {
        f.holdings = holdings->holdings;
        f.sector_weights = holdings->sector_weights;
        f.holdings_captured = holdings->captured_at;
    }
    const QString method = QStringLiteral("yahoo_quote_summary");
    if (caps.isEmpty()) {
        f.aum = ResearchValue::unavailable(QStringLiteral("no_capture"), method);
        f.aum.add_flag(flag::kNoObservation);
        f.nav = f.aum;
        f.implied_shares = f.aum;
        f.reported_shares = f.aum;
        f.shares_gap_pct = f.aum;
        f.expense_pct = f.yield_pct = f.beta3y = f.ytd_pct = f.ret3y_pct = f.ret5y_pct = f.pe = f.nav_premium_pct =
            f.aum;
        return f;
    }
    const FundCapture& c = caps.last();
    const QJsonObject& o = c.fields;
    f.captured_at = c.captured_at;
    QString dating_reason = QStringLiteral("no_close_to_date_nav");
    const FundCapture dc = bars ? nav_dated(c, *bars, &dating_reason) : FundCapture{};
    const bool nav_dated_ok = dc.effective_session.isValid();
    // The NAV-matched session when the NAV can be dated; otherwise the stored
    // upper bound, and the premium (which needs the right close) is unavailable.
    const QDate session = nav_dated_ok ? dc.effective_session : c.effective_session;
    f.assumed_session = session;
    f.session_rule = nav_dated_ok ? QString(QLatin1String(kNavDatingRule))
                                  : c.effective_rule + QStringLiteral(":nav_undated:") + dating_reason;
    bool stale = false;
    if (expected_session.isValid() && session.isValid()) {
        const int lag = nyse_sessions_between(session, expected_session);
        stale = lag < 0 || lag > 1;
    }
    f.aum = observed(c.total_assets, QStringLiteral("usd"), method + QStringLiteral(":totalAssets"), session, stale,
                     QStringLiteral("field_not_supplied"));
    f.nav = observed(c.nav, QStringLiteral("per_share"), method + QStringLiteral(":navPrice"), session, stale,
                     QStringLiteral("field_not_supplied"));
    f.reported_shares =
        observed(c.shares_outstanding, QStringLiteral("shares"), method + QStringLiteral(":sharesOutstanding"), session,
                 stale, QStringLiteral("field_not_supplied"));
    if (c.total_assets && c.nav && *c.nav > 0) {
        f.implied_shares = graded(*c.total_assets / *c.nav, EvidenceClass::Estimated, Credibility::Medium,
                                  {CredCondition::SourceTimingAmbiguous}, QStringLiteral("shares"),
                                  QStringLiteral("etfr_implied_shares_v1"), kFundSource, session);
        if (c.shares_outstanding && *c.shares_outstanding > 0)
            f.shares_gap_pct =
                graded((*c.total_assets / *c.nav / *c.shares_outstanding - 1.0) * 100.0, EvidenceClass::Estimated,
                       Credibility::Medium, {CredCondition::SourceTimingAmbiguous}, QStringLiteral("pct"),
                       QStringLiteral("etfr_shares_consistency_v1"), kFundSource, session);
        else
            f.shares_gap_pct = ResearchValue::unavailable(QStringLiteral("reported_shares_missing"));
    } else {
        f.implied_shares = ResearchValue::unavailable(QStringLiteral("aum_or_nav_missing"));
        f.shares_gap_pct = f.implied_shares;
    }
    f.name = o.value(QStringLiteral("longName")).toString(o.value(QStringLiteral("shortName")).toString());
    f.family = o.value(QStringLiteral("fundFamily")).toString();
    f.category = o.value(QStringLiteral("category")).toString();
    f.exchange = o.value(QStringLiteral("fullExchangeName")).toString(o.value(QStringLiteral("exchange")).toString());
    f.legal_type = o.value(QStringLiteral("legalType")).toString(o.value(QStringLiteral("quoteType")).toString());
    f.currency = o.value(QStringLiteral("currency")).toString();
    // Yahoo units differ by field; this table is the single place they are fixed.
    //   netExpenseRatio: percent; annualReportExpenseRatio: fraction; yield: fraction;
    //   ytdReturn: percent; threeYear/fiveYearAverageReturn: fraction; beta3Year: ratio.
    auto pct_field = [&](const char* key, double scale, const QString& units) {
        return observed(json_num(o, key) ? std::optional<double>(*json_num(o, key) * scale) : std::nullopt, units,
                        method + QLatin1Char(':') + QLatin1String(key), session, stale,
                        QStringLiteral("field_not_supplied"));
    };
    f.expense_pct = json_num(o, "netExpenseRatio")
                        ? pct_field("netExpenseRatio", 1.0, QStringLiteral("pct"))
                        : pct_field("annualReportExpenseRatio", 100.0, QStringLiteral("pct"));
    f.yield_pct = pct_field("yield", 100.0, QStringLiteral("pct"));
    f.beta3y = pct_field("beta3Year", 1.0, QStringLiteral("ratio"));
    f.ytd_pct = pct_field("ytdReturn", 1.0, QStringLiteral("pct"));
    f.ret3y_pct = pct_field("threeYearAverageReturn", 100.0, QStringLiteral("pct"));
    f.ret5y_pct = pct_field("fiveYearAverageReturn", 100.0, QStringLiteral("pct"));
    f.pe = pct_field("trailingPE", 1.0, QStringLiteral("ratio"));
    for (ResearchValue* v : {&f.expense_pct, &f.yield_pct, &f.beta3y, &f.ytd_pct, &f.ret3y_pct, &f.ret5y_pct, &f.pe})
        v->flags.removeAll(QLatin1String(flag::kAssumedDate)); // metadata, not dated observations
    if (const auto inc = json_num(o, "fundInceptionDate"))
        f.inception = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(*inc), QTimeZone::UTC).date();
    const auto close = bars && nav_dated_ok ? unadjusted_close_on(*bars, session) : std::nullopt;
    if (!nav_dated_ok && c.nav)
        f.nav_premium_pct = ResearchValue::unavailable(dating_reason);
    else if (close && c.nav && *c.nav > 0)
        f.nav_premium_pct = graded((*close / *c.nav - 1.0) * 100.0, EvidenceClass::Estimated, Credibility::Low,
                                   {CredCondition::SourceTimingAmbiguous}, QStringLiteral("pct"),
                                   QStringLiteral("etfr_premium_discount_v1"),
                                   QStringLiteral("yahoo_chart+yahoo_fund_snapshot"), session);
    else
        f.nav_premium_pct = ResearchValue::unavailable(QStringLiteral("close_or_nav_missing"));
    return f;
}

namespace {

void etfr_measured(UniverseRow& row, const QVector<MeasuredMonth>& months) {
    row.measured.months = months;
    const QString method = QStringLiteral("regulatory_flow_analytics_v1(batch_c)");
    if (row.inst.sec_reporting.isEmpty()) {
        row.measured.latest = ResearchValue::unavailable(QStringLiteral("no_declared_sec_reporting_identity"), method);
        row.measured.sum_3m = row.measured.latest;
        return;
    }
    const MeasuredMonth* latest = nullptr;
    for (int i = months.size() - 1; i >= 0; --i)
        if (months[i].flow_usd) {
            latest = &months[i];
            break;
        }
    if (!latest) {
        row.measured.latest = ResearchValue::unavailable(QStringLiteral("no_available_month"), method);
        row.measured.latest.add_flag(flag::kNoObservation);
        row.measured.sum_3m = row.measured.latest;
        return;
    }
    row.measured.reporting_key = latest->reporting_key;
    auto make = [&](double v, const QDate& eff, bool revised) {
        ResearchValue r;
        r.value = v;
        r.evidence = EvidenceClass::Measured;
        r.source_quality = QLatin1String(revised ? quality::kRevised : quality::kConfirmed);
        r.units = QStringLiteral("usd");
        r.method = method;
        r.source = QStringLiteral("sec_nport");
        r.effective = eff;
        r.add_flag(flag::kMonthly);
        if (revised)
            r.add_flag(flag::kRevised);
        if (v == 0.0)
            r.add_flag(flag::kActualZero);
        return r;
    };
    row.measured.latest = make(*latest->flow_usd, latest->month, latest->quality == QLatin1String("REVISED"));
    // Three consecutive months ending at the latest, all available.
    double s = 0;
    int n = 0;
    bool revised = false;
    for (const auto& m : months) {
        if (m.month > latest->month || m.month <= latest->month.addMonths(-3))
            continue;
        if (!m.flow_usd)
            continue;
        s += *m.flow_usd;
        ++n;
        revised = revised || m.quality == QLatin1String("REVISED");
    }
    if (n == 3)
        row.measured.sum_3m = make(s, latest->month, revised);
    else
        row.measured.sum_3m = ResearchValue::unavailable(QStringLiteral("three_months_not_all_available"), method);
}

void etfr_cross_check(UniverseRow& row, const QVector<QPair<QDate, double>>& ibkr, const BarSeries* bars) {
    if (ibkr.size() < 22 || !bars) {
        row.cross_check = ResearchValue::unavailable(
            ibkr.isEmpty() ? QStringLiteral("no_ibkr_bars")
                           : (!bars ? QStringLiteral("no_yahoo_history") : QStringLiteral("insufficient_ibkr_bars")),
            QStringLiteral("etfr_ibkr_yahoo_crosscheck_v1"));
        return;
    }
    // 21-session price-return difference on the latest common session.
    QHash<QDate, double> y;
    for (const auto& b : bars->bars)
        y[b.date] = b.close;
    QVector<QPair<double, double>> common;
    QDate last;
    for (const auto& p : ibkr)
        if (y.contains(p.first)) {
            common.append({p.second, y[p.first]});
            last = p.first;
        }
    if (common.size() < 22) {
        row.cross_check = ResearchValue::unavailable(QStringLiteral("insufficient_common_sessions"),
                                                     QStringLiteral("etfr_ibkr_yahoo_crosscheck_v1"));
        return;
    }
    const auto& a = common[common.size() - 22];
    const auto& b = common.last();
    const double diff = ((b.first / a.first) - (b.second / a.second)) * 100.0;
    row.cross_check =
        graded(diff, EvidenceClass::Proxy, Credibility::High, {}, QStringLiteral("pp"),
               QStringLiteral("etfr_ibkr_yahoo_crosscheck_v1"), QStringLiteral("ibkr_tws_readonly+yahoo_chart"), last);
}

} // namespace

ResearchSnapshot compute_snapshot(const ResearchInputs& in) {
    ResearchSnapshot s;
    s.engine_version = QLatin1String(kResearchEngineVersion);
    s.universe_version = in.universe.version;
    s.reference_commit = in.universe.reference_commit;
    s.as_of = in.as_of;
    s.known_at = in.known_at;
    s.computed_at = QDateTime::currentDateTimeUtc();
    s.expected_us_session = in.expected_us_session;
    s.sources = in.last_refresh;
    s.group_flows = in.group_flows;
    s.group_flows_loaded = in.group_flows_loaded;
    s.last_refresh_run_id = in.last_refresh_run_id;
    s.last_refresh_finished = in.last_refresh_finished;
    s.warnings = in.load_warnings;
    const QDate as_of_date = in.as_of.toUTC().date();

    QHash<QString, TrIndex> idx;
    for (auto it = in.bars.begin(); it != in.bars.end(); ++it)
        idx.insert(it.key(), make_tr_index(it.value()));
    auto tr = [&](const QString& sym) -> const TrIndex* {
        const auto it = idx.constFind(sym);
        return it == idx.constEnd() || it->empty() ? nullptr : &it.value();
    };
    auto stale_of = [&](const QString& sym) {
        const TrIndex* t = tr(sym);
        return series_is_stale(sym, t ? t->last() : QDate(), in.expected_us_session, as_of_date);
    };

    // ── Universe rows ────────────────────────────────────────────────────────
    for (const UniverseInstrument& inst : in.universe.instruments) {
        UniverseRow row;
        row.inst = inst;
        row.peer_group = peer_group_of(inst);
        const TrIndex* t = tr(inst.symbol);
        const auto bit = in.bars.constFind(inst.symbol);
        const BarSeries* bars = bit == in.bars.constEnd() ? nullptr : &bit.value();
        row.last_bar = t ? t->last() : QDate();
        row.stale = stale_of(inst.symbol);
        row.freshness =
            !t ? QStringLiteral("no_history") : (row.stale ? QStringLiteral("stale") : QStringLiteral("fresh"));
        const QVector<CredCondition> conds = base_conditions(row.stale && t, t && t->revised);
        const QString bench = inst.benchmark;
        const TrIndex* b = bench.isEmpty() ? nullptr : tr(bench);
        if (t) {
            row.ret = compute_returns(*t, b, bench, conds);
            row.rrg = compute_rrg(*t, b, bench, conds);
            const auto above = above_moving_average(*t);
            row.above_200dma = above ? graded(*above ? 1.0 : 0.0, EvidenceClass::Proxy, Credibility::High, conds,
                                              QStringLiteral("flag"), QLatin1String(kRotationMethod),
                                              QLatin1String(kSourceYahoo), t->last())
                                     : short_history_value(*t, QLatin1String(kRotationMethod));
            row.above_200dma.flags.removeAll(QLatin1String(flag::kActualZero));
            const auto cy = trailing_distribution_yield(*t);
            row.carry_12m = cy ? graded(*cy, EvidenceClass::Proxy, Credibility::Medium, conds, QStringLiteral("pct"),
                                        QStringLiteral("etfr_trailing_distribution_yield_v1"),
                                        QLatin1String(kSourceYahoo), t->last())
                               : short_history_value(*t, QStringLiteral("etfr_trailing_distribution_yield_v1"));
        } else {
            row.ret = compute_returns(TrIndex{}, nullptr, bench, {});
            row.rrg.quadrant = ResearchValue::unavailable(QStringLiteral("no_observation"), QLatin1String(kRrgMethod));
            row.rrg.quadrant.add_flag(flag::kNoObservation);
            row.above_200dma = ResearchValue::unavailable(QStringLiteral("no_observation"));
            row.carry_12m = row.above_200dma;
        }
        row.history_status = bars ? QStringLiteral("stored") : QStringLiteral("no_observation");
        if (t) {
            // Chart series: subject (and benchmark on common sessions), last 260, rebased to 100.
            if (b && b != t) {
                const Aligned al = align(*t, *b);
                const int start = std::max(0, static_cast<int>(al.dates.size()) - 260);
                for (int i = start; i < al.dates.size(); ++i) {
                    row.chart_dates.append(al.dates[i]);
                    row.chart_tr.append(100.0 * al.a[i] / al.a[start]);
                    row.chart_bench.append(100.0 * al.b[i] / al.b[start]);
                }
            } else {
                const int start = std::max(0, t->size() - 260);
                for (int i = start; i < t->size(); ++i) {
                    row.chart_dates.append(t->dates[i]);
                    row.chart_tr.append(100.0 * t->tr[i] / t->tr[start]);
                }
            }
        }
        if (inst.is_fund()) {
            const auto caps = in.funds.value(inst.symbol);
            const auto hit = in.holdings.constFind(inst.symbol);
            const HoldingsCapture* h = hit == in.holdings.constEnd() ? nullptr : &hit.value();
            row.fund = fund_facts_from_captures(caps, bars, h, in.expected_us_session);
            row.fund.holdings_read_failed = in.holdings_read_failed.value(inst.symbol);
            row.est =
                estimate_flow(caps, bars ? *bars : BarSeries{}, in.measured.value(inst.symbol), in.expected_us_session);
        } else {
            row.fund = fund_facts_from_captures({}, nullptr, nullptr, QDate());
            row.est.latest = ResearchValue::unavailable(QStringLiteral("not_a_fund"));
            row.est.latest.add_flag(flag::kNotApplicable);
            row.est.sum_5 = row.est.sum_20 = row.est.pct_aum_20 = row.est.coverage_20 = row.est.latest_e2 =
                row.est.latest_e3 = row.est.validation_vs_measured = row.est.best = row.est.latest;
        }
        etfr_measured(row, in.measured.value(inst.symbol));
        etfr_cross_check(row, in.ibkr_close.value(inst.symbol), bars);
        if (row.cross_check.value && std::abs(*row.cross_check.value) > 0.25) {
            // A second source disagrees with Yahoo over the same month: lower the returns.
            for (ResearchValue* v : {&row.ret.d1, &row.ret.w1, &row.ret.m1, &row.ret.m3, &row.ret.rel_m1}) {
                if (!v->value)
                    continue;
                v->credibility =
                    grade_credibility(v->credibility, {CredCondition::CrossSourceDisagree}, &v->credibility_reasons);
            }
        }
        if (const auto cm = in.universe.cftc_market.constFind(inst.symbol); cm != in.universe.cftc_market.constEnd())
            row.cftc = cftc_positioning(*cm, in.cftc.value(*cm), in.as_of.toUTC().date());
        // The same rule the FLOW view shows (best_flow_evidence); funds only.
        if (row.inst.is_fund()) {
            const ResearchValue& best = best_flow_evidence(row);
            row.flow_evidence = best.usable() ? best.evidence : EvidenceClass::Unavailable;
        }
        s.rows.append(row);
    }

    // ── Momentum per peer group ──────────────────────────────────────────────
    for (const QString& group : {QStringLiteral("us_sector"), QStringLiteral("theme"), QStringLiteral("country")}) {
        const bool reversal = group != QLatin1String("country");
        const double clamp = group == QLatin1String("country") ? 2.0 : 2.5;
        QVector<int> members;
        std::vector<std::optional<double>> raw;
        QVector<MomentumParts> parts;
        for (int i = 0; i < s.rows.size(); ++i) {
            const auto& r = s.rows[i];
            if (r.peer_group != group)
                continue;
            members.append(i);
            const TrIndex* t = tr(r.inst.symbol);
            const MomentumParts p = t ? momentum_parts(*t, reversal) : MomentumParts{};
            parts.append(p);
            raw.push_back(p.composite);
        }
        const auto z = math::cross_section_z(raw, clamp);
        const int defined =
            static_cast<int>(std::count_if(raw.begin(), raw.end(), [](const auto& v) { return v.has_value(); }));
        for (int k = 0; k < members.size(); ++k) {
            UniverseRow& r = s.rows[members[k]];
            QVector<CredCondition> conds = base_conditions(r.stale && r.last_bar.isValid(), false);
            if (defined < members.size())
                conds.append(CredCondition::Partial);
            const QString method = QLatin1String(kMomentumMethod) +
                                   (reversal ? QStringLiteral(":reversal") : QStringLiteral(":no_reversal"));
            r.momentum_raw =
                model_value(parts[k].composite ? std::optional<double>(*parts[k].composite * 100.0) : std::nullopt,
                            Credibility::Medium, conds, QStringLiteral("pct"), method, QLatin1String(kSourceYahoo),
                            r.last_bar, QStringLiteral("insufficient_history"));
            r.momentum_z = model_value(z[static_cast<size_t>(k)], Credibility::Medium, conds, QStringLiteral("z"),
                                       method, QLatin1String(kSourceYahoo), r.last_bar,
                                       parts[k].composite ? QStringLiteral("cross_section_undefined")
                                                          : QStringLiteral("insufficient_history"));
        }
    }
    for (auto& r : s.rows) {
        if (!r.momentum_z.usable() && r.momentum_z.reason.isEmpty())
            r.momentum_z = ResearchValue::unavailable(QStringLiteral("no_comparable_peer_group"));
        if (!r.momentum_raw.usable() && r.momentum_raw.reason.isEmpty())
            r.momentum_raw = ResearchValue::unavailable(QStringLiteral("no_comparable_peer_group"));
    }

    // ── Turnover, tilt, breadth (U.S. sector complex) ───────────────────────
    QStringList sector_syms, defensive, cyclical;
    for (const auto& r : s.rows)
        if (r.inst.has_role("us_sector")) {
            sector_syms.append(r.inst.symbol);
            if (r.inst.bucket == QLatin1String("defensive"))
                defensive.append(r.inst.symbol);
            else if (r.inst.bucket == QLatin1String("cyclical"))
                cyclical.append(r.inst.symbol);
        }
    {
        QHash<QString, TrIndex> sidx;
        bool all = true;
        bool any_stale = false;
        for (const auto& sym : sector_syms) {
            if (!tr(sym))
                all = false;
            else
                sidx.insert(sym, *tr(sym));
            any_stale = any_stale || stale_of(sym);
        }
        const QVector<CredCondition> conds = base_conditions(any_stale, false);
        if (all && !sector_syms.isEmpty()) {
            const TurnoverResult t = compute_turnover(sidx, sector_syms, defensive, cyclical, 20, 10.0, conds);
            for (auto& r : s.rows)
                if (r.inst.has_role("us_sector")) {
                    r.turnover_share = t.share.value(r.inst.symbol);
                    r.turnover_delta_bp = t.delta_bp.value(r.inst.symbol);
                    r.turnover_z = t.z.value(r.inst.symbol);
                }
            s.tilt.tilt_bp = t.tilt_bp;
            s.tilt.tilt_z = t.tilt_z;
            s.tilt.state = t.tilt_state;
        } else {
            s.tilt.tilt_bp =
                ResearchValue::unavailable(QStringLiteral("sector_history_incomplete"), QLatin1String(kTurnoverMethod));
            s.tilt.tilt_z = s.tilt.tilt_bp;
            for (auto& r : s.rows)
                if (r.inst.has_role("us_sector"))
                    r.turnover_share = r.turnover_delta_bp = r.turnover_z = s.tilt.tilt_bp;
        }
        for (const QString q : {QStringLiteral("Leading"), QStringLiteral("Improving"), QStringLiteral("Weakening"),
                                QStringLiteral("Lagging")})
            s.tilt.quadrants.insert(q, 0);
        for (const auto& r : s.rows) {
            if (!r.inst.has_role("us_sector"))
                continue;
            if (!r.rrg.quadrant.label.isEmpty())
                s.tilt.quadrants[r.rrg.quadrant.label] += 1;
            if (r.ret.rel_m1.value) {
                ++s.tilt.breadth_total;
                if (*r.ret.rel_m1.value > 0)
                    ++s.tilt.above_bench;
            }
        }
    }
    for (auto& r : s.rows)
        if (!r.inst.has_role("us_sector")) {
            r.turnover_share = ResearchValue::unavailable(QStringLiteral("sector_complex_only"));
            r.turnover_share.add_flag(flag::kNotApplicable);
            r.turnover_delta_bp = r.turnover_z = r.turnover_share;
        }

    // ── Sector models ────────────────────────────────────────────────────────
    {
        QHash<QString, ResearchValue> bc_z;
        s.sectors.cycle =
            compute_business_cycle(in.fred, tr(QStringLiteral("HYG")), tr(QStringLiteral("LQD")), as_of_date, &bc_z);
        int above = 0, known = 0;
        QHash<QString, TrIndex> sector_idx;
        for (const auto& sym : sector_syms) {
            if (const TrIndex* t = tr(sym)) {
                sector_idx.insert(sym, *t);
                if (const auto a = above_moving_average(*t)) {
                    ++known;
                    above += *a ? 1 : 0;
                }
            }
        }
        if (known == sector_syms.size() && known > 0)
            s.sectors.cycle.sectors_above_200dma = graded(
                static_cast<double>(above) / known, EvidenceClass::Proxy, Credibility::High, {},
                QStringLiteral("fraction"), QLatin1String(kRotationMethod), QLatin1String(kSourceYahoo), QDate());
        else
            s.sectors.cycle.sectors_above_200dma =
                ResearchValue::unavailable(QStringLiteral("sector_history_incomplete"));
        FactorModelResult fm;
        if (const TrIndex* spy = tr(in.universe.bench_us))
            fm = compute_factor_model(in.fred, sector_idx, *spy, as_of_date);
        s.sectors.factor_state = fm.factor_state;
        s.sectors.factor_names = fm.factor_names;

        // Valuation: own-history P/E z when 20 capture sessions over 60+ days exist,
        // else the cross-sectional earnings-yield spread over the FRED 10-year yield.
        QDate d10;
        const math::Vec y10 = macro_values(series_ptr(in.fred, "DGS10"), as_of_date, &d10);
        std::vector<std::optional<double>> ey_spread;
        QStringList keys;
        QHash<QString, std::optional<double>> own_z;
        QHash<QString, std::optional<double>> pe_now;
        for (const auto& sym : sector_syms) {
            keys.append(sym);
            const auto caps = in.funds.value(sym);
            std::optional<double> pe;
            QHash<QDate, double> by_session;
            for (const auto& c : caps)
                if (const auto v = json_num(c.fields, "trailingPE"); v && *v > 5 && *v < 200) {
                    by_session[c.effective_session] = *v;
                    pe = *v;
                }
            pe_now[sym] = pe;
            if (pe && !y10.empty())
                ey_spread.push_back(100.0 / *pe - y10.back());
            else
                ey_spread.push_back(std::nullopt);
            if (by_session.size() >= 20) {
                QList<QDate> ds = by_session.keys();
                std::sort(ds.begin(), ds.end());
                if (ds.first().daysTo(ds.last()) >= 60) {
                    math::Vec hist;
                    for (const QDate& d : ds)
                        hist.push_back(by_session[d]);
                    if (const auto z = math::zscore_last(hist, hist.size(), 20))
                        own_z[sym] = -*z; // contrarian: expensive vs own history = negative
                }
            }
        }
        const auto ey_z = math::cross_section_z(ey_spread, 2.5);
        const QHash<QString, double> w_normal{{QStringLiteral("BC"), 0.30},
                                              {QStringLiteral("MOM"), 0.35},
                                              {QStringLiteral("VAL"), 0.20},
                                              {QStringLiteral("F"), 0.15}};
        const QHash<QString, double> w_crisis{{QStringLiteral("BC"), 0.35},
                                              {QStringLiteral("MOM"), 0.15},
                                              {QStringLiteral("VAL"), 0.30},
                                              {QStringLiteral("F"), 0.20}};
        s.sectors.volatility_brake = s.sectors.cycle.vix.value && *s.sectors.cycle.vix.value > 35.0;
        s.sectors.weights_used = s.sectors.volatility_brake ? w_crisis : w_normal;
        for (int k = 0; k < keys.size(); ++k) {
            const QString& sym = keys[k];
            const UniverseRow* ur = s.row(sym);
            SectorModelRow m;
            m.symbol = sym;
            m.name = ur ? ur->inst.sector : sym;
            m.bc_z = bc_z.value(sym, ResearchValue::unavailable(QStringLiteral("no_stovall_mapping")));
            m.mom_z = ur ? ur->momentum_z : ResearchValue::unavailable(QStringLiteral("no_row"));
            m.factor_z = fm.sector_z.value(sym, ResearchValue::unavailable(QStringLiteral("factor_model_unavailable")));
            m.factor_betas = fm.betas.value(sym);
            m.factor_tstats = fm.tstats.value(sym);
            m.factor_r2 = fm.r2.value(sym);
            m.pe = pe_now[sym] ? graded(*pe_now[sym], EvidenceClass::Measured, Credibility::NotGraded, {},
                                        QStringLiteral("ratio"), QStringLiteral("yahoo_quote_summary:trailingPE"),
                                        kFundSource, QDate())
                               : ResearchValue::unavailable(QStringLiteral("pe_missing_or_outside_5_200"));
            m.pe.credibility_reasons.clear();
            if (m.pe.usable())
                m.pe.source_quality = QLatin1String(quality::kProviderUndated);
            if (ey_spread[static_cast<size_t>(k)]) {
                QVector<CredCondition> c;
                if (macro_input_stale(d10, as_of_date, 7))
                    c.append(CredCondition::MacroStale);
                m.earnings_yield_spread = graded(
                    *ey_spread[static_cast<size_t>(k)], EvidenceClass::Model, Credibility::Low, c, QStringLiteral("pp"),
                    QLatin1String(kValuationMethod), QStringLiteral("yahoo_quote_summary+fred_DGS10"), d10);
            } else {
                m.earnings_yield_spread = ResearchValue::unavailable(QStringLiteral("pe_or_10y_missing"));
            }
            if (own_z.contains(sym) && own_z[sym]) {
                m.pe_own_z =
                    graded(*own_z[sym], EvidenceClass::Model, Credibility::Medium, {}, QStringLiteral("z"),
                           QLatin1String(kValuationMethod) + QStringLiteral(":own_history"), kFundSource, QDate());
                m.val_z = m.pe_own_z;
            } else {
                m.pe_own_z =
                    ResearchValue::unavailable(QStringLiteral("own_pe_history_insufficient_20_sessions_60_days"));
                m.val_z = model_value(
                    ey_z[static_cast<size_t>(k)], Credibility::Low, {CredCondition::Unvalidated}, QStringLiteral("z"),
                    QLatin1String(kValuationMethod) + QStringLiteral(":cross_section_ey_spread"),
                    QStringLiteral("yahoo_quote_summary+fred_DGS10"), d10, QStringLiteral("valuation_inputs_missing"));
            }
            double disp = 0;
            m.confluence = confluence_score({{QStringLiteral("BC"), m.bc_z},
                                             {QStringLiteral("MOM"), m.mom_z},
                                             {QStringLiteral("VAL"), m.val_z},
                                             {QStringLiteral("F"), m.factor_z}},
                                            s.sectors.weights_used, &disp);
            m.layer_dispersion = graded(disp, EvidenceClass::Model, Credibility::Medium, {}, QStringLiteral("z"),
                                        QLatin1String(kConfluenceMethod), QStringLiteral("model_layers"), QDate());
            if (m.confluence.value)
                m.band = model_band(*m.confluence.value);
            if (m.bc_z.value && m.mom_z.value && std::abs(*m.bc_z.value) > 0.3 && std::abs(*m.mom_z.value) > 0.3 &&
                (*m.bc_z.value > 0) != (*m.mom_z.value > 0)) {
                m.conflicts.append(QStringLiteral("BC_vs_MOM"));
                s.sectors.bc_mom_conflicts.append(sym);
            }
            s.sectors.rows.append(m);
        }
        // Rank and top-layer divergence.
        QVector<int> order;
        for (int i = 0; i < s.sectors.rows.size(); ++i)
            if (s.sectors.rows[i].confluence.value)
                order.append(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return *s.sectors.rows[a].confluence.value > *s.sectors.rows[b].confluence.value;
        });
        for (int r = 0; r < order.size(); ++r)
            s.sectors.rows[order[r]].rank = r + 1;
        auto top_of = [&](auto getter) {
            QString best;
            double v = -1e9;
            for (const auto& m : s.sectors.rows)
                if (const ResearchValue& x = getter(m); x.value && *x.value > v) {
                    v = *x.value;
                    best = m.symbol;
                }
            return best;
        };
        s.sectors.top_momentum = top_of([](const SectorModelRow& m) -> const ResearchValue& { return m.mom_z; });
        s.sectors.top_valuation = top_of([](const SectorModelRow& m) -> const ResearchValue& { return m.val_z; });
        auto conf_of = [&](const QString& sym) -> std::optional<double> {
            for (const auto& m : s.sectors.rows)
                if (m.symbol == sym)
                    return m.confluence.value;
            return std::nullopt;
        };
        const auto cm = conf_of(s.sectors.top_momentum), cv = conf_of(s.sectors.top_valuation);
        s.sectors.mom_val_divergence = !s.sectors.top_momentum.isEmpty() &&
                                       s.sectors.top_momentum != s.sectors.top_valuation && cm && cv && *cm > 0.3 &&
                                       *cv > 0.3;
        for (auto& r : s.rows)
            for (const auto& m : s.sectors.rows)
                if (m.symbol == r.inst.symbol && r.inst.has_role("us_sector")) {
                    r.model_score = m.confluence;
                    r.model_band = m.band;
                }
        s.sectors.notes.append(QStringLiteral("Stovall favourability, confluence weights and the VIX brake are the "
                                              "reference's stylised settings, not validated by MarketLab."));
    }

    // ── Country models ───────────────────────────────────────────────────────
    {
        QVector<int> rows;
        for (int i = 0; i < s.rows.size(); ++i)
            if (s.rows[i].inst.has_role("country"))
                rows.append(i);
        const QDate as_of_d = as_of_date;
        std::vector<std::optional<double>> gdp(rows.size()), ca(rows.size()), carry(rows.size()), mom(rows.size());
        QVector<int> gdp_year(rows.size(), 0), ca_year(rows.size(), 0);
        QVector<bool> ca_revised(rows.size(), false);
        QVector<bool> macro_stale(rows.size(), false);
        auto wb_series = [&](const char* ind, const QString& iso2) -> const MacroSeries* {
            const auto it = in.world_bank.constFind(QLatin1String(ind));
            if (it == in.world_bank.constEnd())
                return nullptr;
            const auto jt = it->constFind(iso2);
            return jt == it->constEnd() ? nullptr : &jt.value();
        };
        for (int k = 0; k < rows.size(); ++k) {
            const UniverseRow& r = s.rows[rows[k]];
            const TrIndex* t = tr(r.inst.symbol);
            if (t)
                mom[static_cast<size_t>(k)] = momentum_parts(*t, false).composite;
            if (t)
                carry[static_cast<size_t>(k)] = trailing_distribution_yield(*t);
            if (r.inst.wb_code.isEmpty() || r.inst.country_type != QLatin1String("single"))
                continue;
            if (const MacroSeries* g = wb_series("NY.GDP.MKTP.KD.ZG", r.inst.wb_code); g && g->points.size() >= 3) {
                double sum = 0;
                for (int i = g->points.size() - 3; i < g->points.size(); ++i)
                    sum += g->points[i].value;
                gdp[static_cast<size_t>(k)] = sum / 3.0;
                gdp_year[k] = g->points.last().date.year();
            }
            if (const MacroSeries* c = wb_series("BN.CAB.XOKA.GD.ZS", r.inst.wb_code); c && !c->points.isEmpty()) {
                ca[static_cast<size_t>(k)] = c->points.last().value;
                ca_year[k] = c->points.last().date.year();
                ca_revised[k] = c->points.last().revised;
            }
            macro_stale[k] =
                (gdp_year[k] && gdp_year[k] < as_of_d.year() - 2) || (ca_year[k] && ca_year[k] < as_of_d.year() - 2);
        }
        const auto mz = math::cross_section_z(mom, 2.0);
        const auto gz = math::cross_section_z(gdp, 2.0);
        const auto cz = math::cross_section_z(ca, 2.0);
        std::vector<std::optional<double>> qraw(rows.size());
        for (size_t k = 0; k < static_cast<size_t>(rows.size()); ++k)
            if (gz[k] && cz[k])
                qraw[k] = 0.6 * *gz[k] + 0.4 * *cz[k];
        const auto qz = math::cross_section_z(qraw, 2.0);
        const auto carz = math::cross_section_z(carry, 2.0);
        const QString method = QLatin1String(kCountryMethod);
        for (int k = 0; k < rows.size(); ++k) {
            const size_t u = static_cast<size_t>(k);
            const UniverseRow& r = s.rows[rows[k]];
            CountryRow c;
            c.symbol = r.inst.symbol;
            c.country = r.inst.country;
            c.region = r.inst.country_region;
            c.type = r.inst.country_type;
            c.gdp_year = gdp_year[k];
            c.ca_year = ca_year[k];
            const QVector<CredCondition> base = base_conditions(r.stale && r.last_bar.isValid(), false);
            c.m_z = model_value(mz[u], Credibility::Medium, base, QStringLiteral("z"), method + QStringLiteral(":M"),
                                QLatin1String(kSourceYahoo), r.last_bar, QStringLiteral("insufficient_history"));
            QVector<CredCondition> qc;
            if (macro_stale[k])
                qc.append(CredCondition::MacroStale);
            QString q_missing = QStringLiteral("world_bank_inputs_missing");
            if (r.inst.country_type == QLatin1String("regional"))
                q_missing = QStringLiteral("not_applicable_regional_fund");
            else if (r.inst.wb_code == QLatin1String("TW"))
                q_missing = QStringLiteral("world_bank_publishes_no_taiwan_series");
            c.q_z = model_value(qz[u], Credibility::Low, qc, QStringLiteral("z"), method + QStringLiteral(":Q"),
                                QLatin1String(kSourceWorldBank), gdp_year[k] ? QDate(gdp_year[k], 12, 31) : QDate(),
                                q_missing);
            if (r.inst.country_type == QLatin1String("regional"))
                c.q_z.add_flag(flag::kNotApplicable);
            c.c_z = model_value(carz[u], Credibility::Medium, base, QStringLiteral("z"), method + QStringLiteral(":C"),
                                QLatin1String(kSourceYahoo), r.last_bar, QStringLiteral("insufficient_history"));
            // A three-year mean is a calculated statistic of measured values, not an observation.
            c.gdp_3y = gdp[u] ? graded(*gdp[u], EvidenceClass::Proxy, Credibility::High, qc, QStringLiteral("pct"),
                                       QStringLiteral("world_bank:NY.GDP.MKTP.KD.ZG:3y_mean"),
                                       QLatin1String(kSourceWorldBank), QDate(gdp_year[k], 12, 31))
                              : ResearchValue::unavailable(q_missing);
            c.current_account = ca[u]
                                    ? graded(*ca[u], EvidenceClass::Measured, Credibility::NotGraded, qc,
                                             QStringLiteral("pct_gdp"), QStringLiteral("world_bank:BN.CAB.XOKA.GD.ZS"),
                                             QLatin1String(kSourceWorldBank), QDate(ca_year[k], 12, 31))
                                    : ResearchValue::unavailable(q_missing);
            if (c.current_account.usable())
                c.current_account.source_quality =
                    QLatin1String(ca_revised[k] ? quality::kRevised : quality::kPublished);
            c.carry = carry[u] ? graded(*carry[u], EvidenceClass::Proxy, Credibility::Medium, base,
                                        QStringLiteral("pct"), QStringLiteral("etfr_trailing_distribution_yield_v1"),
                                        QLatin1String(kSourceYahoo), r.last_bar)
                               : ResearchValue::unavailable(QStringLiteral("insufficient_history"));
            // Composite: single .5M+.3Q+.2C, regional .7M+.3C; renormalise missing (PARTIAL), M required.
            const bool single = r.inst.country_type == QLatin1String("single");
            QVector<QPair<double, std::optional<double>>> parts{{single ? 0.5 : 0.7, c.m_z.value}};
            if (single)
                parts.append({0.3, c.q_z.value});
            parts.append({single ? 0.2 : 0.3, c.c_z.value});
            double w = 0, sum = 0;
            bool missing = false;
            for (const auto& p : parts) {
                if (!p.second) {
                    missing = true;
                    continue;
                }
                w += p.first;
                sum += p.first * *p.second;
            }
            if (!c.m_z.value || !(w > 0)) {
                c.composite = ResearchValue::unavailable(QStringLiteral("momentum_layer_unavailable"), method);
            } else {
                QVector<CredCondition> cc = base;
                cc.append(CredCondition::Unvalidated);
                if (missing)
                    cc.append(CredCondition::Partial);
                if (macro_stale[k])
                    cc.append(CredCondition::MacroStale);
                c.composite = graded(sum / w, EvidenceClass::Model, Credibility::Low, cc, QStringLiteral("z"), method,
                                     QStringLiteral("yahoo_chart+world_bank"), r.last_bar);
                c.band = model_band(*c.composite.value);
            }
            s.countries.append(c);
        }
        QVector<int> order;
        for (int i = 0; i < s.countries.size(); ++i)
            if (s.countries[i].composite.value)
                order.append(i);
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return *s.countries[a].composite.value > *s.countries[b].composite.value; });
        for (int r = 0; r < order.size(); ++r)
            s.countries[order[r]].rank = r + 1;
        for (auto& r : s.rows)
            if (r.peer_group == QLatin1String("country"))
                for (const auto& c : s.countries)
                    if (c.symbol == r.inst.symbol) {
                        r.model_score = c.composite;
                        r.model_band = c.band;
                    }
        for (auto& r : s.rows)
            if (!r.model_score.usable() && r.model_score.reason.isEmpty())
                r.model_score = ResearchValue::unavailable(QStringLiteral("no_model_for_this_role"));
    }

    // ── Regime ───────────────────────────────────────────────────────────────
    {
        QVector<const TrIndex*> sectors;
        for (const auto& sym : sector_syms)
            if (const TrIndex* t = tr(sym))
                sectors.append(t);
        QVector<const TrIndex*> core;
        for (const char* sym : {"XLK", "XLF", "XLV", "XLE", "XLI", "XLY", "XLP", "XLU", "XLB"})
            if (const TrIndex* t = tr(QLatin1String(sym)))
                core.append(t);
        MrsFit mrs;
        if (core.size() == 9)
            mrs = fit_mrs(rolling_avg_abs_corr(core));
        s.regime.mrs_ready = mrs.ready;
        s.regime.mrs_thresholds = mrs.thresholds;
        s.regime.mrs_threshold_std = mrs.threshold_std;
        s.regime.mrs_means = mrs.means;
        const struct {
            const char* period;
            int n;
            bool weekly;
        } periods[] = {{"1m", 21, false}, {"3m", 63, false}, {"6m", 126, false}, {"1y", 252, false}, {"2y", 104, true}};
        for (const auto& p : periods)
            s.regime.views.append(correlation_view(QLatin1String(p.period), sectors, p.n, p.weekly,
                                                   mrs.ready ? &mrs.thresholds : nullptr));
        // MRS label on its own basis: 9 core sectors, 63 sessions.
        if (mrs.ready && core.size() == 9) {
            const CorrelationView cv = correlation_view(QStringLiteral("3m_core9"), core, 63, false, &mrs.thresholds);
            s.regime.mrs_label = cv.corr_label;
            bool unstable = false;
            for (double sd : mrs.threshold_std)
                unstable = unstable || sd < 0 || sd > 0.05;
            if (unstable && s.regime.mrs_label.value)
                s.regime.mrs_label.credibility =
                    grade_credibility(s.regime.mrs_label.credibility, {CredCondition::Unvalidated},
                                      &s.regime.mrs_label.credibility_reasons);
            if (mrs.hmm && cv.avg_abs_corr.value) {
                // Current state by threshold band; 20-step transition probabilities from it.
                int st = 0;
                for (int k = 0; k < 3; ++k)
                    if (*cv.avg_abs_corr.value > mrs.thresholds[k])
                        st = k + 1;
                math::Mat a = mrs.hmm->trans;
                math::Mat p = a;
                for (int step = 1; step < 20; ++step) {
                    math::Mat n(4, math::Vec(4, 0.0));
                    for (int i = 0; i < 4; ++i)
                        for (int j = 0; j < 4; ++j)
                            for (int k = 0; k < 4; ++k)
                                n[i][j] += p[i][k] * a[k][j];
                    p = n;
                }
                for (int j = 0; j < 4; ++j)
                    s.regime.mrs_transition_20d.append(p[static_cast<size_t>(st)][static_cast<size_t>(j)]);
            }
        } else {
            s.regime.mrs_label = ResearchValue::unavailable(QStringLiteral("mrs_history_insufficient"), kMrsMethod);
        }
        auto risk = [&](const QString& period, bool corr) -> std::optional<double> {
            for (const auto& v : s.regime.views)
                if (v.period == period) {
                    if (corr)
                        return v.avg_abs_corr.value;
                    return v.geom_score.value ? std::optional<double>(1.0 - *v.geom_score.value) : std::nullopt;
                }
            return std::nullopt;
        };
        auto trend = [&](bool corr, double* delta) {
            const auto r1 = risk(QStringLiteral("1m"), corr), r12 = risk(QStringLiteral("1y"), corr);
            if (!r1 || !r12)
                return ResearchValue::unavailable(QStringLiteral("trend_inputs_missing"), kRegimeMethod);
            *delta = *r1 - *r12;
            const QString label = std::abs(*delta) < 0.05 ? QStringLiteral("STABLE")
                                                          : (*delta > 0 ? QStringLiteral("CORRELATION RISING")
                                                                        : QStringLiteral("CORRELATION FALLING"));
            return model_label(label, *delta, Credibility::Medium, {CredCondition::HeuristicThreshold},
                               QLatin1String(kRegimeMethod), QDate());
        };
        s.regime.trend_corr = trend(true, &s.regime.risk_delta_corr);
        s.regime.trend_geom = trend(false, &s.regime.risk_delta_geom);
        const auto gr = risk(QStringLiteral("3m"), false), cr = risk(QStringLiteral("3m"), true);
        if (gr && cr) {
            s.regime.risk_gap = std::abs(*gr - *cr);
            s.regime.corr_geom_conflict = s.regime.risk_gap > 0.20;
        }
        const TrIndex* spy = tr(QStringLiteral("SPY"));
        const TrIndex* hyg = tr(QStringLiteral("HYG"));
        const TrIndex* ief = tr(QStringLiteral("IEF"));
        if (core.size() == 9 && spy && hyg && ief)
            s.regime.v2 = compute_regime_v2(core, *spy, *hyg, *ief, series_ptr(in.fred, "VIXCLS"));
        else
            s.regime.v2.label = ResearchValue::unavailable(QStringLiteral("regime_inputs_missing"), kRegimeV2Method);
    }

    // ── Thai equal-weight baskets ────────────────────────────────────────────
    {
        const TrIndex* th = tr(in.universe.bench_thailand);
        const TrIndex* thf = tr(in.universe.bench_thailand_fallback);
        const TrIndex* bench = nullptr;
        if (th && th->size() >= 70) {
            bench = th;
            s.th_benchmark_used = in.universe.bench_thailand;
        } else if (thf && thf->size() >= 70) {
            bench = thf;
            s.th_benchmark_used = in.universe.bench_thailand_fallback;
            s.th_benchmark_fallback = true;
        }
        for (const Basket& b : in.universe.baskets) {
            BasketRow br;
            br.basket = b;
            br.benchmark_used = s.th_benchmark_used;
            br.benchmark_fallback = s.th_benchmark_fallback;
            QVector<const TrIndex*> members;
            bool any_stale = false;
            for (const auto& m : b.members) {
                members.append(tr(m));
                any_stale = any_stale || (tr(m) && stale_of(m));
            }
            const TrIndex ew = equal_weight_basket(b.id, members, b.min_member_fraction, &br.members_available);
            QVector<CredCondition> conds = base_conditions(any_stale, false);
            if (br.members_available < b.members.size())
                conds.append(CredCondition::Partial);
            if (s.th_benchmark_fallback)
                conds.append(CredCondition::Fallback);
            br.ret = compute_returns(ew, bench, s.th_benchmark_used, conds);
            br.rrg = compute_rrg(ew, bench, s.th_benchmark_used, conds);
            s.baskets.append(br);
        }
    }

    // ── Constituents (top holdings of sector/theme/country funds) ───────────
    {
        const TrIndex* spy = tr(in.universe.bench_us);
        for (const UniverseRow& r : s.rows) {
            if (!(r.inst.has_role("us_sector") || r.inst.has_role("theme") || r.inst.has_role("country")))
                continue;
            const auto hit = in.holdings.constFind(r.inst.symbol);
            if (hit == in.holdings.constEnd())
                continue;
            ConstituentAggregate agg;
            agg.parent = r.inst.symbol;
            math::Vec pes, fpes, des;
            double cap_num = 0, cap_den = 0;
            QSet<QString> currencies;
            for (const Holding& h : hit->holdings) {
                if (h.rank > 10)
                    continue;
                ConstituentRow c;
                c.parent = r.inst.symbol;
                c.symbol = h.symbol;
                c.name = h.name;
                c.weight = h.weight;
                // The holding as Yahoo lists it may lack its exchange suffix: a
                // reviewed map gives the research symbol of the same security.
                const QString ys = in.universe.holding_research_symbol(h.symbol);
                c.research_symbol = ys;
                const auto fit = in.fundamentals.constFind(ys);
                const QJsonObject fo = fit == in.fundamentals.constEnd() ? QJsonObject() : fit->fields;
                c.currency = fo.value(QStringLiteral("currency")).toString();
                const TrIndex* t = in.universe.holding_symbol_excluded.contains(h.symbol) ? nullptr : tr(ys);
                const bool usd =
                    c.currency.isEmpty() ? !ys.contains(QLatin1Char('.')) : c.currency == QLatin1String("USD");
                if (in.universe.holding_symbol_excluded.contains(h.symbol)) {
                    c.ret = compute_returns(TrIndex{}, nullptr, QString(), {});
                    c.ret.m1 = ResearchValue::unavailable(QStringLiteral("non_equity_holding"));
                    c.ret.rel_m1 = c.ret.m1;
                } else if (t) {
                    c.ret = compute_returns(*t, usd ? spy : nullptr, usd ? in.universe.bench_us : QString(),
                                            base_conditions(stale_of(ys), t->revised));
                    if (!usd)
                        c.ret.rel_m1 = ResearchValue::unavailable(QStringLiteral("currency_differs_from_benchmark"));
                    if (usd && spy) {
                        const RrgResult rr = compute_rrg(*t, spy, in.universe.bench_us, {});
                        c.quadrant = rr.quadrant.label;
                    }
                } else {
                    c.ret = compute_returns(TrIndex{}, nullptr, QString(), {});
                }
                const auto pe = json_num(fo, "trailingPE");
                const auto fpe = json_num(fo, "forwardPE");
                const auto mc = json_num(fo, "marketCap");
                const auto de = json_num(fo, "debtToEquity");
                auto meas = [&](std::optional<double> v, const QString& units, const char* key) {
                    if (!v)
                        return ResearchValue::unavailable(QStringLiteral("field_not_supplied"));
                    ResearchValue r = graded(*v, EvidenceClass::Measured, Credibility::NotGraded, {}, units,
                                             QStringLiteral("yahoo_quote_summary:") + QLatin1String(key),
                                             QStringLiteral("yahoo_quote_summary"), QDate());
                    r.source_quality = QLatin1String(quality::kProviderUndated);
                    return r;
                };
                c.pe = meas(pe, QStringLiteral("ratio"), "trailingPE");
                c.forward_pe = meas(fpe, QStringLiteral("ratio"), "forwardPE");
                c.market_cap = meas(mc, QStringLiteral("local_currency"), "marketCap");
                if (pe && *pe > 0 && *pe < 10000) {
                    pes.push_back(*pe);
                    if (mc && *mc > 0) {
                        cap_num += *mc;
                        cap_den += *mc / *pe;
                    }
                }
                if (fpe && *fpe > 0 && *fpe < 10000)
                    fpes.push_back(*fpe);
                if (de && *de > 0 && *de < 10000)
                    des.push_back(*de);
                if (!c.currency.isEmpty())
                    currencies.insert(c.currency);
                if (h.weight && (pe || t))
                    agg.weight_covered += *h.weight;
                ++agg.count;
                s.constituents.append(c);
            }
            const QString m = QStringLiteral("etfr_constituent_aggregate_v1");
            auto med = [&](const math::Vec& v, const QString& why) {
                const auto x = math::median(v);
                return x ? graded(*x, EvidenceClass::Model, Credibility::Low,
                                  agg.count < 10 ? QVector<CredCondition>{CredCondition::Partial}
                                                 : QVector<CredCondition>{},
                                  QStringLiteral("ratio"), m, QStringLiteral("yahoo_quote_summary"), QDate())
                         : ResearchValue::unavailable(why, m);
            };
            agg.median_pe = med(pes, QStringLiteral("no_pe"));
            agg.median_forward_pe = med(fpes, QStringLiteral("no_forward_pe"));
            agg.median_debt_equity = med(des, QStringLiteral("no_debt_equity"));
            agg.currency_basis = currencies.size() == 1 ? *currencies.begin()
                                                        : (currencies.isEmpty() ? QString() : QStringLiteral("mixed"));
            if (currencies.size() == 1 && cap_den > 0)
                agg.cap_weighted_pe =
                    graded(cap_num / cap_den, EvidenceClass::Model, Credibility::Low, {}, QStringLiteral("ratio"), m,
                           QStringLiteral("yahoo_quote_summary"), QDate());
            else
                agg.cap_weighted_pe = ResearchValue::unavailable(
                    currencies.size() > 1 ? QStringLiteral("mixed_currency_market_caps") : QStringLiteral("no_caps"),
                    m);
            s.constituent_aggregates.append(agg);
        }
    }

    // ── International sector heatmaps (equal-weight of member stocks) ───────
    for (const QString& market : in.universe.intl_markets) {
        QMap<QString, QVector<IntlStock>> by_sector;
        QStringList sector_order;
        for (const IntlStock& st : in.universe.intl.value(market)) {
            if (!by_sector.contains(st.sector))
                sector_order.append(st.sector);
            by_sector[st.sector].append(st);
        }
        for (const QString& sec : sector_order) {
            IntlSector is;
            is.market = market;
            is.sector = sec;
            math::Vec d1, m1, m3, pes, fpes, des;
            QSet<QString> currencies;
            for (const IntlStock& st : by_sector[sec]) {
                ++is.members;
                ConstituentRow c;
                c.parent = market + QLatin1Char(':') + sec;
                c.symbol = st.symbol;
                c.name = st.label;
                const TrIndex* t = tr(st.symbol);
                const auto fit = in.fundamentals.constFind(st.symbol);
                const QJsonObject fo = fit == in.fundamentals.constEnd() ? QJsonObject() : fit->fields;
                c.currency = fo.value(QStringLiteral("currency")).toString();
                if (!c.currency.isEmpty())
                    currencies.insert(c.currency);
                if (t) {
                    ++is.members_with_data;
                    c.ret = compute_returns(*t, nullptr, QString(), base_conditions(stale_of(st.symbol), t->revised));
                    if (c.ret.d1.value)
                        d1.push_back(*c.ret.d1.value);
                    if (c.ret.m1.value)
                        m1.push_back(*c.ret.m1.value);
                    if (c.ret.m3.value)
                        m3.push_back(*c.ret.m3.value);
                } else {
                    c.ret = compute_returns(TrIndex{}, nullptr, QString(), {});
                }
                if (const auto pe = json_num(fo, "trailingPE"); pe && *pe > 0 && *pe < 10000)
                    pes.push_back(*pe);
                if (const auto fpe = json_num(fo, "forwardPE"); fpe && *fpe > 0 && *fpe < 10000)
                    fpes.push_back(*fpe);
                if (const auto de = json_num(fo, "debtToEquity"); de && *de > 0 && *de < 10000)
                    des.push_back(*de);
                c.pe = json_num(fo, "trailingPE")
                           ? graded(*json_num(fo, "trailingPE"), EvidenceClass::Measured, Credibility::NotGraded, {},
                                    QStringLiteral("ratio"), QStringLiteral("yahoo_quote_summary:trailingPE"),
                                    QStringLiteral("yahoo_quote_summary"), QDate())
                           : ResearchValue::unavailable(QStringLiteral("field_not_supplied"));
                if (c.pe.usable())
                    c.pe.source_quality = QLatin1String(quality::kProviderUndated);
                is.stocks.append(c);
            }
            auto ew = [&](const math::Vec& v, const QString& eff_method) {
                if (v.empty())
                    return ResearchValue::unavailable(QStringLiteral("no_member_data"), eff_method);
                QVector<CredCondition> c;
                if (static_cast<int>(v.size()) < is.members)
                    c.append(CredCondition::Partial);
                return graded(*math::mean(v), EvidenceClass::Proxy, Credibility::Medium, c, QStringLiteral("pct"),
                              eff_method, QLatin1String(kSourceYahoo), QDate());
            };
            const QString m = QStringLiteral("etfr_intl_sector_equal_weight_v1");
            is.ew_d1 = ew(d1, m);
            is.ew_m1 = ew(m1, m);
            is.ew_m3 = ew(m3, m);
            is.fundamentals.parent = is.market + QLatin1Char(':') + sec;
            is.fundamentals.count = is.members;
            is.fundamentals.median_pe =
                pes.empty() ? ResearchValue::unavailable(QStringLiteral("no_pe"))
                            : graded(*math::median(pes), EvidenceClass::Model, Credibility::Low, {},
                                     QStringLiteral("ratio"), m, QStringLiteral("yahoo_quote_summary"), QDate());
            is.fundamentals.median_forward_pe =
                fpes.empty() ? ResearchValue::unavailable(QStringLiteral("no_forward_pe"))
                             : graded(*math::median(fpes), EvidenceClass::Model, Credibility::Low, {},
                                      QStringLiteral("ratio"), m, QStringLiteral("yahoo_quote_summary"), QDate());
            is.fundamentals.median_debt_equity =
                des.empty() ? ResearchValue::unavailable(QStringLiteral("no_debt_equity"))
                            : graded(*math::median(des), EvidenceClass::Model, Credibility::Low, {},
                                     QStringLiteral("ratio"), m, QStringLiteral("yahoo_quote_summary"), QDate());
            is.fundamentals.cap_weighted_pe =
                ResearchValue::unavailable(currencies.size() > 1 ? QStringLiteral("mixed_currency_market_caps")
                                                                 : QStringLiteral("not_computed_for_intl_baskets"));
            is.fundamentals.currency_basis = currencies.size() == 1
                                                 ? *currencies.begin()
                                                 : (currencies.isEmpty() ? QString() : QStringLiteral("mixed"));
            s.intl.append(is);
        }
    }

    // ── Counts ───────────────────────────────────────────────────────────────
    int measured = 0, estimated = 0, proxy = 0, unavailable = 0, stale = 0, funds = 0;
    for (const auto& r : s.rows) {
        if (r.stale && r.last_bar.isValid())
            ++stale;
        if (!r.inst.is_fund())
            continue; // flow evidence describes funds; the index rows have none
        ++funds;
        switch (r.flow_evidence) {
            case EvidenceClass::Measured:
                ++measured;
                break;
            case EvidenceClass::Estimated:
                ++estimated;
                break;
            case EvidenceClass::Proxy:
                ++proxy;
                break;
            default:
                ++unavailable;
        }
    }
    s.counts = {{QStringLiteral("rows"), static_cast<int>(s.rows.size())},
                {QStringLiteral("funds"), funds},
                {QStringLiteral("flow_measured"), measured},
                {QStringLiteral("flow_estimated"), estimated},
                {QStringLiteral("proxy_only"), proxy},
                {QStringLiteral("unavailable"), unavailable},
                {QStringLiteral("stale"), stale}};
    return s;
}

// ── JSON export ──────────────────────────────────────────────────────────────

namespace {

QJsonArray etfr_trail_json(const QVector<RrgPoint>& t) {
    QJsonArray a;
    for (const auto& p : t)
        a.append(QJsonArray{p.date.toString(Qt::ISODate), p.ratio, p.mom});
    return a;
}

QJsonObject etfr_returns_json(const ReturnSet& r) {
    return {{QStringLiteral("d1"), r.d1.to_json()},
            {QStringLiteral("w1"), r.w1.to_json()},
            {QStringLiteral("m1"), r.m1.to_json()},
            {QStringLiteral("m3"), r.m3.to_json()},
            {QStringLiteral("rel_m1"), r.rel_m1.to_json()},
            {QStringLiteral("benchmark"), r.benchmark},
            {QStringLiteral("last_session"), r.last_session.toString(Qt::ISODate)}};
}

QJsonObject etfr_flow_json(const EstimatedFlow& e, bool series) {
    QJsonObject o{{QStringLiteral("latest"), e.latest.to_json()},
                  {QStringLiteral("latest_e2_reported_shares"), e.latest_e2.to_json()},
                  {QStringLiteral("latest_e3_close_adjusted"), e.latest_e3.to_json()},
                  {QStringLiteral("best"), e.best.to_json()},
                  {QStringLiteral("sum_5"), e.sum_5.to_json()},
                  {QStringLiteral("sum_20"), e.sum_20.to_json()},
                  {QStringLiteral("pct_aum_20"), e.pct_aum_20.to_json()},
                  {QStringLiteral("coverage_20"), e.coverage_20.to_json()},
                  {QStringLiteral("agreement"), e.agreement},
                  {QStringLiteral("captures"), e.captures},
                  {QStringLiteral("capture_sessions"), e.capture_sessions},
                  {QStringLiteral("undated_captures"), e.undated_captures},
                  {QStringLiteral("undated_reason"), e.undated_reason},
                  {QStringLiteral("validation_vs_measured"), e.validation_vs_measured.to_json()}};
    if (series) {
        QJsonArray iv;
        auto opt = [](const std::optional<double>& v) { return v ? QJsonValue(*v) : QJsonValue(QJsonValue::Null); };
        for (const auto& f : e.intervals)
            iv.append(QJsonObject{{QStringLiteral("from"), f.from.toString(Qt::ISODate)},
                                  {QStringLiteral("to"), f.to.toString(Qt::ISODate)},
                                  {QStringLiteral("sessions"), f.sessions},
                                  {QStringLiteral("aum_prev"), opt(f.aum_prev)},
                                  {QStringLiteral("aum_cur"), opt(f.aum_cur)},
                                  {QStringLiteral("nav_prev"), opt(f.nav_prev)},
                                  {QStringLiteral("nav_cur"), opt(f.nav_cur)},
                                  {QStringLiteral("e1"), opt(f.e1_implied_shares)},
                                  {QStringLiteral("e2"), opt(f.e2_reported_shares)},
                                  {QStringLiteral("e3"), opt(f.e3_price_adjusted)},
                                  {QStringLiteral("shares_level_gap"), opt(f.shares_level_gap)},
                                  {QStringLiteral("flags"), QJsonArray::fromStringList(f.flags)},
                                  {QStringLiteral("reason"), f.reason}});
        o.insert(QStringLiteral("intervals"), iv);
    }
    return o;
}

} // namespace

QJsonObject snapshot_to_json(const ResearchSnapshot& s, bool include_series) {
    QJsonObject root;
    root.insert(QStringLiteral("engine_version"), s.engine_version);
    root.insert(QStringLiteral("universe_version"), s.universe_version);
    root.insert(QStringLiteral("reference_commit"), s.reference_commit);
    root.insert(QStringLiteral("as_of"), s.as_of.toUTC().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("known_at"), s.known_at.toUTC().toString(Qt::ISODateWithMs));
    root.insert(QStringLiteral("expected_us_session"), s.expected_us_session.toString(Qt::ISODate));
    QJsonObject counts;
    for (auto it = s.counts.begin(); it != s.counts.end(); ++it)
        counts.insert(it.key(), it.value());
    root.insert(QStringLiteral("counts"), counts);
    QJsonArray rows;
    for (const UniverseRow& r : s.rows) {
        QJsonObject o;
        o.insert(QStringLiteral("symbol"), r.inst.symbol);
        o.insert(QStringLiteral("roles"), QJsonArray::fromStringList(r.inst.roles));
        o.insert(QStringLiteral("structure"), r.inst.structure);
        o.insert(QStringLiteral("structure_basis"), r.inst.structure_basis);
        o.insert(QStringLiteral("peer_group"), r.peer_group);
        o.insert(QStringLiteral("freshness"), r.freshness);
        o.insert(QStringLiteral("last_bar"), r.last_bar.toString(Qt::ISODate));
        o.insert(QStringLiteral("flow_evidence"), QLatin1String(evidence_id(r.flow_evidence)));
        o.insert(QStringLiteral("returns"), etfr_returns_json(r.ret));
        QJsonObject rrg{{QStringLiteral("quadrant"), r.rrg.quadrant.to_json()},
                        {QStringLiteral("direction"), r.rrg.direction},
                        {QStringLiteral("benchmark"), r.rrg.benchmark}};
        if (include_series)
            rrg.insert(QStringLiteral("trail"), etfr_trail_json(r.rrg.trail));
        o.insert(QStringLiteral("rrg"), rrg);
        o.insert(QStringLiteral("momentum_z"), r.momentum_z.to_json());
        o.insert(QStringLiteral("momentum_raw"), r.momentum_raw.to_json());
        if (!r.cftc.market.isEmpty())
            o.insert(QStringLiteral("cftc_positioning"),
                     QJsonObject{{QStringLiteral("market"), r.cftc.market},
                                 {QStringLiteral("report_date"), r.cftc.report_date.toString(Qt::ISODate)},
                                 {QStringLiteral("reports"), r.cftc.reports},
                                 {QStringLiteral("net_pct_oi"), r.cftc.net_pct_oi.to_json()},
                                 {QStringLiteral("percentile_3y"), r.cftc.percentile_3y.to_json()},
                                 {QStringLiteral("change_4w_pp"), r.cftc.change_4w_pp.to_json()}});
        o.insert(QStringLiteral("model_score"), r.model_score.to_json());
        o.insert(QStringLiteral("model_band"), r.model_band);
        o.insert(QStringLiteral("turnover_share"), r.turnover_share.to_json());
        o.insert(QStringLiteral("turnover_delta_bp"), r.turnover_delta_bp.to_json());
        o.insert(QStringLiteral("turnover_z"), r.turnover_z.to_json());
        o.insert(QStringLiteral("above_200dma"), r.above_200dma.to_json());
        o.insert(QStringLiteral("carry_12m"), r.carry_12m.to_json());
        o.insert(QStringLiteral("cross_check_ibkr"), r.cross_check.to_json());
        QJsonObject fund{{QStringLiteral("aum"), r.fund.aum.to_json()},
                         {QStringLiteral("nav"), r.fund.nav.to_json()},
                         {QStringLiteral("implied_shares"), r.fund.implied_shares.to_json()},
                         {QStringLiteral("reported_shares"), r.fund.reported_shares.to_json()},
                         {QStringLiteral("shares_gap_pct"), r.fund.shares_gap_pct.to_json()},
                         {QStringLiteral("expense_pct"), r.fund.expense_pct.to_json()},
                         {QStringLiteral("yield_pct"), r.fund.yield_pct.to_json()},
                         {QStringLiteral("beta3y"), r.fund.beta3y.to_json()},
                         {QStringLiteral("pe"), r.fund.pe.to_json()},
                         {QStringLiteral("nav_premium_pct"), r.fund.nav_premium_pct.to_json()},
                         {QStringLiteral("name"), r.fund.name},
                         {QStringLiteral("family"), r.fund.family},
                         {QStringLiteral("category"), r.fund.category},
                         {QStringLiteral("exchange"), r.fund.exchange},
                         {QStringLiteral("captures"), r.fund.captures},
                         {QStringLiteral("holdings"), r.fund.holdings.size()},
                         {QStringLiteral("assumed_session"), r.fund.assumed_session.toString(Qt::ISODate)},
                         {QStringLiteral("session_rule"), r.fund.session_rule}};
        if (!r.fund.holdings_read_failed.isEmpty())
            fund.insert(QStringLiteral("holdings_read_failed"), r.fund.holdings_read_failed);
        o.insert(QStringLiteral("fund"), fund);
        o.insert(QStringLiteral("estimated_flow"), etfr_flow_json(r.est, include_series));
        o.insert(QStringLiteral("measured_flow"),
                 QJsonObject{{QStringLiteral("latest"), r.measured.latest.to_json()},
                             {QStringLiteral("sum_3m"), r.measured.sum_3m.to_json()},
                             {QStringLiteral("reporting_key"), r.measured.reporting_key},
                             {QStringLiteral("months"), static_cast<int>(r.measured.months.size())}});
        rows.append(o);
    }
    root.insert(QStringLiteral("rows"), rows);

    QJsonObject sectors;
    const BusinessCycle& c = s.sectors.cycle;
    sectors.insert(QStringLiteral("cycle"),
                   QJsonObject{{QStringLiteral("phase"), c.phase.to_json()},
                               {QStringLiteral("level"), c.level.to_json()},
                               {QStringLiteral("direction"), c.direction.to_json()},
                               {QStringLiteral("p_recovery"), c.p_recovery},
                               {QStringLiteral("p_expansion"), c.p_expansion},
                               {QStringLiteral("p_slowdown"), c.p_slowdown},
                               {QStringLiteral("p_contraction"), c.p_contraction},
                               {QStringLiteral("z_slope"), c.z_slope.to_json()},
                               {QStringLiteral("z_activity"), c.z_activity.to_json()},
                               {QStringLiteral("z_credit"), c.z_credit.to_json()},
                               {QStringLiteral("z_momentum"), c.z_momentum.to_json()},
                               {QStringLiteral("credit_fallback"), c.credit_fallback},
                               {QStringLiteral("vix"), c.vix.to_json()},
                               {QStringLiteral("sectors_above_200dma"), c.sectors_above_200dma.to_json()}});
    QJsonArray srows;
    for (const auto& m : s.sectors.rows) {
        QJsonArray betas, ts;
        for (double b : m.factor_betas)
            betas.append(b);
        for (double t : m.factor_tstats)
            ts.append(t);
        srows.append(QJsonObject{{QStringLiteral("symbol"), m.symbol},
                                 {QStringLiteral("bc_z"), m.bc_z.to_json()},
                                 {QStringLiteral("mom_z"), m.mom_z.to_json()},
                                 {QStringLiteral("val_z"), m.val_z.to_json()},
                                 {QStringLiteral("factor_z"), m.factor_z.to_json()},
                                 {QStringLiteral("confluence"), m.confluence.to_json()},
                                 {QStringLiteral("band"), m.band},
                                 {QStringLiteral("rank"), m.rank},
                                 {QStringLiteral("layer_dispersion"), m.layer_dispersion.to_json()},
                                 {QStringLiteral("conflicts"), QJsonArray::fromStringList(m.conflicts)},
                                 {QStringLiteral("pe"), m.pe.to_json()},
                                 {QStringLiteral("earnings_yield_spread"), m.earnings_yield_spread.to_json()},
                                 {QStringLiteral("pe_own_z"), m.pe_own_z.to_json()},
                                 {QStringLiteral("factor_betas"), betas},
                                 {QStringLiteral("factor_tstats"), ts},
                                 {QStringLiteral("factor_r2"), m.factor_r2}});
    }
    sectors.insert(QStringLiteral("rows"), srows);
    sectors.insert(QStringLiteral("volatility_brake"), s.sectors.volatility_brake);
    sectors.insert(QStringLiteral("top_momentum"), s.sectors.top_momentum);
    sectors.insert(QStringLiteral("top_valuation"), s.sectors.top_valuation);
    sectors.insert(QStringLiteral("mom_val_divergence"), s.sectors.mom_val_divergence);
    sectors.insert(QStringLiteral("bc_mom_conflicts"), QJsonArray::fromStringList(s.sectors.bc_mom_conflicts));
    QJsonArray fs;
    for (int i = 0; i < s.sectors.factor_state.size(); ++i)
        fs.append(QJsonObject{{QStringLiteral("factor"), s.sectors.factor_names.value(i)},
                              {QStringLiteral("z_4w_change"), s.sectors.factor_state[i].to_json()}});
    sectors.insert(QStringLiteral("factor_state"), fs);
    root.insert(QStringLiteral("sector_models"), sectors);

    QJsonArray countries;
    for (const auto& k : s.countries)
        countries.append(QJsonObject{{QStringLiteral("symbol"), k.symbol},
                                     {QStringLiteral("country"), k.country},
                                     {QStringLiteral("type"), k.type},
                                     {QStringLiteral("m_z"), k.m_z.to_json()},
                                     {QStringLiteral("q_z"), k.q_z.to_json()},
                                     {QStringLiteral("c_z"), k.c_z.to_json()},
                                     {QStringLiteral("composite"), k.composite.to_json()},
                                     {QStringLiteral("band"), k.band},
                                     {QStringLiteral("rank"), k.rank},
                                     {QStringLiteral("gdp_3y"), k.gdp_3y.to_json()},
                                     {QStringLiteral("current_account"), k.current_account.to_json()},
                                     {QStringLiteral("carry"), k.carry.to_json()}});
    root.insert(QStringLiteral("country_models"), countries);

    QJsonObject quads;
    for (auto it = s.tilt.quadrants.begin(); it != s.tilt.quadrants.end(); ++it)
        quads.insert(it.key(), it.value());
    root.insert(QStringLiteral("tilt"), QJsonObject{{QStringLiteral("tilt_bp"), s.tilt.tilt_bp.to_json()},
                                                    {QStringLiteral("tilt_z"), s.tilt.tilt_z.to_json()},
                                                    {QStringLiteral("state"), s.tilt.state},
                                                    {QStringLiteral("quadrants"), quads},
                                                    {QStringLiteral("above_bench"), s.tilt.above_bench},
                                                    {QStringLiteral("breadth_total"), s.tilt.breadth_total}});

    QJsonObject regime;
    QJsonArray views;
    for (const auto& v : s.regime.views) {
        QJsonObject vo{{QStringLiteral("period"), v.period},
                       {QStringLiteral("symbols"), QJsonArray::fromStringList(v.symbols)},
                       {QStringLiteral("observations"), v.observations},
                       {QStringLiteral("avg_abs_corr"), v.avg_abs_corr.to_json()},
                       {QStringLiteral("corr_label"), v.corr_label.to_json()},
                       {QStringLiteral("geom_score"), v.geom_score.to_json()},
                       {QStringLiteral("geom_label"), v.geom_label.to_json()},
                       {QStringLiteral("k_signal"), v.k_signal},
                       {QStringLiteral("lambda_max"), v.lambda_max},
                       {QStringLiteral("lambda_1"), v.lambda_1},
                       {QStringLiteral("factor_regime"), v.factor_regime},
                       {QStringLiteral("var_pc1"), v.var_pc1},
                       {QStringLiteral("var_pc2"), v.var_pc2},
                       {QStringLiteral("calibration"), v.calibration}};
        if (include_series) {
            QJsonArray m;
            for (const auto& row : v.corr) {
                QJsonArray r;
                for (double x : row)
                    r.append(x);
                m.append(r);
            }
            vo.insert(QStringLiteral("corr"), m);
            QJsonArray p;
            for (const auto& xy : v.pca)
                p.append(QJsonArray{xy.first, xy.second});
            vo.insert(QStringLiteral("pca"), p);
        }
        views.append(vo);
    }
    regime.insert(QStringLiteral("views"), views);
    regime.insert(QStringLiteral("trend_corr"), s.regime.trend_corr.to_json());
    regime.insert(QStringLiteral("trend_geom"), s.regime.trend_geom.to_json());
    regime.insert(QStringLiteral("corr_geom_conflict"), s.regime.corr_geom_conflict);
    regime.insert(QStringLiteral("risk_gap"), s.regime.risk_gap);
    regime.insert(QStringLiteral("mrs_ready"), s.regime.mrs_ready);
    QJsonArray th, ts2, mm, tr20;
    for (double x : s.regime.mrs_thresholds)
        th.append(x);
    for (double x : s.regime.mrs_threshold_std)
        ts2.append(x);
    for (double x : s.regime.mrs_means)
        mm.append(x);
    for (double x : s.regime.mrs_transition_20d)
        tr20.append(x);
    regime.insert(QStringLiteral("mrs_thresholds"), th);
    regime.insert(QStringLiteral("mrs_threshold_std"), ts2);
    regime.insert(QStringLiteral("mrs_means"), mm);
    regime.insert(QStringLiteral("mrs_transition_20d"), tr20);
    regime.insert(QStringLiteral("mrs_label"), s.regime.mrs_label.to_json());
    QJsonArray probs;
    for (double p : s.regime.v2.probabilities)
        probs.append(p);
    QJsonObject feats;
    for (auto it = s.regime.v2.features.begin(); it != s.regime.v2.features.end(); ++it)
        feats.insert(it.key(), it.value());
    regime.insert(QStringLiteral("v2"), QJsonObject{{QStringLiteral("label"), s.regime.v2.label.to_json()},
                                                    {QStringLiteral("probabilities"), probs},
                                                    {QStringLiteral("days_in_state"), s.regime.v2.days_in_state},
                                                    {QStringLiteral("since"), s.regime.v2.since.toString(Qt::ISODate)},
                                                    {QStringLiteral("features"), feats},
                                                    {QStringLiteral("observations"), s.regime.v2.observations},
                                                    {QStringLiteral("converged"), s.regime.v2.converged}});
    root.insert(QStringLiteral("regime"), regime);

    QJsonArray baskets;
    for (const auto& b : s.baskets)
        baskets.append(QJsonObject{{QStringLiteral("id"), b.basket.id},
                                   {QStringLiteral("members"), static_cast<int>(b.basket.members.size())},
                                   {QStringLiteral("members_available"), b.members_available},
                                   {QStringLiteral("benchmark_used"), b.benchmark_used},
                                   {QStringLiteral("benchmark_fallback"), b.benchmark_fallback},
                                   {QStringLiteral("returns"), etfr_returns_json(b.ret)},
                                   {QStringLiteral("quadrant"), b.rrg.quadrant.to_json()}});
    root.insert(QStringLiteral("baskets"), baskets);
    QJsonArray aggs;
    for (const auto& a : s.constituent_aggregates)
        aggs.append(QJsonObject{{QStringLiteral("parent"), a.parent},
                                {QStringLiteral("count"), a.count},
                                {QStringLiteral("weight_covered"), a.weight_covered},
                                {QStringLiteral("median_pe"), a.median_pe.to_json()},
                                {QStringLiteral("cap_weighted_pe"), a.cap_weighted_pe.to_json()},
                                {QStringLiteral("currency_basis"), a.currency_basis}});
    root.insert(QStringLiteral("constituent_aggregates"), aggs);
    root.insert(QStringLiteral("constituent_rows"), static_cast<int>(s.constituents.size()));
    QJsonArray intl;
    for (const auto& is : s.intl)
        intl.append(QJsonObject{{QStringLiteral("market"), is.market},
                                {QStringLiteral("sector"), is.sector},
                                {QStringLiteral("members"), is.members},
                                {QStringLiteral("members_with_data"), is.members_with_data},
                                {QStringLiteral("ew_d1"), is.ew_d1.to_json()},
                                {QStringLiteral("ew_m1"), is.ew_m1.to_json()},
                                {QStringLiteral("median_pe"), is.fundamentals.median_pe.to_json()},
                                {QStringLiteral("currency_basis"), is.fundamentals.currency_basis}});
    root.insert(QStringLiteral("intl_sectors"), intl);
    QJsonArray src;
    for (const auto& st : s.sources)
        src.append(QJsonObject{{QStringLiteral("stage"), st.stage},
                               {QStringLiteral("status"), st.status},
                               {QStringLiteral("detail"), st.detail},
                               {QStringLiteral("requested_at"), st.requested_at.toString(Qt::ISODateWithMs)},
                               {QStringLiteral("retrieved_at"), st.retrieved_at.toString(Qt::ISODateWithMs)},
                               {QStringLiteral("latest_effective"), st.latest_effective},
                               {QStringLiteral("items_requested"), st.items_requested},
                               {QStringLiteral("items_ok"), st.items_ok},
                               {QStringLiteral("rows_inserted"), st.rows_inserted},
                               {QStringLiteral("rows_revised"), st.rows_revised},
                               {QStringLiteral("rows_confirmed"), st.rows_confirmed}});
    root.insert(QStringLiteral("sources"), src);
    QJsonArray gf;
    for (const auto& g : s.group_flows)
        gf.append(QJsonObject{{QStringLiteral("level"), g.level},
                              {QStringLiteral("group_id"), g.group_id},
                              {QStringLiteral("month"), g.month.toString(QStringLiteral("yyyy-MM"))},
                              {QStringLiteral("observed_net_flow_usd"),
                               g.observed_net_flow_usd ? QJsonValue(*g.observed_net_flow_usd) : QJsonValue()},
                              {QStringLiteral("complete_net_flow_usd"),
                               g.complete_net_flow_usd ? QJsonValue(*g.complete_net_flow_usd) : QJsonValue()},
                              {QStringLiteral("quality"), g.quality},
                              {QStringLiteral("observed_reporting_identities"), g.observed_reporting_identities},
                              {QStringLiteral("unique_reporting_identities"), g.unique_reporting_identities},
                              {QStringLiteral("unresolved_subjects"), g.unresolved_subjects}});
    root.insert(QStringLiteral("group_flows_batch_d"), gf);
    root.insert(QStringLiteral("th_benchmark_used"), s.th_benchmark_used);
    root.insert(QStringLiteral("th_benchmark_fallback"), s.th_benchmark_fallback);
    root.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(s.warnings));
    return root;
}

} // namespace fincept::services::etf::research
