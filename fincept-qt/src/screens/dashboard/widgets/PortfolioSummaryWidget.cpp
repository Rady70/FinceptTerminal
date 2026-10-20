#include "screens/dashboard/widgets/PortfolioSummaryWidget.h"

#include "core/currency/Currency.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/dashboard/widgets/PortfolioValuation.h"
#include "screens/markets/QuoteDisplayFormat.h"
#include "services/portfolio/PortfolioService.h"
#include "storage/repositories/PortfolioRepository.h"
#include "ui/theme/Theme.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QScrollArea>

namespace fincept::screens::widgets {

namespace {
constexpr const char* kConfigKeyPortfolioId = "portfolio_id";
} // namespace

PortfolioSummaryWidget::PortfolioSummaryWidget(QWidget* parent)
    : BaseWidget(tr("PORTFOLIO SUMMARY"), parent, ui::colors::POSITIVE) {
    auto* vl = content_layout();
    vl->setContentsMargins(8, 8, 8, 8);
    vl->setSpacing(6);

    // ── Selected-portfolio label ────────────────────────────────────────────
    // Sits above the summary card so the user always knows which portfolio
    // the numbers belong to. Updated by load_holdings(); hidden until then.
    portfolio_name_lbl_ = new QLabel(this);
    portfolio_name_lbl_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    portfolio_name_lbl_->hide();
    vl->addWidget(portfolio_name_lbl_);

    // ── Summary card ──
    summary_card_ = new QWidget(this);
    auto* sl = new QGridLayout(summary_card_);
    sl->setContentsMargins(10, 8, 10, 8);
    sl->setHorizontalSpacing(16);
    sl->setVerticalSpacing(4);

    auto make_metric = [&](const QString& label, QLabel*& value_out, int row, int col) {
        auto* lbl = new QLabel(label);
        metric_labels_.append(lbl);
        sl->addWidget(lbl, row * 2, col);

        value_out = new QLabel("--");
        metric_values_.append(value_out);
        sl->addWidget(value_out, row * 2 + 1, col);
    };

    make_metric(tr("TOTAL VALUE"), total_value_lbl_, 0, 0);
    make_metric(tr("DAY P&L"), day_pnl_lbl_, 0, 1);
    make_metric(tr("TOTAL P&L"), total_pnl_lbl_, 1, 0);
    make_metric(tr("HOLDINGS"), num_holdings_lbl_, 1, 1);

    vl->addWidget(summary_card_);

    // ── Holdings list header ──
    header_row_ = new QWidget(this);
    auto* hl = new QHBoxLayout(header_row_);
    hl->setContentsMargins(8, 3, 8, 3);

    auto make_hdr_lbl = [&](const QString& t, int s, Qt::Alignment a = Qt::AlignLeft) {
        auto* l = new QLabel(t);
        l->setAlignment(a);
        header_labels_.append(l);
        hl->addWidget(l, s);
    };
    make_hdr_lbl(tr("SYM"), 1);
    make_hdr_lbl(tr("SHARES"), 1, Qt::AlignRight);
    make_hdr_lbl(tr("PRICE"), 1, Qt::AlignRight);
    make_hdr_lbl(tr("VALUE"), 1, Qt::AlignRight);
    make_hdr_lbl(tr("P&L"), 1, Qt::AlignRight);
    vl->addWidget(header_row_);

    // Scrollable holdings list
    scroll_area_ = new QScrollArea;
    scroll_area_->setWidgetResizable(true);

    list_widget_ = new QWidget(this);
    list_widget_->setObjectName("psList");
    list_layout_ = new QVBoxLayout(list_widget_);
    list_layout_->setContentsMargins(0, 0, 0, 0);
    list_layout_->setSpacing(0);
    list_layout_->addStretch();

    scroll_area_->setWidget(list_widget_);
    vl->addWidget(scroll_area_, 1);

    // Enable the BaseWidget gear icon — make_config_dialog() handles the rest.
    set_configurable(true);

    // Manual refresh button on the title bar → reload holdings.
    connect(this, &BaseWidget::refresh_requested, this, [this] { load_holdings(); });

    // ── Cross-tab sync ─────────────────────────────────────────────────────
    // When the Portfolio screen mutates data (add/sell asset, create/delete
    // portfolio), the user expects this tile to update without a manual
    // refresh. PortfolioService is the single source of truth; we listen to
    // its signals rather than polling.
    auto& svc = services::PortfolioService::instance();
    connect(&svc, &services::PortfolioService::asset_added, this, [this](const QString& pid) {
        if (pid == selected_portfolio_id_)
            load_holdings();
    });
    connect(&svc, &services::PortfolioService::asset_sold, this, [this](const QString& pid) {
        if (pid == selected_portfolio_id_)
            load_holdings();
    });
    connect(&svc, &services::PortfolioService::portfolio_created, this, [this](const portfolio::Portfolio&) {
        refresh_portfolio_cache();
        // If we had no selection yet, pick the first one now.
        if (selected_portfolio_id_.isEmpty())
            load_holdings();
    });
    connect(&svc, &services::PortfolioService::portfolio_deleted, this, [this](const QString& deleted_id) {
        refresh_portfolio_cache();
        if (deleted_id == selected_portfolio_id_) {
            selected_portfolio_id_.clear();
            selected_portfolio_name_.clear();
            emit config_changed(config());
            load_holdings();
        }
    });

    apply_styles();
    set_loading(true);
}

QJsonObject PortfolioSummaryWidget::config() const {
    QJsonObject cfg;
    if (!selected_portfolio_id_.isEmpty())
        cfg.insert(kConfigKeyPortfolioId, selected_portfolio_id_);
    return cfg;
}

void PortfolioSummaryWidget::apply_config(const QJsonObject& cfg) {
    const QString pid = cfg.value(kConfigKeyPortfolioId).toString();
    if (pid == selected_portfolio_id_)
        return;
    selected_portfolio_id_ = pid;
    selected_portfolio_name_.clear();
    if (isVisible())
        load_holdings();
}

void PortfolioSummaryWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        load_holdings();
}

void PortfolioSummaryWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void PortfolioSummaryWidget::apply_styles() {
    summary_card_->setStyleSheet(QString("background: %1; border-radius: 2px;").arg(ui::colors::BG_RAISED()));
    for (auto* lbl : metric_labels_)
        lbl->setStyleSheet(
            QString("color: %1; font-size: 9px; background: transparent;").arg(ui::colors::TEXT_TERTIARY()));
    for (auto* val : metric_values_)
        val->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: bold; background: transparent;")
                               .arg(ui::colors::TEXT_PRIMARY()));
    header_row_->setStyleSheet(QString("background: %1;").arg(ui::colors::BG_RAISED()));
    for (auto* lbl : header_labels_)
        lbl->setStyleSheet(QString("color: %1; font-size: 9px; font-weight: bold; background: transparent;")
                               .arg(ui::colors::TEXT_TERTIARY()));
    if (portfolio_name_lbl_)
        portfolio_name_lbl_->setStyleSheet(
            QString("color: %1; font-size: 10px; font-weight: 600; background: transparent; padding: 0 2px;")
                .arg(ui::colors::TEXT_SECONDARY()));
    scroll_area_->setStyleSheet(
        QString("QScrollArea { border: none; background: transparent; }"
                "QScrollBar:vertical { width: 4px; background: transparent; }"
                "QScrollBar::handle:vertical { background: %1; border-radius: 2px; min-height: 20px; }"
                "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }")
            .arg(ui::colors::BORDER_MED()));

    // Holdings-row chrome, hoisted (P7). render() draws 5 cells per holding
    // and used to call setStyleSheet on every one of them on every re-render.
    if (list_widget_)
        list_widget_->setStyleSheet(QString("#psList{background:transparent;}"
                                            "#psList QWidget#psRow{background:transparent;}"
                                            "#psList QWidget#psRowAlt{background:%1;}"
                                            "#psList QLabel{font-size:10px;background:transparent;}"
                                            "#psList QLabel#psSym{color:%2;}"
                                            "#psList QLabel#psShares{color:%3;}"
                                            "#psList QLabel#psNum{color:%2;}"
                                            "#psList QLabel#psPnlPos{color:%4;}"
                                            "#psList QLabel#psPnlNeg{color:%5;}"
                                            "#psList QLabel#psEmpty{color:%6;font-size:11px;padding:20px;}")
                                        .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(),
                                             ui::colors::TEXT_SECONDARY(), ui::colors::POSITIVE(),
                                             ui::colors::NEGATIVE(), ui::colors::TEXT_TERTIARY()));
}

void PortfolioSummaryWidget::on_theme_changed() {
    apply_styles();
    if (!last_holdings_.isEmpty())
        rebuild_from_cache();
}

void PortfolioSummaryWidget::refresh_portfolio_cache() {
    auto r = fincept::PortfolioRepository::instance().list_portfolios();
    if (r.is_ok())
        portfolio_cache_ = r.value();
    else
        portfolio_cache_.clear();
}

void PortfolioSummaryWidget::load_holdings() {
    refresh_portfolio_cache();

    if (portfolio_cache_.isEmpty()) {
        render_empty(tr("No portfolios yet.\nCreate one from the Portfolio tab."));
        return;
    }

    // Resolve selection. If the configured portfolio no longer exists
    // (deleted from another tab), fall through to the first available.
    const portfolio::Portfolio* picked = nullptr;
    if (!selected_portfolio_id_.isEmpty()) {
        for (const auto& p : portfolio_cache_) {
            if (p.id == selected_portfolio_id_) {
                picked = &p;
                break;
            }
        }
    }
    if (!picked) {
        picked = &portfolio_cache_.first();
        selected_portfolio_id_ = picked->id;
        emit config_changed(config());
    }
    selected_portfolio_name_ = picked->name;
    // This tile shows the portfolio's OWN currency (intrinsic) — not the global
    // preferred currency. Values are not converted; we only use the right symbol.
    selected_portfolio_currency_ = picked->currency.isEmpty() ? QStringLiteral("USD") : picked->currency;
    portfolio_name_lbl_->setText(picked->name.toUpper());
    portfolio_name_lbl_->setVisible(true);

    auto assets_r = fincept::PortfolioRepository::instance().get_assets(selected_portfolio_id_);
    QVector<Holding> holdings;
    if (assets_r.is_ok()) {
        for (const auto& a : assets_r.value()) {
            if (a.symbol.isEmpty() || a.quantity <= 0)
                continue;
            Holding h;
            h.symbol = a.symbol;
            h.shares = a.quantity;
            h.avg_cost = a.avg_buy_price;
            holdings.append(h);
        }
    }

    if (holdings.isEmpty()) {
        render_empty(tr("'%1' has no holdings.\nAdd positions from the Portfolio tab.").arg(picked->name));
        return;
    }

    last_holdings_ = holdings;
    hub_resubscribe(holdings);
}

void PortfolioSummaryWidget::render_empty(const QString& message) {
    set_loading(false);
    set_footer(QString());
    last_holdings_.clear();
    hub_unsubscribe_all();

    // Wipe rows + zero the summary counters so old numbers don't linger
    // after a portfolio switch / deletion.
    while (list_layout_->count() > 0) {
        auto* item = list_layout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    auto* empty = new QLabel(message, list_widget_);
    empty->setObjectName("psEmpty");
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    list_layout_->addWidget(empty);
    list_layout_->addStretch();
    const QString sym = cur::symbol_for(selected_portfolio_currency_);
    total_value_lbl_->setText(sym + QStringLiteral("0"));
    day_pnl_lbl_->setText(sym + QStringLiteral("0"));
    total_pnl_lbl_->setText(sym + QStringLiteral("0"));
    num_holdings_lbl_->setText(QStringLiteral("0"));

    // Reset the colour overrides applied during a populated render so the
    // zeroed-out P&L labels don't get stuck red/green from the prior state.
    const QString default_value_css = QString("color: %1; font-size: 13px; font-weight: bold; background: transparent;")
                                          .arg(ui::colors::TEXT_PRIMARY());
    day_pnl_lbl_->setStyleSheet(default_value_css);
    total_pnl_lbl_->setStyleSheet(default_value_css);

    if (!selected_portfolio_name_.isEmpty()) {
        portfolio_name_lbl_->setText(selected_portfolio_name_.toUpper());
        portfolio_name_lbl_->setVisible(true);
    } else {
        portfolio_name_lbl_->setVisible(false);
    }
}

void PortfolioSummaryWidget::hub_resubscribe(const QVector<Holding>& holdings) {
    // Holdings set may have changed since last subscribe — wipe all and
    // re-register so we don't leave stale topic subs behind.
    QuoteSubscriptions::unsubscribe(this);
    feed_.clear();
    fx_symbols_.clear();
    holding_symbols_.clear();
    for (const auto& h : holdings)
        holding_symbols_ << h.symbol;
    // One holdings-table rebuild per delivery burst, not one per symbol.
    feed_.subscribe(this, holding_symbols_, [this]() { schedule_render([this]() { rebuild_from_cache(); }); });
    hub_active_ = true;
    // Each holding's quote currency decides whether (and which) FX rate it needs.
    QPointer<PortfolioSummaryWidget> self = this;
    services::MarketDataService::instance().resolve_names(holding_symbols_, [self](const QHash<QString, QString>&) {
        if (self)
            self->ensure_fx_subscriptions();
    });
}

void PortfolioSummaryWidget::hub_unsubscribe_all() {
    QuoteSubscriptions::unsubscribe(this);
    hub_active_ = false;
}

QString PortfolioSummaryWidget::holding_currency(const QString& symbol) const {
    QString code = services::MarketDataService::instance().currency_code(symbol);
    // A forex pair has no quote currency in the name cache (its price is a
    // rate); the quote side of "EURUSD=X" is USD.
    if (code.isEmpty() && symbol.endsWith(QLatin1String("=X")) && symbol.size() >= 8)
        code = symbol.mid(3, 3);
    return code;
}

void PortfolioSummaryWidget::ensure_fx_subscriptions() {
    const QString pc = selected_portfolio_currency_.trimmed().toUpper();
    QStringList wanted;
    for (const auto& sym : holding_symbols_) {
        const QString code = holding_currency(sym);
        if (code.isEmpty())
            continue;
        const QString major = valuation::normalize_currency(code).code;
        if (major == pc)
            continue;
        const QString fx = valuation::fx_symbol(major, pc);
        if (!fx_symbols_.contains(fx) && !wanted.contains(fx))
            wanted << fx;
    }
    if (!wanted.isEmpty()) {
        fx_symbols_ << wanted;
        feed_.subscribe(this, wanted, [this]() { schedule_render([this]() { rebuild_from_cache(); }); });
    }
    schedule_render([this]() { rebuild_from_cache(); });
}

void PortfolioSummaryWidget::rebuild_from_cache() {
    if (last_holdings_.isEmpty())
        return;
    bool any = false;
    for (const auto& h : last_holdings_)
        any = any || feed_.quote(h.symbol) || !feed_.error(h.symbol).isEmpty();
    if (any) {
        set_loading(false);
        render();
    }
}

void PortfolioSummaryWidget::render() {
    const QVector<Holding>& holdings = last_holdings_;
    auto& market = services::MarketDataService::instance();
    // Totals are in the portfolio's own currency: every holding is converted
    // from its quote currency first (PortfolioValuation.h).
    const QString pc = selected_portfolio_currency_.trimmed().toUpper();
    const QString sym = cur::symbol_for(pc);

    double total_value = 0;
    double total_cost = 0;
    double day_pnl = 0;
    int change_readings = 0;
    int priced_count = 0;
    int converted_count = 0;
    QStringList left_out;

    // Clear list
    while (list_layout_->count() > 0) {
        auto* item = list_layout_->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    auto money = [&sym](double v, bool sign) {
        const QString body = sym + QLocale(QLocale::English).toString(std::abs(v), 'f', 0);
        if (!sign)
            return (v < 0 ? QStringLiteral("-") : QString()) + body;
        return v > 0 ? QStringLiteral("+") + body : v < 0 ? QStringLiteral("-") + body : sym + QStringLiteral("0");
    };

    bool alt = false;
    for (const auto& h : holdings) {
        const services::QuoteData* q = feed_.quote(h.symbol);
        // A value kept after a failed refresh is not a current price: it falls
        // into the same average-cost fallback as a missing quote.
        const bool current = q && q->has_price && feed_.error(h.symbol).isEmpty();
        if (current)
            ++priced_count;

        valuation::HoldingInput in;
        in.shares = h.shares;
        in.avg_cost = h.avg_cost;
        in.has_price = current;
        in.price = current ? q->price : 0.0;
        in.has_change = current && q->has_change;
        in.change = in.has_change ? q->change : 0.0;
        in.quote_currency = holding_currency(h.symbol);
        std::optional<double> rate;
        if (!in.quote_currency.isEmpty()) {
            const QString major = valuation::normalize_currency(in.quote_currency).code;
            if (major == pc) {
                rate = 1.0;
            } else if (const auto* fx = feed_.quote(valuation::fx_symbol(major, pc)); fx && fx->has_price) {
                rate = fx->price;
            }
        }
        const valuation::HoldingValue hv = valuation::value_holding(in, pc, rate);
        if (hv.converted) {
            ++converted_count;
            total_value += hv.value;
            total_cost += hv.cost;
            if (hv.day) {
                day_pnl += *hv.day;
                ++change_readings;
            }
        } else {
            left_out << QStringLiteral("%1 (%2)").arg(h.symbol, hv.why_not);
        }

        // No setStyleSheet in this loop — colours come from the single
        // stylesheet on list_widget_ (see apply_styles) via these object names.
        auto* row = new QWidget(list_widget_);
        row->setObjectName(alt ? QStringLiteral("psRowAlt") : QStringLiteral("psRow"));
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(8, 4, 8, 4);

        auto cell = [&](const QString& text, Qt::Alignment align, const QString& object_name) {
            auto* lbl = new QLabel(text);
            lbl->setObjectName(object_name);
            lbl->setAlignment(align);
            rl->addWidget(lbl, 1);
        };

        const int decimals = fincept::screens::quote_price_decimals(current, in.price, 2);
        cell(h.symbol, Qt::AlignLeft, QStringLiteral("psSym"));
        cell(valuation::shares_text(h.shares), Qt::AlignRight, QStringLiteral("psShares"));
        // The price stays in the holding's own currency; value and P&L are in
        // the portfolio's currency.
        cell(current ? market.currency_prefix(h.symbol) + QString::number(in.price, 'f', decimals)
                     : QStringLiteral("--"),
             Qt::AlignRight, QStringLiteral("psNum"));
        cell(current && hv.converted ? money(hv.value, false) : QStringLiteral("--"), Qt::AlignRight,
             QStringLiteral("psNum"));
        if (!current || !hv.converted) {
            cell(QStringLiteral("--"), Qt::AlignRight, QStringLiteral("psNum"));
        } else {
            const double pnl = hv.value - hv.cost;
            cell(money(pnl, true), Qt::AlignRight,
                 pnl > 0   ? QStringLiteral("psPnlPos")
                 : pnl < 0 ? QStringLiteral("psPnlNeg")
                           : QStringLiteral("psNum"));
        }
        row->setToolTip(hv.converted ? (q ? fincept::screens::quote_status_line(h.symbol, q, feed_.error(h.symbol))
                                          : tr("%1 — no current quote; valued at average cost").arg(h.symbol))
                                     : tr("%1 — left out of the totals: %2").arg(h.symbol, hv.why_not));

        list_layout_->addWidget(row);
        alt = !alt;
    }
    list_layout_->addStretch();

    // Coverage: unpriced holdings are valued at average cost, holdings whose
    // currency or FX rate is not known are left out, and the day total sums
    // only holdings with a change reading — each shown as partial.
    const int n = static_cast<int>(holdings.size());
    const bool value_partial = priced_count < n || converted_count < n;
    const bool day_partial = change_readings > 0 && change_readings < n;

    total_value_lbl_->setText(money(total_value, false) + (value_partial ? tr(" (partial)") : QString()));
    QStringList why;
    if (priced_count < n)
        why << tr("Holdings without a current quote are valued at average cost.");
    if (!left_out.isEmpty())
        why << tr("Not in the totals: %1.").arg(left_out.join(QStringLiteral(", ")));
    total_value_lbl_->setToolTip(why.join(QLatin1Char(' ')));
    num_holdings_lbl_->setText(QString::number(n));

    const double total_pnl = total_value - total_cost;
    const QString day_str = change_readings == 0 ? QStringLiteral("--")
                                                 : money(day_pnl, true) + (day_partial ? tr(" (partial)") : QString());
    const QString tot_str = money(total_pnl, true) + (value_partial ? tr(" (partial)") : QString());

    day_pnl_lbl_->setText(day_str);
    day_pnl_lbl_->setToolTip(
        day_partial ? tr("Only %1 of %2 holdings carried a day-change reading.").arg(change_readings).arg(n)
                    : tr("Sum of each holding's change since its previous close (or 00:00 Helsinki for forex, "
                         "previous settlement for futures), converted to %1.")
                          .arg(pc));
    day_pnl_lbl_->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: bold; background: transparent;")
                                    .arg(change_readings == 0 ? ui::colors::TEXT_DIM()
                                         : day_pnl > 0        ? ui::colors::POSITIVE()
                                         : day_pnl < 0        ? ui::colors::NEGATIVE()
                                                              : ui::colors::TEXT_PRIMARY()));

    total_pnl_lbl_->setText(tot_str);
    total_pnl_lbl_->setToolTip(total_value_lbl_->toolTip());
    total_pnl_lbl_->setStyleSheet(QString("color: %1; font-size: 13px; font-weight: bold; background: transparent;")
                                      .arg(total_pnl > 0   ? ui::colors::POSITIVE()
                                           : total_pnl < 0 ? ui::colors::NEGATIVE()
                                                           : ui::colors::TEXT_PRIMARY()));

    QStringList feed_symbols = holding_symbols_;
    feed_symbols << fx_symbols_;
    auto summary = feed_.summary(feed_symbols);
    if (!left_out.isEmpty()) {
        summary.text +=
            (summary.text.isEmpty() ? QString() : QStringLiteral(" · ")) + tr("%1 not in totals").arg(left_out.size());
        summary.warn = true;
    }
    set_footer(summary.text, summary.tooltip, summary.warn);
}

QDialog* PortfolioSummaryWidget::make_config_dialog(QWidget* parent) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle(tr("Configure — Portfolio Summary"));
    auto* form = new QFormLayout(dlg);

    // Ensure the cache reflects the current state of the DB (the Portfolio
    // tab may have added/renamed entries since the widget was constructed).
    refresh_portfolio_cache();

    auto* combo = new QComboBox(dlg);
    if (portfolio_cache_.isEmpty()) {
        combo->addItem(tr("(no portfolios — create one in the Portfolio tab)"), QString());
        combo->setEnabled(false);
    } else {
        for (const auto& p : portfolio_cache_) {
            const QString label =
                p.name + (p.currency.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(p.currency));
            combo->addItem(label, p.id);
            if (p.id == selected_portfolio_id_)
                combo->setCurrentIndex(combo->count() - 1);
        }
    }
    form->addRow(tr("Portfolio"), combo);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, dlg, [this, dlg, combo]() {
        const QString picked = combo->currentData().toString();
        if (!picked.isEmpty() && picked != selected_portfolio_id_) {
            QJsonObject cfg;
            cfg.insert(kConfigKeyPortfolioId, picked);
            apply_config(cfg);
            emit config_changed(cfg);
        }
        dlg->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    return dlg;
}

void PortfolioSummaryWidget::retranslateUi() {
    BaseWidget::retranslateUi();
    set_title(tr("PORTFOLIO SUMMARY"));
    rebuild_from_cache(); // re-renders header/metric labels in the new language
}

} // namespace fincept::screens::widgets
