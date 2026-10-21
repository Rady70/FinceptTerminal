#include "screens/dashboard/widgets/EtfFlowHighlightsWidget.h"

#include "screens/etf_research/EtfResearchBindings.h"
#include "screens/etf_research/EtfResearchFormat.h"
#include "services/ResearchDataEvents.h"
#include "ui/theme/Theme.h"

#include <QHBoxLayout>
#include <QLocale>
#include <QPointer>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cmath>

namespace fincept::screens::widgets {

namespace er = fincept::services::etf::research;

namespace {

struct EtfFlowRow {
    QString symbol;
    QString what; ///< sector / theme / country / exposure
    double usd = 0;
    QDate period;
};

QString etf_subject(const er::UniverseRow& r) {
    if (r.inst.has_role("us_sector"))
        return r.inst.sector;
    if (r.inst.has_role("theme"))
        return r.inst.theme;
    if (r.inst.has_role("country"))
        return r.inst.country;
    return r.inst.exposure;
}

/// Funds with a usable dollar value of `pick`, largest inflows and outflows.
template <typename Pick>
void etf_rank(const er::ResearchSnapshot& s, Pick pick, QVector<EtfFlowRow>& in, QVector<EtfFlowRow>& out,
              int per_side) {
    QVector<EtfFlowRow> all;
    for (const auto& r : s.rows) {
        if (!r.inst.is_fund())
            continue;
        const er::ResearchValue& v = pick(r);
        if (!v.value || v.units != QLatin1String("usd"))
            continue;
        all.append({r.inst.symbol, etf_subject(r), *v.value, v.effective});
    }
    // Rank only funds of the latest period (the same SEC month / estimate
    // interval end), so a ranking never compares flows of different periods.
    QDate latest;
    for (const auto& f : all)
        if (f.period.isValid() && (!latest.isValid() || f.period > latest))
            latest = f.period;
    if (latest.isValid())
        all.erase(std::remove_if(all.begin(), all.end(), [&latest](const EtfFlowRow& f) { return f.period != latest; }),
                  all.end());
    std::sort(all.begin(), all.end(), [](const EtfFlowRow& a, const EtfFlowRow& b) { return a.usd > b.usd; });
    for (const auto& f : all)
        if (f.usd > 0 && in.size() < per_side)
            in.append(f);
    for (auto it = all.crbegin(); it != all.crend(); ++it)
        if (it->usd < 0 && out.size() < per_side)
            out.append(*it);
}

/// Signed one-decimal number with a real minus sign.
QString etf_signed(double v) {
    const QString text = QString::number(std::abs(v), 'f', 1);
    if (std::abs(v) < 0.05)
        return QStringLiteral("0.0");
    return (v > 0 ? QStringLiteral("+") : QString::fromUtf8("\u2212")) + text;
}

} // namespace

EtfFlowHighlightsWidget::EtfFlowHighlightsWidget(QWidget* parent)
    : BaseWidget(tr("ETF FLOWS · SECTOR ROTATION"), parent, ui::colors::CYAN()) {
    body_ = new QWidget(this);
    body_->setObjectName("etfFlowHighlights");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    // Headline: the sector tilt and how many sectors beat the market.
    auto* hero = new QWidget(body_);
    hero->setObjectName("efHero");
    hero->setAttribute(Qt::WA_StyledBackground, true);
    auto* hl = new QHBoxLayout(hero);
    hl->setContentsMargins(10, 6, 10, 6);
    auto* left = new QVBoxLayout;
    left->setSpacing(0);
    tilt_caption_ = new QLabel(tr("SECTOR TILT (TRADING VOLUME)"));
    tilt_caption_->setObjectName(QStringLiteral("efCap"));
    tilt_ = new QLabel(QStringLiteral("--"));
    tilt_->setObjectName(QStringLiteral("efBig"));
    tilt_->setToolTip(tr("Defensive minus cyclical sectors' change in share of U.S. sector-ETF dollar turnover over 20 "
                         "sessions (a PROXY of rotation, not a flow)."));
    left->addWidget(tilt_caption_);
    left->addWidget(tilt_);
    auto* right = new QVBoxLayout;
    right->setSpacing(0);
    breadth_caption_ = new QLabel(tr("SECTORS BEATING SPY · 1M"));
    breadth_caption_->setObjectName(QStringLiteral("efCap"));
    breadth_caption_->setAlignment(Qt::AlignRight);
    breadth_ = new QLabel(QStringLiteral("--"));
    breadth_->setObjectName(QStringLiteral("efBig"));
    breadth_->setAlignment(Qt::AlignRight);
    right->addWidget(breadth_caption_);
    right->addWidget(breadth_);
    hl->addLayout(left);
    hl->addStretch();
    hl->addLayout(right);
    vl->addWidget(hero);

    sectors_title_ = new QLabel(tr("U.S. SECTORS · 1-MONTH TOTAL RETURN MINUS SPY"));
    sectors_title_->setObjectName(QStringLiteral("efHead"));
    vl->addWidget(sectors_title_);
    sectors_ = new charts::BarList(body_);
    sectors_->set_empty_text(tr("No sector returns stored yet: refresh the ETF Flow screen."));
    vl->addWidget(sectors_, 3);

    flows_title_ = new QLabel(tr("FUND FLOWS"));
    flows_title_->setObjectName(QStringLiteral("efHead"));
    vl->addWidget(flows_title_);
    flows_ = new charts::BarList(body_);
    vl->addWidget(flows_, 1);
    content_layout()->addWidget(body_);

    // Refresh = the ETF Flow screen's "Refresh ETF Research Data" (the same
    // sources, the same store), then recompute from the stored data.
    connect(this, &BaseWidget::refresh_requested, this, &EtfFlowHighlightsWidget::refresh_sources);
    // A refresh on the ETF Flow screen updated the store: recompute here too.
    connect(&services::ResearchDataEvents::instance(), &services::ResearchDataEvents::etf_saved, this,
            [this](QObject* origin) {
                if (origin != this && loaded_once_ && !refreshing_)
                    load();
            });
    apply_styles();
    set_loading(true);
}

void EtfFlowHighlightsWidget::apply_styles() {
    body_->setStyleSheet(
        QString("#etfFlowHighlights QLabel{background:transparent;font-size:10px;color:%1;}"
                "#etfFlowHighlights #efHero{background:%4;border-radius:2px;}"
                "#etfFlowHighlights QLabel#efBig{font-size:16px;font-weight:bold;}"
                "#etfFlowHighlights QLabel#efCap{font-size:9px;font-weight:bold;color:%2;"
                "letter-spacing:0.5px;}"
                "#etfFlowHighlights QLabel#efHead{font-size:9px;font-weight:bold;color:%3;"
                "padding-top:8px;letter-spacing:0.5px;}")
            .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_TERTIARY(), ui::colors::CYAN(), ui::colors::BG_RAISED()));
    sectors_->update();
    flows_->update();
    if (snap_)
        render(); // the tilt colour comes from the theme
}

void EtfFlowHighlightsWidget::on_theme_changed() {
    apply_styles();
}

void EtfFlowHighlightsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("ETF FLOWS · SECTOR ROTATION"));
    tilt_caption_->setText(tr("SECTOR TILT (TRADING VOLUME)"));
    breadth_caption_->setText(tr("SECTORS BEATING SPY · 1M"));
    sectors_title_->setText(tr("U.S. SECTORS · 1-MONTH TOTAL RETURN MINUS SPY"));
    sectors_->set_empty_text(tr("No sector returns stored yet: refresh the ETF Flow screen."));
    if (snap_)
        render();
}

void EtfFlowHighlightsWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!loaded_once_) {
        loaded_once_ = true;
        load();
    }
}

void EtfFlowHighlightsWidget::refresh_sources() {
    if (refreshing_ || etfr::manual_refresh_running()) {
        set_footer(tr("An ETF data refresh is already running; the data updates when it ends."), QString(), false);
        return;
    }
    refreshing_ = true;
    set_loading(true);
    QPointer<EtfFlowHighlightsWidget> self(this);
    etfr::start_manual_refresh(
        [self](const QString& stage, int done, int total) {
            if (self)
                self->set_footer(tr("Refreshing ETF data: %1 %2/%3").arg(stage).arg(done).arg(total));
        },
        [self](const er::RefreshResult& r) {
            if (!self)
                return;
            self->refreshing_ = false;
            self->refresh_error_ = (!r.ok && !r.error.isEmpty()) ? r.error.left(160) : QString();
            self->load();
            emit services::ResearchDataEvents::instance().etf_saved(self.data());
        });
}

void EtfFlowHighlightsWidget::load() {
    if (watcher_)
        return;
    set_loading(true);
    auto result = std::make_shared<std::shared_ptr<er::ResearchSnapshot>>();
    auto error = std::make_shared<QString>();
    watcher_ = new QFutureWatcher<void>(this);
    connect(watcher_, &QFutureWatcher<void>::finished, this, [this, result, error]() {
        watcher_->deleteLater();
        watcher_ = nullptr;
        set_loading(false);
        if (!*result) {
            set_footer(tr("Stored ETF research data could not be read: %1").arg(error->left(120)), *error, true);
            return;
        }
        snap_ = *result;
        render();
    });
    // Reads only: the same stored observations the ETF Flow screen computes from.
    watcher_->setFuture(QtConcurrent::run([result, error]() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        auto snap = etfr::load_research_snapshot(now, now);
        if (snap.is_err()) {
            *error = QString::fromStdString(snap.error());
            return;
        }
        *result = std::make_shared<er::ResearchSnapshot>(snap.value());
    }));
}

void EtfFlowHighlightsWidget::render() {
    const er::ResearchSnapshot& s = *snap_;
    const auto& tilt = s.tilt;
    tilt_->setText(tilt.state.isEmpty() ? QStringLiteral("—") : tilt.state);
    const QString tilt_color = tilt.state == QLatin1String("DEFENSIVE")  ? ui::colors::WARNING()
                               : tilt.state == QLatin1String("CYCLICAL") ? ui::colors::POSITIVE()
                                                                         : ui::colors::TEXT_PRIMARY();
    tilt_->setStyleSheet(QString("color:%1;").arg(tilt_color));
    breadth_->setText(tilt.breadth_total > 0 ? tr("%1 of %2").arg(tilt.above_bench).arg(tilt.breadth_total)
                                             : QStringLiteral("—"));

    // The eleven U.S. sector funds: 1-month total return minus SPY, best first.
    QVector<charts::BarRow> sectors;
    QDate last_session;
    for (const auto& r : s.rows) {
        if (!r.inst.has_role("us_sector") || !r.ret.rel_m1.value)
            continue;
        const double rel = *r.ret.rel_m1.value;
        const QString m1 = r.ret.m1.value ? tr("1M %1%").arg(etf_signed(*r.ret.m1.value)) : QString();
        sectors.append(
            {QStringLiteral("%1 %2").arg(r.inst.symbol, r.inst.sector), etf_signed(rel) + tr(" pp"), m1, rel,
             tr("%1 (%2): 1-month total return %3%, SPY-relative %4 pp, last session %5")
                 .arg(r.inst.symbol, r.inst.sector, r.ret.m1.value ? etf_signed(*r.ret.m1.value) : QStringLiteral("—"),
                      etf_signed(rel), r.ret.last_session.toString(Qt::ISODate))});
        if (r.ret.last_session.isValid() && (!last_session.isValid() || r.ret.last_session > last_session))
            last_session = r.ret.last_session;
    }
    std::sort(sectors.begin(), sectors.end(),
              [](const charts::BarRow& a, const charts::BarRow& b) { return a.value > b.value; });
    sectors_->set_rows(sectors, true);
    sectors_title_->setText(last_session.isValid()
                                ? tr("U.S. SECTORS · 1-MONTH TOTAL RETURN MINUS SPY · TO %1")
                                      .arg(QLocale::c().toString(last_session, QStringLiteral("d MMM")))
                                : tr("U.S. SECTORS · 1-MONTH TOTAL RETURN MINUS SPY"));

    // Fund flows: SEC N-PORT measured months first, else the daily estimate.
    QVector<EtfFlowRow> in, out;
    etf_rank(s, [](const er::UniverseRow& r) -> const er::ResearchValue& { return r.measured.latest; }, in, out, 3);
    QString flows_kind = tr("FUND FLOWS · SEC N-PORT, MONTHLY (MEASURED)");
    QString period_fmt = QStringLiteral("MMM yy");
    if (in.isEmpty() && out.isEmpty()) {
        etf_rank(s, [](const er::UniverseRow& r) -> const er::ResearchValue& { return r.est.best; }, in, out, 3);
        flows_kind = tr("FUND FLOWS · LATEST DAILY ESTIMATE (AUM / NAV)");
        period_fmt = QStringLiteral("d MMM");
    }
    QVector<charts::BarRow> flows;
    for (const auto* list : {&in, &out})
        for (const auto& f : *list)
            flows.append({QStringLiteral("%1 %2").arg(f.symbol, f.what), etfr::fmt_usd(f.usd),
                          f.period.isValid() ? QLocale::c().toString(f.period, period_fmt) : QString(), f.usd, f.what});
    flows_->set_rows(flows, true);
    flows_title_->setText(flows.isEmpty() ? tr("FUND FLOWS") : flows_kind);
    // Why there is no measured flow: the SEC step's own status of the last refresh.
    QString sec_reason = tr("SEC monthly flows: the ETF data has not been refreshed yet.");
    for (const auto& src : s.sources) {
        if (src.stage != QLatin1String("sec_nport"))
            continue;
        if (src.status == QLatin1String("NOT_CONFIGURED"))
            sec_reason = tr("SEC monthly flows need the SEC User-Agent (Settings → Data Sources).");
        else if (src.items_requested == 0)
            sec_reason = tr("SEC monthly flows: no fund is linked to its SEC filer (CIK) yet, so none was looked up.");
        else
            sec_reason = tr("SEC monthly flows: last lookup %1 (%2 of %3 filers read).")
                             .arg(src.status.toLower())
                             .arg(src.items_ok)
                             .arg(src.items_requested);
    }
    flows_->set_empty_text(tr("No fund-flow data stored. %1 Daily estimates appear once a fund's reported assets "
                              "change between captures.")
                               .arg(sec_reason));
    flows_->setMinimumHeight(flows.isEmpty() ? 44 : 0);

    int funds = 0, measured = 0, estimated = 0;
    for (const auto& r : s.rows) {
        if (!r.inst.is_fund())
            continue;
        ++funds;
        measured += r.measured.latest.value ? 1 : 0;
        estimated += r.est.best.value ? 1 : 0;
    }
    QString footer =
        s.last_refresh_finished.isValid()
            ? tr("Stored data · refreshed %1 UTC").arg(s.last_refresh_finished.toString(QStringLiteral("MM-dd HH:mm")))
            : tr("Stored data · no refresh recorded");
    footer += tr(" · flows for %1 of %2 funds").arg(measured + estimated).arg(funds);
    int unhealthy = 0;
    QStringList unhealthy_names;
    for (const auto& src : s.sources)
        if (src.status == QLatin1String("FAILED") || src.status == QLatin1String("PARTIAL") ||
            src.status == QLatin1String("STALE")) {
            ++unhealthy;
            unhealthy_names << QStringLiteral("%1 (%2)").arg(src.stage, src.status);
        }
    if (unhealthy > 0)
        footer += tr(" · %1 source(s) failed or stale").arg(unhealthy);
    if (!refresh_error_.isEmpty()) {
        footer = tr("Last refresh failed: %1 · ").arg(refresh_error_) + footer;
        ++unhealthy;
    }
    set_footer(footer,
               tr("Same stored snapshot as ETF Flow & Sector Rotation (computed %1 UTC). Refresh the data on that "
                  "screen; this button only re-reads it.%2")
                   .arg(s.computed_at.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                        unhealthy_names.isEmpty() ? QString() : QStringLiteral("\n") + unhealthy_names.join(", ")),
               unhealthy > 0 || !s.last_refresh_finished.isValid());
}

} // namespace fincept::screens::widgets
