// src/screens/economics/panels/GlobalCentralBanksPanel.cpp
// Global Central Banks — BOE, RBA, Bank of Canada, Riksbank, SNB, Norges Bank.
// No API key required for any source.
//
// Response shapes (all use {success, data:[{date, <value_col>}]} or similar):
//   BOE:     { success, data:[{date:"02 Jan 1975", <CDID>:value}] }
//   RBA:     { success, table, data:[{date:"04-Jan-2011", <col>:value}] }
//   BOC:     { success, series, data:[{date:"2026-02-01", <series_id>:value}] }
//   Riksbank:{ success, series_id, data:[{date:"2025-03-28", value:2.25}] }
//   SNB:     { success, cube, data:[{date:"2026-03-16", <col>:value}] }
//   Norges:  { success, flow, data:[{date:"2026-03-17", <col>:value}] }
#include "screens/economics/panels/GlobalCentralBanksPanel.h"

#include "core/logging/Logger.h"
#include "services/economics/EconomicsService.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QDate>

namespace fincept::screens {
namespace {

static constexpr const char* kGlobalCentralBanksSourceId = "global_cb";
static constexpr const char* kGlobalCentralBanksColor = "#6366F1"; // indigo
} // namespace

// ── Per-bank descriptor ──────────────────────────────────────────────────────

struct CbSeries {
    QString label;
    QString command;
    // Default arguments that make the command work through the panel (the
    // panel has no argument widgets). @today@ / @prevbusiness@ / @start30@ / @year@ are
    // expanded at fetch time. Empty when the command needs no arguments.
    QStringList args;
};

struct CbBank {
    QString label;      // display name
    QString script;     // python script filename
    QString req_prefix; // request_id prefix
    QList<CbSeries> series;
};

static const QList<CbBank> kBanks = {
    {"BOE — Bank of England",
     "boe_data.py",
     "boe",
     {
         {"Bank Rate", "bank_rate"},
         {"SONIA Overnight Rate", "sonia"},
         {"Exchange Rates (GBP)", "exchange_rates"},
         {"Monetary Aggregates (M0/M4)", "monetary_aggregates"},
         {"Quoted Interest Rates", "quoted_rates"},
     }},
    {"RBA — Reserve Bank of Australia",
     "rba_data.py",
     "rba",
     {
         {"Cash Rate (F1)", "cash_rate"},
         {"Bond Yields (F2)", "bond_yields"},
         {"Exchange Rates (F11)", "exchange_rates"},
         {"Inflation / CPI", "inflation"},
         {"Lending Rates", "lending_rates"},
         {"Monetary Aggregates", "monetary"},
     }},
    {"BOC — Bank of Canada",
     "boc_data.py",
     "boc",
     {
         {"Overnight Policy Rate", "policy_rate"},
         {"CORRA", "corra"},
         {"Prime Rate", "prime"},
         {"USD/CAD", "usd"},
         {"EUR/CAD", "eur"},
     }},
    {"Riksbank — Sweden",
     "riksbank_data.py",
     "riksbank",
     {
         {"Policy Rate", "policy_rate"},
         {"Policy + Deposit + Lending", "policy_all"},
         {"T-Bills (1M-6M)", "tbills"},
         {"Mortgage Bond Yields", "mortgage"},
     }},
    {"SNB — Swiss National Bank",
     "snb_data.py",
     "snb",
     {
         {"Policy Rate + SARON", "policy_rate"},
         {"Bond Yields (Monthly)", "bond_yields"},
         {"Bond Yields (Daily)", "bond_yields_d"},
         {"CHF Exchange Rates", "exchange_rates"},
     }},
    {"Norges Bank — Norway",
     "norges_bank_data.py",
     "norges",
     {
         {"Policy Rate Announcements", "policy_rate"},
         {"Exchange Rates (NOK)", "exchange_rates"},
         {"NIBOR / Interest Rates", "interest_rates"},
     }},
    // ── Central & Eastern Europe, Middle East, SE Asia ───────────────────────
    // Every entry below is verified with its default arguments (the panel has
    // no argument widgets): argument-needing commands carry fixed defaults and
    // BNR is omitted because its public XML feed was retired and now returns
    // the bank's HTML site for every documented XML URL.
    {"CNB — Czech National Bank",
     "cnb_data.py",
     "cnb",
     {
         {"Exchange Rates (Latest)", "exchange_rates", {}},
         {"PRIBOR (Latest)", "pribor", {}},
         {"PRIBOR (Year)", "pribor_year", {"@year@"}},
         {"PRIBOR (History, 2y)", "pribor_history", {}},
         {"CZEONIA (Year)", "czeonia_year", {"@year@"}},
         {"CZEONIA (Latest)", "czeonia", {}},
         {"Exchange Rates (Year)", "exchange_rates_y", {"@year@"}},
         {"Monthly Average FX", "monthly_avg", {"@year@"}},
         {"Open Market Operations (last business day)", "omo", {"@prevbusiness@"}},
         {"Overview", "overview", {}},
     }},
    {"NBP — National Bank of Poland",
     "nbp_data.py",
     "nbp",
     {
         {"Today", "today", {}},
         {"Overview", "overview", {}},
         {"Major Currencies", "major", {}},
         {"USD/PLN", "usd", {}},
         {"EUR/PLN", "eur", {}},
         {"Bid/Ask Spreads", "bid_ask", {}},
         {"Single Currency (USD)", "currency", {"USD"}},
         {"Exchange Rates (Range, 30 days)", "range", {"@start30@", "@prevbusiness@"}},
     }},
    {"MNB — National Bank of Hungary",
     "mnb_data.py",
     "mnb",
     {
         {"Today", "today", {}},
         {"Overview", "overview", {}},
         {"Major Currencies", "major", {}},
         {"USD/HUF", "usd", {}},
         {"EUR/HUF", "eur", {}},
         {"Single Currency (USD)", "currency", {"USD"}},
         {"Exchange Rates (Range, 30 days)", "range", {"@start30@", "@prevbusiness@"}},
     }},
    {"HNB — Croatian National Bank",
     "hnb_data.py",
     "hnb",
     {
         {"Today", "today", {}},
         {"Overview", "overview", {}},
         {"USD/EUR", "usd", {}},
         {"GBP", "gbp", {}},
         {"Single Currency (USD)", "currency", {"USD"}},
     }},
    {"TCMB — Central Bank of Türkiye",
     "tcmb_data.py",
     "tcmb",
     {
         {"Today", "today", {}},
         {"Overview", "overview", {}},
         {"Major Currencies", "major", {}},
         {"Single Currency (USD)", "currency", {"USD"}},
         {"By Date (last business day)", "date", {"@prevbusiness@"}},
         {"Exchange Rates (Range, 30 days)", "range", {"@start30@"}},
     }},
    {"BOI — Bank of Israel",
     "boi_data.py",
     "boi",
     {
         {"All Exchange Rates", "all"},
         {"USD/ILS", "usd"},
         {"EUR/ILS", "eur"},
         {"Today", "today"},
         {"Overview", "overview"},
     }},
    {"BNM — Bank Negara Malaysia",
     "bnm_data.py",
     "bnm",
     {
         {"Overnight Policy Rate (OPR)", "opr"},
         {"Major Currencies", "major"},
         {"ASEAN Currencies", "asean"},
         {"Single Currency (USD)", "currency", {"USD"}},
         {"Trading Sessions (USD)", "sessions", {"USD"}},
         {"Overview", "overview"},
     }},
};

// ── Default-argument expansion ───────────────────────────────────────────────
// Fixed per-command defaults keep every selectable entry functional; the date
// tokens are resolved at fetch time so the ranges stay current.
static QDate last_business_day(QDate date) {
    while (date.dayOfWeek() > 5) // 6 = Saturday, 7 = Sunday
        date = date.addDays(-1);
    return date;
}

// The most recent business day whose official publication should already
// exist (some providers publish during the day, so "today" can 404/400).
static QDate previous_business_day(QDate date) {
    return last_business_day(date.addDays(-1));
}

static QStringList expand_cb_args(const QStringList& raw) {
    const QDate today = QDate::currentDate();
    QStringList out;
    out.reserve(raw.size());
    for (const QString& a : raw) {
        if (a == QLatin1String("@today@"))
            out << today.toString(Qt::ISODate);
        else if (a == QLatin1String("@lastbusiness@"))
            out << last_business_day(today).toString(Qt::ISODate);
        else if (a == QLatin1String("@prevbusiness@"))
            out << previous_business_day(today).toString(Qt::ISODate);
        else if (a == QLatin1String("@start30@"))
            out << previous_business_day(today).addDays(-30).toString(Qt::ISODate);
        else if (a == QLatin1String("@year@"))
            out << QString::number(today.year());
        else
            out << a;
    }
    return out;
}

// ── Flatten helpers ──────────────────────────────────────────────────────────

// Bank responses put their payload under "data" with a "date" key and one or
// more numeric value columns. We keep all columns as-is.
//
// The shape is NOT uniform per script — it varies per COMMAND. Series commands
// ("year", "range", "currency", "pribor_history") return an array of rows, but
// point-in-time commands ("today", "date", "overview", "skd") return a single
// object: e.g. bnr_data.py returns `"data": latest`, cnb_data.py returns
// `"data": rows[0] if rows else {}`, tcmb_data.py returns `"data": snapshot`.
// A bare .toArray() silently yields an empty array for all of those, so the
// panel rendered "no data" for a request that actually succeeded. Normalise
// both into a row list.
static QJsonArray extract_cb_rows(const QJsonObject& data) {
    const QJsonValue payload = data["data"];
    if (payload.isArray())
        return payload.toArray();
    if (payload.isObject()) {
        const QJsonObject obj = payload.toObject();
        if (obj.isEmpty())
            return {};
        // Some point-in-time payloads nest the real rows one level down
        // (boi_data.py "overview" → {"data": {"rates": [...]}}). Prefer a
        // nested array over presenting the wrapper object as a single row.
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.value().isArray() && !it.value().toArray().isEmpty())
                return it.value().toArray();
        }
        return QJsonArray{obj}; // genuine single row
    }
    return {};
}

// ── Panel ────────────────────────────────────────────────────────────────────

GlobalCentralBanksPanel::GlobalCentralBanksPanel(QWidget* parent)
    : EconPanelBase(kGlobalCentralBanksSourceId, kGlobalCentralBanksColor, parent) {
    build_base_ui(this);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &GlobalCentralBanksPanel::on_result);
}

void GlobalCentralBanksPanel::activate() {
    show_empty(tr("Select a central bank and series, then click FETCH\n"
                  "Sources: BOE, RBA, Bank of Canada, Riksbank, SNB, Norges Bank,\n"
                  "CNB (Czechia), NBP (Poland), MNB (Hungary),\n"
                  "HNB (Croatia), TCMB (Türkiye), BOI (Israel), BNM (Malaysia)\n"
                  "No API key required for any source\n"
                  "BNR (Romania) unavailable: its public XML feed was retired."));
}

void GlobalCentralBanksPanel::build_controls(QHBoxLayout* thl) {
    bank_lbl_ = new QLabel(tr("BANK"));
    bank_lbl_->setStyleSheet(ctrl_label_style());

    bank_combo_ = new QComboBox;
    bank_combo_->setObjectName("cbBankCombo");
    for (const auto& b : kBanks)
        bank_combo_->addItem(b.label);
    bank_combo_->setFixedHeight(26);
    bank_combo_->setMinimumWidth(230);

    series_lbl_ = new QLabel(tr("SERIES"));
    series_lbl_->setStyleSheet(ctrl_label_style());

    series_combo_ = new QComboBox;
    series_combo_->setObjectName("cbSeriesCombo");
    series_combo_->setFixedHeight(26);
    series_combo_->setMinimumWidth(200);

    // Populate series for the initial bank
    update_series_for_bank(0);

    connect(bank_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &GlobalCentralBanksPanel::update_series_for_bank);

    thl->addWidget(bank_lbl_);
    thl->addWidget(bank_combo_);
    thl->addSpacing(8);
    thl->addWidget(series_lbl_);
    thl->addWidget(series_combo_);
}

void GlobalCentralBanksPanel::update_series_for_bank(int bank_idx) {
    if (bank_idx < 0 || bank_idx >= kBanks.size())
        return;
    series_combo_->clear();
    for (const auto& s : kBanks[bank_idx].series)
        series_combo_->addItem(s.label, s.command);
}

void GlobalCentralBanksPanel::on_fetch() {
    const int bi = bank_combo_->currentIndex();
    const int si = series_combo_->currentIndex();
    if (bi < 0 || bi >= kBanks.size())
        return;
    if (si < 0 || si >= kBanks[bi].series.size())
        return;

    const auto& bank = kBanks[bi];
    const auto& series = bank.series[si];

    show_loading(tr("Fetching %1: %2…").arg(bank.label, series.label));
    services::EconomicsService::instance().execute(kGlobalCentralBanksSourceId, bank.script, series.command,
                                                   expand_cb_args(series.args),
                                                   bank.req_prefix + "_" + series.command);
}

void GlobalCentralBanksPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != kGlobalCentralBanksSourceId)
        return;

    // Check this result belongs to one of our banks
    bool matched = false;
    int matched_bank = -1;
    int matched_series = -1;
    for (int bi = 0; bi < kBanks.size(); ++bi) {
        const auto& bank = kBanks[bi];
        if (!request_id.startsWith(bank.req_prefix + "_"))
            continue;
        matched = true;
        matched_bank = bi;
        const QString cmd = request_id.mid(bank.req_prefix.size() + 1);
        for (int si = 0; si < bank.series.size(); ++si) {
            if (bank.series[si].command == cmd) {
                matched_series = si;
                break;
            }
        }
        break;
    }
    if (!matched)
        return;

    if (!result.success) {
        show_error(result.error);
        return;
    }

    const QString inline_err = result.data["error"].toString();
    if (!inline_err.isEmpty()) {
        show_error(inline_err);
        return;
    }

    QJsonArray raw = extract_cb_rows(result.data);

    // Filter rows: keep only those with at least one numeric value
    QJsonArray rows;
    for (const auto& rv : raw) {
        const QJsonObject r = rv.toObject();
        bool has_value = false;
        for (const auto& key : r.keys()) {
            if (key == "date")
                continue;
            if (r[key].isDouble()) {
                has_value = true;
                break;
            }
        }
        if (has_value)
            rows.append(r);
    }

    if (rows.isEmpty()) {
        show_error(tr("No data returned"));
        return;
    }

    const QString bank_name = matched_bank >= 0 ? kBanks[matched_bank].label : "";
    const QString series_name =
        (matched_bank >= 0 && matched_series >= 0) ? kBanks[matched_bank].series[matched_series].label : request_id;
    const QString title = bank_name + ": " + series_name;

    display(rows, title);
    LOG_INFO("GlobalCentralBanksPanel", QString("Displayed %1 rows: %2").arg(rows.size()).arg(title));
}

// ── i18n ──────────────────────────────────────────────────────────────────────

void GlobalCentralBanksPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    EconPanelBase::changeEvent(event);
}

void GlobalCentralBanksPanel::retranslateUi() {
    if (bank_lbl_)
        bank_lbl_->setText(tr("BANK"));
    if (series_lbl_)
        series_lbl_->setText(tr("SERIES"));
    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
