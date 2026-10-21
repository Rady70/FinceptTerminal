#include "screens/dashboard/widgets/RiskMetricsWidget.h"

#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/dashboard/widgets/MarketGauges.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "ui/theme/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>

#include <cmath>

namespace fincept::screens::widgets {

// Symbols: VIX + SPY/QQQ/IWM/TLT for correlation proxies + 6 volatile stocks
static const QStringList kRiskSymbols = {"^VIX", "SPY", "QQQ",  "IWM",  "TLT", "NVDA",
                                         "TSLA", "AMD", "META", "PLTR", "COIN"};

RiskMetricsWidget::RiskMetricsWidget(QWidget* parent) : BaseWidget(tr("RISK METRICS"), parent, ui::colors::NEGATIVE()) {
    // The body scrolls when the tile is shorter than its content: sections
    // never draw over each other (they did with a pinned VIX card height).
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea{background:transparent;border:none;}"
                          "QScrollBar:vertical{width:4px;background:transparent;}"
                          "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}");
    auto* body = new QWidget;
    body->setObjectName("riskBody");
    body->setStyleSheet("#riskBody{background:transparent;}");
    scroll->setWidget(body);
    scroll->viewport()->setAutoFillBackground(false);
    content_layout()->addWidget(scroll);

    auto* vl = new QVBoxLayout(body);
    vl->setContentsMargins(8, 8, 8, 8);
    vl->setSpacing(6);

    // ── VIX section ──
    vix_card_ = new QWidget(body);
    vix_card_->setAttribute(Qt::WA_StyledBackground, true);
    auto* vcl = new QVBoxLayout(vix_card_);
    vcl->setContentsMargins(10, 6, 10, 6);
    vcl->setSpacing(2);

    auto* vix_header = new QHBoxLayout;
    vix_header_lbl_ = new QLabel(tr("VIX FEAR GAUGE"));
    vix_header->addWidget(vix_header_lbl_);
    vix_header->addStretch();
    vix_value_ = new QLabel("--");
    vix_header->addWidget(vix_value_);
    vix_header->addSpacing(8);
    vix_regime_ = new QLabel("--");
    vix_header->addWidget(vix_regime_);
    vcl->addLayout(vix_header);

    vix_gauge_ = new charts::VixGauge(vix_card_);
    vix_gauge_->setToolTip(tr("VIX on a 0–50 scale. Bands: below 15 low volatility, 15–20 normal, 20–30 elevated, "
                              "30–40 high stress, 40+ extreme fear."));
    vcl->addWidget(vix_gauge_);
    vl->addWidget(vix_card_);

    // ── Section separator ──
    sep1_ = new QFrame;
    sep1_->setFixedHeight(1);
    vl->addWidget(sep1_);

    // ── High-beta stocks: today's move and day range ──
    auto* hdr_row = new QWidget(body);
    auto* hrl = new QHBoxLayout(hdr_row);
    hrl->setContentsMargins(0, 0, 0, 0);
    stocks_hdr_ = new QLabel(tr("HIGH-BETA STOCKS"));
    stocks_hdr_->setToolTip(tr("A fixed list of usually volatile stocks; beta is not computed here."));
    hrl->addWidget(stocks_hdr_, 1);
    chg_hdr_ = new QLabel(tr("TODAY"));
    chg_hdr_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    hrl->addWidget(chg_hdr_, 1);
    range_hdr_ = new QLabel(tr("DAY RANGE"));
    range_hdr_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    range_hdr_->setToolTip(tr("(High − low) as % of the previous close: how far the stock swung today."));
    hrl->addWidget(range_hdr_, 1);
    vl->addWidget(hdr_row);

    static const QStringList kVolatile = {"NVDA", "TSLA", "AMD", "META", "PLTR", "COIN"};
    for (const auto& sym : kVolatile) {
        auto* row = new QWidget(body);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 1, 0, 1);

        StockRisk sr;
        sr.symbol = new QLabel(sym);
        rl->addWidget(sr.symbol, 1);

        sr.chg_pct = new QLabel("--");
        sr.chg_pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rl->addWidget(sr.chg_pct, 1);

        sr.hi_lo = new QLabel("--");
        sr.hi_lo->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        rl->addWidget(sr.hi_lo, 1);

        stock_rows_.append(sr);
        vl->addWidget(row);
    }

    // ── Relative moves today ──
    sep2_ = new QFrame;
    sep2_->setFixedHeight(1);
    vl->addWidget(sep2_);

    corr_hdr_ = new QLabel(tr("RELATIVE MOVES TODAY (PERCENTAGE POINTS)"));
    corr_hdr_->setToolTip(
        tr("Today's % change of SPY minus today's % change of the other fund. Positive: SPY did better."));
    vl->addWidget(corr_hdr_);

    auto make_spread_row = [&](const QString& label, const QString& tip, QLabel*& out) {
        auto* row = new QWidget(body);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 1, 0, 1);
        auto* lbl = new QLabel(label);
        lbl->setToolTip(tip);
        spread_labels_.append(lbl);
        rl->addWidget(lbl);
        rl->addStretch();
        out = new QLabel("--");
        rl->addWidget(out);
        vl->addWidget(row);
    };

    make_spread_row(tr("S&P 500 vs Nasdaq 100 (SPY − QQQ)"), tr("Large caps vs tech-heavy growth"), spy_qqq_spread_);
    make_spread_row(tr("Large vs small caps (SPY − IWM)"), tr("S&P 500 vs Russell 2000 small caps"), spy_iwm_spread_);
    make_spread_row(tr("Stocks vs long bonds (SPY − TLT)"), tr("S&P 500 vs 20+ year Treasuries"), equity_bond_lbl_);

    vl->addStretch();

    connect(this, &BaseWidget::refresh_requested, this, &RiskMetricsWidget::refresh_data);

    apply_styles();
    set_loading(true);
}

void RiskMetricsWidget::apply_styles() {
    vix_card_->setStyleSheet(QString("background: %1; border-radius: 2px;").arg(ui::colors::BG_RAISED()));
    vix_header_lbl_->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                                       .arg(ui::colors::TEXT_TERTIARY()));
    vix_regime_->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                                   .arg(ui::colors::TEXT_TERTIARY()));
    vix_value_->setStyleSheet(QString("color: %1; font-size: 22px; font-weight: bold; background: transparent;")
                                  .arg(ui::colors::TEXT_TERTIARY()));
    sep1_->setStyleSheet(QString("background: %1;").arg(ui::colors::BORDER_DIM()));
    sep2_->setStyleSheet(QString("background: %1;").arg(ui::colors::BORDER_DIM()));

    stocks_hdr_->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                                   .arg(ui::colors::TEXT_TERTIARY()));
    corr_hdr_->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                                 .arg(ui::colors::TEXT_TERTIARY()));
    for (QLabel* h : {chg_hdr_, range_hdr_})
        h->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                             .arg(ui::colors::TEXT_TERTIARY()));
    vix_gauge_->update();

    for (const auto& sr : stock_rows_) {
        sr.symbol->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                     .arg(ui::colors::TEXT_PRIMARY()));
        sr.chg_pct->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                      .arg(ui::colors::TEXT_TERTIARY()));
        sr.hi_lo->setStyleSheet(
            QString("color: %1; font-size: 10px; background: transparent;").arg(ui::colors::TEXT_SECONDARY()));
    }

    for (auto* lbl : spread_labels_)
        lbl->setStyleSheet(
            QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::TEXT_SECONDARY()));

    spy_qqq_spread_->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                       .arg(ui::colors::TEXT_PRIMARY()));
    spy_iwm_spread_->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                       .arg(ui::colors::TEXT_PRIMARY()));
    equity_bond_lbl_->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                        .arg(ui::colors::TEXT_PRIMARY()));
}

void RiskMetricsWidget::on_theme_changed() {
    apply_styles();
}

void RiskMetricsWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_subscribe_all();
}

void RiskMetricsWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void RiskMetricsWidget::refresh_data() {
    auto& hub = datahub::DataHub::instance();
    QStringList topics;
    topics.reserve(kRiskSymbols.size());
    for (const auto& sym : kRiskSymbols)
        topics.append(QStringLiteral("market:quote:") + sym);
    hub.request(topics, /*force=*/true); // user-triggered: bypass min_interval
}

void RiskMetricsWidget::hub_subscribe_all() {
    show_feed_status(this, feed_.summary(kRiskSymbols), static_cast<int>(kRiskSymbols.size()));
    // One redraw per delivery burst, not one per symbol.
    feed_.subscribe(this, kRiskSymbols, [this]() { schedule_render([this]() { rebuild_from_cache(); }); });
    hub_active_ = true;
}

void RiskMetricsWidget::hub_unsubscribe_all() {
    QuoteSubscriptions::unsubscribe(this);
    hub_active_ = false;
}

void RiskMetricsWidget::rebuild_from_cache() {
    QVector<services::QuoteData> quotes;
    quotes.reserve(feed_.cache().size());
    for (const auto& sym : kRiskSymbols) {
        auto it = feed_.cache().constFind(sym);
        if (it != feed_.cache().constEnd())
            quotes.append(it.value());
    }
    if (!quotes.isEmpty())
        populate(quotes);
    show_feed_status(this, feed_.summary(kRiskSymbols), static_cast<int>(kRiskSymbols.size()));
}

void RiskMetricsWidget::populate(const QVector<services::QuoteData>& quotes) {
    QMap<QString, const services::QuoteData*> map;
    for (const auto& q : quotes)
        map[q.symbol] = &q;

    // VIX
    if (map.contains("^VIX") && map["^VIX"]->has_price) {
        double vix = map["^VIX"]->price;
        vix_value_->setText(QString::number(vix, 'f', 2));
        // The dashboard's one VIX regime rule (MarketGauges.h).
        const auto vix_state = gauges::vix_regime(vix);
        const QString regime = tr(gauges::vix_regime_key(vix_state));
        const auto tone = gauges::vix_tone(vix_state);
        const QString color = tone == gauges::Tone::Positive   ? ui::colors::POSITIVE()
                              : tone == gauges::Tone::Warning  ? ui::colors::WARNING()
                              : tone == gauges::Tone::Negative ? ui::colors::NEGATIVE()
                                                               : ui::colors::TEXT_PRIMARY();
        vix_value_->setStyleSheet(
            QString("color: %1; font-size: 22px; font-weight: bold; background: transparent;").arg(color));
        vix_regime_->setText(regime);
        vix_regime_->setStyleSheet(
            QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;").arg(color));

        vix_gauge_->set_value(vix);
    } else if (vix_value_) {
        // No price reading is not "VIX 0.00" (which reads as LOW VOLATILITY).
        const QString dim = ui::colors::TEXT_DIM();
        vix_value_->setText(QStringLiteral("--"));
        vix_value_->setStyleSheet(
            QString("color: %1; font-size: 22px; font-weight: bold; background: transparent;").arg(dim));
        vix_regime_->setText(QStringLiteral("—"));
        vix_regime_->setStyleSheet(
            QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;").arg(dim));
        vix_gauge_->set_value(std::nullopt);
    }

    // High-beta stocks
    static const QStringList kVolatile = {"NVDA", "TSLA", "AMD", "META", "PLTR", "COIN"};
    for (int i = 0; i < kVolatile.size() && i < stock_rows_.size(); ++i) {
        const QString& sym = kVolatile[i];
        if (!map.contains(sym))
            continue;
        const auto& q = *map[sym];

        if (!q.has_change_pct) {
            stock_rows_[i].chg_pct->setText(QStringLiteral("--"));
            stock_rows_[i].chg_pct->setStyleSheet(
                QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                    .arg(ui::colors::TEXT_DIM()));
        } else {
            const double chg = q.change_pct;
            stock_rows_[i].chg_pct->setText(fincept::screens::quote_signed_text(true, chg, 2, QStringLiteral("%")));
            const QString chg_col = chg > 0   ? ui::colors::POSITIVE()
                                    : chg < 0 ? ui::colors::NEGATIVE()
                                              : ui::colors::TEXT_PRIMARY();
            stock_rows_[i].chg_pct->setStyleSheet(
                QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;").arg(chg_col));
        }

        // Day range as % of the previous close (presence flags, not a value
        // sentinel); the exact high and low are in the tooltip.
        if (q.has_high && q.has_low && q.has_prev_close && q.prev_close > 0 && q.high >= q.low) {
            const int dp = fincept::screens::quote_price_decimals(true, q.high, 2);
            stock_rows_[i].hi_lo->setText(QString("%1%").arg((q.high - q.low) / q.prev_close * 100.0, 0, 'f', 1));
            stock_rows_[i].hi_lo->setToolTip(tr("High %1 · low %2 · previous close %3")
                                                 .arg(q.high, 0, 'f', dp)
                                                 .arg(q.low, 0, 'f', dp)
                                                 .arg(q.prev_close, 0, 'f', dp));
        } else {
            stock_rows_[i].hi_lo->setText(QStringLiteral("--"));
            stock_rows_[i].hi_lo->setToolTip(tr("High, low or previous close not reported"));
        }
    }

    // Spread proxies — only when both operands carry a change reading.
    auto spread_pair = [&](const QString& a_sym, const QString& b_sym, QLabel* label) {
        if (!label)
            return;
        const bool available =
            map.contains(a_sym) && map.contains(b_sym) && map[a_sym]->has_change_pct && map[b_sym]->has_change_pct;
        if (!available) {
            label->setText(QStringLiteral("--"));
            label->setStyleSheet(QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;")
                                     .arg(ui::colors::TEXT_DIM()));
            return;
        }
        const double diff = map[a_sym]->change_pct - map[b_sym]->change_pct;
        label->setText(fincept::screens::quote_signed_text(true, diff, 2, QStringLiteral(" pp")));
        const QString color = diff > 0   ? ui::colors::POSITIVE()
                              : diff < 0 ? ui::colors::NEGATIVE()
                                         : ui::colors::TEXT_PRIMARY();
        label->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: bold; background: transparent;").arg(color));
    };

    spread_pair("SPY", "QQQ", spy_qqq_spread_);
    spread_pair("SPY", "IWM", spy_iwm_spread_);
    spread_pair("SPY", "TLT", equity_bond_lbl_);
}

void RiskMetricsWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("RISK METRICS"));
    rebuild_from_cache(); // re-renders header/regime/section labels in the new language
}

} // namespace fincept::screens::widgets
