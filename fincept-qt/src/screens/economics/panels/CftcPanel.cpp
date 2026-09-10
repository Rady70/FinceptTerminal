// src/screens/economics/panels/CftcPanel.cpp
// CFTC (Commodity Futures Trading Commission) — Commitments of Traders.
// No API key required. Covers 40+ futures markets.
// Commands: cot_data, market_sentiment, cot_historical_trend
// cot_data response: { "success": true, "data": [{flat 40+ fields}], "parameters": {...} }
// sentiment response: { "success": true, "data": { "commercial_positions": {...},
//                       "non_commercial_positions": {...}, "overall_sentiment": {...} } }
// trend response: { "success": true, "data": [{ "date", "open_interest",
//                   "commercial_net", "non_commercial_net", ... }] }
#include "screens/economics/panels/CftcPanel.h"

#include "core/logging/Logger.h"
#include "services/economics/EconomicsService.h"
#include "ui/theme/Theme.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>

namespace fincept::screens {
namespace {

static constexpr const char* kCftcScript = "cftc_data.py";
static constexpr const char* kCftcSourceId = "cftc";
static constexpr const char* kCftcColor = "#FF5722"; // deep orange
} // namespace

static const QList<QPair<QString, QString>> kMarkets = {
    // Metals
    {"Gold", "gold"},
    {"Silver", "silver"},
    {"Copper", "copper"},
    {"Platinum", "platinum"},
    {"Palladium", "palladium"},
    // Energy
    {"Crude Oil (WTI)", "crude_oil"},
    {"Natural Gas", "natural_gas"},
    {"Gasoline", "gasoline"},
    {"Heating Oil", "heating_oil"},
    // Agricultural
    {"Corn", "corn"},
    {"Wheat", "wheat"},
    {"Soybeans", "soybeans"},
    {"Cotton", "cotton"},
    {"Coffee", "coffee"},
    {"Sugar", "sugar"},
    {"Cocoa", "cocoa"},
    {"Live Cattle", "live_cattle"},
    {"Lean Hogs", "lean_hogs"},
    // FX
    {"Euro (EUR/USD)", "euro"},
    {"Japanese Yen", "jpy"},
    {"British Pound", "british_pound"},
    {"Swiss Franc", "swiss_franc"},
    {"Canadian Dollar", "canadian_dollar"},
    {"Australian Dollar", "australian_dollar"},
    // Indices
    {"S&P 500", "s&p_500"},
    {"Nasdaq 100", "nasdaq_100"},
    {"Dow Jones", "dow_jones"},
    {"Nikkei 225", "nikkei"},
    {"VIX", "vix"},
    // Rates
    {"T-Bonds (30Y)", "treasury_bonds"},
    {"T-Notes (10Y)", "treasury_notes_10y"},
    {"T-Notes (5Y)", "treasury_notes_5y"},
    {"T-Notes (2Y)", "treasury_notes_2y"},
    {"Fed Funds", "fed_funds"},
    // Crypto
    {"Bitcoin", "bitcoin"},
    {"Ethereum", "ether"},
    // FX
    {"US Dollar Index", "us_dollar_index"},
};

static const QList<QPair<QString, QString>> kReportTypes = {
    {"Legacy", "legacy"},
    {"Disaggregated", "disaggregated"},
    {"Financial (TFF)", "financial"},
};

static const QList<QPair<QString, QString>> kViews = {
    {"COT Table", "cot"},
    {"Market Sentiment", "sentiment"},
    {"Historical Trend", "trend"},
};

// ── Constructor ───────────────────────────────────────────────────────────────

CftcPanel::CftcPanel(QWidget* parent) : EconPanelBase(kCftcSourceId, kCftcColor, parent) {
    build_base_ui(this);
    build_sentiment_widget();
    // Localize fixed combo item text (report types / views) to the active language.
    retranslateUi();
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &CftcPanel::on_result);
}

void CftcPanel::activate() {
    show_empty(tr("Select a futures market and view, then click FETCH\n"
                  "CFTC data is free — no API key required"));
}

// ── Controls ──────────────────────────────────────────────────────────────────

void CftcPanel::build_controls(QHBoxLayout* thl) {
    auto make_lbl = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet(ctrl_label_style());
        return l;
    };

    market_combo_ = new QComboBox;
    for (const auto& m : kMarkets)
        market_combo_->addItem(m.first, m.second);
    market_combo_->setFixedHeight(26);
    market_combo_->setMinimumWidth(160);

    // Report types and views are fixed UI labels — text set in retranslateUi();
    // only the code is meaningful as item data here.
    report_type_combo_ = new QComboBox;
    for (const auto& r : kReportTypes)
        report_type_combo_->addItem(r.first, r.second);
    report_type_combo_->setFixedHeight(26);
    report_type_combo_->setMinimumWidth(130);

    view_combo_ = new QComboBox;
    for (const auto& v : kViews)
        view_combo_->addItem(v.first, v.second);
    view_combo_->setFixedHeight(26);
    view_combo_->setMinimumWidth(145);

    thl->addWidget(market_lbl_ = make_lbl(tr("MARKET")));
    thl->addWidget(market_combo_);
    thl->addWidget(report_lbl_ = make_lbl(tr("REPORT")));
    thl->addWidget(report_type_combo_);
    thl->addWidget(view_lbl_ = make_lbl(tr("VIEW")));
    thl->addWidget(view_combo_);
}

// ── Sentiment widget ──────────────────────────────────────────────────────────

void CftcPanel::build_sentiment_widget() {
    using namespace ui::colors;

    const auto card_bg = QString("background:%1; border:1px solid %2;").arg(BG_RAISED(), BORDER_DIM());
    const auto lbl_ss = QString("color:%1; font-size:9px; font-weight:700;"
                                " letter-spacing:1px; background:transparent;")
                            .arg(TEXT_SECONDARY());

    sentiment_widget_ = new QWidget(this);
    sentiment_widget_->setStyleSheet(QString("background:%1;").arg(BG_BASE()));
    auto* root = new QVBoxLayout(sentiment_widget_);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(12);

    // Header
    auto* hdr = new QWidget(this);
    hdr->setStyleSheet(card_bg);
    auto* hdr_hl = new QHBoxLayout(hdr);
    hdr_hl->setContentsMargins(14, 10, 14, 10);

    sent_title_lbl_ = new QLabel(tr("COT MARKET SENTIMENT"));
    sent_title_lbl_->setStyleSheet(
        QString("color:%1; font-size:12px; font-weight:700; background:transparent;").arg(TEXT_PRIMARY()));

    sent_market_lbl_ = new QLabel("—");
    sent_market_lbl_->setStyleSheet(
        QString("color:%1; font-size:11px; font-weight:700; background:transparent;").arg(color_));
    sent_date_lbl_ = new QLabel("—");
    sent_date_lbl_->setStyleSheet(QString("color:%1; font-size:10px; background:transparent;").arg(TEXT_TERTIARY()));

    hdr_hl->addWidget(sent_title_lbl_);
    hdr_hl->addStretch(1);
    hdr_hl->addWidget(sent_market_lbl_);
    hdr_hl->addWidget(sent_date_lbl_);
    root->addWidget(hdr);

    // Bias cards
    auto* cards = new QWidget(this);
    auto* cards_hl = new QHBoxLayout(cards);
    cards_hl->setContentsMargins(0, 0, 0, 0);
    cards_hl->setSpacing(10);

    auto make_card = [&](const QString& title_text, QLabel*& bias_out, QLabel*& net_out, const QString& desc,
                         QLabel*& title_out, QLabel*& desc_out) {
        auto* card = new QWidget(this);
        card->setStyleSheet(card_bg);
        card->setMinimumHeight(120);
        auto* cvl = new QVBoxLayout(card);
        cvl->setContentsMargins(14, 12, 14, 12);
        cvl->setSpacing(4);

        auto* ttl = new QLabel(title_text);
        ttl->setStyleSheet(lbl_ss);
        title_out = ttl;

        bias_out = new QLabel("—");
        bias_out->setObjectName("econStatVal");

        net_out = new QLabel(tr("Net: —"));
        net_out->setStyleSheet(QString("color:%1; font-size:10px; background:transparent;").arg(TEXT_TERTIARY()));

        auto* d = new QLabel(desc);
        d->setStyleSheet(QString("color:%1; font-size:9px; background:transparent;").arg(TEXT_DIM()));
        d->setWordWrap(true);
        desc_out = d;

        cvl->addWidget(ttl);
        cvl->addWidget(bias_out);
        cvl->addWidget(net_out);
        cvl->addStretch(1);
        cvl->addWidget(d);
        cards_hl->addWidget(card, 1);
    };

    make_card(tr("COMMERCIAL TRADERS"), sent_comm_bias_, sent_comm_net_,
              tr("Hedgers & producers — usually contrarian signal"), sent_comm_card_lbl_, sent_comm_card_desc_);
    make_card(tr("NON-COMMERCIAL (SPECULATORS)"), sent_noncomm_bias_, sent_noncomm_net_,
              tr("Managed money & funds — trend-following signal"), sent_noncomm_card_lbl_, sent_noncomm_card_desc_);

    // OI & trend row
    auto* oi_row = new QWidget(this);
    oi_row->setStyleSheet(card_bg);
    auto* oi_hl = new QHBoxLayout(oi_row);
    oi_hl->setContentsMargins(14, 8, 14, 8);
    oi_hl->setSpacing(20);

    sent_oi_caption_ = new QLabel(tr("OPEN INTEREST"));
    sent_oi_caption_->setStyleSheet(lbl_ss);
    sent_oi_lbl_ = new QLabel("—");
    sent_oi_lbl_->setStyleSheet(
        QString("color:%1; font-size:13px; font-weight:700; background:transparent;").arg(TEXT_PRIMARY()));

    sent_oi_trend_caption_ = new QLabel(tr("OI TREND"));
    sent_oi_trend_caption_->setStyleSheet(lbl_ss);
    sent_oi_trend_ = new QLabel("—");
    sent_oi_trend_->setObjectName("econStatVal");

    oi_hl->addWidget(sent_oi_caption_);
    oi_hl->addWidget(sent_oi_lbl_);
    oi_hl->addWidget(sent_oi_trend_caption_);
    oi_hl->addWidget(sent_oi_trend_);
    oi_hl->addStretch(1);

    root->addLayout(cards_hl);
    root->addWidget(oi_row);
    root->addStretch(1);

    // Register the sentiment card view as a real page of the shared content
    // stack. Previously this whole widget tree was built, parented to the panel
    // and then never added to any layout — it floated at (0,0) over the toolbar
    // and the Market Sentiment view silently degraded to a one-row table.
    sentiment_page_ = add_content_page(sentiment_widget_);
}

// ── Fetch ─────────────────────────────────────────────────────────────────────

void CftcPanel::on_fetch() {
    const QString market = market_combo_->currentData().toString();
    const QString report_type = report_type_combo_->currentData().toString();
    const QString view = view_combo_->currentData().toString();

    if (market.isEmpty()) {
        show_empty(tr("Select a futures market"));
        return;
    }

    show_loading(tr("Fetching CFTC data for %1…").arg(market_combo_->currentText()));

    if (view == "cot") {
        services::EconomicsService::instance().execute(kCftcSourceId, kCftcScript, "cot_data", {market, report_type},
                                                       "cftc_cot_" + market);
    } else if (view == "sentiment") {
        services::EconomicsService::instance().execute(kCftcSourceId, kCftcScript, "market_sentiment",
                                                       {market, report_type}, "cftc_sent_" + market);
    } else {
        services::EconomicsService::instance().execute(kCftcSourceId, kCftcScript, "cot_historical_trend",
                                                       {market, report_type}, "cftc_trend_" + market);
    }
}

// ── Result ────────────────────────────────────────────────────────────────────

void CftcPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != kCftcSourceId)
        return;

    if (!result.success) {
        show_error(result.error);
        return;
    }

    // CFTC scripts report failures as {"error": {...}} (or a plain string).
    // EconomicsService does not flatten object error envelopes, so surface the
    // nested message here rather than degrading to a generic empty-data state.
    if (result.data.contains("error")) {
        const QJsonValue err = result.data["error"];
        const QString message = err.isString() ? err.toString() : err.toObject()["error"].toString();
        show_error(message.isEmpty() ? tr("CFTC request failed") : message);
        return;
    }

    // Table/trend views are time series — restore the aggregate stat row that
    // the sentiment view hides.
    if (!request_id.startsWith("cftc_sent_"))
        set_stats_visible(true);

    // Sentiment view: data is an object, not an array
    if (request_id.startsWith("cftc_sent_")) {
        const QJsonObject sentiment = result.data["data"].toObject();
        if (sentiment.isEmpty()) {
            show_error(tr("No sentiment data returned"));
            return;
        }
        show_sentiment(sentiment);
        return;
    }

    // COT table or historical trend — data is an array
    // cot_data rows: flat dict with report_date/market/open_interest_all and the
    // report family's participant fields (see the mapping below).
    // trend rows: { "date", "open_interest", "commercial_net", ... }
    QJsonArray rows = result.data["data"].toArray();

    if (rows.isEmpty()) {
        const QString err_str =
            result.data.contains("error") ? result.data["error"].toObject()["error"].toString() : "";
        show_empty(err_str.isEmpty() ? tr("No data returned — try a different market or report type") : err_str);
        return;
    }

    // For COT table, keep only the most useful columns for display. Which
    // participant columns exist depends on the report family: legacy carries
    // commercial/non-commercial, disaggregated carries producer-merchant /
    // managed-money, and the financial (TFF) family carries dealer /
    // asset-manager / leveraged-money. Missing cells stay null and render as
    // "—" (never as a fabricated 0).
    if (request_id.startsWith("cftc_cot_")) {
        // Read the family from the result payload (authoritative for the row
        // set that actually arrived) and fall back to the combo only for a
        // cached result written before parameters.report_family existed. Using
        // the live combo alone could mis-map an in-flight fetch when the user
        // changes the report type while it is running.
        const QJsonObject params = result.data.value("parameters").toObject();
        QString family = params.value("report_family").toString();
        if (family.isEmpty())
            family = report_type_combo_->currentData().toString();
        const QString report_label = params.value("report_type").toString(report_type_combo_->currentText());
        QJsonArray clean;
        for (const auto& v : rows) {
            const auto obj = v.toObject();
            auto pick = [&obj](const QStringList& keys) -> QJsonValue {
                for (const QString& key : keys) {
                    const QJsonValue value = obj.value(key);
                    if (!value.isUndefined() && !value.isNull())
                        return value;
                }
                return {};
            };

            QJsonObject row;
            row["date"] = pick({"report_date_as_yyyy_mm_dd"});
            row["market"] = pick({"market_and_exchange_names"});
            row["open_interest"] = pick({"open_interest_all"});

            if (family == "disaggregated") {
                row["prod_merc_long"] = pick({"prod_merc_positions_long", "prod_merc_positions_long_all"});
                row["prod_merc_short"] = pick({"prod_merc_positions_short", "prod_merc_positions_short_all"});
                row["m_money_long"] = pick({"m_money_positions_long_all", "m_money_positions_long"});
                row["m_money_short"] = pick({"m_money_positions_short_all", "m_money_positions_short"});
            } else if (family == "tff" || family == "financial") {
                row["dealer_long"] = pick({"dealer_positions_long_all", "dealer_positions_long"});
                row["dealer_short"] = pick({"dealer_positions_short_all", "dealer_positions_short"});
                row["asset_mgr_long"] = pick({"asset_mgr_positions_long", "asset_mgr_positions_long_all"});
                row["asset_mgr_short"] = pick({"asset_mgr_positions_short", "asset_mgr_positions_short_all"});
                row["lev_money_long"] = pick({"lev_money_positions_long", "lev_money_positions_long_all"});
                row["lev_money_short"] = pick({"lev_money_positions_short", "lev_money_positions_short_all"});
                row["other_rept_long"] = pick({"other_rept_positions_long", "other_rept_positions_long_all"});
                row["other_rept_short"] = pick({"other_rept_positions_short", "other_rept_positions_short_all"});
            } else {
                row["comm_long"] = pick({"comm_positions_long_all", "comm_long_all"});
                row["comm_short"] = pick({"comm_positions_short_all", "comm_short_all"});
                row["noncomm_long"] = pick({"noncomm_positions_long_all", "noncomm_long_all"});
                row["noncomm_short"] = pick({"noncomm_positions_short_all", "noncomm_short_all"});
            }
            clean.append(row);
        }
        display(clean, "CFTC COT: " + market_combo_->currentText() + " (" + report_label + ")");
    } else {
        // Historical trend — rows already have clean keys
        display(rows, "CFTC Historical Trend: " + market_combo_->currentText());
    }

    LOG_INFO("CftcPanel", QString("Displayed %1 rows for %2").arg(rows.size()).arg(request_id));
}

// ── Sentiment display ─────────────────────────────────────────────────────────

void CftcPanel::show_sentiment(const QJsonObject& s) {
    if (!sentiment_widget_)
        return;

    // Populate labels
    sent_market_lbl_->setText(s["market_name"].toString(market_combo_->currentText()));
    sent_date_lbl_->setText(tr("Report: %1").arg(s["latest_report"].toString("—")));

    const QJsonValue oi_value = s.value("open_interest");
    const double oi = oi_value.isDouble() ? oi_value.toDouble() : 0.0;
    sent_oi_lbl_->setText(oi_value.isDouble() ? QString::number(static_cast<qint64>(oi)) : "—");

    const QString oi_trend = s["overall_sentiment"].toObject()["oi_trend"].toString();
    sent_oi_trend_->setText(oi_trend.isEmpty() ? "—" : oi_trend.toUpper());
    sent_oi_trend_->setObjectName(oi_trend == "increasing" ? "econStatPos" : "econStatNeg");
    sent_oi_trend_->style()->unpolish(sent_oi_trend_);
    sent_oi_trend_->style()->polish(sent_oi_trend_);

    const QString comm_bias = s["overall_sentiment"].toObject()["commercial_bias"].toString();
    sent_comm_bias_->setText(comm_bias.isEmpty() ? "—" : comm_bias.toUpper());
    sent_comm_bias_->setObjectName(comm_bias == "bullish" ? "econStatPos" : "econStatNeg");
    sent_comm_bias_->style()->unpolish(sent_comm_bias_);
    sent_comm_bias_->style()->polish(sent_comm_bias_);

    const QString noncomm_bias = s["overall_sentiment"].toObject()["non_commercial_bias"].toString();
    sent_noncomm_bias_->setText(noncomm_bias.isEmpty() ? "—" : noncomm_bias.toUpper());
    sent_noncomm_bias_->setObjectName(noncomm_bias == "bullish" ? "econStatPos" : "econStatNeg");
    sent_noncomm_bias_->style()->unpolish(sent_noncomm_bias_);
    sent_noncomm_bias_->style()->polish(sent_noncomm_bias_);

    // NB: QString::arg(qlonglong, int fieldWidth) — the previous `.arg(v, 'd')`
    // passed the *char literal* 'd' (=100) as the field width, padding every
    // net-position number with 100 leading spaces. Format the number first.
    // A net that the provider did not return stays "—", never a fabricated 0.
    const QJsonObject comm = s["commercial_positions"].toObject();
    const QJsonValue comm_net_value = comm.value("net");
    const double comm_net = comm_net_value.isDouble() ? comm_net_value.toDouble() : 0.0;
    sent_comm_net_->setText(comm_net_value.isDouble() ? tr("Net: %1%2")
                                                            .arg(comm_net >= 0 ? QStringLiteral("+") : QString())
                                                            .arg(QString::number(static_cast<qint64>(comm_net)))
                                                      : tr("Net: —"));

    const QJsonObject noncomm = s["non_commercial_positions"].toObject();
    const QJsonValue noncomm_net_value = noncomm.value("net");
    const double noncomm_net = noncomm_net_value.isDouble() ? noncomm_net_value.toDouble() : 0.0;
    sent_noncomm_net_->setText(noncomm_net_value.isDouble()
                                   ? tr("Net: %1%2")
                                         .arg(noncomm_net >= 0 ? QStringLiteral("+") : QString())
                                         .arg(QString::number(static_cast<qint64>(noncomm_net)))
                                   : tr("Net: —"));

    // Populate the shared table too (one summary row) so CSV export still has
    // something to write, then switch the stack to the sentiment card view.
    QJsonArray rows;
    QJsonObject row;
    row["market"] = s["market_name"].toString(market_combo_->currentText());
    row["report_date"] = s["latest_report"];
    row["open_interest"] = oi_value;
    row["comm_net"] = comm_net_value;
    row["noncomm_net"] = noncomm_net_value;
    row["comm_bias"] = comm_bias;
    row["noncomm_bias"] = noncomm_bias;
    row["oi_trend"] = oi_trend;
    rows.append(row);
    // A one-row snapshot has no meaningful LATEST/CHANGE/MIN/MAX/AVG.
    set_stats_visible(false);
    display(rows, "CFTC Sentiment: " + market_combo_->currentText());
    show_content_page(sentiment_page_);

    LOG_INFO("CftcPanel", "Displayed sentiment for " + market_combo_->currentText());
}

// ── i18n ──────────────────────────────────────────────────────────────────────

void CftcPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    EconPanelBase::changeEvent(event);
}

void CftcPanel::retranslateUi() {
    // Toolbar labels
    if (market_lbl_)
        market_lbl_->setText(tr("MARKET"));
    if (report_lbl_)
        report_lbl_->setText(tr("REPORT"));
    if (view_lbl_)
        view_lbl_->setText(tr("VIEW"));

    // Fixed combo items (report types / views) — markets stay as data.
    if (report_type_combo_ && report_type_combo_->count() >= 3) {
        report_type_combo_->setItemText(0, tr("Legacy"));
        report_type_combo_->setItemText(1, tr("Disaggregated"));
        report_type_combo_->setItemText(2, tr("Financial (TFF)"));
    }
    if (view_combo_ && view_combo_->count() >= 3) {
        view_combo_->setItemText(0, tr("COT Table"));
        view_combo_->setItemText(1, tr("Market Sentiment"));
        view_combo_->setItemText(2, tr("Historical Trend"));
    }

    // Sentiment widget static labels
    if (sent_title_lbl_)
        sent_title_lbl_->setText(tr("COT MARKET SENTIMENT"));
    if (sent_comm_card_lbl_)
        sent_comm_card_lbl_->setText(tr("COMMERCIAL TRADERS"));
    if (sent_comm_card_desc_)
        sent_comm_card_desc_->setText(tr("Hedgers & producers — usually contrarian signal"));
    if (sent_noncomm_card_lbl_)
        sent_noncomm_card_lbl_->setText(tr("NON-COMMERCIAL (SPECULATORS)"));
    if (sent_noncomm_card_desc_)
        sent_noncomm_card_desc_->setText(tr("Managed money & funds — trend-following signal"));
    if (sent_oi_caption_)
        sent_oi_caption_->setText(tr("OPEN INTEREST"));
    if (sent_oi_trend_caption_)
        sent_oi_trend_caption_->setText(tr("OI TREND"));

    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
