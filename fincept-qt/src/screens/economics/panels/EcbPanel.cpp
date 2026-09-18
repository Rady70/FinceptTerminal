// src/screens/economics/panels/EcbPanel.cpp
// ECB SDMX data panel.
// Response shape: {<key>: [{dimensions:{...}, observations:[{period,value}], obs_count}], ...}
// Working commands: exchange_rates, inflation, money_supply
#include "screens/economics/panels/EcbPanel.h"

#include "core/logging/Logger.h"
#include "services/economics/EconomicsService.h"
#include "ui/charts/TimeSeriesData.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>

namespace fincept::screens {
namespace {

static constexpr const char* kEcbScript = "ecb_sdmx_data.py";
static constexpr const char* kEcbSourceId = "ecb";
static constexpr const char* kEcbColor = "#2563EB"; // ECB blue

// Series: display name -> {command, arg (optional currency)}
struct EcbSeries {
    QString label;
    QString command;
    QString arg;        // currency for exchange_rates, empty otherwise
    QString result_key; // top-level JSON key in response
    QString unit{};     // provider unit where stated for the series
    QString frequency{}; // provider observation frequency where stated
};

static const QList<EcbSeries> kEcbSeries = {
    {"EUR/USD Exchange Rate", "exchange_rates", "USD", "exchange_rates"},
    {"EUR/GBP Exchange Rate", "exchange_rates", "GBP", "exchange_rates"},
    {"EUR/JPY Exchange Rate", "exchange_rates", "JPY", "exchange_rates"},
    {"EUR/CHF Exchange Rate", "exchange_rates", "CHF", "exchange_rates"},
    {"EUR/CNY Exchange Rate", "exchange_rates", "CNY", "exchange_rates"},
    {"Eurozone HICP Inflation", "inflation", "", "inflation"},
    {"M3 Money Supply", "money_supply", "", "money_supply"},
    // ECB key interest rate (FM dataset, daily). Unit and frequency are the
    // dataset's own semantics and are shown as provenance with the chart.
    {"ECB Main Refinancing Rate (MRO)", "interest_rates", "MRO", "interest_rates", "percent per annum", "Daily"},
};

} // namespace

EcbPanel::EcbPanel(QWidget* parent) : EconPanelBase(kEcbSourceId, kEcbColor, parent) {
    build_base_ui(this);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &EcbPanel::on_result);
}

void EcbPanel::activate() {
    show_empty(tr("Select a series and click FETCH\n"
                  "Source: European Central Bank SDMX 2.1 API"));
}

void EcbPanel::build_controls(QHBoxLayout* thl) {
    auto lbl = [](const QString& t) {
        auto* l = new QLabel(t);
        l->setStyleSheet(ctrl_label_style());
        return l;
    };

    series_combo_ = new QComboBox;
    for (const auto& s : kEcbSeries)
        series_combo_->addItem(s.label, s.command + "|" + s.arg);
    series_combo_->setFixedHeight(26);
    series_combo_->setMinimumWidth(220);

    thl->addWidget(series_lbl_ = lbl(tr("SERIES")));
    thl->addWidget(series_combo_);
}

void EcbPanel::on_fetch() {
    const int idx = series_combo_->currentIndex();
    const auto& series = kEcbSeries[idx];

    show_loading(tr("Fetching ECB data: %1…").arg(series.label));

    // EconomicsService::execute() already prepends `command` to the argv, so
    // args must carry ONLY the extra positional arguments. Repeating the
    // command here shifted every parameter by one — the CLI read
    // `exchange_rates exchange_rates USD` as currency="exchange_rates",
    // freq="USD", so every ECB fetch silently queried a bogus series.
    QStringList args;
    if (!series.arg.isEmpty())
        args << series.arg;

    const QString req_id = "ecb_" + series.command + (series.arg.isEmpty() ? "" : "_" + series.arg);

    pending_request_ = req_id;
    services::EconomicsService::instance().execute(kEcbSourceId, kEcbScript, series.command, args, req_id);
}

// ECB response: {<result_key>: [{dimensions:{}, observations:[{period,value}], obs_count}], ...}
// Flatten the first series' observations into a plain [{period, value}] array.
void EcbPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != kEcbSourceId)
        return;
    if (!request_id.startsWith("ecb_"))
        return;

    // A newer fetch supersedes an older response that arrives late — including
    // a stale failure, which must not overwrite a newer success.
    if (request_id != pending_request_)
        return;

    if (!result.success) {
        show_error(result.error);
        return;
    }

    // Resolve the descriptor from the request id, not the live combo, so an
    // async result is never mapped to a later user selection.
    const QString payload = request_id.mid(4); // strip "ecb_"
    const EcbSeries* series = nullptr;
    for (const auto& candidate : kEcbSeries) {
        const QString id = candidate.command + (candidate.arg.isEmpty() ? QString() : "_" + candidate.arg);
        if (id == payload) {
            series = &candidate;
            break;
        }
    }
    if (!series) {
        show_error(tr("Unrecognised ECB response for %1").arg(request_id));
        return;
    }

    // Extract observations from first element of the result array
    const QJsonValue top = result.data[series->result_key];
    QJsonArray obs;

    if (top.isArray()) {
        const QJsonArray arr = top.toArray();
        if (!arr.isEmpty())
            obs = arr[0].toObject()["observations"].toArray();
    } else if (top.isObject()) {
        // Fallback: result wrapped as object
        obs = top.toObject()["observations"].toArray();
    }

    // Also accept flat data array from service normalisation
    if (obs.isEmpty())
        obs = result.data["data"].toArray();

    if (obs.isEmpty()) {
        show_error(tr("No observations returned for %1").arg(series->label));
        return;
    }

    const QString title = "ECB: " + series->label;
    if (series->command == QLatin1String("interest_rates")) {
        ui::TimeSeriesMeta meta;
        meta.source = QStringLiteral("ECB SDMX 2.1 API");
        meta.unit = series->unit;
        meta.frequency = series->frequency;
        display_time_series(obs, title, QStringLiteral("period"), QStringLiteral("value"), meta);
    } else {
        display(obs, title);
    }
    LOG_INFO("EcbPanel", QString("Displayed %1 observations for %2").arg(obs.size()).arg(series->label));
}

// ── i18n ──────────────────────────────────────────────────────────────────────

void EcbPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    EconPanelBase::changeEvent(event);
}

void EcbPanel::retranslateUi() {
    if (series_lbl_)
        series_lbl_->setText(tr("SERIES"));
    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
