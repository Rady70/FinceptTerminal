// Detail panel (built for the selected row only) and off-screen capture of the
// ETF Flow & Sector Rotation workspace.
#include "screens/etf_research/EtfResearchFormat.h"
#include "screens/etf_research/EtfResearchScreen.h"
#include "screens/etf_research/EtfResearchWidgets.h"
#include "services/economics/CftcMarketCatalog.h"
#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QDir>
#include <QHash>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QToolButton>

namespace fincept::screens {

using namespace services::etf::research;
using namespace etfr;

namespace {

QString etfr_row_html(const QString& label, const ResearchValue& v, int digits = 2) {
    const auto& t = ui::ThemeManager::instance().tokens();
    QString val = fmt_value(v, digits);
    if (v.units == QLatin1String("usd") && v.value && v.evidence == EvidenceClass::Measured &&
        v.method.contains(QLatin1String("totalAssets")))
        val = fmt_usd(*v.value, false);
    QString tags = QStringLiteral("<span style='color:%1'>%2</span>")
                       .arg(evidence_color(v.usable() ? v.evidence : EvidenceClass::Unavailable).name(),
                            QLatin1String(evidence_tag(v.usable() ? v.evidence : EvidenceClass::Unavailable)));
    if (v.credibility != Credibility::NotGraded)
        tags += QStringLiteral(" <span style='color:%1'>%2</span>")
                    .arg(credibility_color(v.credibility).name(), QLatin1String(credibility_tag(v.credibility)));
    else if (!v.source_quality.isEmpty())
        tags += QStringLiteral(" <span style='color:%1'>Q:%2</span>")
                    .arg(QLatin1String(t.text_secondary), v.source_quality.toHtmlEscaped());
    QString extra;
    if (!v.usable())
        extra = QStringLiteral(" <span style='color:%1'>(%2)</span>").arg(QLatin1String(t.text_tertiary), v.reason);
    else if (!v.flags.isEmpty())
        extra = QStringLiteral(" <span style='color:%1'>%2</span>")
                    .arg(QLatin1String(t.text_tertiary), v.flags.join(QLatin1Char(' ')));
    if (v.effective.isValid())
        extra += QStringLiteral(" <span style='color:%1'>@%2</span>")
                     .arg(QLatin1String(t.text_tertiary), v.effective.toString(Qt::ISODate));
    return QStringLiteral(
               "<tr><td style='color:%1;padding-right:8px'>%2</td><td><b>%3</b></td><td>&nbsp;%4%5</td></tr>")
        .arg(QLatin1String(t.text_secondary), label.toHtmlEscaped(), val.toHtmlEscaped(), tags, extra);
}

QString etfr_reasons_html(const QString& label, const ResearchValue& v) {
    return QStringLiteral("<tr><td style='padding-right:8px'>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
        .arg(label.toHtmlEscaped(), QLatin1String(evidence_id(v.evidence)),
             v.credibility == Credibility::NotGraded ? QStringLiteral("—")
                                                     : QLatin1String(credibility_id(v.credibility)),
             (v.usable() ? v.credibility_reasons.join(QStringLiteral(" · ")) + QStringLiteral(" | ") + v.method +
                               QStringLiteral(" | ") + v.source
                         : QStringLiteral("unavailable: ") + v.reason)
                 .toHtmlEscaped());
}

} // namespace

void EtfResearchScreen::populate_detail() {
    if (!snap_)
        return;
    const UniverseRow* r = snap_->row(selected_);
    if (!r)
        return;
    const auto& t = ui::ThemeManager::instance().tokens();
    const auto& i = r->inst;
    QStringList roles;
    for (const QString& x : i.roles)
        roles << x.toUpper();
    detail_title_->setText(
        QStringLiteral("<span style='color:%1;font-size:15px;font-weight:700'>%2</span> "
                       "<span style='color:%3'>%4</span><br/><span style='color:%3'>%5 · %6 (%7)%8</span>")
            .arg(QLatin1String(t.accent), i.symbol.toHtmlEscaped(), QLatin1String(t.text_secondary),
                 (r->fund.name.isEmpty() ? i.name : r->fund.name).toHtmlEscaped(), roles.join(QStringLiteral(" · ")),
                 i.structure, i.structure_basis,
                 i.policy_excluded_from_aggregates() ? tr(" · leveraged/inverse: excluded from aggregates")
                                                     : QString()));

    // OVERVIEW
    QString o = QStringLiteral("<table cellspacing='0' cellpadding='3'>");
    o += QStringLiteral("<tr><td colspan='3' style='color:%1'><b>%2</b></td></tr>")
             .arg(QLatin1String(t.accent), tr("Flow evidence"));
    o += etfr_row_html(tr("Measured (SEC N-PORT month)"), r->measured.latest);
    o += etfr_row_html(tr("Measured, 3 months"), r->measured.sum_3m);
    o += etfr_row_html(tr("Estimated, latest interval"), r->est.latest);
    o += etfr_row_html(tr("Estimated, 20 sessions"), r->est.sum_20);
    o += etfr_row_html(tr("Estimated, % of AUM (20)"), r->est.pct_aum_20);
    o += QStringLiteral("<tr><td colspan='3' style='color:%1'><b>%2</b></td></tr>")
             .arg(QLatin1String(t.accent), tr("Market behaviour (PROXY) and models"));
    o += etfr_row_html(tr("1D / 1W"), r->ret.d1) + etfr_row_html(QString(), r->ret.w1);
    o += etfr_row_html(tr("1M / 3M"), r->ret.m1) + etfr_row_html(QString(), r->ret.m3);
    o += etfr_row_html(tr("1M vs %1").arg(r->ret.benchmark), r->ret.rel_m1);
    o += etfr_row_html(tr("RRG vs %1").arg(r->rrg.benchmark), r->rrg.quadrant);
    o += etfr_row_html(tr("Momentum z (%1)").arg(r->peer_group), r->momentum_z);
    o += etfr_row_html(tr("Momentum composite"), r->momentum_raw);
    o += etfr_row_html(tr("Model score %1").arg(r->model_band), r->model_score);
    o += etfr_row_html(tr("Turnover share / Δ / z"), r->turnover_share);
    if (r->turnover_delta_bp.usable())
        o += etfr_row_html(QString(), r->turnover_delta_bp) + etfr_row_html(QString(), r->turnover_z);
    o += etfr_row_html(tr("Above 200-day"), r->above_200dma, 0);
    o += etfr_row_html(tr("Trailing 12M distribution yield"), r->carry_12m);
    o += etfr_row_html(tr("IBKR vs Yahoo 1M price-return gap"), r->cross_check);
    if (!r->cftc.market.isEmpty()) {
        o += QStringLiteral("<tr><td colspan='3' style='color:%1'><b>%2</b></td></tr>")
                 .arg(QLatin1String(t.accent),
                      tr("CFTC positioning: %1 futures, non-commercial (legacy, futures only); market context, "
                         "not ETF flow")
                          .arg(services::cftc_market_definition(r->cftc.market).label.toHtmlEscaped()));
        o += etfr_row_html(tr("Net position, % of open interest"), r->cftc.net_pct_oi, 1) +
             etfr_row_html(tr("Percentile, last 156 reports"), r->cftc.percentile_3y, 0) +
             etfr_row_html(tr("Change over 4 reports"), r->cftc.change_4w_pp, 1);
    }
    o += QStringLiteral("<tr><td colspan='3' style='color:%1'><b>%2</b></td></tr>")
             .arg(QLatin1String(t.accent),
                  tr("Fund facts (Yahoo quote summary, capture %1, session %2 by %3)")
                      .arg(r->fund.captured_at.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                           r->fund.assumed_session.toString(Qt::ISODate), r->fund.session_rule.toHtmlEscaped()));
    o += etfr_row_html(tr("AUM"), r->fund.aum) + etfr_row_html(tr("NAV"), r->fund.nav);
    o += etfr_row_html(tr("Implied shares (AUM/NAV)"), r->fund.implied_shares) +
         etfr_row_html(tr("Reported shares"), r->fund.reported_shares) +
         etfr_row_html(tr("Implied vs reported"), r->fund.shares_gap_pct, 1);
    o += etfr_row_html(tr("Close vs NAV"), r->fund.nav_premium_pct);
    o += etfr_row_html(tr("Expense ratio"), r->fund.expense_pct) + etfr_row_html(tr("Yield"), r->fund.yield_pct);
    o += etfr_row_html(tr("Beta (3y)"), r->fund.beta3y) + etfr_row_html(tr("Trailing P/E"), r->fund.pe, 1);
    o += etfr_row_html(tr("YTD / 3y / 5y return"), r->fund.ytd_pct) + etfr_row_html(QString(), r->fund.ret3y_pct) +
         etfr_row_html(QString(), r->fund.ret5y_pct);
    o += QStringLiteral("<tr><td style='color:%1'>%2</td><td colspan='2'>%3</td></tr>")
             .arg(QLatin1String(t.text_secondary), tr("Family / category / exchange"),
                  QStringLiteral("%1 · %2 · %3 · inception %4")
                      .arg(r->fund.family, r->fund.category, r->fund.exchange, r->fund.inception.toString(Qt::ISODate))
                      .toHtmlEscaped());
    o += QStringLiteral("</table>");
    if (!i.note.isEmpty())
        o += QStringLiteral("<p style='color:%1'>%2</p>").arg(QLatin1String(t.warning), i.note.toHtmlEscaped());
    detail_overview_->setText(o);

    // CHART
    QVector<LineSeries> ls;
    if (!r->chart_dates.isEmpty()) {
        ls << LineSeries{i.symbol, r->chart_dates, r->chart_tr, token(&ui::ThemeTokens::accent), false};
        if (!r->chart_bench.isEmpty())
            ls << LineSeries{r->ret.benchmark, r->chart_dates, r->chart_bench, token(&ui::ThemeTokens::text_secondary),
                             true};
    }
    detail_chart_->set_series(
        ls, tr("Total-return index (=100 at start), %1 vs %2 · PROXY").arg(i.symbol, r->ret.benchmark), QString());
    QVector<RrgSeries> rs;
    if (!r->rrg.trail.isEmpty())
        rs << RrgSeries{i.symbol, i.symbol, r->rrg.trail, token(&ui::ThemeTokens::accent)};
    detail_rrg_->set_series(rs, tr("RRG trail vs %1 (MODEL)").arg(r->rrg.benchmark));

    // FLOW
    QVector<Bar> bars;
    const int from = std::max(0, static_cast<int>(r->measured.months.size()) - 12);
    for (int k = from; k < r->measured.months.size(); ++k) {
        const MeasuredMonth& m = r->measured.months[k];
        Bar b;
        b.label = m.month.toString(QStringLiteral("MMMyy"));
        b.value = m.flow_usd;
        b.value_text = m.flow_usd ? fmt_usd(*m.flow_usd) : na();
        b.tooltip = tr("%1 %2 · %3 · accession %4 · available %5")
                        .arg(m.reporting_key, m.quality, m.reason, m.accession,
                             m.available_from.toString(QStringLiteral("yyyy-MM-dd")));
        bars << b;
    }
    detail_measured_->set_bars(bars, r->measured.months.isEmpty()
                                         ? tr("Measured SEC N-PORT monthly flow: %1").arg(r->measured.latest.reason)
                                         : tr("Measured SEC N-PORT monthly flow (MEASURED, monthly, delayed)"));
    detail_measured_->setVisible(!r->measured.months.isEmpty());
    if (auto* fs = qobject_cast<QSplitter*>(detail_measured_->parentWidget()); fs && !r->measured.months.isEmpty()) {
        const int h = std::max(fs->height(), 2);
        fs->setSizes({h * 11 / 20, h * 9 / 20});
    }
    QString f;
    if (r->measured.months.isEmpty())
        f += tr("<b>Measured SEC N-PORT monthly flow</b>: <span style='color:%1'>UNAVAILABLE (%2)</span><br/><br/>")
                 .arg(QLatin1String(t.text_tertiary), r->measured.latest.reason.toHtmlEscaped());
    f += QStringLiteral("<b>%1</b><br/>").arg(tr("Estimated creation/redemption from MarketLab's own captures"));
    f += tr("Captures %1 on %2 NAV-dated sessions (%3 → %4). Agreement E1/E2: %5.<br/>")
             .arg(r->est.captures)
             .arg(r->est.capture_sessions)
             .arg(r->est.first_session.toString(Qt::ISODate), r->est.last_session.toString(Qt::ISODate),
                  r->est.agreement.isEmpty() ? na() : r->est.agreement);
    if (r->est.undated_captures > 0)
        f += tr("<span style='color:%1'>%2 capture(s) not used: the NAV matched no session unambiguously "
                "(%3).</span><br/>")
                 .arg(QLatin1String(t.warning))
                 .arg(r->est.undated_captures)
                 .arg(r->est.undated_reason.toHtmlEscaped());
    f += QStringLiteral("<table cellspacing='0' cellpadding='3'>") +
         etfr_row_html(tr("E1 implied shares"), r->est.latest) +
         etfr_row_html(tr("E2 reported shares"), r->est.latest_e2) +
         etfr_row_html(tr("E3 close-adjusted AUM"), r->est.latest_e3) + etfr_row_html(tr("5 sessions"), r->est.sum_5) +
         etfr_row_html(tr("20 sessions"), r->est.sum_20) + etfr_row_html(tr("Coverage 20"), r->est.coverage_20, 0) +
         etfr_row_html(tr("E1 month − SEC month"), r->est.validation_vs_measured) + QStringLiteral("</table>");
    if (!r->est.intervals.isEmpty()) {
        f += QStringLiteral("<table cellspacing='0' cellpadding='2'><tr style='color:%1'><td>%2</td><td>%3</td>"
                            "<td>%4</td><td>%5</td><td>%6</td><td>%7</td></tr>")
                 .arg(QLatin1String(t.text_secondary), tr("Interval"), tr("Sessions"), tr("E1"), tr("E2"), tr("E3"),
                      tr("Flags / reason"));
        const int start = std::max(0, static_cast<int>(r->est.intervals.size()) - 30);
        for (int k = r->est.intervals.size() - 1; k >= start; --k) {
            const FlowInterval& iv = r->est.intervals[k];
            auto opt = [](const std::optional<double>& v) { return v ? fmt_usd(*v) : na(); };
            f += QStringLiteral("<tr><td>%1→%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td><td>%7</td></tr>")
                     .arg(iv.from.toString(QStringLiteral("MM-dd")), iv.to.toString(QStringLiteral("MM-dd")))
                     .arg(iv.sessions)
                     .arg(opt(iv.e1_implied_shares), opt(iv.e2_reported_shares), opt(iv.e3_price_adjusted),
                          (iv.flags.join(QLatin1Char(' ')) + QLatin1Char(' ') + iv.reason).toHtmlEscaped());
        }
        f += QStringLiteral("</table>");
    }
    f += tr("<p style='color:%1'>Estimates use Yahoo's undated AUM/NAV, assigned to the prior completed NYSE session "
            "(ASSUMED_EFFECTIVE_DATE). A missed manual capture leaves a gap: the next interval spans several sessions "
            "and "
            "is never spread back over them.</p>")
             .arg(QLatin1String(t.text_tertiary));
    detail_flow_text_->setText(f);

    // HOLDINGS
    QString h;
    // A failed read is not "none published": say so, and when older holdings
    // are shown below, that they are not the latest read.
    if (!r->fund.holdings_read_failed.isEmpty())
        h = tr("<p style='color:%1'>The latest holdings read failed: %2</p>")
                .arg(QLatin1String(t.warning), r->fund.holdings_read_failed.toHtmlEscaped());
    if (r->fund.holdings.isEmpty() && !r->fund.holdings_read_failed.isEmpty()) {
        h += tr("<span style='color:%1'>No earlier holdings were captured for this fund.</span>")
                 .arg(QLatin1String(t.text_tertiary));
    } else if (r->fund.holdings.isEmpty()) {
        h = tr("<span style='color:%1'>No holdings captured for this fund (Yahoo publishes none, or no manual refresh "
               "has captured them).</span>")
                .arg(QLatin1String(t.text_tertiary));
    } else {
        h += tr("<b>Top holdings</b> (captured %1)")
                 .arg(r->fund.holdings_captured.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        h += QStringLiteral(
                 "<table cellspacing='0' cellpadding='4'><tr style='color:%1'><td>#</td><td>%2</td><td>%3</td>"
                 "<td>%4</td><td>%5</td><td>%6</td><td>%7</td><td>%8</td></tr>")
                 .arg(QLatin1String(t.text_secondary), tr("Symbol"), tr("Name"), tr("Weight"), tr("1M"), tr("vs SPY"),
                      tr("RRG"), tr("P/E"));
        QStringList restarted;
        for (const Holding& hd : r->fund.holdings) {
            const ConstituentRow* cr = nullptr;
            for (const auto& c : snap_->constituents)
                if (c.parent == i.symbol && c.symbol == hd.symbol)
                    cr = &c;
            if (cr && cr->ret.m1.has_flag(flag::kHistoryRestarted))
                restarted << QStringLiteral("%1: %2").arg(
                    hd.symbol, QString(cr->ret.m1.reason).replace(QLatin1Char('_'), QLatin1Char(' ')));
            h += QStringLiteral("<tr><td>%1</td><td><b>%2</b></td><td>%3</td><td>%4</td><td>%5</td><td>%6</td>"
                                "<td>%7</td><td>%8</td></tr>")
                     .arg(hd.rank)
                     .arg(hd.symbol.toHtmlEscaped(), hd.name.left(24).toHtmlEscaped(),
                          hd.weight ? QString::number(*hd.weight * 100, 'f', 2) + QLatin1Char('%') : na(),
                          cr ? fmt_value(cr->ret.m1) : na(), cr ? fmt_value(cr->ret.rel_m1) : na(),
                          cr ? quadrant_tag(cr->quadrant) : na(), cr ? fmt_value(cr->pe, 1) : na());
        }
        h += QStringLiteral("</table>");
        if (!restarted.isEmpty())
            h += tr("<p style='color:%1'>Returns unavailable across a suspected unadjusted corporate action "
                    "(one-session "
                    "close ratio outside [1/3, 3] with no recorded distribution): %2</p>")
                     .arg(QLatin1String(t.warning), restarted.join(QStringLiteral("; ")).toHtmlEscaped());
        for (const auto& a : snap_->constituent_aggregates)
            if (a.parent == i.symbol)
                h += tr("<p>Top-10 aggregates (MODEL, LOW): median P/E %1 · cap-weighted P/E %2 · median fwd P/E %3 · "
                        "median D/E %4 · weight covered %5% · currency %6</p>")
                         .arg(fmt_value(a.median_pe, 1), fmt_value(a.cap_weighted_pe, 1),
                              fmt_value(a.median_forward_pe, 1), fmt_value(a.median_debt_equity, 0))
                         .arg(a.weight_covered * 100, 0, 'f', 1)
                         .arg(a.currency_basis.isEmpty() ? na() : a.currency_basis);
    }
    detail_holdings_->setText(h);
    QVector<Bar> wb;
    QStringList keys = r->fund.sector_weights.keys();
    std::sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) {
        return r->fund.sector_weights[a] > r->fund.sector_weights[b];
    });
    for (const QString& k : keys) {
        Bar b;
        static const QHash<QString, QString> kShort = {
            {QStringLiteral("basic_materials"), QStringLiteral("MATL")},
            {QStringLiteral("consumer_cyclical"), QStringLiteral("CYCL")},
            {QStringLiteral("consumer_defensive"), QStringLiteral("STPL")},
            {QStringLiteral("industrials"), QStringLiteral("INDU")},
            {QStringLiteral("realestate"), QStringLiteral("REAL")},
            {QStringLiteral("financial_services"), QStringLiteral("FINL")},
            {QStringLiteral("technology"), QStringLiteral("TECH")},
            {QStringLiteral("healthcare"), QStringLiteral("HLTH")},
            {QStringLiteral("communication_services"), QStringLiteral("COMM")},
            {QStringLiteral("utilities"), QStringLiteral("UTIL")},
            {QStringLiteral("energy"), QStringLiteral("ENRG")}};
        b.label = kShort.value(k, k.left(6));
        b.value = r->fund.sector_weights[k] * 100.0;
        b.value_text = QString::number(*b.value, 'f', 0) + QLatin1Char('%');
        b.color = token(&ui::ThemeTokens::cyan);
        b.tooltip = k;
        wb << b;
    }
    detail_weights_->set_bars(wb, tr("Sector weights (Yahoo fund data)"));
    detail_weights_->setVisible(!wb.isEmpty());
    if (auto* hs = qobject_cast<QSplitter*>(detail_weights_->parentWidget()); hs && !wb.isEmpty()) {
        const int h = std::max(hs->height(), 2);
        hs->setSizes({h * 11 / 20, h * 9 / 20});
    }

    // PROVENANCE
    QString p = QStringLiteral("<b>%1</b><br/>").arg(tr("Identity"));
    p += tr("Yahoo symbol %1 · universe %2 · structure %3 (%4) · mechanism %5<br/>")
             .arg(i.symbol, snap_->universe_version, i.structure, i.structure_basis, i.mechanism);
    p += i.taxonomy_con_id
             ? tr("Taxonomy v2: IBKR conId %1, category %2<br/>").arg(*i.taxonomy_con_id).arg(i.taxonomy_category)
             : tr("Not in the reviewed taxonomy v2 seed (research curation only)<br/>");
    for (const auto& s : i.sec_reporting)
        p += tr("SEC reporting identity %1/%2 · %3 · %4 → %5 · %6 · <a href='%7'>evidence</a><br/>")
                 .arg(s.cik, s.series_id.isEmpty() ? tr("(registrant)") : s.series_id, s.relationship,
                      s.effective_from.toString(Qt::ISODate),
                      s.effective_to.isValid() ? s.effective_to.toString(Qt::ISODate) : tr("open"), s.identity_basis,
                      s.evidence_url);
    p += QStringLiteral("<br/><b>%1</b><table cellspacing='0' cellpadding='3'>").arg(tr("Grades and methods"));
    p += etfr_reasons_html(tr("Measured"), r->measured.latest) + etfr_reasons_html(tr("Estimated E1"), r->est.latest) +
         etfr_reasons_html(tr("1M return"), r->ret.m1) + etfr_reasons_html(tr("Relative 1M"), r->ret.rel_m1) +
         etfr_reasons_html(tr("RRG"), r->rrg.quadrant) + etfr_reasons_html(tr("Momentum z"), r->momentum_z) +
         etfr_reasons_html(tr("Model score"), r->model_score) +
         etfr_reasons_html(tr("Turnover share"), r->turnover_share) + etfr_reasons_html(tr("AUM"), r->fund.aum) +
         etfr_reasons_html(tr("IBKR cross-check"), r->cross_check);
    p += QStringLiteral("</table><br/>");
    p += tr("Frame: as_of %1 · known_at %2 · last stored session %3 (%4) · engine %5 · reference "
            "triphopp/bloomberg-terminal@%6")
             .arg(snap_->as_of.toString(Qt::ISODate), snap_->known_at.toString(Qt::ISODate),
                  r->last_bar.toString(Qt::ISODate), r->freshness, snap_->engine_version,
                  snap_->reference_commit.left(9));
    detail_provenance_->setText(p);
}

QJsonArray capture_etf_research_views(const ResearchSnapshot& s, const QString& dir, int width, int height) {
    QJsonArray files;
    EtfResearchScreen screen;
    screen.set_autoload(false);
    screen.setAttribute(Qt::WA_DontShowOnScreen, true);
    screen.resize(width, height);
    screen.show();
    screen.set_snapshot(s);
    auto save = [&](const QString& name) {
        QApplication::processEvents();
        const QPixmap pm = screen.grab();
        const QString path = QDir(dir).filePath(name + QStringLiteral(".png"));
        if (pm.save(path))
            files.append(QJsonObject{{"file", path},
                                     {"view", screen.current_view()},
                                     {"selected", screen.selected_symbol()},
                                     {"width", pm.width()},
                                     {"height", pm.height()}});
    };
    for (const QString& v : screen.view_ids()) {
        screen.show_view(v);
        save(QStringLiteral("etfr_%1").arg(v));
    }
    // Selected ETF detail: a sector with the most evidence.
    QString pick;
    for (const auto& r : s.rows)
        if (r.inst.has_role("us_sector") && r.fund.aum.usable()) {
            pick = r.inst.symbol;
            break;
        }
    if (pick.isEmpty() && !s.rows.isEmpty())
        pick = s.rows.first().inst.symbol;
    screen.show_view(QStringLiteral("universe"));
    screen.select_symbol(pick);
    screen.show_detail_tab(QStringLiteral("overview"));
    save(QStringLiteral("etfr_detail_%1_overview").arg(pick));
    screen.show_detail_tab(QStringLiteral("chart"));
    save(QStringLiteral("etfr_detail_%1_chart").arg(pick));
    screen.show_detail_tab(QStringLiteral("holdings"));
    save(QStringLiteral("etfr_detail_%1_holdings").arg(pick));
    // Measured flow present (SPY) and a row with no measured flow but proxy/model information.
    if (s.row(QStringLiteral("SPY"))) {
        screen.select_symbol(QStringLiteral("SPY"));
        screen.show_detail_tab(QStringLiteral("flow"));
        save(QStringLiteral("etfr_detail_SPY_flow_measured"));
    }
    for (const auto& r : s.rows)
        if (r.inst.has_role("theme") && !r.measured.latest.usable() && r.ret.m1.usable()) {
            screen.select_symbol(r.inst.symbol);
            screen.show_detail_tab(QStringLiteral("flow"));
            save(QStringLiteral("etfr_detail_%1_flow_no_measured").arg(r.inst.symbol));
            screen.show_detail_tab(QStringLiteral("overview"));
            save(QStringLiteral("etfr_detail_%1_overview_no_measured").arg(r.inst.symbol));
            break;
        }
    screen.show_detail_tab(QStringLiteral("provenance"));
    save(QStringLiteral("etfr_detail_provenance"));
    // Narrow window.
    screen.resize(820, height);
    screen.show_view(QStringLiteral("universe"));
    save(QStringLiteral("etfr_narrow_universe_detail"));
    screen.resize(820, height);
    screen.show_view(QStringLiteral("sectors"));
    save(QStringLiteral("etfr_narrow_sectors"));
    return files;
}

} // namespace fincept::screens
