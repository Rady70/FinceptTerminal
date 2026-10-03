#include "screens/etf_research/EtfResearchScreen.h"

#include "core/logging/Logger.h"
#include "screens/etf_research/EtfResearchBindings.h"
#include "screens/etf_research/EtfResearchFormat.h"
#include "screens/etf_research/EtfResearchTableModel.h"
#include "screens/etf_research/EtfResearchWidgets.h"
#include "ui/theme/ThemeManager.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

using namespace services::etf::research;
using namespace etfr;

namespace {

// ── Cell builders ────────────────────────────────────────────────────────────

Cell etfr_text(const QString& t, const QString& tip = QString(), bool bold = false) {
    Cell c;
    c.text = t;
    c.sort_text = t;
    c.tooltip = tip;
    c.bold = bold;
    c.align = Qt::AlignLeft | Qt::AlignVCenter;
    return c;
}

Cell etfr_value(const ResearchValue& v, int digits = 2, const QString& title = QString()) {
    Cell c;
    c.text = fmt_value(v, digits);
    if (v.value)
        c.sort = *v.value;
    else if (!v.label.isEmpty())
        c.sort_text = v.label;
    const bool signed_units = v.units == QLatin1String("pct") || v.units == QLatin1String("pp") ||
                              v.units == QLatin1String("z") || v.units == QLatin1String("usd") ||
                              v.units == QLatin1String("bp");
    c.fg = signed_units ? value_color(v)
                        : (v.usable() ? token(&ui::ThemeTokens::text_primary) : token(&ui::ThemeTokens::text_tertiary));
    c.tooltip = value_tooltip(v, title);
    if (v.has_flag(flag::kStale) && v.value)
        c.text += QStringLiteral("·s"); // stale marker in text, not only colour
    return c;
}

Cell etfr_evidence(EvidenceClass e, const QString& tip) {
    Cell c;
    c.text = QLatin1String(evidence_tag(e));
    c.sort_text = c.text;
    c.fg = evidence_color(e);
    c.bold = true;
    c.align = Qt::AlignCenter;
    c.tooltip = tip;
    return c;
}

Cell etfr_cred(Credibility k, const QStringList& reasons) {
    Cell c;
    c.text = k == Credibility::NotGraded ? na() : QLatin1String(credibility_tag(k));
    c.sort = static_cast<double>(static_cast<int>(k));
    if (k == Credibility::NotGraded)
        c.sort.reset();
    c.fg = credibility_color(k);
    c.align = Qt::AlignCenter;
    c.tooltip = reasons.join(QStringLiteral("<br/>"));
    return c;
}

/// The Cred cell of a value: a credibility grade for calculated values; the
/// source's own quality for MEASURED values, which are never graded.
Cell etfr_grade(const ResearchValue& v) {
    if (v.usable() && v.evidence == EvidenceClass::Measured) {
        Cell c;
        c.text = v.source_quality.isEmpty() ? na() : QStringLiteral("Q:") + v.source_quality.left(4);
        c.sort_text = c.text;
        c.fg = token(&ui::ThemeTokens::text_secondary);
        c.align = Qt::AlignCenter;
        c.tooltip = tr_("MEASURED values are source observations and are not credibility-graded.<br/>"
                        "Source quality: %1")
                        .arg(v.source_quality.isEmpty() ? na() : v.source_quality);
        return c;
    }
    return etfr_cred(v.usable() ? v.credibility : Credibility::NotGraded, v.credibility_reasons);
}

Cell etfr_quadrant(const RrgResult& r) {
    Cell c;
    if (r.quadrant.label.isEmpty()) {
        c.text = na();
        c.fg = token(&ui::ThemeTokens::text_tertiary);
        c.tooltip = value_tooltip(r.quadrant, tr_("RRG quadrant"));
        c.align = Qt::AlignCenter;
        return c;
    }
    const QString arrow = r.direction == QLatin1String("up")
                              ? QStringLiteral(" ↑")
                              : (r.direction == QLatin1String("down") ? QStringLiteral(" ↓") : QString());
    c.text = quadrant_tag(r.quadrant.label) + arrow;
    c.sort_text = r.quadrant.label;
    c.fg = quadrant_color(r.quadrant.label);
    c.bold = true;
    c.align = Qt::AlignCenter;
    c.tooltip = value_tooltip(
        r.quadrant, tr_("RRG vs %1 (week of %2)").arg(r.benchmark, r.quadrant.effective.toString(Qt::ISODate)));
    return c;
}

QString etfr_structure_tag(const UniverseInstrument& i) {
    QString s;
    if (i.structure == QLatin1String("open_end_etf"))
        s = QStringLiteral("ETF");
    else if (i.structure == QLatin1String("unit_investment_trust"))
        s = QStringLiteral("UIT");
    else if (i.structure == QLatin1String("grantor_trust"))
        s = QStringLiteral("TRUST");
    else if (i.structure == QLatin1String("commodity_pool"))
        s = QStringLiteral("POOL");
    else
        s = QStringLiteral("INDEX");
    if (i.leveraged)
        s += QStringLiteral(" %1x").arg(i.leverage_multiple, 0, 'f', 0);
    if (i.inverse)
        s += QStringLiteral(" INV");
    if (i.option_overlay)
        s += QStringLiteral(" OPT");
    return s;
}

QString etfr_group_text(const UniverseRow& r) {
    const auto& i = r.inst;
    if (i.has_role("us_sector"))
        return tr_("SECTOR ") + i.sector;
    if (i.has_role("theme"))
        return tr_("THEME ") + i.theme;
    if (i.has_role("country"))
        return tr_("CTRY ") + i.country;
    if (i.has_role("cross_asset"))
        return tr_("XA ") + i.asset_class + QLatin1Char(' ') + i.exposure;
    return tr_("BENCH");
}

QStringList etfr_row_tags(const UniverseRow& r) {
    QStringList t = r.inst.roles;
    t << r.inst.symbol << r.inst.sector << r.inst.theme << r.inst.country << r.inst.asset_class << r.inst.exposure
      << r.inst.structure;
    return t;
}

/// The flow cell and evidence cell of a universe row: measured beats
/// estimated; a proxy-only row shows a compact dash, never a zero.
std::pair<Cell, Cell> etfr_flow_cells(const UniverseRow& r) {
    if (r.measured.latest.usable()) {
        Cell c = etfr_value(r.measured.latest, 2, tr_("SEC N-PORT monthly regulatory flow"));
        c.text += QLatin1Char(' ') + r.measured.latest.effective.toString(QStringLiteral("MMMyy"));
        return {c, etfr_evidence(EvidenceClass::Measured,
                                 tr_("MEASURED: SEC N-PORT monthly flow (%1)").arg(r.measured.reporting_key))};
    }
    if (r.est.best.usable()) {
        Cell c = etfr_value(r.est.best, 2,
                            r.est.latest.usable() ? tr_("Estimated creation/redemption (E1 implied shares)")
                                                  : tr_("Estimated creation/redemption (E2 reported shares; E1 "
                                                        "refused: %1)")
                                                        .arg(r.est.latest.reason));
        return {c,
                etfr_evidence(
                    EvidenceClass::Estimated,
                    tr_("ESTIMATED from MarketLab's own Yahoo AUM/NAV captures; agreement: %1").arg(r.est.agreement))};
    }
    Cell c;
    c.text = na();
    c.fg = token(&ui::ThemeTokens::text_tertiary);
    c.align = Qt::AlignCenter;
    c.tooltip = tr_("No measured or estimated flow (%1; %2). Market-behaviour proxies remain in the rotation columns.")
                    .arg(r.measured.latest.reason, r.est.latest.reason);
    const EvidenceClass e = r.ret.m1.usable() ? EvidenceClass::Proxy : EvidenceClass::Unavailable;
    return {c, etfr_evidence(e, e == EvidenceClass::Proxy ? tr_("PROXY only: returns, RRG and turnover describe market "
                                                                "behaviour, not ETF creation/redemption")
                                                          : tr_("UNAVAILABLE: no stored observation"))};
}

Cell etfr_fresh(const UniverseRow& r) {
    Cell c;
    c.align = Qt::AlignCenter;
    if (r.freshness == QLatin1String("no_history")) {
        c.text = tr_("NO DATA");
        c.fg = token(&ui::ThemeTokens::text_tertiary);
    } else if (r.stale) {
        c.text = tr_("STALE");
        c.fg = token(&ui::ThemeTokens::warning);
    } else {
        c.text = tr_("FRESH");
        c.fg = token(&ui::ThemeTokens::positive);
    }
    c.sort_text = c.text;
    c.tooltip = tr_("Last stored session %1").arg(r.last_bar.toString(Qt::ISODate));
    return c;
}

const ResearchValue& etfr_horizon(const ReturnSet& r, const QString& h) {
    if (h == QLatin1String("d1"))
        return r.d1;
    if (h == QLatin1String("w1"))
        return r.w1;
    if (h == QLatin1String("m3"))
        return r.m3;
    return r.m1;
}

double etfr_range(const QVector<HeatTile>& tiles) {
    std::vector<double> a;
    for (const auto& t : tiles)
        if (t.value)
            a.push_back(std::abs(*t.value));
    if (a.empty())
        return 1.0;
    std::sort(a.begin(), a.end());
    return std::max(1e-9, a[static_cast<size_t>(std::min<double>(a.size() - 1, std::floor(0.9 * a.size())))]);
}

HeatTile etfr_return_tile(const UniverseRow& r, const QString& horizon, const QString& group, const QString& sub) {
    HeatTile t;
    t.key = r.inst.symbol;
    t.label = r.inst.symbol;
    t.sub = sub;
    t.group = group;
    const ResearchValue& v = etfr_horizon(r.ret, horizon);
    t.value = v.value;
    t.value_text = fmt_value(v, 1);
    t.tag = quadrant_tag(r.rrg.quadrant.label);
    t.tag_color = quadrant_color(r.rrg.quadrant.label);
    t.stale = r.stale && r.last_bar.isValid();
    t.tooltip = value_tooltip(v, QStringLiteral("%1 %2 total return").arg(r.inst.symbol, horizon.toUpper())) +
                QStringLiteral("<br/>") + value_tooltip(r.rrg.quadrant, tr_("RRG"));
    return t;
}

QVector<RrgSeries> etfr_rrg_series(const QVector<const UniverseRow*>& rows) {
    QVector<RrgSeries> out;
    int k = 0;
    for (const UniverseRow* r : rows) {
        RrgSeries s;
        s.key = r->inst.symbol;
        s.label = r->inst.symbol;
        s.points = r->rrg.trail;
        s.color = QColor::fromHsv((k++ * 47) % 360, 170, 235);
        if (!s.points.isEmpty())
            out.append(s);
    }
    return out;
}

} // namespace

// ── Construction ─────────────────────────────────────────────────────────────

EtfResearchScreen::EtfResearchScreen(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("etfrScreen"));
    build_ui();
    apply_style();
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this, [this]() {
        apply_style();
        populated_.clear();
        if (snap_) {
            populate_summary();
            populate_view(current_view_);
            if (detail_visible())
                populate_detail();
        }
    });
}

EtfResearchScreen::~EtfResearchScreen() = default;

QTableView* EtfResearchScreen::make_table(const QString& id, ResearchTableModel** model, ResearchSortProxy** proxy) {
    auto* view = new QTableView;
    view->setObjectName(QStringLiteral("etfrTable"));
    auto* m = new ResearchTableModel(view);
    auto* p = new ResearchSortProxy(view);
    p->setSourceModel(m);
    view->setModel(p);
    view->setSortingEnabled(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::SingleSelection);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->verticalHeader()->setVisible(false);
    view->verticalHeader()->setDefaultSectionSize(20);
    view->horizontalHeader()->setHighlightSections(false);
    view->horizontalHeader()->setStretchLastSection(false);
    view->setWordWrap(false);
    view->setShowGrid(false);
    view->setAlternatingRowColors(true);
    connect(view->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this, p](const QModelIndex& cur, const QModelIndex&) {
                if (!cur.isValid())
                    return;
                const QString key = p->data(cur, ResearchTableModel::kKeyRole).toString();
                if (!key.isEmpty())
                    on_row_activated(key);
            });
    tables_.insert(id, view);
    models_.insert(id, m);
    proxies_.insert(id, p);
    if (model)
        *model = m;
    if (proxy)
        *proxy = p;
    return view;
}

void EtfResearchScreen::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Command bar (scrolls horizontally on a narrow window rather than clipping).
    header_ = new QWidget;
    header_->setObjectName(QStringLiteral("etfrHeader"));
    auto* hl = new QHBoxLayout(header_);
    hl->setContentsMargins(6, 3, 6, 3);
    hl->setSpacing(4);
    title_ = new QLabel(tr("ETF FLOW & ROTATION"));
    title_->setObjectName(QStringLiteral("etfrTitle"));
    hl->addWidget(title_);
    view_group_ = new QButtonGroup(this);
    view_group_->setExclusive(true);
    const QList<QPair<QString, QString>> views = {
        {QStringLiteral("universe"), tr("UNIVERSE")}, {QStringLiteral("sectors"), tr("SECTORS")},
        {QStringLiteral("themes"), tr("THEMES")},     {QStringLiteral("countries"), tr("COUNTRIES")},
        {QStringLiteral("flow"), tr("FLOW")},         {QStringLiteral("rrg"), tr("RRG")},
        {QStringLiteral("regime"), tr("REGIME")},     {QStringLiteral("models"), tr("MODELS")},
        {QStringLiteral("intl"), tr("INTL")},         {QStringLiteral("sources"), tr("SOURCES")}};
    for (const auto& v : views) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("etfrViewButton"));
        b->setText(v.second);
        b->setCheckable(true);
        b->setAutoRaise(true);
        view_group_->addButton(b);
        view_buttons_.insert(v.first, b);
        hl->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, id = v.first]() { show_view(id); });
    }
    hl->addSpacing(8);
    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("etfrSearch"));
    search_->setPlaceholderText(tr("Filter ticker / name / sector / theme / country"));
    search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(150);
    hl->addWidget(search_, 1);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) {
        for (auto* p : proxies_)
            p->set_search(t);
    });
    horizon_box_ = new QComboBox;
    horizon_box_->setObjectName(QStringLiteral("etfrHorizon"));
    horizon_box_->addItem(tr("1D"), QStringLiteral("d1"));
    horizon_box_->addItem(tr("1W"), QStringLiteral("w1"));
    horizon_box_->addItem(tr("1M"), QStringLiteral("m1"));
    horizon_box_->addItem(tr("3M"), QStringLiteral("m3"));
    horizon_box_->setCurrentIndex(2);
    horizon_box_->setToolTip(tr("Heatmap horizon (total return, distributions reinvested)"));
    hl->addWidget(horizon_box_);
    connect(horizon_box_, &QComboBox::currentIndexChanged, this, [this]() {
        horizon_ = horizon_box_->currentData().toString();
        for (const QString& v :
             {QStringLiteral("sectors"), QStringLiteral("themes"), QStringLiteral("countries"), QStringLiteral("intl")})
            populated_.remove(v);
        populate_view(current_view_);
    });
    refresh_ = new QPushButton(tr("Refresh ETF Research Data"));
    refresh_->setObjectName(QStringLiteral("etfrRefresh"));
    refresh_->setToolTip(tr("Manual acquisition only: Yahoo (prices, AUM/NAV, holdings), FRED, World Bank, and the "
                            "read-only IBKR / SEC N-PORT routes when configured. Nothing is fetched automatically."));
    hl->addWidget(refresh_);
    connect(refresh_, &QPushButton::clicked, this, &EtfResearchScreen::on_refresh);
    auto* header_scroll = new QScrollArea;
    header_scroll->setObjectName(QStringLiteral("etfrHeaderScroll"));
    header_scroll->setWidget(header_);
    header_scroll->setWidgetResizable(true);
    header_scroll->setFrameShape(QFrame::NoFrame);
    header_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    header_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    header_scroll->setFixedHeight(header_->sizeHint().height() + 12);
    root->addWidget(header_scroll);

    // Summary strip.
    summary_ = new QWidget;
    summary_->setObjectName(QStringLiteral("etfrSummary"));
    auto* sl = new QHBoxLayout(summary_);
    sl->setContentsMargins(6, 2, 6, 2);
    sl->setSpacing(10);
    const QList<QPair<QString, QString>> tiles = {
        {QStringLiteral("universe"), tr("UNIVERSE")}, {QStringLiteral("flow"), tr("FLOW EVIDENCE")},
        {QStringLiteral("tilt"), tr("SECTOR TILT")},  {QStringLiteral("quad"), tr("RRG QUADRANTS")},
        {QStringLiteral("breadth"), tr("BREADTH")},   {QStringLiteral("cycle"), tr("CYCLE")},
        {QStringLiteral("regime"), tr("REGIME")},     {QStringLiteral("vix"), tr("VIX")},
        {QStringLiteral("sources"), tr("SOURCES")}};
    for (const auto& t : tiles) {
        auto* box = new QWidget;
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(0);
        auto* cap = new QLabel(t.second);
        cap->setObjectName(QStringLiteral("etfrTileCaption"));
        auto* val = new QLabel(na());
        val->setObjectName(QStringLiteral("etfrTileValue"));
        val->setTextFormat(Qt::RichText);
        bl->addWidget(cap);
        bl->addWidget(val);
        sl->addWidget(box);
        tiles_.insert(t.first, val);
    }
    sl->addStretch(1);
    status_ = new QLabel(tr("Loading stored research data…"));
    status_->setObjectName(QStringLiteral("etfrStatus"));
    status_->setTextFormat(Qt::RichText);
    sl->addWidget(status_);
    auto* summary_scroll = new QScrollArea;
    summary_scroll->setObjectName(QStringLiteral("etfrSummaryScroll"));
    summary_scroll->setWidget(summary_);
    summary_scroll->setWidgetResizable(true);
    summary_scroll->setFrameShape(QFrame::NoFrame);
    summary_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    summary_scroll->setFixedHeight(summary_->sizeHint().height() + 12);
    root->addWidget(summary_scroll);

    // Body: views + detail on selection.
    body_ = new QSplitter(Qt::Horizontal);
    body_->setObjectName(QStringLiteral("etfrBody"));
    body_->setChildrenCollapsible(false);
    stack_ = new QStackedWidget;
    const QList<QPair<QString, QWidget*>> built = {
        {QStringLiteral("universe"), build_universe_view()}, {QStringLiteral("sectors"), build_sectors_view()},
        {QStringLiteral("themes"), build_themes_view()},     {QStringLiteral("countries"), build_countries_view()},
        {QStringLiteral("flow"), build_flow_view()},         {QStringLiteral("rrg"), build_rrg_view()},
        {QStringLiteral("regime"), build_regime_view()},     {QStringLiteral("models"), build_models_view()},
        {QStringLiteral("intl"), build_intl_view()},         {QStringLiteral("sources"), build_sources_view()}};
    for (const auto& v : built) {
        views_.insert(v.first, v.second);
        stack_->addWidget(v.second);
    }
    body_->addWidget(stack_);
    body_->addWidget(build_detail());
    body_->setStretchFactor(0, 3);
    body_->setStretchFactor(1, 2);
    detail_->setVisible(false);
    root->addWidget(body_, 1);
    view_buttons_.value(current_view_)->setChecked(true);
}

QWidget* EtfResearchScreen::build_universe_view() {
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto* chips = new QWidget;
    chips->setObjectName(QStringLiteral("etfrChips"));
    chips->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* cl = new QHBoxLayout(chips);
    cl->setContentsMargins(6, 2, 6, 2);
    cl->setSpacing(2);
    role_group_ = new QButtonGroup(this);
    const QList<QPair<QString, QString>> roles = {{QString(), tr("ALL")},
                                                  {QStringLiteral("cross_asset"), tr("CROSS-ASSET")},
                                                  {QStringLiteral("us_sector"), tr("US SECTORS")},
                                                  {QStringLiteral("theme"), tr("THEMES")},
                                                  {QStringLiteral("country"), tr("COUNTRY / REGION")},
                                                  {QStringLiteral("benchmark"), tr("BENCHMARKS")}};
    for (const auto& r : roles) {
        auto* b = new QToolButton;
        b->setObjectName(QStringLiteral("etfrChip"));
        b->setText(r.second);
        b->setCheckable(true);
        b->setAutoRaise(true);
        role_group_->addButton(b);
        cl->addWidget(b);
        if (r.first.isEmpty())
            b->setChecked(true);
        connect(b, &QToolButton::clicked, this, [this, tag = r.first]() {
            if (auto* p = proxies_.value(QStringLiteral("universe")))
                p->set_required_tag(tag);
        });
    }
    auto* legend =
        new QLabel(tr("Flow evidence: MEAS = SEC N-PORT measured · EST = estimated from MarketLab captures · "
                      "PRXY = market behaviour only · N/A = unavailable.  Returns are total returns."));
    legend->setObjectName(QStringLiteral("etfrHint"));
    legend->setToolTip(legend->text());
    legend->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    cl->addSpacing(12);
    cl->addWidget(legend, 1);
    auto* chips_scroll = new QScrollArea;
    chips_scroll->setObjectName(QStringLiteral("etfrHeaderScroll"));
    chips_scroll->setWidget(chips);
    chips_scroll->setWidgetResizable(true);
    chips_scroll->setFrameShape(QFrame::NoFrame);
    chips_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chips_scroll->setFixedHeight(chips->sizeHint().height() + 10);
    l->addWidget(chips_scroll);
    l->addWidget(make_table(QStringLiteral("universe"), nullptr, nullptr), 1);
    return w;
}

QWidget* EtfResearchScreen::build_sectors_view() {
    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(make_table(QStringLiteral("sectors"), nullptr, nullptr));
    auto* bottom = new QSplitter(Qt::Horizontal);
    sector_rrg_ = new RrgWidget;
    connect(sector_rrg_, &RrgWidget::series_activated, this, &EtfResearchScreen::on_row_activated);
    bottom->addWidget(sector_rrg_);
    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(2);
    tilt_label_ = new QLabel;
    tilt_label_->setObjectName(QStringLiteral("etfrPanelText"));
    tilt_label_->setTextFormat(Qt::RichText);
    tilt_label_->setWordWrap(true);
    tilt_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rl->addWidget(tilt_label_);
    sector_heat_ = new HeatmapWidget;
    connect(sector_heat_, &HeatmapWidget::tile_activated, this, &EtfResearchScreen::on_row_activated);
    rl->addWidget(sector_heat_, 1);
    bottom->addWidget(right);
    split->addWidget(bottom);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    return split;
}

QWidget* EtfResearchScreen::build_themes_view() {
    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(make_table(QStringLiteral("themes"), nullptr, nullptr));
    auto* bottom = new QSplitter(Qt::Horizontal);
    theme_heat_ = new HeatmapWidget;
    connect(theme_heat_, &HeatmapWidget::tile_activated, this, &EtfResearchScreen::on_row_activated);
    auto* hs = new QScrollArea;
    hs->setWidget(theme_heat_);
    hs->setWidgetResizable(true);
    hs->setFrameShape(QFrame::NoFrame);
    bottom->addWidget(hs);
    theme_rrg_ = new RrgWidget;
    connect(theme_rrg_, &RrgWidget::series_activated, this, &EtfResearchScreen::on_row_activated);
    bottom->addWidget(theme_rrg_);
    split->addWidget(bottom);
    return split;
}

QWidget* EtfResearchScreen::build_countries_view() {
    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(make_table(QStringLiteral("countries"), nullptr, nullptr));
    auto* bottom = new QSplitter(Qt::Horizontal);
    country_heat_ = new HeatmapWidget;
    connect(country_heat_, &HeatmapWidget::tile_activated, this, &EtfResearchScreen::on_row_activated);
    bottom->addWidget(country_heat_);
    country_rrg_ = new RrgWidget;
    connect(country_rrg_, &RrgWidget::series_activated, this, &EtfResearchScreen::on_row_activated);
    bottom->addWidget(country_rrg_);
    bottom->addWidget(make_table(QStringLiteral("baskets"), nullptr, nullptr));
    split->addWidget(bottom);
    return split;
}

QWidget* EtfResearchScreen::build_flow_view() {
    auto* split = new QSplitter(Qt::Vertical);
    auto* top = new QWidget;
    auto* tl = new QVBoxLayout(top);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("etfrChips"));
    bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(6, 2, 6, 2);
    flow_mode_ = new QComboBox;
    flow_mode_->setObjectName(QStringLiteral("etfrFlowMode"));
    flow_mode_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    flow_mode_->setMinimumContentsLength(24);
    flow_mode_->addItem(tr("Estimated flow, 20 sessions, % of AUM (ESTIMATED)"), QStringLiteral("est_pct20"));
    flow_mode_->addItem(tr("Estimated flow, latest capture interval, $ (ESTIMATED)"), QStringLiteral("est_latest"));
    flow_mode_->addItem(tr("Measured SEC N-PORT, latest month, $ (MEASURED)"), QStringLiteral("measured"));
    flow_mode_->addItem(tr("1M total return (PROXY, not flow)"), QStringLiteral("proxy_m1"));
    bl->addWidget(flow_mode_);
    flow_note_ = new QLabel;
    flow_note_->setObjectName(QStringLiteral("etfrHint"));
    flow_note_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    bl->addWidget(flow_note_, 1);
    tl->addWidget(bar);
    flow_heat_ = new HeatmapWidget;
    connect(flow_heat_, &HeatmapWidget::tile_activated, this, &EtfResearchScreen::on_row_activated);
    auto* hs = new QScrollArea;
    hs->setWidget(flow_heat_);
    hs->setWidgetResizable(true);
    hs->setFrameShape(QFrame::NoFrame);
    tl->addWidget(hs, 1);
    connect(flow_mode_, &QComboBox::currentIndexChanged, this, [this]() {
        populated_.remove(QStringLiteral("flow"));
        populate_view(QStringLiteral("flow"));
    });
    split->addWidget(top);
    auto* bottom = new QSplitter(Qt::Horizontal);
    bottom->addWidget(make_table(QStringLiteral("flow"), nullptr, nullptr));
    bottom->addWidget(make_table(QStringLiteral("groups"), nullptr, nullptr));
    bottom->setStretchFactor(0, 3);
    bottom->setStretchFactor(1, 2);
    split->addWidget(bottom);
    return split;
}

QWidget* EtfResearchScreen::build_rrg_view() {
    auto* split = new QSplitter(Qt::Horizontal);
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("etfrChips"));
    bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(6, 2, 6, 2);
    rrg_set_ = new QComboBox;
    rrg_set_->setObjectName(QStringLiteral("etfrRrgSet"));
    rrg_set_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    rrg_set_->setMinimumContentsLength(30);
    rrg_set_->addItem(tr("U.S. sectors vs SPY"), QStringLiteral("us_sector"));
    rrg_set_->addItem(tr("Themes vs SPY"), QStringLiteral("theme"));
    rrg_set_->addItem(tr("Countries / regions vs ACWI"), QStringLiteral("country"));
    rrg_set_->addItem(tr("Thai equal-weight baskets vs SET"), QStringLiteral("baskets"));
    rrg_tail_ = new QComboBox;
    rrg_tail_->setObjectName(QStringLiteral("etfrRrgTail"));
    for (int t : {4, 6, 8})
        rrg_tail_->addItem(tr("%1-week trail").arg(t), t);
    rrg_tail_->setCurrentIndex(2);
    bl->addWidget(rrg_set_);
    bl->addWidget(rrg_tail_);
    auto* hint = new QLabel(tr("RS-Ratio = 100·RS/SMA8(RS), RS-Momentum = 100·Ratio/SMA4(Ratio), weekly, completed "
                               "weeks only. JdK-style approximation (MODEL), not the licensed RRG."));
    hint->setObjectName(QStringLiteral("etfrHint"));
    hint->setToolTip(hint->text());
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    bl->addWidget(hint, 1);
    ll->addWidget(bar);
    big_rrg_ = new RrgWidget;
    connect(big_rrg_, &RrgWidget::series_activated, this, &EtfResearchScreen::on_row_activated);
    ll->addWidget(big_rrg_, 1);
    split->addWidget(left);
    split->addWidget(make_table(QStringLiteral("rrg"), nullptr, nullptr));
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    for (QComboBox* c : {rrg_set_, rrg_tail_})
        connect(c, &QComboBox::currentIndexChanged, this, [this]() {
            populated_.remove(QStringLiteral("rrg"));
            populate_view(QStringLiteral("rrg"));
        });
    return split;
}

QWidget* EtfResearchScreen::build_regime_view() {
    auto* split = new QSplitter(Qt::Horizontal);
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("etfrChips"));
    bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(6, 2, 6, 2);
    regime_mode_ = new QComboBox;
    regime_mode_->setObjectName(QStringLiteral("etfrRegimeMode"));
    regime_mode_->addItem(tr("CORR  Pearson ρ"), QStringLiteral("corr"));
    regime_mode_->addItem(tr("GEOM  wedge |sin θ|"), QStringLiteral("geom"));
    regime_period_ = new QComboBox;
    regime_period_->setObjectName(QStringLiteral("etfrRegimePeriod"));
    for (const QString& p :
         {QStringLiteral("1m"), QStringLiteral("3m"), QStringLiteral("6m"), QStringLiteral("1y"), QStringLiteral("2y")})
        regime_period_->addItem(p.toUpper(), p);
    regime_period_->setCurrentIndex(1);
    bl->addWidget(regime_mode_);
    bl->addWidget(regime_period_);
    bl->addStretch(1);
    ll->addWidget(bar);
    matrix_ = new MatrixWidget;
    ll->addWidget(matrix_, 1);
    split->addWidget(left);
    auto* right = new QSplitter(Qt::Vertical);
    pca_ = new PcaWidget;
    right->addWidget(pca_);
    auto* stats_scroll = new QScrollArea;
    regime_stats_ = new QLabel;
    regime_stats_->setObjectName(QStringLiteral("etfrPanelText"));
    regime_stats_->setTextFormat(Qt::RichText);
    regime_stats_->setWordWrap(true);
    regime_stats_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    stats_scroll->setWidget(regime_stats_);
    stats_scroll->setWidgetResizable(true);
    stats_scroll->setFrameShape(QFrame::NoFrame);
    right->addWidget(stats_scroll);
    regime_probs_ = new BarChartWidget;
    right->addWidget(regime_probs_);
    right->setStretchFactor(0, 5);
    right->setStretchFactor(1, 4);
    right->setStretchFactor(2, 3);
    stats_scroll->setMinimumHeight(140);
    split->addWidget(right);
    for (QComboBox* c : {regime_mode_, regime_period_})
        connect(c, &QComboBox::currentIndexChanged, this, [this]() {
            populated_.remove(QStringLiteral("regime"));
            populate_view(QStringLiteral("regime"));
        });
    return split;
}

QWidget* EtfResearchScreen::build_models_view() {
    auto* split = new QSplitter(Qt::Vertical);
    auto* top = new QSplitter(Qt::Horizontal);
    auto* cyc = new QWidget;
    auto* cl = new QVBoxLayout(cyc);
    cl->setContentsMargins(4, 2, 4, 2);
    cycle_label_ = new QLabel;
    cycle_label_->setObjectName(QStringLiteral("etfrPanelText"));
    cycle_label_->setTextFormat(Qt::RichText);
    cycle_label_->setWordWrap(true);
    cycle_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    cl->addWidget(cycle_label_);
    cycle_bars_ = new BarChartWidget;
    cl->addWidget(cycle_bars_, 1);
    top->addWidget(cyc);
    top->addWidget(make_table(QStringLiteral("factors"), nullptr, nullptr));
    split->addWidget(top);
    split->addWidget(make_table(QStringLiteral("sector_models"), nullptr, nullptr));
    split->addWidget(make_table(QStringLiteral("country_models"), nullptr, nullptr));
    return split;
}

QWidget* EtfResearchScreen::build_intl_view() {
    auto* split = new QSplitter(Qt::Vertical);
    auto* top = new QWidget;
    auto* tl = new QVBoxLayout(top);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    auto* bar = new QWidget;
    bar->setObjectName(QStringLiteral("etfrChips"));
    bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto* bl = new QHBoxLayout(bar);
    bl->setContentsMargins(6, 2, 6, 2);
    intl_market_ = new QComboBox;
    intl_market_->setObjectName(QStringLiteral("etfrIntlMarket"));
    bl->addWidget(intl_market_);
    auto* hint = new QLabel(tr("Equal-weight sector baskets of representative stocks (not ETFs; no flow meaning). "
                               "Local-currency total returns."));
    hint->setObjectName(QStringLiteral("etfrHint"));
    hint->setToolTip(hint->text());
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    bl->addWidget(hint, 1);
    tl->addWidget(bar);
    intl_heat_ = new HeatmapWidget;
    auto* hs = new QScrollArea;
    hs->setWidget(intl_heat_);
    hs->setWidgetResizable(true);
    hs->setFrameShape(QFrame::NoFrame);
    tl->addWidget(hs, 1);
    connect(intl_market_, &QComboBox::currentIndexChanged, this, [this]() {
        populated_.remove(QStringLiteral("intl"));
        populate_view(QStringLiteral("intl"));
    });
    split->addWidget(top);
    split->addWidget(make_table(QStringLiteral("intl"), nullptr, nullptr));
    return split;
}

QWidget* EtfResearchScreen::build_sources_view() {
    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(make_table(QStringLiteral("sources"), nullptr, nullptr));
    auto* sc = new QScrollArea;
    sources_note_ = new QLabel;
    sources_note_->setObjectName(QStringLiteral("etfrPanelText"));
    sources_note_->setTextFormat(Qt::RichText);
    sources_note_->setWordWrap(true);
    sources_note_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    sources_note_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sc->setWidget(sources_note_);
    sc->setWidgetResizable(true);
    sc->setFrameShape(QFrame::NoFrame);
    sc->setMinimumHeight(160);
    split->addWidget(sc);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    return split;
}

QWidget* EtfResearchScreen::build_detail() {
    detail_ = new QFrame;
    detail_->setObjectName(QStringLiteral("etfrDetail"));
    auto* l = new QVBoxLayout(detail_);
    l->setContentsMargins(4, 2, 4, 2);
    l->setSpacing(2);
    auto* head = new QHBoxLayout;
    detail_title_ = new QLabel;
    detail_title_->setObjectName(QStringLiteral("etfrDetailTitle"));
    detail_title_->setTextFormat(Qt::RichText);
    detail_title_->setWordWrap(true);
    head->addWidget(detail_title_, 1);
    detail_close_ = new QToolButton;
    detail_close_->setObjectName(QStringLiteral("etfrDetailClose"));
    detail_close_->setText(tr("✕"));
    detail_close_->setToolTip(tr("Close detail"));
    detail_close_->setAutoRaise(true);
    connect(detail_close_, &QToolButton::clicked, this, &EtfResearchScreen::close_detail);
    head->addWidget(detail_close_);
    l->addLayout(head);
    detail_tabs_ = new QTabWidget;
    detail_tabs_->setObjectName(QStringLiteral("etfrDetailTabs"));
    auto scroll_label = [](QLabel** out) {
        auto* sc = new QScrollArea;
        auto* lab = new QLabel;
        lab->setObjectName(QStringLiteral("etfrPanelText"));
        lab->setTextFormat(Qt::RichText);
        lab->setWordWrap(true);
        lab->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        lab->setTextInteractionFlags(Qt::TextSelectableByMouse);
        sc->setWidget(lab);
        sc->setWidgetResizable(true);
        sc->setFrameShape(QFrame::NoFrame);
        *out = lab;
        return sc;
    };
    detail_tabs_->addTab(scroll_label(&detail_overview_), tr("OVERVIEW"));
    auto* chart = new QSplitter(Qt::Vertical);
    detail_chart_ = new LineChartWidget;
    detail_rrg_ = new RrgWidget;
    chart->addWidget(detail_chart_);
    chart->addWidget(detail_rrg_);
    detail_tabs_->addTab(chart, tr("CHART"));
    auto* flow = new QSplitter(Qt::Vertical);
    flow->addWidget(scroll_label(&detail_flow_text_));
    detail_measured_ = new BarChartWidget;
    flow->addWidget(detail_measured_);
    flow->setStretchFactor(0, 3);
    flow->setStretchFactor(1, 2);
    detail_tabs_->addTab(flow, tr("FLOW"));
    auto* hold = new QSplitter(Qt::Vertical);
    hold->addWidget(scroll_label(&detail_holdings_));
    detail_weights_ = new BarChartWidget;
    hold->addWidget(detail_weights_);
    detail_tabs_->addTab(hold, tr("HOLDINGS"));
    detail_tabs_->addTab(scroll_label(&detail_provenance_), tr("PROVENANCE"));
    l->addWidget(detail_tabs_, 1);
    return detail_;
}

void EtfResearchScreen::apply_style() {
    const auto& t = ui::ThemeManager::instance().tokens();
    // One stylesheet for the whole workspace, selected by objectName.
    setStyleSheet(
        QStringLiteral(
            "#etfrScreen { background:%1; }"
            "#etfrHeader, #etfrSummary, #etfrChips { background:%2; }"
            "#etfrHeaderScroll, #etfrSummaryScroll { background:%2; border:none; border-bottom:1px solid %3; }"
            "#etfrTitle { color:%4; font-weight:700; padding-right:8px; }"
            "#etfrViewButton, #etfrChip { color:%5; padding:2px 6px; border:1px solid transparent; "
            "font-weight:600; }"
            "#etfrViewButton:checked, #etfrChip:checked { color:%4; border:1px solid %4; background:%6; }"
            "#etfrViewButton:hover, #etfrChip:hover { color:%7; }"
            "#etfrTileCaption { color:%5; font-size:9px; font-weight:600; }"
            "#etfrTileValue { color:%7; font-weight:600; }"
            "#etfrStatus, #etfrHint { color:%5; }"
            "#etfrPanelText { color:%7; padding:4px; }"
            "#etfrDetail { background:%2; border-left:1px solid %3; }"
            "#etfrDetailTitle { color:%7; }"
            "#etfrRefresh { color:%8; background:%4; font-weight:700; padding:3px 10px; border:none; }"
            "#etfrRefresh:disabled { background:%3; color:%5; }"
            "#etfrTable { background:%1; alternate-background-color:%9; color:%7; border:none; "
            "selection-background-color:%6; selection-color:%7; }"
            "#etfrTable QHeaderView::section { background:%2; color:%5; border:none; "
            "border-bottom:1px solid %3; padding:2px 4px; font-weight:600; }")
            .arg(QLatin1String(t.bg_base), QLatin1String(t.bg_surface), QLatin1String(t.border_dim),
                 QLatin1String(t.accent), QLatin1String(t.text_secondary), QLatin1String(t.accent_bg),
                 QLatin1String(t.text_primary), QLatin1String(t.text_on_accent), QLatin1String(t.row_alt)));
}

// ── Loading ──────────────────────────────────────────────────────────────────

void EtfResearchScreen::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (autoload_ && !loaded_once_) {
        loaded_once_ = true;
        start_load();
    }
}

void EtfResearchScreen::start_load() {
    ++load_requests_;
    if (watcher_)
        return;
    status_->setText(tr("Reading stored research data…"));
    auto result = std::make_shared<std::shared_ptr<ResearchSnapshot>>();
    auto error = std::make_shared<QString>();
    watcher_ = new QFutureWatcher<void>(this);
    connect(watcher_, &QFutureWatcher<void>::finished, this, [this, result, error]() {
        watcher_->deleteLater();
        watcher_ = nullptr;
        if (*result) {
            set_snapshot(**result);
        } else {
            status_->setText(
                tr("<span style='color:%1'>Could not read stored research data: %2</span>")
                    .arg(QLatin1String(ui::ThemeManager::instance().tokens().negative), error->toHtmlEscaped()));
        }
    });
    // Reads only: the stored observations, Batch C/D results and IBKR closes.
    watcher_->setFuture(QtConcurrent::run([result, error]() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        auto snap = etfr::load_research_snapshot(now, now);
        if (snap.is_err()) {
            *error = QString::fromStdString(snap.error());
            return;
        }
        *result = std::make_shared<ResearchSnapshot>(snap.value());
    }));
}

void EtfResearchScreen::on_refresh() {
    if (etfr::manual_refresh_running())
        return;
    refresh_->setEnabled(false);
    status_->setText(tr("Refreshing ETF research data…"));
    etfr::start_manual_refresh(
        [this](const QString& stage, int d, int t) {
            status_->setText(tr("Refreshing: %1 %2/%3").arg(stage).arg(d).arg(t));
        },
        [this](const RefreshResult& r) {
            refresh_->setEnabled(true);
            int failed = 0;
            for (const auto& s : r.stages)
                if (s.status == QLatin1String("FAILED") || s.status == QLatin1String("PARTIAL"))
                    ++failed;
            if (!r.ok && !r.error.isEmpty())
                status_->setText(
                    tr("<span style='color:%1'>Refresh failed: %2</span>")
                        .arg(QLatin1String(ui::ThemeManager::instance().tokens().negative), r.error.toHtmlEscaped()));
            else
                status_->setText(tr("Refresh %1 finished: %2 source stage(s) with failures. Recomputing…")
                                     .arg(r.run_id)
                                     .arg(failed));
            loaded_once_ = true;
            start_load();
        });
}

void EtfResearchScreen::set_snapshot(const ResearchSnapshot& s) {
    snap_ = std::make_shared<ResearchSnapshot>(s);
    populated_.clear();
    // Market list for the international view.
    {
        QSignalBlocker block(intl_market_);
        const QString cur = intl_market_->currentData().toString();
        intl_market_->clear();
        QStringList markets;
        for (const auto& is : snap_->intl)
            if (!markets.contains(is.market))
                markets.append(is.market);
        for (const QString& m : markets)
            intl_market_->addItem(m.toUpper(), m);
        const int idx = intl_market_->findData(cur);
        intl_market_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    populate_summary();
    populate_view(current_view_);
    if (!selected_.isEmpty() && snap_->row(selected_))
        populate_detail();
    emit snapshot_applied();
}

// ── Summary ──────────────────────────────────────────────────────────────────

void EtfResearchScreen::populate_summary() {
    if (!snap_)
        return;
    const auto& t = ui::ThemeManager::instance().tokens();
    auto span = [](const QString& text, const char* col) {
        return QStringLiteral("<span style='color:%1'>%2</span>").arg(QLatin1String(col), text.toHtmlEscaped());
    };
    const auto& c = snap_->counts;
    tiles_[QStringLiteral("universe")]->setText(
        tr("%1 rows · %2")
            .arg(c.value(QStringLiteral("rows")))
            .arg(c.value(QStringLiteral("stale")) > 0
                     ? span(tr("%1 stale").arg(c.value(QStringLiteral("stale"))), t.warning)
                     : span(tr("fresh"), t.positive)));
    tiles_[QStringLiteral("flow")]->setText(QStringLiteral("%1 %2 · %3 %4 · %5 %6 · %7 %8")
                                                .arg(span(QStringLiteral("MEAS"), t.info))
                                                .arg(c.value(QStringLiteral("flow_measured")))
                                                .arg(span(QStringLiteral("EST"), t.warning))
                                                .arg(c.value(QStringLiteral("flow_estimated")))
                                                .arg(span(QStringLiteral("PRXY"), t.cyan))
                                                .arg(c.value(QStringLiteral("proxy_only")))
                                                .arg(span(QStringLiteral("N/A"), t.text_tertiary))
                                                .arg(c.value(QStringLiteral("unavailable"))));
    const auto& tilt = snap_->tilt;
    tiles_[QStringLiteral("tilt")]->setText(tilt.tilt_bp.value
                                                ? QStringLiteral("%1 %2 (z %3)")
                                                      .arg(tilt.state, fmt_value(tilt.tilt_bp),
                                                           tilt.tilt_z.value ? fmt_signed(*tilt.tilt_z.value, 1) : na())
                                                : span(tr("unavailable"), t.text_tertiary));
    tiles_[QStringLiteral("tilt")]->setToolTip(value_tooltip(tilt.tilt_bp, tr("Defensive − cyclical turnover-share "
                                                                              "change, 20-session windows (PROXY)")));
    tiles_[QStringLiteral("quad")]->setText(QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8")
                                                .arg(span(QStringLiteral("L"), t.positive))
                                                .arg(tilt.quadrants.value(QStringLiteral("Leading")))
                                                .arg(span(QStringLiteral("I"), t.info))
                                                .arg(tilt.quadrants.value(QStringLiteral("Improving")))
                                                .arg(span(QStringLiteral("W"), t.warning))
                                                .arg(tilt.quadrants.value(QStringLiteral("Weakening")))
                                                .arg(span(QStringLiteral("Lg"), t.negative))
                                                .arg(tilt.quadrants.value(QStringLiteral("Lagging"))));
    tiles_[QStringLiteral("breadth")]->setText(
        tilt.breadth_total ? tr("%1/%2 sectors > SPY 1M").arg(tilt.above_bench).arg(tilt.breadth_total)
                           : span(tr("unavailable"), t.text_tertiary));
    const auto& cyc = snap_->sectors.cycle;
    tiles_[QStringLiteral("cycle")]->setText(cyc.phase.label.isEmpty()
                                                 ? span(tr("unavailable"), t.text_tertiary)
                                                 : QStringLiteral("%1 p=%2%  <span style='color:%3'>%4</span>")
                                                       .arg(cyc.phase.label)
                                                       .arg(*cyc.phase.value * 100, 0, 'f', 0)
                                                       .arg(QLatin1String(t.warning), tr("[EXPERIMENTAL]")));
    tiles_[QStringLiteral("cycle")]->setToolTip(value_tooltip(cyc.phase, tr("Business-cycle clock (MODEL)")));
    QString regime;
    for (const auto& v : snap_->regime.views)
        if (v.period == QLatin1String("3m"))
            regime = v.corr_label.label.isEmpty() ? na() : v.corr_label.label;
    if (!snap_->regime.v2.label.label.isEmpty())
        regime += QStringLiteral(" · HMM ") + snap_->regime.v2.label.label;
    tiles_[QStringLiteral("regime")]->setText(regime.isEmpty() ? na() : regime);
    tiles_[QStringLiteral("vix")]->setText(cyc.vix.value
                                               ? QString::number(*cyc.vix.value, 'f', 1) + QStringLiteral(" ") +
                                                     cyc.vix.effective.toString(QStringLiteral("MM-dd"))
                                               : span(tr("n/a"), t.text_tertiary));
    int bad = 0, total = 0;
    for (const auto& s : snap_->sources) {
        ++total;
        if (s.status == QLatin1String("FAILED") || s.status == QLatin1String("PARTIAL") ||
            s.status == QLatin1String("STALE"))
            ++bad;
    }
    tiles_[QStringLiteral("sources")]->setText(
        total == 0 ? span(tr("no refresh yet"), t.warning)
                   : (bad ? span(tr("%1/%2 need attention").arg(bad).arg(total), t.warning)
                          : span(tr("%1 ok").arg(total), t.positive)));
    status_->setText(
        snap_->last_refresh_finished.isValid()
            ? tr("Refreshed %1 UTC").arg(snap_->last_refresh_finished.toString(QStringLiteral("MM-dd HH:mm")))
            : tr("No refresh yet"));
    status_->setToolTip(snap_->last_refresh_finished.isValid()
                            ? tr("Stored data only (no acquisition on open). Computed %1 UTC; last manual refresh "
                                 "finished %2 UTC.")
                                  .arg(snap_->computed_at.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                                       snap_->last_refresh_finished.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                            : tr("Stored data only; no manual refresh has been recorded in this profile."));
}

// ── Views ────────────────────────────────────────────────────────────────────

QStringList EtfResearchScreen::view_ids() const {
    return {QStringLiteral("universe"),  QStringLiteral("sectors"), QStringLiteral("themes"),
            QStringLiteral("countries"), QStringLiteral("flow"),    QStringLiteral("rrg"),
            QStringLiteral("regime"),    QStringLiteral("models"),  QStringLiteral("intl"),
            QStringLiteral("sources")};
}

void EtfResearchScreen::show_view(const QString& id) {
    if (!views_.contains(id))
        return;
    current_view_ = id;
    stack_->setCurrentWidget(views_.value(id));
    if (auto* b = view_buttons_.value(id))
        b->setChecked(true);
    populate_view(id);
}

QString EtfResearchScreen::current_view() const {
    return current_view_;
}

QWidget* EtfResearchScreen::view_widget(const QString& id) const {
    return views_.value(id);
}

QTableView* EtfResearchScreen::table(const QString& id) const {
    return tables_.value(id);
}

QString EtfResearchScreen::status_text() const {
    return status_->text();
}

void EtfResearchScreen::populate_view(const QString& id) {
    if (!snap_ || populated_.contains(id))
        return;
    populated_.insert(id);
    if (id == QLatin1String("universe"))
        populate_universe();
    else if (id == QLatin1String("sectors"))
        populate_sectors();
    else if (id == QLatin1String("themes"))
        populate_themes();
    else if (id == QLatin1String("countries"))
        populate_countries();
    else if (id == QLatin1String("flow"))
        populate_flow();
    else if (id == QLatin1String("rrg"))
        populate_rrg();
    else if (id == QLatin1String("regime"))
        populate_regime();
    else if (id == QLatin1String("models"))
        populate_models();
    else if (id == QLatin1String("intl"))
        populate_intl();
    else if (id == QLatin1String("sources"))
        populate_sources();
}

namespace {

void etfr_fit_columns(QTableView* v, const QVector<Column>& cols) {
    v->ensurePolished();
    v->horizontalHeader()->ensurePolished();
    const QFontMetrics cell_fm(v->font());
    const QFontMetrics head_fm(v->horizontalHeader()->font());
    const QAbstractItemModel* m = v->model();
    const int rows = m ? m->rowCount() : 0;
    for (int i = 0; i < cols.size(); ++i) {
        int w = head_fm.horizontalAdvance(cols[i].header) + 26; // sort indicator and padding
        for (int r = 0; r < rows && r < 600; ++r)
            w = std::max(w, cell_fm.horizontalAdvance(m->index(r, i).data(Qt::DisplayRole).toString()) + 16);
        // Free text (names, groups, notes) is capped and elided; its full value is in the tooltip.
        const int cap = cols[i].text ? std::max(cols[i].width * 3 / 2, 90) : 260;
        v->setColumnWidth(i, std::clamp(w, 36, cap));
    }
}

} // namespace

void EtfResearchScreen::populate_universe() {
    const QVector<Column> cols = {
        {tr("Ticker"), tr("Yahoo symbol of the research row"), 58, true},
        {tr("Name"), QString(), 190, true},
        {tr("Group"), tr("Role in the research universe"), 150, true},
        {tr("Struct"), tr("ETF / UIT / grantor TRUST / commodity POOL / INDEX; leverage, inverse and option flags"), 64,
         true},
        {tr("1D"), tr("1-session total return (PROXY)"), 56},
        {tr("1W"), tr("5-session total return (PROXY)"), 56},
        {tr("1M"), tr("21-session total return (PROXY)"), 60},
        {tr("3M"), tr("63-session total return (PROXY)"), 60},
        {tr("Rel1M"), tr("1M total return minus the row's declared benchmark, percentage points (PROXY)"), 64},
        {tr("RRG"), tr("Relative-rotation quadrant vs benchmark, with momentum direction (MODEL)"), 58, true},
        {tr("Mom z"), tr("Cross-sectional momentum composite z within the peer group (MODEL)"), 54},
        {tr("Model"), tr("Sector confluence / country composite score (MODEL)"), 62},
        {tr("Flow"), tr("Best available flow: SEC N-PORT month (MEASURED) or latest captured interval (ESTIMATED)"),
         104},
        {tr("Ev"), tr("Evidence class of the flow column"), 42, true},
        {tr("Cred"), tr("Credibility of the flow value"), 42},
        {tr("Est 20D"), tr("Estimated flow over the last 20 sessions (ESTIMATED), with coverage in the tooltip"), 82},
        {tr("AUM"), tr("Provider-reported total assets from the latest capture (undated by Yahoo)"), 72},
        {tr("TurnΔ"), tr("Turnover-share change vs the previous 20 sessions, U.S. sectors only (PROXY)"), 56},
        {tr("Fresh"), tr("Freshness of the stored daily history"), 58, true},
        {tr("Last"), tr("Last stored completed session"), 74, true}};
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    for (const UniverseRow& r : snap_->rows) {
        QVector<Cell> c;
        c << etfr_text(r.inst.symbol, r.inst.note, true) << etfr_text(r.fund.name.isEmpty() ? r.inst.name : r.fund.name)
          << etfr_text(etfr_group_text(r)) << etfr_text(etfr_structure_tag(r.inst), r.inst.structure_basis)
          << etfr_value(r.ret.d1, 2, tr("1D")) << etfr_value(r.ret.w1, 2, tr("1W")) << etfr_value(r.ret.m1, 2, tr("1M"))
          << etfr_value(r.ret.m3, 2, tr("3M")) << etfr_value(r.ret.rel_m1, 2, tr("vs %1").arg(r.ret.benchmark))
          << etfr_quadrant(r.rrg) << etfr_value(r.momentum_z) << etfr_value(r.model_score);
        const auto flow = etfr_flow_cells(r);
        c << flow.first << flow.second;
        const ResearchValue& fv = r.measured.latest.usable() ? r.measured.latest : r.est.best;
        c << etfr_grade(fv);
        Cell e20 = etfr_value(r.est.sum_20, 2, tr("Estimated flow, 20 sessions"));
        e20.tooltip += QStringLiteral("<br/>") + value_tooltip(r.est.coverage_20, tr("Coverage of the 20 sessions"));
        c << e20 << etfr_value(r.fund.aum, 2, tr("AUM")) << etfr_value(r.turnover_delta_bp) << etfr_fresh(r)
          << etfr_text(r.last_bar.toString(Qt::ISODate));
        c[16].text = r.fund.aum.value ? fmt_usd(*r.fund.aum.value, false) : na();
        rows << c;
        keys << r.inst.symbol;
        tags << etfr_row_tags(r);
    }
    models_[QStringLiteral("universe")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("universe")], cols);
}

void EtfResearchScreen::populate_sectors() {
    const QVector<Column> cols = {
        {tr("Ticker"), QString(), 52, true},
        {tr("Sector"), QString(), 150, true},
        {tr("Bucket"),
         tr("Defensive / cyclical / unaligned (reference definition; XLE "
            "unaligned)"),
         70, true},
        {tr("1D"), QString(), 54},
        {tr("1W"), QString(), 54},
        {tr("1M"), QString(), 58},
        {tr("3M"), QString(), 58},
        {tr("vsSPY"), tr("1M minus SPY, pp"), 58},
        {tr("RRG"), QString(), 58, true},
        {tr("Turn%"), tr("Share of sector-complex dollar turnover, 20-session mean"), 56},
        {tr("Δbp"), tr("Change vs previous 20 sessions"), 52},
        {tr("Δz"), tr("z of that change vs trailing year (overlapping windows: descriptive)"), 48},
        {tr("BC"), tr("Business-cycle layer z (MODEL, EXPERIMENTAL)"), 50},
        {tr("MOM"), tr("Momentum layer z"), 50},
        {tr("VAL"), tr("Valuation layer z"), 50},
        {tr("F"), tr("Macro-factor layer z"), 50},
        {tr("Model"), tr("Confluence of the available layers"), 56},
        {tr("Band"), QString(), 64, true},
        {tr("Conflicts"), QString(), 90, true}};
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    QVector<HeatTile> tiles;
    QVector<const UniverseRow*> rrg_rows;
    for (const UniverseRow& r : snap_->rows) {
        if (!r.inst.has_role("us_sector"))
            continue;
        const SectorModelRow* m = nullptr;
        for (const auto& s : snap_->sectors.rows)
            if (s.symbol == r.inst.symbol)
                m = &s;
        QVector<Cell> c;
        c << etfr_text(r.inst.symbol, QString(), true) << etfr_text(r.inst.sector) << etfr_text(r.inst.bucket.toUpper())
          << etfr_value(r.ret.d1) << etfr_value(r.ret.w1) << etfr_value(r.ret.m1) << etfr_value(r.ret.m3)
          << etfr_value(r.ret.rel_m1) << etfr_quadrant(r.rrg) << etfr_value(r.turnover_share)
          << etfr_value(r.turnover_delta_bp) << etfr_value(r.turnover_z);
        if (m) {
            c << etfr_value(m->bc_z) << etfr_value(m->mom_z) << etfr_value(m->val_z) << etfr_value(m->factor_z)
              << etfr_value(m->confluence) << etfr_text(m->band) << etfr_text(m->conflicts.join(QStringLiteral(",")));
        } else {
            for (int k = 0; k < 7; ++k)
                c << etfr_text(na());
        }
        rows << c;
        keys << r.inst.symbol;
        tags << etfr_row_tags(r);
        tiles << etfr_return_tile(r, horizon_, r.inst.bucket.toUpper(), r.inst.sector);
        rrg_rows << &r;
    }
    models_[QStringLiteral("sectors")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("sectors")], cols);
    sector_heat_->set_tiles(
        tiles, etfr_range(tiles),
        tr("U.S. sectors · %1 total return (PROXY) · corner: RRG quadrant").arg(horizon_.toUpper()));
    sector_rrg_->set_series(etfr_rrg_series(rrg_rows), tr("Sector RRG vs SPY (weekly, completed weeks)"));
    sector_rrg_->set_selected(selected_);
    const auto& t = snap_->tilt;
    tilt_label_->setText(
        tr("<b>Defensive vs cyclical tilt</b> (turnover share, PROXY — not flow): %1 · Δ %2 · z %3 · band ±%4bp. "
           "Defensive XLP XLU XLV XLRE; cyclical XLK XLY XLI XLF XLB XLC; XLE unaligned. "
           "Breadth: %5/%6 sectors beat SPY over 1M.")
            .arg(t.state.isEmpty() ? na() : t.state, fmt_value(t.tilt_bp), fmt_value(t.tilt_z))
            .arg(t.band_bp, 0, 'f', 0)
            .arg(t.above_bench)
            .arg(t.breadth_total));
}

void EtfResearchScreen::populate_themes() {
    const QVector<Column> cols = {
        {tr("Ticker"), QString(), 52, true}, {tr("Theme"), QString(), 140, true}, {tr("Name"), QString(), 200, true},
        {tr("1D"), QString(), 54},           {tr("1W"), QString(), 54},           {tr("1M"), QString(), 58},
        {tr("3M"), QString(), 58},           {tr("vsSPY"), QString(), 58},        {tr("RRG"), QString(), 58, true},
        {tr("Mom z"), QString(), 54},        {tr("Flow"), QString(), 100},        {tr("Ev"), QString(), 42, true},
        {tr("AUM"), QString(), 72},          {tr("Fresh"), QString(), 58, true}};
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    QVector<HeatTile> tiles;
    QVector<const UniverseRow*> rrg_rows;
    for (const UniverseRow& r : snap_->rows) {
        if (!r.inst.has_role("theme"))
            continue;
        QVector<Cell> c;
        const auto flow = etfr_flow_cells(r);
        c << etfr_text(r.inst.symbol, r.inst.note, true) << etfr_text(r.inst.theme)
          << etfr_text(r.fund.name.isEmpty() ? r.inst.name : r.fund.name) << etfr_value(r.ret.d1)
          << etfr_value(r.ret.w1) << etfr_value(r.ret.m1) << etfr_value(r.ret.m3) << etfr_value(r.ret.rel_m1)
          << etfr_quadrant(r.rrg) << etfr_value(r.momentum_z) << flow.first << flow.second << etfr_value(r.fund.aum)
          << etfr_fresh(r);
        c[12].text = r.fund.aum.value ? fmt_usd(*r.fund.aum.value, false) : na();
        rows << c;
        keys << r.inst.symbol;
        tags << etfr_row_tags(r);
        tiles << etfr_return_tile(r, horizon_, QString(), r.inst.theme);
        rrg_rows << &r;
    }
    models_[QStringLiteral("themes")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("themes")], cols);
    theme_heat_->set_tiles(tiles, etfr_range(tiles),
                           tr("Themes (ETF proxies) · %1 total return (PROXY)").arg(horizon_.toUpper()));
    theme_rrg_->set_series(etfr_rrg_series(rrg_rows), tr("Theme RRG vs SPY"));
}

void EtfResearchScreen::populate_countries() {
    const QVector<Column> cols = {{tr("Ticker"), QString(), 52, true},
                                  {tr("Country / region"), QString(), 130, true},
                                  {tr("Type"), QString(), 64, true},
                                  {tr("1M"), QString(), 58},
                                  {tr("3M"), QString(), 58},
                                  {tr("vsACWI"), tr("1M minus ACWI, pp"), 60},
                                  {tr("RRG"), QString(), 58, true},
                                  {tr("M"), tr("Momentum z (12-1, 6, 3 month)"), 50},
                                  {tr("Q"), tr("World Bank macro quality z (GDP 3y + current account)"), 50},
                                  {tr("C"), tr("Carry z (trailing 12M distributions / price)"), 50},
                                  {tr("Comp"), tr("Composite (MODEL)"), 56},
                                  {tr("Band"), QString(), 64, true},
                                  {tr("GDP3y"), QString(), 58},
                                  {tr("CA%GDP"), QString(), 58},
                                  {tr("Carry"), QString(), 56}};
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    QVector<HeatTile> tiles;
    QVector<const UniverseRow*> rrg_rows;
    for (const CountryRow& k : snap_->countries) {
        const UniverseRow* r = snap_->row(k.symbol);
        if (!r)
            continue;
        QVector<Cell> c;
        Cell gdp = etfr_value(k.gdp_3y, 1, tr("GDP growth, 3 latest years"));
        if (k.gdp_year)
            gdp.text += QStringLiteral(" '%1").arg(k.gdp_year % 100, 2, 10, QLatin1Char('0'));
        c << etfr_text(k.symbol, QString(), true) << etfr_text(k.country) << etfr_text(k.type.toUpper())
          << etfr_value(r->ret.m1) << etfr_value(r->ret.m3) << etfr_value(r->ret.rel_m1) << etfr_quadrant(r->rrg)
          << etfr_value(k.m_z) << etfr_value(k.q_z) << etfr_value(k.c_z) << etfr_value(k.composite) << etfr_text(k.band)
          << gdp << etfr_value(k.current_account, 1) << etfr_value(k.carry);
        rows << c;
        keys << k.symbol;
        tags << etfr_row_tags(*r);
        tiles << etfr_return_tile(*r, horizon_, k.region.isEmpty() ? r->inst.country_region : k.region, k.country);
        rrg_rows << r;
    }
    models_[QStringLiteral("countries")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("countries")], cols);
    country_heat_->set_tiles(tiles, etfr_range(tiles),
                             tr("Countries / regions · %1 total return in USD (PROXY)").arg(horizon_.toUpper()));
    country_rrg_->set_series(etfr_rrg_series(rrg_rows), tr("Country RRG vs ACWI"));
    // Thai equal-weight baskets.
    const QVector<Column> bcols = {
        {tr("Basket"), tr("Equal-weight, daily-rebalanced stock basket (not an ETF)"), 120, true},
        {tr("Members"), QString(), 60, true},
        {tr("1M"), QString(), 56},
        {tr("3M"), QString(), 56},
        {tr("Rel1M"), QString(), 58},
        {tr("RRG"), QString(), 56, true},
        {tr("Bench"), QString(), 70, true}};
    QVector<QVector<Cell>> brows;
    QStringList bkeys;
    QVector<QStringList> btags;
    for (const BasketRow& b : snap_->baskets) {
        QVector<Cell> c;
        c << etfr_text(b.basket.name, b.basket.members.join(QStringLiteral(", ")), true)
          << etfr_text(QStringLiteral("%1/%2").arg(b.members_available).arg(b.basket.members.size()))
          << etfr_value(b.ret.m1) << etfr_value(b.ret.m3) << etfr_value(b.ret.rel_m1) << etfr_quadrant(b.rrg)
          << etfr_text(b.benchmark_used + (b.benchmark_fallback ? tr(" (fallback)") : QString()),
                       b.benchmark_fallback ? tr("SET index history unusable; TDEX.BK SET50 ETF used (FALLBACK)")
                                            : QString());
        brows << c;
        bkeys << QString();
        btags << QStringList{b.basket.id, b.basket.name};
    }
    models_[QStringLiteral("baskets")]->set_content(bcols, brows, bkeys, btags);
    etfr_fit_columns(tables_[QStringLiteral("baskets")], bcols);
}

void EtfResearchScreen::populate_flow() {
    const QString mode = flow_mode_->currentData().toString();
    QVector<HeatTile> tiles;
    for (const UniverseRow& r : snap_->rows) {
        if (!r.inst.is_fund())
            continue;
        HeatTile t;
        t.key = r.inst.symbol;
        t.label = r.inst.symbol;
        t.sub = r.inst.has_role("us_sector")
                    ? r.inst.sector
                    : (r.inst.has_role("theme") ? r.inst.theme
                                                : (r.inst.has_role("country") ? r.inst.country : r.inst.exposure));
        t.group = r.peer_group == QLatin1String("us_sector") ? tr("US SECTORS")
                  : r.peer_group == QLatin1String("theme")   ? tr("THEMES")
                  : r.peer_group == QLatin1String("country") ? tr("COUNTRY / REGION")
                                                             : tr("CROSS-ASSET / BENCHMARK");
        const ResearchValue* v = nullptr;
        if (mode == QLatin1String("est_pct20"))
            v = &r.est.pct_aum_20;
        else if (mode == QLatin1String("est_latest"))
            v = &r.est.latest;
        else if (mode == QLatin1String("measured"))
            v = &r.measured.latest;
        else
            v = &r.ret.m1;
        t.value = v->value;
        t.value_text = fmt_value(*v, mode == QLatin1String("est_pct20") ? 2 : 1);
        t.tag = v->usable() ? QLatin1String(evidence_tag(v->evidence)) : QStringLiteral("N/A");
        t.tag_color = evidence_color(v->usable() ? v->evidence : EvidenceClass::Unavailable);
        t.tooltip = value_tooltip(*v, r.inst.symbol);
        tiles << t;
    }
    int with = 0;
    for (const auto& t : tiles)
        with += t.value ? 1 : 0;
    flow_heat_->set_tiles(tiles, etfr_range(tiles), flow_mode_->currentText());
    // Why the latest estimated interval is unavailable, counted over the funds.
    QMap<QString, int> why;
    for (const UniverseRow& r : snap_->rows)
        if (r.inst.is_fund() && !r.est.latest.usable() && !r.est.latest.reason.isEmpty())
            ++why[r.est.latest.reason];
    QStringList why_text;
    for (auto it = why.cbegin(); it != why.cend(); ++it)
        why_text << QStringLiteral("%1 %2").arg(it.value()).arg(it.key());
    flow_note_->setText(tr("%1 of %2 funds have a value in this mode; hatched tiles have none (never zero). "
                           "Latest estimate unavailable: %3.")
                            .arg(with)
                            .arg(tiles.size())
                            .arg(why_text.isEmpty() ? tr("none") : why_text.join(QStringLiteral(", "))));
    flow_note_->setToolTip(flow_note_->text());
    const QVector<Column> cols = {
        {tr("Ticker"), QString(), 52, true},
        {tr("Measured"), tr("SEC N-PORT latest available month (MEASURED)"), 104},
        {tr("3M meas"), QString(), 80},
        {tr("E1 latest"), tr("Δ(AUM/NAV) × NAV over the latest captured interval"), 84},
        {tr("E2"), tr("Δ(reported shares) × NAV"), 80},
        {tr("E3"), tr("AUM − AUM_prev × close ratio"), 80},
        {tr("Agree"), tr("E1 vs E2 within 0.2% of prior AUM"), 110, true},
        {tr("Why not"),
         tr("Reason the latest E1 interval has no value (aum_not_updated = Yahoo re-served the "
            "previous AUM while NAV moved; record_too_young = one capture session)"),
         150, true},
        {tr("Cred"), QString(), 42},
        {tr("Est 5D"), QString(), 80},
        {tr("Est 20D"), QString(), 80},
        {tr("%AUM 20D"), QString(), 66},
        {tr("Cov 20D"), tr("Share of the 20 sessions covered by captured intervals"), 58},
        {tr("Captures"), tr("Capture sessions / captures"), 64, true},
        {tr("AUM"), QString(), 72},
        {tr("Sh gap"), tr("Implied (AUM/NAV) vs reported shares, %"), 58},
        {tr("Prem"), tr("Close vs NAV at the assumed session, %"), 54},
        {tr("Valid."), tr("E1 month sum minus SEC month (when a month is fully covered)"), 80}};
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    for (const UniverseRow& r : snap_->rows) {
        if (!r.inst.is_fund())
            continue;
        QVector<Cell> c;
        Cell meas = etfr_value(r.measured.latest);
        if (r.measured.latest.usable())
            meas.text += QLatin1Char(' ') + r.measured.latest.effective.toString(QStringLiteral("MMMyy"));
        c << etfr_text(r.inst.symbol, QString(), true) << meas << etfr_value(r.measured.sum_3m)
          << etfr_value(r.est.latest) << etfr_value(r.est.latest_e2) << etfr_value(r.est.latest_e3)
          << etfr_text(r.est.agreement.isEmpty() ? na() : r.est.agreement.toUpper())
          << etfr_text(r.est.latest.usable() ? QString() : r.est.latest.reason)
          << etfr_cred(r.est.latest.usable() ? r.est.latest.credibility : Credibility::NotGraded,
                       r.est.latest.credibility_reasons)
          << etfr_value(r.est.sum_5) << etfr_value(r.est.sum_20) << etfr_value(r.est.pct_aum_20)
          << etfr_value(r.est.coverage_20, 0)
          << etfr_text(QStringLiteral("%1/%2").arg(r.est.capture_sessions).arg(r.est.captures))
          << etfr_value(r.fund.aum) << etfr_value(r.fund.shares_gap_pct, 1) << etfr_value(r.fund.nav_premium_pct)
          << etfr_value(r.est.validation_vs_measured);
        c[14].text = r.fund.aum.value ? fmt_usd(*r.fund.aum.value, false) : na();
        rows << c;
        keys << r.inst.symbol;
        tags << etfr_row_tags(r);
    }
    models_[QStringLiteral("flow")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("flow")], cols);
    // Batch D measured group flows (latest month per group).
    const QVector<Column> gcols = {{tr("Level"), QString(), 70, true},
                                   {tr("Group"), QString(), 150, true},
                                   {tr("Month"), QString(), 62, true},
                                   {tr("Observed"), tr("Sum of measured constituents (MEASURED, SEC N-PORT)"), 90},
                                   {tr("Complete"), tr("Only when every eligible reporting identity is measured"), 90},
                                   {tr("Quality"), QString(), 70, true},
                                   {tr("Ids obs/unique"), QString(), 80, true},
                                   {tr("Unresolved"), QString(), 70}};
    QHash<QString, const GroupFlowRow*> latest;
    for (const auto& g : snap_->group_flows) {
        const QString k = g.level + QLatin1Char('|') + g.group_id;
        if (!latest.contains(k) || latest[k]->month < g.month)
            latest[k] = &g;
    }
    QStringList gk = latest.keys();
    std::sort(gk.begin(), gk.end());
    QVector<QVector<Cell>> grows;
    QStringList gkeys;
    QVector<QStringList> gtags;
    for (const QString& k : gk) {
        const GroupFlowRow& g = *latest[k];
        auto money = [](const std::optional<double>& v) {
            Cell c;
            c.text = v ? fmt_usd(*v) : na();
            c.sort = v;
            c.fg = !v ? token(&ui::ThemeTokens::text_tertiary)
                      : (*v >= 0 ? token(&ui::ThemeTokens::positive) : token(&ui::ThemeTokens::negative));
            return c;
        };
        QVector<Cell> c;
        Cell unres;
        unres.text = QString::number(g.unresolved_subjects);
        unres.sort = g.unresolved_subjects;
        c << etfr_text(g.level) << etfr_text(g.group_id, g.taxonomy_version)
          << etfr_text(g.month.toString(QStringLiteral("yyyy-MM"))) << money(g.observed_net_flow_usd)
          << money(g.complete_net_flow_usd) << etfr_text(g.quality)
          << etfr_text(QStringLiteral("%1/%2").arg(g.observed_reporting_identities).arg(g.unique_reporting_identities))
          << unres;
        grows << c;
        gkeys << QString();
        gtags << QStringList{g.level, g.group_id};
    }
    models_[QStringLiteral("groups")]->set_content(gcols, grows, gkeys, gtags);
    etfr_fit_columns(tables_[QStringLiteral("groups")], gcols);
}

void EtfResearchScreen::populate_rrg() {
    const QString set = rrg_set_->currentData().toString();
    const int tail = rrg_tail_->currentData().toInt();
    QVector<RrgSeries> series;
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    int k = 0;
    auto add = [&](const QString& key, const QString& label, const RrgResult& rr, const QString& name) {
        RrgSeries s;
        s.key = key;
        s.label = label;
        s.points = rr.trail.mid(std::max(0, static_cast<int>(rr.trail.size()) - tail));
        s.color = QColor::fromHsv((k++ * 47) % 360, 170, 235);
        if (!s.points.isEmpty())
            series << s;
        QVector<Cell> c;
        Cell ratio, mom;
        ratio.text = rr.trail.isEmpty() ? na() : QString::number(rr.trail.last().ratio, 'f', 2);
        mom.text = rr.trail.isEmpty() ? na() : QString::number(rr.trail.last().mom, 'f', 2);
        if (!rr.trail.isEmpty()) {
            ratio.sort = rr.trail.last().ratio;
            mom.sort = rr.trail.last().mom;
        }
        c << etfr_text(label, name, true) << etfr_quadrant(rr) << ratio << mom
          << etfr_text(rr.trail.isEmpty() ? na() : rr.trail.last().date.toString(Qt::ISODate));
        rows << c;
        keys << key;
        tags << QStringList{label, name};
    };
    if (set == QLatin1String("baskets")) {
        for (const BasketRow& b : snap_->baskets)
            add(QString(), b.basket.name, b.rrg, b.basket.members.join(QStringLiteral(", ")));
    } else {
        for (const UniverseRow& r : snap_->rows)
            if (r.peer_group == set || (set == QLatin1String("country") && r.inst.has_role("country")))
                add(r.inst.symbol, r.inst.symbol, r.rrg, r.inst.name);
    }
    big_rrg_->set_series(series, rrg_set_->currentText());
    big_rrg_->set_selected(selected_);
    const QVector<Column> cols = {{tr("Name"), QString(), 90, true},
                                  {tr("Quadrant"), QString(), 70, true},
                                  {tr("RS-Ratio"), QString(), 66},
                                  {tr("RS-Mom"), QString(), 66},
                                  {tr("Week"), QString(), 80, true}};
    models_[QStringLiteral("rrg")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("rrg")], cols);
}

void EtfResearchScreen::populate_regime() {
    const auto& t = ui::ThemeManager::instance().tokens();
    const QString period = regime_period_->currentData().toString();
    const QString mode = regime_mode_->currentData().toString();
    const CorrelationView* v = nullptr;
    for (const auto& x : snap_->regime.views)
        if (x.period == period)
            v = &x;
    QStringList labels;
    if (v) {
        for (const QString& s : v->symbols)
            labels << s;
        matrix_->set_matrix(labels, mode == QLatin1String("geom") ? v->wedge : v->corr, mode);
        QStringList pl;
        for (const QString& s : v->symbols)
            pl << s;
        pca_->set_vectors(pl, v->pca, v->var_pc1, v->var_pc2);
    }
    const auto& rg = snap_->regime;
    auto lv = [](const ResearchValue& x) {
        return QStringLiteral("%1 <span style='color:#888'>[%2 · %3]</span>")
            .arg(x.label.isEmpty() ? fmt_value(x, 3)
                                   : x.label + QStringLiteral(" (") + fmt_num(*x.value, 3) + QLatin1Char(')'),
                 QLatin1String(evidence_id(x.evidence)), QLatin1String(credibility_id(x.credibility)));
    };
    QString html;
    if (v && v->avg_abs_corr.value) {
        html += tr("<b>%1 window</b> · %2 observations · calibration %3<br/>")
                    .arg(period.toUpper())
                    .arg(v->observations)
                    .arg(v->calibration);
        html += tr("Mean |ρ|: %1<br/>").arg(lv(v->corr_label));
        html += tr("Gram score det(C)<sup>1/N</sup>: %1<br/>").arg(lv(v->geom_label));
        html +=
            tr("Marchenko–Pastur λ<sub>max</sub> %1 · k<sub>signal</sub> %2 · λ<sub>1</sub> %3 (%4% of N) · %5<br/>")
                .arg(v->lambda_max, 0, 'f', 2)
                .arg(v->k_signal)
                .arg(v->lambda_1, 0, 'f', 2)
                .arg(v->lambda_1 / std::max(1, static_cast<int>(v->symbols.size())) * 100, 0, 'f', 0)
                .arg(v->factor_regime);
    } else {
        html += tr("<span style='color:%1'>No correlation view for %2: %3</span><br/>")
                    .arg(QLatin1String(t.text_tertiary), period, v ? v->avg_abs_corr.reason : tr("missing"));
    }
    html += tr("<b>Trend 1M vs 1Y</b> — correlation: %1 (Δ %2) · geometry: %3 (Δ %4)%5<br/>")
                .arg(rg.trend_corr.label.isEmpty() ? na() : rg.trend_corr.label)
                .arg(rg.risk_delta_corr, 0, 'f', 3)
                .arg(rg.trend_geom.label.isEmpty() ? na() : rg.trend_geom.label)
                .arg(rg.risk_delta_geom, 0, 'f', 3)
                .arg(rg.corr_geom_conflict ? tr(" · <span style='color:%1'>CORR/GEOM disagree (gap %2)</span>")
                                                 .arg(QLatin1String(t.warning))
                                                 .arg(rg.risk_gap, 0, 'f', 2)
                                           : QString());
    if (rg.mrs_ready) {
        QStringList th, sd;
        for (double x : rg.mrs_thresholds)
            th << QString::number(x, 'f', 3);
        for (double x : rg.mrs_threshold_std)
            sd << (x < 0 ? na() : QString::number(x, 'f', 3));
        html +=
            tr("<b>MRS</b> 4-state HMM on 63-session mean |ρ| (9 core sectors): %1<br/>thresholds %2 · expanding-refit "
               "std %3 (in-sample; > 0.05 = unstable)<br/>")
                .arg(lv(rg.mrs_label), th.join(QStringLiteral(" / ")), sd.join(QStringLiteral(" / ")));
    } else {
        html += tr("<b>MRS</b> unavailable: %1<br/>").arg(rg.mrs_label.reason);
    }
    const auto& v2 = rg.v2;
    if (!v2.label.label.isEmpty()) {
        QStringList f;
        for (auto it = v2.features.begin(); it != v2.features.end(); ++it)
            f << QStringLiteral("%1 %2").arg(it.key()).arg(it.value(), 0, 'f', 3);
        f.sort();
        html += tr("<b>Regime HMM</b> (6 features, in-sample, EXPERIMENTAL): %1 · %2 sessions since %3 · %4 obs%5<br/>"
                   "features: %6<br/><i>Descriptive state only; no allocation or trading posture is implied.</i>")
                    .arg(lv(v2.label))
                    .arg(v2.days_in_state)
                    .arg(v2.since.toString(Qt::ISODate))
                    .arg(v2.observations)
                    .arg(v2.converged ? QString() : tr(" · not converged"))
                    .arg(f.join(QStringLiteral(" · ")));
    } else {
        html += tr("<b>Regime HMM</b> unavailable: %1").arg(v2.label.reason);
    }
    regime_stats_->setText(html);
    QVector<Bar> bars;
    const QStringList names = {QStringLiteral("DIVERGENT"), QStringLiteral("TRENDING"), QStringLiteral("RISK-OFF"),
                               QStringLiteral("CRISIS")};
    for (int i = 0; i < 4; ++i) {
        Bar b;
        b.label = names[i];
        if (i < v2.probabilities.size()) {
            b.value = v2.probabilities[i];
            b.value_text = QString::number(v2.probabilities[i] * 100, 'f', 0) + QLatin1Char('%');
        }
        b.color = token(&ui::ThemeTokens::info);
        bars << b;
    }
    regime_probs_->set_bars(bars, tr("Regime HMM filtered probabilities (MODEL, EXPERIMENTAL)"));
}

void EtfResearchScreen::populate_models() {
    const auto& c = snap_->sectors.cycle;
    auto comp = [](const ResearchValue& v, const QString& n) {
        return QStringLiteral("%1 %2").arg(n,
                                           v.value ? fmt_signed(*v.value, 2) : QStringLiteral("— (%1)").arg(v.reason));
    };
    cycle_label_->setText(
        tr("<b>Business-cycle clock</b> (MODEL, %1): phase <b>%2</b> · level %3 · direction %4<br/>%5 · %6 · %7 · %8%9"
           "<br/>Sectors above 200-day: %10 · VIX %11 · weights %12%13")
            .arg(QLatin1String(credibility_id(c.phase.credibility)),
                 c.phase.label.isEmpty() ? QStringLiteral("— (%1)").arg(c.phase.reason) : c.phase.label,
                 fmt_value(c.level), fmt_value(c.direction), comp(c.z_slope, tr("slope z")),
                 comp(c.z_activity, tr("CFNAI z")), comp(c.z_credit, tr("credit z")), comp(c.z_momentum, tr("Δ3m z")),
                 c.credit_fallback ? tr(" (credit via HYG−LQD FALLBACK)") : QString(),
                 fmt_value(c.sectors_above_200dma), fmt_value(c.vix, 1),
                 snap_->sectors.volatility_brake ? tr("CRISIS (VIX>35)") : tr("NORMAL"),
                 snap_->sectors.mom_val_divergence ? tr(" · MOM/VAL leaders diverge (%1 vs %2)")
                                                         .arg(snap_->sectors.top_momentum, snap_->sectors.top_valuation)
                                                   : QString()));
    QVector<Bar> bars;
    const QList<QPair<QString, double>> ph = {{tr("RECOV"), c.p_recovery},
                                              {tr("EXPAN"), c.p_expansion},
                                              {tr("SLOWD"), c.p_slowdown},
                                              {tr("CONTR"), c.p_contraction}};
    for (const auto& p : ph) {
        Bar b;
        b.label = p.first;
        if (c.level.value) {
            b.value = p.second;
            b.value_text = QString::number(p.second * 100, 'f', 0) + QLatin1Char('%');
        }
        b.color = token(&ui::ThemeTokens::accent);
        bars << b;
    }
    cycle_bars_->set_bars(bars, tr("Phase probabilities (level × direction)"));
    // Factor sensitivities.
    QVector<Column> fcols = {{tr("Ticker"), QString(), 50, true}};
    for (const QString& n : snap_->sectors.factor_names)
        fcols.append({tr("β ") + n, tr("OLS sensitivity of weekly sector−SPY return; t-stat in tooltip"), 74});
    fcols.append({tr("R²"), QString(), 50});
    fcols.append({tr("F z"), QString(), 54});
    QVector<QVector<Cell>> frows;
    QStringList fkeys;
    QVector<QStringList> ftags;
    for (const auto& m : snap_->sectors.rows) {
        QVector<Cell> c2;
        c2 << etfr_text(m.symbol, QString(), true);
        for (int i = 0; i < snap_->sectors.factor_names.size(); ++i) {
            Cell b;
            if (i < m.factor_betas.size()) {
                b.text = QString::number(m.factor_betas[i], 'f', 3);
                b.sort = m.factor_betas[i];
                b.tooltip = tr("t = %1").arg(m.factor_tstats.value(i), 0, 'f', 2);
                b.fg = std::abs(m.factor_tstats.value(i)) >= 2 ? token(&ui::ThemeTokens::text_primary)
                                                               : token(&ui::ThemeTokens::text_tertiary);
            } else {
                b.text = na();
            }
            c2 << b;
        }
        Cell r2;
        r2.text = m.factor_betas.isEmpty() ? na() : QString::number(m.factor_r2, 'f', 2);
        if (!m.factor_betas.isEmpty())
            r2.sort = m.factor_r2;
        c2 << r2 << etfr_value(m.factor_z);
        frows << c2;
        fkeys << m.symbol;
        ftags << QStringList{m.symbol};
    }
    models_[QStringLiteral("factors")]->set_content(fcols, frows, fkeys, ftags);
    etfr_fit_columns(tables_[QStringLiteral("factors")], fcols);
    // Sector model table.
    const QVector<Column> scols = {
        {tr("Ticker"), QString(), 50, true},
        {tr("Sector"), QString(), 140, true},
        {tr("BC z"), QString(), 54},
        {tr("MOM z"), QString(), 54},
        {tr("VAL z"), tr("Own-history P/E z when available, else cross-section EY−10Y z"), 54},
        {tr("F z"), QString(), 54},
        {tr("Confl."), QString(), 58},
        {tr("Band"), QString(), 64, true},
        {tr("Rank"), QString(), 44},
        {tr("Disp."), tr("Std-dev of available layer z: model disagreement"), 52},
        {tr("Conflicts"), QString(), 80, true},
        {tr("P/E"), QString(), 52},
        {tr("EY−10Y"), QString(), 60},
        {tr("P/E own z"), QString(), 64}};
    QVector<QVector<Cell>> srows;
    QStringList skeys;
    QVector<QStringList> stags;
    for (const auto& m : snap_->sectors.rows) {
        QVector<Cell> c3;
        Cell rank;
        rank.text = m.rank ? QString::number(m.rank) : na();
        if (m.rank)
            rank.sort = m.rank;
        c3 << etfr_text(m.symbol, QString(), true) << etfr_text(m.name) << etfr_value(m.bc_z) << etfr_value(m.mom_z)
           << etfr_value(m.val_z) << etfr_value(m.factor_z) << etfr_value(m.confluence) << etfr_text(m.band) << rank
           << etfr_value(m.layer_dispersion) << etfr_text(m.conflicts.join(QStringLiteral(","))) << etfr_value(m.pe, 1)
           << etfr_value(m.earnings_yield_spread) << etfr_value(m.pe_own_z);
        srows << c3;
        skeys << m.symbol;
        stags << QStringList{m.symbol, m.name};
    }
    models_[QStringLiteral("sector_models")]->set_content(scols, srows, skeys, stags);
    etfr_fit_columns(tables_[QStringLiteral("sector_models")], scols);
    // Country model table.
    const QVector<Column> ccols = {{tr("Ticker"), QString(), 50, true}, {tr("Country"), QString(), 130, true},
                                   {tr("Type"), QString(), 64, true},   {tr("M z"), QString(), 54},
                                   {tr("Q z"), QString(), 54},          {tr("C z"), QString(), 54},
                                   {tr("Composite"), QString(), 70},    {tr("Band"), QString(), 64, true},
                                   {tr("Rank"), QString(), 44}};
    QVector<QVector<Cell>> crows;
    QStringList ckeys;
    QVector<QStringList> ctags;
    for (const auto& k : snap_->countries) {
        QVector<Cell> c4;
        Cell rank;
        rank.text = k.rank ? QString::number(k.rank) : na();
        if (k.rank)
            rank.sort = k.rank;
        c4 << etfr_text(k.symbol, QString(), true) << etfr_text(k.country) << etfr_text(k.type) << etfr_value(k.m_z)
           << etfr_value(k.q_z) << etfr_value(k.c_z) << etfr_value(k.composite) << etfr_text(k.band) << rank;
        crows << c4;
        ckeys << k.symbol;
        ctags << QStringList{k.symbol, k.country};
    }
    models_[QStringLiteral("country_models")]->set_content(ccols, crows, ckeys, ctags);
    etfr_fit_columns(tables_[QStringLiteral("country_models")], ccols);
}

void EtfResearchScreen::populate_intl() {
    const QString market = intl_market_->currentData().toString();
    QVector<HeatTile> tiles;
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    for (const IntlSector& is : snap_->intl) {
        QVector<Cell> c;
        c << etfr_text(is.market.toUpper()) << etfr_text(is.sector, QString(), true)
          << etfr_text(QStringLiteral("%1/%2").arg(is.members_with_data).arg(is.members)) << etfr_value(is.ew_d1)
          << etfr_value(is.ew_m1) << etfr_value(is.ew_m3) << etfr_value(is.fundamentals.median_pe, 1)
          << etfr_value(is.fundamentals.median_forward_pe, 1) << etfr_value(is.fundamentals.median_debt_equity, 0)
          << etfr_text(is.fundamentals.currency_basis);
        rows << c;
        keys << QString();
        tags << QStringList{is.market, is.sector};
        if (is.market != market)
            continue;
        for (const ConstituentRow& st : is.stocks) {
            HeatTile t;
            t.key = st.symbol;
            t.label = st.name;
            t.sub = st.symbol;
            t.group = is.sector + QStringLiteral("  (EW %1)")
                                      .arg(fmt_value(etfr_horizon(ReturnSet{is.ew_d1, is.ew_d1, is.ew_m1, is.ew_m3,
                                                                            ResearchValue(), QString(), QDate()},
                                                                  horizon_)));
            const ResearchValue& v = etfr_horizon(st.ret, horizon_);
            t.value = v.value;
            t.value_text = fmt_value(v, 1);
            t.tooltip =
                value_tooltip(v, st.symbol) + QStringLiteral("<br/>") + value_tooltip(st.pe, tr("Trailing P/E"));
            tiles << t;
        }
    }
    intl_heat_->set_tiles(tiles, etfr_range(tiles),
                          tr("%1 · %2 local-currency total return of representative stocks (PROXY)")
                              .arg(market.toUpper(), horizon_.toUpper()));
    const QVector<Column> cols = {
        {tr("Mkt"), QString(), 40, true},  {tr("Sector"), QString(), 110, true},
        {tr("Data"), QString(), 50, true}, {tr("EW 1D"), QString(), 58},
        {tr("EW 1M"), QString(), 58},      {tr("EW 3M"), QString(), 58},
        {tr("Med P/E"), QString(), 60},    {tr("Med fP/E"), QString(), 60},
        {tr("Med D/E"), QString(), 60},    {tr("Ccy"), tr("Currency basis; mixed = no cap weighting"), 50, true}};
    models_[QStringLiteral("intl")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("intl")], cols);
}

void EtfResearchScreen::populate_sources() {
    const auto& t = ui::ThemeManager::instance().tokens();
    const QVector<Column> cols = {{tr("Stage"), QString(), 150, true},
                                  {tr("Status"), QString(), 90, true},
                                  {tr("Items ok"), QString(), 64, true},
                                  {tr("Inserted"), QString(), 64},
                                  {tr("Revised"), QString(), 60},
                                  {tr("Confirmed"), QString(), 70},
                                  {tr("Stale"), QString(), 46},
                                  {tr("Latest effective"), QString(), 100, true},
                                  {tr("Retrieved (UTC)"), QString(), 130, true},
                                  {tr("Detail"), QString(), 360, true}};
    QVector<QVector<Cell>> rows;
    QStringList keys;
    QVector<QStringList> tags;
    for (const auto& s : snap_->sources) {
        QVector<Cell> c;
        Cell st = etfr_text(s.status, QString(), true);
        st.fg = s.status == QLatin1String("UPDATED") || s.status == QLatin1String("UNCHANGED")
                    ? QColor(QLatin1String(t.positive))
                    : (s.status == QLatin1String("FAILED") ? QColor(QLatin1String(t.negative))
                                                           : QColor(QLatin1String(t.warning)));
        auto num = [](int v) {
            Cell c5;
            c5.text = QString::number(v);
            c5.sort = v;
            return c5;
        };
        c << etfr_text(s.stage, s.dependent_calculations.join(QStringLiteral(", ")), true) << st
          << etfr_text(QStringLiteral("%1/%2").arg(s.items_ok).arg(s.items_requested)) << num(s.rows_inserted)
          << num(s.rows_revised) << num(s.rows_confirmed) << num(s.stale_items) << etfr_text(s.latest_effective)
          << etfr_text(s.retrieved_at.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")))
          << etfr_text(s.detail, s.detail.toHtmlEscaped());
        rows << c;
        keys << QString();
        tags << QStringList{s.stage, s.status};
    }
    models_[QStringLiteral("sources")]->set_content(cols, rows, keys, tags);
    etfr_fit_columns(tables_[QStringLiteral("sources")], cols);
    sources_note_->setText(
        tr("<b>Provenance</b> · engine %1 · universe %2 · reference triphopp/bloomberg-terminal@%3 · frame as_of %4, "
           "known_at %5 · expected NYSE session %6 · last refresh run %7<br/>"
           "Acquisition is manual only (Refresh ETF Research Data). A failed source keeps its earlier stored "
           "observations, which are then shown as STALE where their freshness rule is exceeded; they are never "
           "presented as this refresh's result.<br/>Thai baskets benchmark: %8%9<br/>%10")
            .arg(snap_->engine_version, snap_->universe_version, snap_->reference_commit.left(9),
                 snap_->as_of.toString(Qt::ISODate), snap_->known_at.toString(Qt::ISODate),
                 snap_->expected_us_session.toString(Qt::ISODate),
                 snap_->last_refresh_run_id.isEmpty() ? tr("none") : snap_->last_refresh_run_id,
                 snap_->th_benchmark_used,
                 snap_->th_benchmark_fallback ? tr(" (FALLBACK: SET index history unusable)") : QString(),
                 snap_->warnings.isEmpty() ? QString()
                                           : tr("<span style='color:%1'>Load warnings: %2</span>")
                                                 .arg(QLatin1String(t.warning),
                                                      snap_->warnings.join(QStringLiteral(" · ")).toHtmlEscaped())));
}

// ── Selection / layout ───────────────────────────────────────────────────────

void EtfResearchScreen::on_row_activated(const QString& key) {
    if (key.isEmpty() || !snap_ || !snap_->row(key))
        return;
    select_symbol(key);
}

void EtfResearchScreen::select_symbol(const QString& symbol) {
    if (!snap_ || !snap_->row(symbol))
        return;
    selected_ = symbol;
    for (auto* w : {sector_heat_, theme_heat_, country_heat_, flow_heat_})
        w->set_selected(symbol);
    for (auto* w : {sector_rrg_, theme_rrg_, country_rrg_, big_rrg_})
        w->set_selected(symbol);
    const bool was_hidden = detail_->isHidden();
    detail_->setVisible(true);
    update_layout_mode();
    if (was_hidden && body_->orientation() == Qt::Horizontal) {
        // Open the detail at a readable width instead of the splitter's minimum.
        const int total = std::max(body_->width(), 1);
        const int d = std::clamp(total * 2 / 5, std::min(560, total / 2), 780);
        body_->setSizes({total - d, d});
    }
    populate_detail();
}

bool EtfResearchScreen::detail_visible() const {
    return detail_->isVisible() || (!isVisible() && !detail_->isHidden());
}

void EtfResearchScreen::close_detail() {
    detail_->setVisible(false);
    selected_.clear();
}

void EtfResearchScreen::show_detail_tab(const QString& id) {
    const QStringList ids = {QStringLiteral("overview"), QStringLiteral("chart"), QStringLiteral("flow"),
                             QStringLiteral("holdings"), QStringLiteral("provenance")};
    const int i = ids.indexOf(id);
    if (i >= 0)
        detail_tabs_->setCurrentIndex(i);
}

void EtfResearchScreen::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    update_layout_mode();
}

void EtfResearchScreen::update_layout_mode() {
    // Narrow windows stack the detail below the views instead of squeezing both.
    const Qt::Orientation o = width() < 1000 ? Qt::Vertical : Qt::Horizontal;
    if (body_->orientation() != o)
        body_->setOrientation(o);
}

void EtfResearchScreen::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::LanguageChange && snap_) {
        populated_.clear();
        populate_summary();
        populate_view(current_view_);
    }
}

void EtfResearchScreen::restore_state(const QVariantMap& state) {
    const QString v = state.value(QStringLiteral("view")).toString();
    if (views_.contains(v))
        show_view(v);
    const int h = horizon_box_->findData(state.value(QStringLiteral("horizon")).toString());
    if (h >= 0)
        horizon_box_->setCurrentIndex(h);
}

QVariantMap EtfResearchScreen::save_state() const {
    return {{QStringLiteral("view"), current_view_}, {QStringLiteral("horizon"), horizon_}};
}

} // namespace fincept::screens
