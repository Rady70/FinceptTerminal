// MarketLab ETF Flow & Sector Rotation (consolidated Batch E) — research
// engine calculations over deterministic synthetic fixtures. Header-only
// calculation modules plus the engine translation unit; no database, network,
// Python or UI.
#include "etf_research_fixtures.h"
#include "services/etf/research/EtfResearchEngine.h"
#include "services/etf/research/EtfResearchFlow.h"
#include "services/etf/research/EtfResearchMath.h"
#include "services/etf/research/EtfResearchModels.h"
#include "services/etf/research/EtfResearchRegime.h"
#include "services/etf/research/EtfResearchRotation.h"
#include "services/etf/research/EtfResearchUniverse.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

#include <functional>

using namespace fincept::services::etf;
using namespace fincept::services::etf::research;
using namespace etfr_fixture;

class TstEtfResearchCalc : public QObject {
    Q_OBJECT
  private slots:
    // ── vocabulary ──
    void credibility_rules();
    void observation_is_not_graded();
    void unavailable_is_not_zero();
    // ── universe ──
    void bundled_universe_parses_and_is_unique();
    void universe_rejects_duplicates_and_bad_vocab();
    // ── rotation ──
    void total_return_counts_distributions();
    void returns_and_relative();
    void unadjusted_corporate_action_restarts_history();
    void rrg_quadrants_and_trail();
    void rrg_needs_history();
    void turnover_skips_incomplete_days();
    void turnover_tilt_states();
    void momentum_requires_full_history();
    void basket_rebalanced_and_min_members();
    void staleness_rules();
    // ── estimated flow ──
    void reference_estimators_are_algebraically_identical();
    void distribution_is_not_a_flow();
    void reported_shares_disagreement_is_experimental();
    void stale_aum_and_split_are_unavailable();
    void capture_gap_is_one_interval();
    void stale_aum_capture_is_not_an_anchor();
    void nav_dating_follows_the_fund_lag();
    void one_capture_is_record_too_young();
    void positive_negative_and_zero_flow();
    // ── models ──
    void business_cycle_probabilities_and_missing_input();
    void ols_recovers_coefficients();
    void confluence_needs_three_layers();
    void cftc_positioning_context();
    // ── regime ──
    void correlation_geometry();
    void weekly_regime_uses_completed_weeks();
    void hmm_recovers_two_regimes();
    void mrs_thresholds_ordered();
    // ── engine ──
    void snapshot_end_to_end_and_deterministic();
};

void TstEtfResearchCalc::credibility_rules() {
    QStringList why;
    QCOMPARE(grade_credibility(Credibility::High, {CredCondition::Stale}, &why), Credibility::Low);
    QCOMPARE(why.size(), 2);
    QCOMPARE(grade_credibility(Credibility::High, {CredCondition::Partial}, nullptr), Credibility::Medium);
    QCOMPARE(grade_credibility(Credibility::High, {CredCondition::Partial, CredCondition::Fallback}, nullptr),
             Credibility::Low);
    QCOMPARE(grade_credibility(Credibility::Medium, {CredCondition::EstimatorDisagreement}, nullptr),
             Credibility::Experimental);
    QCOMPARE(grade_credibility(Credibility::High, {CredCondition::InputRevised}, nullptr), Credibility::High);
    QCOMPARE(grade_credibility(Credibility::High, {CredCondition::HeuristicThreshold}, nullptr), Credibility::Low);
    QCOMPARE(grade_credibility(Credibility::Low, {CredCondition::Partial}, nullptr), Credibility::Experimental);
}

void TstEtfResearchCalc::observation_is_not_graded() {
    QStringList why;
    QCOMPARE(grade_credibility(Credibility::NotGraded, {CredCondition::MacroStale}, &why), Credibility::NotGraded);
    QVERIFY(why.first().startsWith(QLatin1String("macro_input_stale")));
}

void TstEtfResearchCalc::unavailable_is_not_zero() {
    const ResearchValue u = ResearchValue::unavailable(QStringLiteral("no_capture"));
    QVERIFY(!u.value.has_value());
    QCOMPARE(u.evidence, EvidenceClass::Unavailable);
    const ResearchValue z = graded(0.0, EvidenceClass::Estimated, Credibility::Medium, {}, QStringLiteral("usd"),
                                   QStringLiteral("m"), QStringLiteral("s"), QDate(2026, 9, 30));
    QVERIFY(z.value.has_value());
    QVERIFY(z.has_flag(flag::kActualZero));
    QCOMPARE(u.to_json().value(QStringLiteral("value")).type(), QJsonValue::Null);
}

void TstEtfResearchCalc::bundled_universe_parses_and_is_unique() {
    QString err;
    const auto u = load_universe(QLatin1String(kUniverseResourcePath), &err);
    QVERIFY2(u.has_value(), qPrintable(err));
    QCOMPARE(u->with_role("us_sector").size(), 11);
    QCOMPARE(u->with_role("theme").size(), 24);
    QCOMPARE(u->with_role("country").size(), 14);
    QSet<QString> seen;
    for (const auto& i : u->instruments) {
        QVERIFY2(!seen.contains(i.symbol), qPrintable(i.symbol));
        seen.insert(i.symbol);
    }
    // XLK is one row with both roles (taxonomy seed and SPDR sector), SPY likewise.
    QVERIFY(u->find(QStringLiteral("XLK"))->has_role("us_sector"));
    QVERIFY(u->find(QStringLiteral("XLK"))->has_role("cross_asset"));
    QVERIFY(u->find(QStringLiteral("SPY"))->has_role("country"));
    // Structures stay distinguishable and leverage/inverse are flagged.
    QCOMPARE(u->find(QStringLiteral("GLD"))->structure, QStringLiteral("grantor_trust"));
    QCOMPARE(u->find(QStringLiteral("USO"))->structure, QStringLiteral("commodity_pool"));
    QCOMPARE(u->find(QStringLiteral("SPY"))->structure, QStringLiteral("unit_investment_trust"));
    QVERIFY(u->find(QStringLiteral("SSO"))->leveraged);
    QVERIFY(u->find(QStringLiteral("SH"))->inverse);
    QVERIFY(u->find(QStringLiteral("JEPI"))->option_overlay);
    QCOMPARE(u->find(QStringLiteral("^SET.BK"))->instrument_type, QStringLiteral("index"));
    // Explicit SEC reporting identities only where declared (SPY, IVV, QQQ).
    int declared = 0;
    for (const auto& i : u->instruments)
        declared += i.sec_reporting.isEmpty() ? 0 : 1;
    QCOMPARE(declared, 3);
    QCOMPARE(u->find(QStringLiteral("QQQ"))->sec_reporting.size(), 3);
    QCOMPARE(u->baskets.size(), 13);
    QVERIFY(u->intl.contains(QStringLiteral("eu")));
}

void TstEtfResearchCalc::universe_rejects_duplicates_and_bad_vocab() {
    QFile f{QString::fromLatin1(kUniverseResourcePath)};
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    QString err;
    {
        QJsonObject d = doc;
        QJsonArray a = d.value(QStringLiteral("instruments")).toArray();
        a.append(a.first());
        d.insert(QStringLiteral("instruments"), a);
        QVERIFY(!parse_universe(d, &err));
        QVERIFY(err.contains(QLatin1String("duplicate")));
    }
    {
        QJsonObject d = doc;
        QJsonArray a = d.value(QStringLiteral("instruments")).toArray();
        QJsonObject o = a.first().toObject();
        o.insert(QStringLiteral("structure"), QStringLiteral("mystery"));
        a.replace(0, o);
        d.insert(QStringLiteral("instruments"), a);
        QVERIFY(!parse_universe(d, &err));
    }
    {
        QJsonObject d = doc;
        d.insert(QStringLiteral("version"), QStringLiteral("etf-research-universe-v0"));
        QVERIFY(!parse_universe(d, &err));
    }
    {
        // A multi-class series flow would be presented as the ETF's flow: refused.
        QJsonObject d = doc;
        QJsonArray a = d.value(QStringLiteral("instruments")).toArray();
        for (int i = 0; i < a.size(); ++i) {
            QJsonObject o = a[i].toObject();
            if (o.value(QStringLiteral("symbol")).toString() != QLatin1String("SPY"))
                continue;
            QJsonArray sr = o.value(QStringLiteral("sec_reporting")).toArray();
            QJsonObject id = sr.first().toObject();
            id.insert(QStringLiteral("relationship"), QStringLiteral("class_of_multi_class_series"));
            sr.replace(0, id);
            o.insert(QStringLiteral("sec_reporting"), sr);
            a.replace(i, o);
        }
        d.insert(QStringLiteral("instruments"), a);
        QVERIFY(!parse_universe(d, &err));
    }
}

void TstEtfResearchCalc::unadjusted_corporate_action_restarts_history() {
    // Live shape (CTVA 2026-10-01, Yahoo): 77.65 -> 12.57 with no split or
    // distribution recorded. Booking that as a -84% return would be fabricated.
    const QVector<QDate> d = etfr_fixture::nyse_sessions(QDate(2026, 10, 9), 90);
    BarSeries s = etfr_fixture::series(QStringLiteral("CTVA"), d, 80.0, -0.0005);
    const int brk = 80;
    for (int i = brk; i < s.bars.size(); ++i)
        s.bars[i].close *= 12.57 / 77.65;
    const TrIndex t = make_tr_index(s);
    QCOMPARE(t.restarted_at, d[brk]);
    QVERIFY(t.restart_ratio < 1.0 / kDiscontinuityRatio);
    QCOMPARE(t.size(), s.bars.size() - brk); // nothing before the restart is bridged
    const ReturnSet r = compute_returns(t, nullptr, QString(), {});
    QVERIFY(r.d1.usable()); // the session after the restart is an ordinary return
    QVERIFY(std::abs(*r.d1.value - (std::exp(-0.0005) - 1.0) * 100.0) < 1e-9);
    for (const ResearchValue* v : {&r.m1, &r.m3}) {
        QVERIFY(!v->usable());
        QVERIFY(!v->value);
        QVERIFY(v->has_flag(flag::kHistoryRestarted));
        QVERIFY(v->reason.contains(QStringLiteral("suspect_unadjusted_corporate_action")));
    }
    // A genuine -60% day (ratio 0.4, inside the band) stays a return.
    BarSeries crash = etfr_fixture::series(QStringLiteral("BIO"), d, 50.0, 0.0);
    for (int i = brk; i < crash.bars.size(); ++i)
        crash.bars[i].close *= 0.4;
    const TrIndex c = make_tr_index(crash);
    QVERIFY(!c.restarted_at.isValid());
    const auto m1 = compute_returns(c, nullptr, QString(), {}).m1;
    QVERIFY(m1.usable() && std::abs(*m1.value - (-60.0)) < 1e-6);
    // A large drop explained by a recorded distribution is not a discontinuity.
    BarSeries dist;
    dist.symbol = QStringLiteral("D");
    dist.bars = {{QDate(2026, 9, 1), 100.0, 1e6}, {QDate(2026, 9, 2), 20.0, 1e6, 80.0}};
    QVERIFY(!make_tr_index(dist).restarted_at.isValid());
}

void TstEtfResearchCalc::total_return_counts_distributions() {
    BarSeries s;
    s.symbol = QStringLiteral("X");
    s.bars = {{QDate(2026, 9, 1), 100.0, 1e6}, {QDate(2026, 9, 2), 101.0, 1e6}, {QDate(2026, 9, 3), 99.0, 1e6, 2.0}};
    const TrIndex t = make_tr_index(s);
    QCOMPARE(t.size(), 3);
    QVERIFY(qFuzzyCompare(t.tr[2], 1.01 * 101.0 / 101.0)); // ex-date drop fully offset by the 2.00 distribution
    const auto r = pct_change(t.tr, 2, 2);
    QVERIFY(r && std::abs(*r - 1.0) < 1e-9);
    // Price return alone would have shown -1%.
    QVERIFY(std::abs(*pct_change(t.close, 2, 2) + 1.0) < 1e-9);
}

void TstEtfResearchCalc::returns_and_relative() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 120);
    const TrIndex a = make_tr_index(series(QStringLiteral("A"), d, 100, 0.002));
    const TrIndex b = make_tr_index(series(QStringLiteral("B"), d, 50, 0.001));
    const ReturnSet r = compute_returns(a, &b, QStringLiteral("B"), {});
    QVERIFY(r.m1.value);
    const double expect = (std::exp(0.002 * 21) - 1) * 100;
    QVERIFY(std::abs(*r.m1.value - expect) < 1e-9);
    const double rel = expect - (std::exp(0.001 * 21) - 1) * 100;
    QVERIFY(std::abs(*r.rel_m1.value - rel) < 1e-9);
    QCOMPARE(r.m1.evidence, EvidenceClass::Proxy);
    QCOMPARE(r.m1.credibility, Credibility::High);
    // Self-relative is not a number.
    const ReturnSet self = compute_returns(a, &a, QStringLiteral("A"), {});
    QVERIFY(!self.rel_m1.value);
    QCOMPARE(self.rel_m1.reason, QStringLiteral("subject_is_benchmark"));
    // Too short for 3M.
    const TrIndex shortt = make_tr_index(series(QStringLiteral("S"), nyse_sessions(QDate(2026, 9, 30), 30), 10, 0.0));
    const ReturnSet rs = compute_returns(shortt, nullptr, QString(), {});
    QVERIFY(!rs.m3.value);
    QCOMPARE(rs.m3.reason, QStringLiteral("insufficient_history"));
}

void TstEtfResearchCalc::rrg_quadrants_and_trail() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 200);
    BarSeries bench = series(QStringLiteral("B"), d, 100, 0.0005);
    BarSeries up = bench, down = bench;
    up.symbol = QStringLiteral("U");
    down.symbol = QStringLiteral("D");
    for (int t = 0; t < d.size(); ++t) {
        up.bars[t].close *= std::exp(2e-6 * t * t);    // accelerating outperformance
        down.bars[t].close *= std::exp(-2e-6 * t * t); // accelerating underperformance
    }
    const TrIndex b = make_tr_index(bench), u = make_tr_index(up), dn = make_tr_index(down);
    const RrgResult ru = compute_rrg(u, &b, QStringLiteral("B"), {});
    const RrgResult rd = compute_rrg(dn, &b, QStringLiteral("B"), {});
    QCOMPARE(ru.quadrant.label, QStringLiteral("Leading"));
    QCOMPARE(rd.quadrant.label, QStringLiteral("Lagging"));
    QCOMPARE(ru.quadrant.evidence, EvidenceClass::Model);
    QCOMPARE(ru.trail.size(), 8);
    for (int i = 1; i < ru.trail.size(); ++i)
        QVERIFY(ru.trail[i].date > ru.trail[i - 1].date);
    // Week points are labelled by the last real session of a COMPLETED week: the
    // in-progress week ending Wednesday 2026-09-30 is not plotted.
    QCOMPARE(ru.trail.last().date, QDate(2026, 9, 25));
    QVERIFY(week_in_progress(QDate(2026, 9, 30), true));
    QVERIFY(!week_in_progress(QDate(2026, 4, 2), true)); // Good Friday 2026-04-03: Thursday ends the week
    QVERIFY(week_in_progress(QDate(2026, 4, 2), false));
    QCOMPARE(week_ending_friday(QDate(2026, 10, 3)), QDate(2026, 10, 9)); // Saturday -> next Friday bin
    QCOMPARE(rrg_quadrant(100, 100), QStringLiteral("Leading"));
    QCOMPARE(rrg_quadrant(99.9, 100.1), QStringLiteral("Improving"));
    QCOMPARE(rrg_quadrant(100.1, 99.9), QStringLiteral("Weakening"));
}

void TstEtfResearchCalc::rrg_needs_history() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 60);
    const TrIndex a = make_tr_index(series(QStringLiteral("A"), d, 100, 0.001));
    const TrIndex b = make_tr_index(series(QStringLiteral("B"), d, 100, 0.0));
    const RrgResult r = compute_rrg(a, &b, QStringLiteral("B"), {});
    QVERIFY(r.quadrant.label.isEmpty());
    QCOMPARE(r.quadrant.reason, QStringLiteral("insufficient_history"));
}

void TstEtfResearchCalc::turnover_skips_incomplete_days() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 120);
    QHash<QString, TrIndex> idx;
    for (const char* s : {"AA", "BB", "CC"}) {
        BarSeries b = series(QLatin1String(s), d, 100, 0.0, 0.0, 20, 1e6);
        for (auto& bar : b.bars)
            bar.volume = 1e6; // equal dollar volume
        if (QLatin1String(s) == QLatin1String("CC"))
            b.bars[50].volume = std::nullopt; // one missing volume: that day is skipped for everyone
        idx.insert(QLatin1String(s), make_tr_index(b));
    }
    const QStringList m{QStringLiteral("AA"), QStringLiteral("BB"), QStringLiteral("CC")};
    const TurnoverResult r = compute_turnover(idx, m, {QStringLiteral("AA")}, {QStringLiteral("BB")}, 20, 10.0, {});
    QCOMPARE(r.days_skipped, 1);
    QCOMPARE(r.days_used, 119);
    double sum = 0;
    for (const auto& s : m)
        sum += *r.share[s].value;
    QVERIFY(std::abs(sum - 100.0) < 1e-9);
    QVERIFY(std::abs(*r.share[QStringLiteral("AA")].value - 100.0 / 3) < 1e-9);
    QVERIFY(std::abs(*r.delta_bp[QStringLiteral("AA")].value) < 1e-9);
    // Flat shares have no z-score (not a confident 0 or ±1).
    QVERIFY(!r.z[QStringLiteral("AA")].value);
}

void TstEtfResearchCalc::turnover_tilt_states() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 200);
    QHash<QString, TrIndex> idx;
    for (const char* s : {"DEF", "CYC"}) {
        BarSeries b = series(QLatin1String(s), d, 100, 0.0);
        for (int t = 0; t < b.bars.size(); ++t)
            b.bars[t].volume = QLatin1String(s) == QLatin1String("DEF") ? 1e6 * (1.0 + 0.004 * t) : 1e6;
        idx.insert(QLatin1String(s), make_tr_index(b));
    }
    const TurnoverResult r = compute_turnover(idx, {QStringLiteral("DEF"), QStringLiteral("CYC")},
                                              {QStringLiteral("DEF")}, {QStringLiteral("CYC")}, 20, 10.0, {});
    QVERIFY(r.tilt_bp.value);
    QVERIFY(*r.tilt_bp.value > 10.0);
    QCOMPARE(r.tilt_state, QStringLiteral("DEFENSIVE"));
}

void TstEtfResearchCalc::momentum_requires_full_history() {
    const TrIndex longt =
        make_tr_index(series(QStringLiteral("L"), nyse_sessions(QDate(2026, 9, 30), 300), 100, 0.001));
    const MomentumParts p = momentum_parts(longt, true);
    QVERIFY(p.composite);
    const double v = std::exp(0.001);
    auto g = [&](int a, int b) { return std::pow(v, b - a) - 1; };
    const double expect = 0.5 * g(22, 253) + 0.3 * g(22, 127) + 0.2 * g(6, 64) - 0.3 * g(1, 22);
    QVERIFY(std::abs(*p.composite - expect) < 1e-12);
    const MomentumParts nr = momentum_parts(longt, false);
    QVERIFY(std::abs(*nr.composite - (expect + 0.3 * g(1, 22))) < 1e-12);
    const TrIndex shortt =
        make_tr_index(series(QStringLiteral("S"), nyse_sessions(QDate(2026, 9, 30), 200), 100, 0.001));
    QVERIFY(!momentum_parts(shortt, true).composite); // never a silent 0.0
    // Cross-section ignores undefined members and refuses fewer than three.
    const auto z = math::cross_section_z({1.0, std::nullopt, 2.0, 3.0}, 2.5);
    QVERIFY(!z[1]);
    QVERIFY(z[0] && z[2] && z[3]);
    QVERIFY(std::abs(*z[2]) < 1e-12);
    const auto z2 = math::cross_section_z({1.0, std::nullopt, 2.0});
    QVERIFY(!z2[0] && !z2[2]);
}

void TstEtfResearchCalc::basket_rebalanced_and_min_members() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 10);
    BarSeries a = series(QStringLiteral("A.BK"), d, 10, 0.01);
    BarSeries b = series(QStringLiteral("B.BK"), d, 20, -0.01);
    BarSeries late = series(QStringLiteral("C.BK"), d.mid(6), 5, 0.05); // lists late
    const TrIndex ta = make_tr_index(a), tb = make_tr_index(b), tc = make_tr_index(late);
    int with = 0;
    const TrIndex ew = equal_weight_basket(QStringLiteral("x"), {&ta, &tb, &tc, nullptr}, 0.5, &with);
    QCOMPARE(with, 3);
    // Day 1: A and B (2 of 4 >= 0.5): mean of daily returns, not of rebased levels.
    const double r1 = ((std::exp(0.01) - 1) + (std::exp(-0.01) - 1)) / 2;
    QVERIFY(std::abs(ew.tr[1] / ew.tr[0] - 1 - r1) < 1e-12);
    // A required fraction above what trades yields no session at all.
    const TrIndex none = equal_weight_basket(QStringLiteral("y"), {&ta, nullptr, nullptr, nullptr}, 0.5, &with);
    QVERIFY(none.empty());
}

void TstEtfResearchCalc::staleness_rules() {
    QVERIFY(series_is_stale(QStringLiteral("XLK"), QDate(2026, 9, 29), QDate(2026, 9, 30), QDate(2026, 10, 1)));
    QVERIFY(!series_is_stale(QStringLiteral("XLK"), QDate(2026, 9, 30), QDate(2026, 9, 30), QDate(2026, 10, 1)));
    QVERIFY(!series_is_stale(QStringLiteral("SCB.BK"), QDate(2026, 9, 28), QDate(2026, 9, 30), QDate(2026, 10, 2)));
    QVERIFY(series_is_stale(QStringLiteral("SCB.BK"), QDate(2026, 9, 20), QDate(2026, 9, 30), QDate(2026, 10, 2)));
    QVERIFY(series_is_stale(QStringLiteral("NONE"), QDate(), QDate(2026, 9, 30), QDate(2026, 10, 2)));
}

void TstEtfResearchCalc::reference_estimators_are_algebraically_identical() {
    // For ANY inputs, Δ(A/N)·N1 == A1 − A0·N1/N0: the reference's two estimators.
    const double cases[][4] = {{1e9, 50, 1.1e9, 52}, {5e8, 20, 4.9e8, 19.5}, {2e10, 400, 2e10, 401}};
    for (const auto& c : cases) {
        const double shares = (c[2] / c[3] - c[0] / c[1]) * c[3];
        const double ret = c[2] - c[0] * (1 + (c[3] / c[1] - 1));
        QVERIFY(std::abs(shares - ret) <= 1e-6 * std::abs(c[2]));
    }
}

void TstEtfResearchCalc::distribution_is_not_a_flow() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 3);
    BarSeries bars;
    bars.symbol = QStringLiteral("F");
    bars.bars = {{d[0], 50.0, 1e6}, {d[1], 49.0, 1e6, 1.0}, {d[2], 49.2, 1e6}};
    // 1,000,000 shares; NAV falls by the 1.00 distribution; AUM falls by 1,000,000.
    const QVector<FundCapture> caps{capture(d[0], 50e6, 50.0, 1e6), capture(d[1], 49e6, 49.0, 1e6)};
    const EstimatedFlow e = estimate_flow(caps, bars, {}, d[1]);
    QVERIFY(e.latest.value);
    QVERIFY(std::abs(*e.latest.value) < 1e-6);
    QVERIFY(e.latest.has_flag(flag::kActualZero));
    // A total-return adjustment would have booked -1,000,000: an apparent outflow of shares x distribution.
    const double tr_style = 49e6 - 50e6 * ((49.0 + 1.0) / 50.0);
    QVERIFY(std::abs(tr_style + 1e6) < 1e-3);
    QCOMPARE(e.agreement, QStringLiteral("agree"));
}

void TstEtfResearchCalc::reported_shares_disagreement_is_experimental() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 2);
    const BarSeries bars = closes(QStringLiteral("F"), d, {100.0, 100.5});
    // Implied shares grow by 1%, reported shares unchanged (a stale field).
    const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0, 1e6), capture(d[1], 1.01e6 * 100.5, 100.5, 1e6)};
    const EstimatedFlow e = estimate_flow(caps, bars, {}, d[1]);
    QVERIFY(e.latest.value);
    QVERIFY(std::abs(*e.latest.value - 0.01e6 * 100.5) < 1e-3);
    QCOMPARE(e.agreement, QStringLiteral("disagree"));
    QCOMPARE(e.latest.credibility, Credibility::Experimental);
    QVERIFY(e.latest.has_flag(flag::kDisagreement));
    QVERIFY(e.latest.has_flag(flag::kLowConfidence));
    QVERIFY(e.latest_e2.value && *e.latest_e2.value == 0.0);
    QVERIFY(e.intervals.last().flags.contains(QStringLiteral("REPORTED_SHARES_UNCHANGED")));
}

void TstEtfResearchCalc::stale_aum_and_split_are_unavailable() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 3);
    BarSeries bars = series(QStringLiteral("F"), d, 100, 0.0);
    {
        // Live shape (Yahoo, 2026-10-02 -> 10-03 captures): totalAssets and the
        // share count re-served unchanged while NAV and the close moved. E3 would
        // book -AUM x return (-1% here) and E2 a false zero; all must be unavailable.
        BarSeries moved = bars;
        moved.bars[1].close = 101.0;
        const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0, 1e6), capture(d[1], 100e6, 101.0, 1e6)};
        const EstimatedFlow e = estimate_flow(caps, moved, {}, d[1]);
        QVERIFY(!e.latest.value);
        QCOMPARE(e.latest.reason, QStringLiteral("aum_not_updated"));
        QVERIFY(!e.latest_e2.value);
        QVERIFY(!e.latest_e3.value);
        QVERIFY(!e.intervals.last().e2_reported_shares && !e.intervals.last().e3_price_adjusted);
        QVERIFY(e.intervals.last().flags.contains(QStringLiteral("AUM_UNCHANGED_NAV_CHANGED")));
    }
    {
        // The same stale AUM, but the separately reported share count moved: the
        // AUM-based E1/E3 stay refused, E2 stands, graded down, and becomes the
        // best estimate.
        BarSeries moved = bars;
        moved.bars[1].close = 101.0;
        const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0, 1e6), capture(d[1], 100e6, 101.0, 1.02e6)};
        const EstimatedFlow e = estimate_flow(caps, moved, {}, d[1]);
        QVERIFY(!e.latest.value);
        QCOMPARE(e.latest.reason, QStringLiteral("aum_not_updated"));
        QVERIFY(!e.latest_e3.value);
        QVERIFY(e.latest_e2.value);
        QVERIFY(std::abs(*e.latest_e2.value - 0.02e6 * 101.0) < 1e-3);
        QCOMPARE(e.latest_e2.credibility, Credibility::Experimental);
        QVERIFY(e.latest_e2.has_flag(flag::kLowConfidence));
        QVERIFY(e.best.value && *e.best.value == *e.latest_e2.value);
    }
    {
        // Shares unchanged too: no E2, and the reason says why.
        BarSeries moved = bars;
        moved.bars[1].close = 101.0;
        const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0, 1e6), capture(d[1], 100e6, 101.0, 1e6)};
        const EstimatedFlow e = estimate_flow(caps, moved, {}, d[1]);
        QCOMPARE(e.latest_e2.reason, QStringLiteral("reported_shares_unchanged_in_stale_snapshot"));
        QVERIFY(!e.best.value);
    }
    {
        BarSeries split = closes(QStringLiteral("F"), d, {50.0, 50.0, 50.0}); // d0 restated from 100 by the 2:1 split
        split.bars[1].split = 2.0;
        const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0, 1e6), capture(d[1], 100e6, 50.0, 2e6)};
        const EstimatedFlow e = estimate_flow(caps, split, {}, d[1]);
        QVERIFY(!e.latest.value);
        QCOMPARE(e.latest.reason, QStringLiteral("split_in_interval"));
        QVERIFY(!e.latest_e2.value);
    }
}

void TstEtfResearchCalc::stale_aum_capture_is_not_an_anchor() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 3);
    BarSeries bars = series(QStringLiteral("F"), d, 100, 0.0);
    bars.bars[1].close = 101.0;
    bars.bars[2].close = 102.0;
    // d1 re-serves d0's AUM; d2 is fresh. Shares 1.00M -> 1.01M over (d0, d2].
    const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0), capture(d[1], 100e6, 101.0),
                                    capture(d[2], 103.02e6, 102.0)};
    const EstimatedFlow e = estimate_flow(caps, bars, {}, d[2]);
    QCOMPARE(e.intervals.size(), 2);
    QCOMPARE(e.intervals[0].reason, QStringLiteral("aum_not_updated"));
    const FlowInterval& f = e.intervals[1];
    QCOMPARE(f.from, d[0]); // anchored on the last fresh capture, not the stale one
    QCOMPARE(f.to, d[2]);
    QCOMPARE(f.sessions, 2);
    QVERIFY(f.flags.contains(QLatin1String(flag::kGap)));
    QVERIFY(f.e1_implied_shares && std::abs(*f.e1_implied_shares - 0.01e6 * 102.0) < 1e-3);
    QVERIFY(e.latest.usable() && e.latest.has_flag(flag::kGap));
}

void TstEtfResearchCalc::nav_dating_follows_the_fund_lag() {
    // Live shape (Yahoo, 2026-10-03 08:24 New York): the capture is stored as
    // session 10-02 and its navPrice equals the 10-01 close. Synthetic values.
    const auto d = nyse_sessions(QDate(2026, 10, 2), 4); // 09-29 .. 10-02
    const BarSeries spy = closes(QStringLiteral("SPY"), d, {100.00, 99.80, 99.98, 100.72});
    const QDateTime saturday(QDate(2026, 10, 3), QTime(12, 24), QTimeZone::UTC);
    const QDateTime friday_pre_open(QDate(2026, 10, 2), QTime(13, 9), QTimeZone::UTC);
    auto cap = [](const QDate& stored, double nav, const QDateTime& at, double aum = 8e11) {
        FundCapture c;
        c.effective_session = stored;
        c.effective_rule = QStringLiteral("prior_completed_session_v1");
        c.captured_at = at;
        c.total_assets = aum;
        c.nav = nav;
        return c;
    };
    QString why;
    const FundCapture dated = nav_dated(cap(d[3], 99.95, saturday), spy, &why);
    QCOMPARE(dated.effective_session, d[2]);
    QCOMPARE(dated.effective_rule, QLatin1String(kNavDatingRule));
    QVERIFY(why.isEmpty());
    // A quiet market (adjacent closes 1 bp apart) does not contradict the lag.
    const BarSeries quiet = closes(QStringLiteral("Q"), d, {100.00, 100.00, 100.02, 100.03});
    QCOMPARE(nav_dated(cap(d[3], 100.03, saturday), quiet, &why).effective_session, d[2]);
    // A NAV that clearly matches the stored session's close contradicts the lag
    // (live: GLD, whose NAV is struck at the London fix, not the U.S. close).
    QVERIFY(!nav_dated(cap(d[3], 100.70, saturday), spy, &why).effective_session.isValid());
    QCOMPARE(why, QStringLiteral("nav_lag_contradicted"));
    QVERIFY(!nav_dated(cap(d[3], 103.0, saturday), spy, &why).effective_session.isValid());
    QCOMPARE(why, QStringLiteral("nav_far_from_close"));
    // After an NYSE close and before midnight New York time the lag is unobserved.
    const QDateTime friday_evening(QDate(2026, 10, 2), QTime(21, 30), QTimeZone::UTC);
    QVERIFY(!nav_dated(cap(d[3], 99.95, friday_evening), spy, &why).effective_session.isValid());
    QCOMPARE(why, QStringLiteral("nav_lag_unvalidated_after_close"));
    BarSeries gap = spy;
    gap.bars.removeAt(2);
    QVERIFY(!nav_dated(cap(d[3], 99.95, saturday), gap, &why).effective_session.isValid());
    QCOMPARE(why, QStringLiteral("close_missing_for_nav_dating"));
    // Closes restated by a later 2:1 split still date a NAV published before it.
    BarSeries split = closes(QStringLiteral("S"), d, {50.00, 49.90, 49.99, 50.36});
    split.bars[3].split = 2.0;
    QCOMPARE(nav_dated(cap(d[3], 99.97, saturday), split, &why).effective_session, d[2]);

    // Flow: the interval runs between the NAV sessions (09-30 -> 10-01), one
    // session earlier than the stored ones, and E1 always carries the
    // undated-AUM timing condition.
    const QVector<FundCapture> caps{cap(d[2], 99.80, friday_pre_open, 8.00e11), cap(d[3], 99.95, saturday, 8.01e11)};
    const EstimatedFlow e = estimate_flow(caps, spy, {}, d[3]);
    QCOMPARE(e.capture_sessions, 2);
    QCOMPARE(e.undated_captures, 0);
    QCOMPARE(e.intervals.size(), 1);
    QCOMPARE(e.intervals.first().from, d[1]);
    QCOMPARE(e.intervals.first().to, d[2]);
    QVERIFY(e.latest.usable());
    bool timing = false;
    for (const QString& r : e.latest.credibility_reasons)
        timing = timing || r.startsWith(QLatin1String("source_timing_ambiguous"));
    QVERIFY(timing);
    // An undatable capture is counted and never used.
    const EstimatedFlow u =
        estimate_flow({cap(d[2], 99.80, friday_pre_open), cap(d[3], 103.0, saturday)}, spy, {}, d[3]);
    QVERIFY(!u.latest.value);
    QCOMPARE(u.undated_captures, 1);
    QCOMPARE(u.latest.reason, QStringLiteral("nav_far_from_close"));

    // Fund facts: the premium is measured against the NAV session's close (it
    // read +0.77 % against the stored session's close before this rule).
    const FundFacts f = fund_facts_from_captures({cap(d[3], 99.95, saturday)}, &spy, nullptr, d[3]);
    QCOMPARE(f.assumed_session, d[2]);
    QCOMPARE(f.session_rule, QLatin1String(kNavDatingRule));
    QVERIFY(f.nav_premium_pct.value);
    QVERIFY(std::abs(*f.nav_premium_pct.value - (99.98 / 99.95 - 1.0) * 100.0) < 1e-9);
    const FundFacts g = fund_facts_from_captures({cap(d[3], 103.0, saturday)}, &spy, nullptr, d[3]);
    QVERIFY(!g.nav_premium_pct.value);
    QCOMPARE(g.nav_premium_pct.reason, QStringLiteral("nav_far_from_close"));
    QVERIFY(g.session_rule.contains(QLatin1String("nav_undated")));
}

void TstEtfResearchCalc::capture_gap_is_one_interval() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 5);
    const BarSeries bars = closes(QStringLiteral("F"), d, {100.0, 100.25, 100.75, 101.25, 101.5});
    const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0, 1e6), capture(d[3], 1.03e6 * 101.25, 101.25, 1.03e6)};
    const EstimatedFlow e = estimate_flow(caps, bars, {}, d[4]);
    QCOMPARE(e.intervals.size(), 1);
    QCOMPARE(e.intervals.first().sessions, 3);
    QVERIFY(e.intervals.first().flags.contains(QLatin1String(flag::kGap)));
    QVERIFY(e.latest.has_flag(flag::kGap));
    QVERIFY(std::abs(*e.latest.value - 0.03e6 * 101.25) < 1e-3);
    // The 20-session window is only partly covered and says so.
    QVERIFY(e.sum_20.has_flag(flag::kPartial));
    QVERIFY(e.coverage_20.value && *e.coverage_20.value < 100.0);
}

void TstEtfResearchCalc::one_capture_is_record_too_young() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 2);
    const EstimatedFlow e = estimate_flow({capture(d[0], 1e8, 10.0)}, BarSeries{}, {}, d[1]);
    QVERIFY(!e.latest.value);
    QCOMPARE(e.latest.reason, QStringLiteral("record_too_young_one_capture_session"));
    const EstimatedFlow none = estimate_flow({}, BarSeries{}, {}, d[1]);
    QVERIFY(none.latest.has_flag(flag::kNoObservation));
}

void TstEtfResearchCalc::positive_negative_and_zero_flow() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 4);
    const BarSeries bars = closes(QStringLiteral("F"), d, {100.0, 100.5, 100.25, 101.25});
    const QVector<FundCapture> caps{capture(d[0], 100e6, 100.0, 1.00e6), capture(d[1], 1.02e6 * 100.5, 100.5, 1.02e6),
                                    capture(d[2], 1.01e6 * 100.25, 100.25, 1.01e6),
                                    capture(d[3], 1.01e6 * 101.25, 101.25, 1.01e6)};
    const EstimatedFlow e = estimate_flow(caps, bars, {}, d[3]);
    QCOMPARE(e.intervals.size(), 3);
    QVERIFY(*e.intervals[0].e1_implied_shares > 0);
    QVERIFY(*e.intervals[1].e1_implied_shares < 0);
    QVERIFY(std::abs(*e.intervals[2].e1_implied_shares) < 1e-6); // price move only: zero, not missing
    QVERIFY(e.latest.has_flag(flag::kActualZero));
    QVERIFY(e.sum_5.value);
    QVERIFY(std::abs(*e.sum_5.value - (0.02e6 * 100.5 - 0.01e6 * 100.25)) < 1e-3);
}

void TstEtfResearchCalc::business_cycle_probabilities_and_missing_input() {
    QHash<QString, MacroSeries> fred;
    QVector<QPair<QDate, double>> slope, oas, cfnai;
    const QDate end(2026, 9, 30);
    for (int i = 0; i < 400; ++i) {
        const QDate dd = end.addDays(-399 + i);
        slope.append({dd, 0.5 + 0.002 * i}); // rising slope, last value high
        oas.append({dd, 5.0 - 0.004 * i});   // tightening spread
    }
    for (int i = 0; i < 60; ++i)
        cfnai.append({QDate(2021, 10, 1).addMonths(i), -0.5 + 0.02 * i + (i > 56 ? 0.3 : 0.0)});
    fred.insert(QStringLiteral("T10Y2Y"), macro(QStringLiteral("T10Y2Y"), slope));
    fred.insert(QStringLiteral("BAMLH0A0HYM2"), macro(QStringLiteral("BAMLH0A0HYM2"), oas));
    fred.insert(QStringLiteral("CFNAI"), macro(QStringLiteral("CFNAI"), cfnai));
    QHash<QString, ResearchValue> z;
    const BusinessCycle bc = compute_business_cycle(fred, nullptr, nullptr, end, &z);
    QVERIFY(bc.level.value);
    const double sum = bc.p_recovery + bc.p_expansion + bc.p_slowdown + bc.p_contraction;
    QVERIFY(std::abs(sum - 1.0) < 1e-12);
    QCOMPARE(bc.phase.label, QStringLiteral("EXPANSION"));
    QCOMPARE(bc.phase.credibility, Credibility::Experimental); // Stovall mapping not validated here
    QCOMPARE(z.size(), 11);
    QVERIFY(z.value(QStringLiteral("XLK")).value);
    // Remove the activity series: nothing becomes a neutral 0.
    fred.remove(QStringLiteral("CFNAI"));
    QHash<QString, ResearchValue> z2;
    const BusinessCycle missing = compute_business_cycle(fred, nullptr, nullptr, end, &z2);
    QVERIFY(!missing.level.value);
    QCOMPARE(missing.level.reason, QStringLiteral("cycle_inputs_incomplete"));
    for (const auto& v : z2)
        QVERIFY(!v.value);
}

void TstEtfResearchCalc::ols_recovers_coefficients() {
    math::Mat x;
    math::Vec y;
    for (int i = 0; i < 200; ++i) {
        const double a = std::sin(i * 0.37), b = std::cos(i * 0.11) + 0.01 * i;
        x.push_back({a, b});
        y.push_back(1.0 + 2.0 * a - 3.0 * b + 0.001 * std::sin(i * 1.7));
    }
    const auto f = math::ols(x, y);
    QVERIFY(f);
    QVERIFY(std::abs(f->beta[0] - 1.0) < 1e-2);
    QVERIFY(std::abs(f->beta[1] - 2.0) < 1e-2);
    QVERIFY(std::abs(f->beta[2] + 3.0) < 1e-2);
    QVERIFY(f->r2 > 0.999);
}

void TstEtfResearchCalc::confluence_needs_three_layers() {
    auto v = [](double x) {
        return graded(x, EvidenceClass::Model, Credibility::Medium, {}, QStringLiteral("z"), QStringLiteral("m"),
                      QStringLiteral("s"), QDate());
    };
    const QHash<QString, double> w{{QStringLiteral("BC"), 0.3},
                                   {QStringLiteral("MOM"), 0.35},
                                   {QStringLiteral("VAL"), 0.2},
                                   {QStringLiteral("F"), 0.15}};
    const ResearchValue na = ResearchValue::unavailable(QStringLiteral("x"));
    double disp = 0;
    const ResearchValue two = confluence_score({{QStringLiteral("BC"), v(1)},
                                                {QStringLiteral("MOM"), v(1)},
                                                {QStringLiteral("VAL"), na},
                                                {QStringLiteral("F"), na}},
                                               w, &disp);
    QVERIFY(!two.value);
    const ResearchValue three = confluence_score({{QStringLiteral("BC"), v(1)},
                                                  {QStringLiteral("MOM"), v(-1)},
                                                  {QStringLiteral("VAL"), v(2)},
                                                  {QStringLiteral("F"), na}},
                                                 w, &disp);
    QVERIFY(three.value);
    QVERIFY(std::abs(*three.value - (0.3 - 0.35 + 0.4) / 0.85) < 1e-12);
    QVERIFY(three.has_flag(flag::kPartial));
    QCOMPARE(three.credibility, Credibility::Experimental);
    QCOMPARE(model_band(0.7), QStringLiteral("STRONG"));
    QCOMPARE(model_band(-0.7), QStringLiteral("WEAK"));
    QCOMPARE(model_band(-0.3), QStringLiteral("BELOW"));
}

void TstEtfResearchCalc::cftc_positioning_context() {
    // 160 weekly reports: OI 1000, non-commercial long rises 300 -> 459, short 200.
    QHash<QString, MacroSeries> f;
    MacroSeries oi, lo, sh;
    QDate d(2023, 9, 5); // a Tuesday
    for (int i = 0; i < 160; ++i, d = d.addDays(7)) {
        oi.points.append({d, 1000.0});
        lo.points.append({d, 300.0 + i});
        sh.points.append({d, 200.0});
    }
    f.insert(QStringLiteral("open_interest"), oi);
    f.insert(QStringLiteral("non_commercial_long"), lo);
    f.insert(QStringLiteral("non_commercial_short"), sh);
    const QDate last = oi.points.last().date;
    const CftcContext c = cftc_positioning(QStringLiteral("gold"), f, last.addDays(4));
    QCOMPARE(c.report_date, last);
    QCOMPARE(c.reports, 160);
    QVERIFY(std::abs(*c.net_pct_oi.value - (459.0 - 200.0) / 1000.0 * 100.0) < 1e-12);
    QCOMPARE(c.net_pct_oi.evidence, EvidenceClass::Proxy); // context, never MEASURED flow
    QCOMPARE(*c.percentile_3y.value, 100.0);               // the highest of the last 156
    QVERIFY(std::abs(*c.change_4w_pp.value - 0.4) < 1e-12);
    QCOMPARE(c.net_pct_oi.credibility, Credibility::High);
    // Three weeks later the report is stale: graded down.
    const CftcContext stale = cftc_positioning(QStringLiteral("gold"), f, last.addDays(21));
    QCOMPARE(stale.net_pct_oi.credibility, Credibility::Low);
    QVERIFY(stale.net_pct_oi.has_flag(flag::kStale));
    // Short history: < 52 reports graded down, < 26 unavailable, nothing stored unavailable.
    QHash<QString, MacroSeries> few = f;
    for (auto& s : few)
        s.points = s.points.mid(s.points.size() - 30);
    QCOMPARE(cftc_positioning(QStringLiteral("gold"), few, last).percentile_3y.credibility, Credibility::Medium);
    for (auto& s : few)
        s.points = s.points.mid(s.points.size() - 20);
    QVERIFY(!cftc_positioning(QStringLiteral("gold"), few, last).percentile_3y.value);
    const CftcContext none = cftc_positioning(QStringLiteral("gold"), {}, last);
    QVERIFY(!none.net_pct_oi.value);
    QCOMPARE(none.net_pct_oi.reason, QStringLiteral("no_cftc_observation"));
}

void TstEtfResearchCalc::correlation_geometry() {
    const auto d = nyse_sessions(QDate(2026, 9, 30), 80);
    QVector<BarSeries> raw;
    for (int s = 0; s < 4; ++s) {
        BarSeries b;
        b.symbol = QStringLiteral("S%1").arg(s);
        double lvl = 100;
        for (int t = 0; t < d.size(); ++t) {
            const double common = 0.01 * std::sin(t * 0.9);
            const double idio = 0.004 * std::sin(t * (1.3 + s) + s);
            lvl *= 1 + common + idio;
            b.bars.append({d[t], lvl, 1e6});
        }
        raw.append(b);
    }
    QVector<TrIndex> idx;
    for (const auto& b : raw)
        idx.append(make_tr_index(b));
    QVector<const TrIndex*> ptr;
    for (const auto& t : idx)
        ptr.append(&t);
    const CorrelationView v = correlation_view(QStringLiteral("3m"), ptr, 63, false, nullptr);
    QCOMPARE(v.observations, 63);
    QCOMPARE(v.corr.size(), 4);
    for (int i = 0; i < 4; ++i)
        QVERIFY(std::abs(v.corr[i][i] - 1.0) < 1e-12);
    QVERIFY(std::abs(v.wedge[0][1] - std::sqrt(1 - v.corr[0][1] * v.corr[0][1])) < 1e-12);
    QVERIFY(*v.avg_abs_corr.value > 0.5);
    QVERIFY(v.k_signal >= 1);
    QCOMPARE(v.calibration, QStringLiteral("heuristic"));
    QCOMPARE(v.corr_label.credibility, Credibility::Low); // heuristic thresholds are capped
    // det^(1/N) equals the geometric mean of the eigenvalues.
    math::Mat c;
    for (const auto& r : v.corr)
        c.push_back(math::Vec(r.begin(), r.end()));
    const math::Eigen e = math::jacobi_eigen(c);
    double g = 1;
    for (double l : e.values)
        g *= l;
    QVERIFY(std::abs(std::pow(g, 0.25) - *v.geom_score.value) < 1e-9);
    // PCA positions fit in [-1, 1].
    for (const auto& p : v.pca)
        QVERIFY(std::abs(p.first) <= 1 + 1e-12 && std::abs(p.second) <= 1 + 1e-12);
}

void TstEtfResearchCalc::weekly_regime_uses_completed_weeks() {
    // Data through Wednesday 2026-09-30: that week is still running and must
    // not enter the weekly (2Y) correlation view; its last return ends on the
    // previous Friday, 09-25.
    const auto d = nyse_sessions(QDate(2026, 9, 30), 120);
    const TrIndex a = make_tr_index(series(QStringLiteral("A"), d, 50, 0.001, 0.03, 9));
    const TrIndex b = make_tr_index(series(QStringLiteral("B"), d, 70, -0.0005, 0.02, 13));
    QVector<QDate> dates;
    const auto cols = common_returns({&a, &b}, 0, &dates, true);
    QVERIFY(!dates.isEmpty());
    QCOMPARE(dates.last(), QDate(2026, 9, 25));
    QCOMPARE(dates.last().dayOfWeek(), 5);
    // Through Friday 10-02 the week is complete and is used.
    const auto f = nyse_sessions(QDate(2026, 10, 2), 120);
    const TrIndex fa = make_tr_index(series(QStringLiteral("A"), f, 50, 0.001, 0.03, 9));
    const TrIndex fb = make_tr_index(series(QStringLiteral("B"), f, 70, -0.0005, 0.02, 13));
    common_returns({&fa, &fb}, 0, &dates, true);
    QCOMPARE(dates.last(), QDate(2026, 10, 2));
}

void TstEtfResearchCalc::hmm_recovers_two_regimes() {
    math::Mat x;
    for (int i = 0; i < 400; ++i) {
        const bool hi = (i / 50) % 2 == 1;
        x.push_back({(hi ? 5.0 : 0.0) + 0.3 * std::sin(i * 1.3)});
    }
    const auto h = math::hmm_fit(x, 2, 0, 200, 1e-6, 1e-4);
    QVERIFY(h);
    QVERIFY(std::abs(h->means[0][0]) < 0.2);
    QVERIFY(std::abs(h->means[1][0] - 5.0) < 0.2);
    const auto path = math::hmm_viterbi(*h, x);
    int correct = 0;
    for (int i = 0; i < 400; ++i)
        correct += path[static_cast<size_t>(i)] == ((i / 50) % 2) ? 1 : 0;
    QVERIFY(correct >= 395);
    const auto f = math::hmm_filter(*h, x);
    QVERIFY(f);
    QVERIFY(std::abs(f->first.back()[0] + f->first.back()[1] - 1.0) < 1e-12);
}

void TstEtfResearchCalc::mrs_thresholds_ordered() {
    QVector<QPair<QDate, double>> scores;
    QDate d(2015, 1, 1);
    for (int i = 0; i < 1600; ++i) {
        const int regime = (i / 100) % 4;
        scores.append({d.addDays(i), 0.3 + 0.15 * regime + 0.02 * std::sin(i * 0.77)});
    }
    const MrsFit f = fit_mrs(scores);
    QVERIFY(f.ready);
    QCOMPARE(f.thresholds.size(), 3);
    QVERIFY(f.thresholds[0] < f.thresholds[1] && f.thresholds[1] < f.thresholds[2]);
    QCOMPARE(threshold_corr_label(0.9, f.thresholds), QStringLiteral("CRISIS"));
    QCOMPARE(threshold_corr_label(0.2, f.thresholds), QStringLiteral("DIVERGENT"));
    QCOMPARE(f.threshold_std.size(), 3);
}

void TstEtfResearchCalc::snapshot_end_to_end_and_deterministic() {
    QString err;
    const auto u = load_universe(QLatin1String(kUniverseResourcePath), &err);
    QVERIFY(u);
    // Reviewed holding-symbol map: a missing suffix is added for the same
    // security; an ambiguous code stays as listed; a cash fund is excluded.
    QCOMPARE(u->holding_research_symbol(QStringLiteral("00939")), QStringLiteral("0939.HK"));
    QCOMPARE(u->holding_research_symbol(QStringLiteral("ADVANC-R")), QStringLiteral("ADVANC-R.BK"));
    QCOMPARE(u->holding_research_symbol(QStringLiteral("ING.AS")), QStringLiteral("INGA.AS"));
    QCOMPARE(u->holding_research_symbol(QStringLiteral("KAP")), QStringLiteral("KAP")); // ambiguous venue: unmapped
    QCOMPARE(u->holding_research_symbol(QStringLiteral("NVDA")), QStringLiteral("NVDA"));
    QVERIFY(u->holding_symbol_excluded.contains(QStringLiteral("XTSLA")));
    ResearchInputs in;
    in.universe = *u;
    in.as_of = QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC);
    in.known_at = in.as_of;
    in.expected_us_session = QDate(2026, 9, 30);
    const auto d = nyse_sessions(QDate(2026, 9, 30), 320);
    int k = 0;
    for (const auto& i : u->instruments) {
        if (i.symbol == QLatin1String("EWT") || i.symbol == QLatin1String("^SET.BK"))
            continue; // no observation for these two
        BarSeries s = series(i.symbol, d, 50 + k, 0.0002 * (k % 7) - 0.0004, 0.02, 15 + k % 5, 1e6 * (1 + k % 3));
        if (i.symbol == QLatin1String("XLU"))
            s.bars.removeLast(); // stale by one session
        in.bars.insert(i.symbol, s);
        ++k;
    }
    {
        auto& sb = in.bars[QStringLiteral("SPY")].bars;
        sb[316].close = sb[318].close * 0.992; // distinct closes so each NAV dates to one session
        sb[317].close = sb[318].close * 0.996;
        sb[319].close = sb[318].close * 1.004;
        const double n0 = sb[318].close, n1 = sb[319].close;
        in.funds.insert(QStringLiteral("SPY"),
                        {capture(d[318], 8.57e8 * n0, n0, 8.57e8), capture(d[319], 8.58e8 * n1, n1, 8.57e8)});
    }
    MeasuredMonth mm;
    mm.month = QDate(2026, 7, 1);
    mm.flow_usd = 0.0;
    mm.quality = QStringLiteral("CONFIRMED");
    mm.reporting_key = QStringLiteral("0000884394/");
    in.measured.insert(QStringLiteral("SPY"), {mm});
    const ResearchSnapshot s = compute_snapshot(in);
    QCOMPARE(s.rows.size(), u->instruments.size());
    const UniverseRow* ewt = s.row(QStringLiteral("EWT"));
    QVERIFY(ewt);
    QCOMPARE(ewt->freshness, QStringLiteral("no_history"));
    QVERIFY(!ewt->ret.m1.value);
    QVERIFY(ewt->ret.m1.has_flag(flag::kNoObservation));
    const UniverseRow* xlu = s.row(QStringLiteral("XLU"));
    QVERIFY(xlu->stale);
    QCOMPARE(xlu->ret.m1.credibility, Credibility::Low);
    const UniverseRow* spy = s.row(QStringLiteral("SPY"));
    QCOMPARE(spy->flow_evidence, EvidenceClass::Measured);
    QVERIFY(spy->measured.latest.has_flag(flag::kActualZero));
    QCOMPARE(spy->measured.latest.credibility, Credibility::NotGraded);
    QCOMPARE(spy->measured.latest.source_quality, QStringLiteral("CONFIRMED"));
    QVERIFY(spy->est.latest.value); // estimated flow stays separately visible beside measured
    QCOMPARE(spy->est.latest.evidence, EvidenceClass::Estimated);
    const UniverseRow* xlk = s.row(QStringLiteral("XLK"));
    QCOMPARE(xlk->flow_evidence, EvidenceClass::Proxy);
    QVERIFY(xlk->turnover_share.value);
    QVERIFY(!s.row(QStringLiteral("GLD"))->turnover_share.value);
    // Countries: Taiwan has no history here; Q for regional funds is not applicable.
    bool saw_regional = false;
    for (const auto& c : s.countries) {
        if (c.type == QLatin1String("regional")) {
            saw_regional = true;
            QVERIFY(!c.q_z.value);
            QCOMPARE(c.q_z.reason, QStringLiteral("not_applicable_regional_fund"));
        }
        if (c.symbol == QLatin1String("EWT"))
            QVERIFY(!c.composite.value);
    }
    QVERIFY(saw_regional);
    // The SET index has no history: baskets fall back to TDEX.BK and say so.
    QCOMPARE(s.th_benchmark_used, QStringLiteral("TDEX.BK"));
    QVERIFY(s.th_benchmark_fallback);
    // No FRED data: the business cycle is unavailable, not neutral.
    QVERIFY(!s.sectors.cycle.level.value);
    QCOMPARE(s.regime.views.size(), 5);
    // Determinism: the same inputs give byte-identical JSON.
    const QByteArray a = QJsonDocument(snapshot_to_json(s)).toJson(QJsonDocument::Compact);
    const QByteArray b = QJsonDocument(snapshot_to_json(compute_snapshot(in))).toJson(QJsonDocument::Compact);
    QCOMPARE(a, b);
    // Whole document: every UNAVAILABLE value says why and carries no number;
    // every calculated (ESTIMATED / PROXY / MODEL) value carries a grade.
    int unavailable = 0, graded_values = 0;
    std::function<void(const QJsonValue&, const QString&)> walk = [&](const QJsonValue& v, const QString& path) {
        if (v.isArray()) {
            for (const QJsonValue& x : v.toArray())
                walk(x, path + QStringLiteral("[]"));
            return;
        }
        if (!v.isObject())
            return;
        const QJsonObject o = v.toObject();
        const QString ev = o.value(QStringLiteral("evidence")).toString();
        if (ev == QLatin1String("UNAVAILABLE")) {
            ++unavailable;
            QVERIFY2(!o.value(QStringLiteral("reason")).toString().isEmpty(), qPrintable(path));
            QVERIFY2(o.value(QStringLiteral("value")).isNull(), qPrintable(path));
        } else if (ev == QLatin1String("MEASURED")) {
            // Observations carry the source's quality, never a credibility grade.
            QVERIFY2(o.value(QStringLiteral("credibility")).toString().isEmpty(), qPrintable(path));
        } else if (ev == QLatin1String("ESTIMATED") || ev == QLatin1String("PROXY") || ev == QLatin1String("MODEL")) {
            ++graded_values;
            QVERIFY2(!o.value(QStringLiteral("credibility")).toString().isEmpty(), qPrintable(path));
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            walk(it.value(), path + QLatin1Char('.') + it.key());
    };
    walk(QJsonDocument::fromJson(a).object(), QString());
    QVERIFY(unavailable > 0 && graded_values > 0);
}

QTEST_GUILESS_MAIN(TstEtfResearchCalc)
#include "tst_etf_research_calc.moc"
