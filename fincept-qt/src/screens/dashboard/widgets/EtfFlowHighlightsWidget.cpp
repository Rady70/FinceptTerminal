#include "screens/dashboard/widgets/EtfFlowHighlightsWidget.h"

#include "screens/etf_research/EtfResearchBindings.h"
#include "screens/etf_research/EtfResearchFormat.h"
#include "ui/theme/Theme.h"

#include <QLocale>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>

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
    std::sort(all.begin(), all.end(), [](const EtfFlowRow& a, const EtfFlowRow& b) { return a.usd > b.usd; });
    for (const auto& f : all)
        if (f.usd > 0 && in.size() < per_side)
            in.append(f);
    for (auto it = all.crbegin(); it != all.crend(); ++it)
        if (it->usd < 0 && out.size() < per_side)
            out.append(*it);
}

void etf_clear_grid(QGridLayout* grid) {
    while (QLayoutItem* item = grid->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void etf_fill(QGridLayout* grid, const QVector<EtfFlowRow>& in, const QVector<EtfFlowRow>& out,
              const QString& period_fmt, const QString& empty_text) {
    etf_clear_grid(grid);
    int r = 0;
    auto add = [&](const EtfFlowRow& f, bool inflow) {
        auto* sym = new QLabel(f.symbol);
        sym->setObjectName(QStringLiteral("efSym"));
        sym->setToolTip(f.what);
        auto* what = new QLabel(f.what);
        what->setObjectName(QStringLiteral("efSub"));
        auto* usd = new QLabel(etfr::fmt_usd(f.usd));
        usd->setObjectName(inflow ? QStringLiteral("efIn") : QStringLiteral("efOut"));
        usd->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* when = new QLabel(f.period.isValid() ? QLocale::c().toString(f.period, period_fmt) : QString());
        when->setObjectName(QStringLiteral("efSub"));
        when->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(sym, r, 0);
        grid->addWidget(what, r, 1);
        grid->addWidget(usd, r, 2);
        grid->addWidget(when, r, 3);
        ++r;
    };
    for (const auto& f : in)
        add(f, true);
    for (const auto& f : out)
        add(f, false);
    if (r == 0) {
        auto* none = new QLabel(empty_text);
        none->setObjectName(QStringLiteral("efSub"));
        none->setWordWrap(true);
        grid->addWidget(none, 0, 0, 1, 4);
    }
    grid->setColumnStretch(1, 1);
}

} // namespace

EtfFlowHighlightsWidget::EtfFlowHighlightsWidget(QWidget* parent)
    : BaseWidget(tr("ETF FLOWS · HIGHLIGHTS"), parent, ui::colors::CYAN()) {
    body_ = new QWidget(this);
    body_->setObjectName("etfFlowHighlights");
    auto* vl = new QVBoxLayout(body_);
    vl->setContentsMargins(10, 8, 10, 8);
    vl->setSpacing(4);

    tilt_ = new QLabel(QStringLiteral("--"));
    tilt_->setObjectName(QStringLiteral("efBig"));
    tilt_->setWordWrap(true);
    vl->addWidget(tilt_);

    measured_title_ = new QLabel(tr("MEASURED · SEC N-PORT MONTHLY NET FLOW"));
    measured_title_->setObjectName(QStringLiteral("efHead"));
    vl->addWidget(measured_title_);
    measured_ = new QGridLayout;
    measured_->setHorizontalSpacing(8);
    measured_->setVerticalSpacing(2);
    vl->addLayout(measured_);

    estimated_title_ = new QLabel(tr("ESTIMATED · LATEST DAILY INTERVAL (AUM / NAV)"));
    estimated_title_->setObjectName(QStringLiteral("efHead"));
    vl->addWidget(estimated_title_);
    estimated_ = new QGridLayout;
    estimated_->setHorizontalSpacing(8);
    estimated_->setVerticalSpacing(2);
    vl->addLayout(estimated_);
    vl->addStretch(1);
    content_layout()->addWidget(body_);

    connect(this, &BaseWidget::refresh_requested, this, &EtfFlowHighlightsWidget::load);
    apply_styles();
    set_loading(true);
}

void EtfFlowHighlightsWidget::apply_styles() {
    body_->setStyleSheet(QString("#etfFlowHighlights QLabel{background:transparent;font-size:10px;color:%1;}"
                                 "#etfFlowHighlights QLabel#efBig{font-size:11px;font-weight:bold;}"
                                 "#etfFlowHighlights QLabel#efSub{color:%2;}"
                                 "#etfFlowHighlights QLabel#efSym{font-weight:bold;}"
                                 "#etfFlowHighlights QLabel#efHead{font-size:9px;font-weight:bold;color:%3;"
                                 "padding-top:6px;letter-spacing:0.5px;}"
                                 "#etfFlowHighlights QLabel#efIn{color:%4;font-weight:bold;}"
                                 "#etfFlowHighlights QLabel#efOut{color:%5;font-weight:bold;}")
                             .arg(ui::colors::TEXT_PRIMARY(), ui::colors::TEXT_SECONDARY(), ui::colors::TEXT_TERTIARY(),
                                  ui::colors::POSITIVE(), ui::colors::NEGATIVE()));
}

void EtfFlowHighlightsWidget::on_theme_changed() {
    apply_styles();
}

void EtfFlowHighlightsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("ETF FLOWS · HIGHLIGHTS"));
    measured_title_->setText(tr("MEASURED · SEC N-PORT MONTHLY NET FLOW"));
    estimated_title_->setText(tr("ESTIMATED · LATEST DAILY INTERVAL (AUM / NAV)"));
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
    QString tilt_text = tilt.state.isEmpty() ? tr("Sector tilt unavailable") : tr("Sector tilt: %1").arg(tilt.state);
    if (tilt.breadth_total > 0)
        tilt_text += tr(" · %1 of %2 sectors beat SPY over 1M").arg(tilt.above_bench).arg(tilt.breadth_total);
    tilt_->setText(tilt_text);
    tilt_->setToolTip(tr("Defensive minus cyclical turnover-share change over 20 sessions (PROXY); breadth = U.S. "
                         "sector ETFs whose 1-month total return beat SPY."));

    QVector<EtfFlowRow> in, out;
    etf_rank(s, [](const er::UniverseRow& r) -> const er::ResearchValue& { return r.measured.latest; }, in, out, 3);
    etf_fill(measured_, in, out, QStringLiteral("MMM yy"),
             tr("No SEC N-PORT flow stored yet (needs the SEC User-Agent in Data Sources and a refresh on the ETF Flow "
                "screen)."));
    in.clear();
    out.clear();
    etf_rank(s, [](const er::UniverseRow& r) -> const er::ResearchValue& { return r.est.best; }, in, out, 3);
    etf_fill(estimated_, in, out, QStringLiteral("d MMM"),
             tr("No daily estimate: the provider's AUM did not update between captures for any fund."));

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
    footer += tr(" · %1 funds: %2 measured, %3 estimated").arg(funds).arg(measured).arg(estimated);
    int unhealthy = 0;
    for (const auto& src : s.sources)
        if (src.status == QLatin1String("FAILED") || src.status == QLatin1String("PARTIAL") ||
            src.status == QLatin1String("STALE"))
            ++unhealthy;
    if (unhealthy > 0)
        footer += tr(" · %1 source(s) failed or stale").arg(unhealthy);
    set_footer(footer,
               tr("Same stored snapshot as ETF Flow & Sector Rotation (computed %1 UTC). Refresh the data on that "
                  "screen; this button only re-reads it.")
                   .arg(s.computed_at.toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
               unhealthy > 0 || !s.last_refresh_finished.isValid());
}

} // namespace fincept::screens::widgets
