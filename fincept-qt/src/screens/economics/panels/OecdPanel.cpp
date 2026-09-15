// src/screens/economics/panels/OecdPanel.cpp
#include "screens/economics/panels/OecdPanel.h"

#include "core/logging/Logger.h"
#include "services/economics/EconomicsService.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>

namespace fincept::screens {
namespace {

static constexpr const char* kOecdScript = "oecd_data.py";
static constexpr const char* kOecdSourceId = "oecd";
static constexpr const char* kOecdColor = "#F59E0B"; // amber

static const QList<QPair<QString, QString>> kOecdDatasets = {
    {"GDP (Real)", "gdp_real"},       {"CPI / Inflation", "cpi"},           {"GDP Forecast", "gdp_forecast"},
    {"Unemployment", "unemployment"}, {"Interest Rates", "interest_rates"}, {"Trade Balance", "trade_balance"},
};

static const QList<QPair<QString, QString>> kOecdCountries = {
    {"United States", "US"},  {"Germany", "DE"}, {"Japan", "JP"},     {"France", "FR"},
    {"United Kingdom", "GB"}, {"Canada", "CA"},  {"Australia", "AU"}, {"South Korea", "KR"},
    {"Italy", "IT"},          {"Spain", "ES"},   {"G7", "G-7"},       {"OECD Total", "OECD"},
};

} // namespace

OecdPanel::OecdPanel(QWidget* parent) : EconPanelBase(kOecdSourceId, kOecdColor, parent) {
    build_base_ui(this);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &OecdPanel::on_result);
}

void OecdPanel::activate() {
    mark_source_unavailable(tr("The OECD SDMX queries in this connector were built for older data "
                               "structures and every dataset is rejected by the current API. A query "
                               "rewrite is pending, so this source is unavailable."));
}

void OecdPanel::build_controls(QHBoxLayout* thl) {
    auto lbl = [](const QString& t) {
        auto* l = new QLabel(t);
        l->setStyleSheet(ctrl_label_style());
        return l;
    };

    dataset_combo_ = new QComboBox;
    for (const auto& p : kOecdDatasets)
        dataset_combo_->addItem(p.first, p.second);
    dataset_combo_->setFixedHeight(26);

    country_combo_ = new QComboBox;
    for (const auto& p : kOecdCountries)
        country_combo_->addItem(p.first, p.second);
    country_combo_->setFixedHeight(26);

    frequency_combo_ = new QComboBox;
    frequency_combo_->setFixedHeight(26);
    update_frequency_options();

    connect(dataset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { update_frequency_options(); });

    thl->addWidget(dataset_lbl_ = lbl(tr("DATASET")));
    thl->addWidget(dataset_combo_);
    thl->addWidget(country_lbl_ = lbl(tr("COUNTRY")));
    thl->addWidget(country_combo_);
    thl->addWidget(freq_lbl_ = lbl(tr("FREQ")));
    thl->addWidget(frequency_combo_);
}

void OecdPanel::update_frequency_options() {
    if (!frequency_combo_)
        return;
    const QString cmd = dataset_combo_ ? dataset_combo_->currentData().toString() : QString();
    const QString current = frequency_combo_->currentData().toString();

    frequency_combo_->blockSignals(true);
    frequency_combo_->clear();
    if (cmd == QLatin1String("gdp_forecast")) {
        // This dataflow takes no frequency argument.
        frequency_combo_->addItem(tr("n/a"), QString());
        frequency_combo_->setEnabled(false);
    } else if (cmd == QLatin1String("gdp_real")) {
        // oecd_data.py accepts only quarter/annual for real GDP.
        frequency_combo_->setEnabled(true);
        frequency_combo_->addItem(tr("Quarterly"), "quarter");
        frequency_combo_->addItem(tr("Annual"), "annual");
    } else {
        frequency_combo_->setEnabled(true);
        frequency_combo_->addItem(tr("Monthly"), "monthly");
        frequency_combo_->addItem(tr("Quarterly"), "quarter");
        frequency_combo_->addItem(tr("Annual"), "annual");
    }
    const int idx = frequency_combo_->findData(current);
    if (idx >= 0)
        frequency_combo_->setCurrentIndex(idx);
    frequency_combo_->blockSignals(false);
    if (freq_lbl_)
        freq_lbl_->setEnabled(frequency_combo_->isEnabled());
}

void OecdPanel::on_fetch() {
    const QString cmd = dataset_combo_->currentData().toString();
    const QString country = country_combo_->currentData().toString();
    const QString freq = frequency_combo_->currentData().toString();

    // oecd_data.py has three argv shapes: (country, expenditure, frequency,
    // units) for cpi, (country) for gdp_forecast, and (country, frequency)
    // for the rest. The panel used to send (country, frequency) to all of
    // them, which mis-bound the arguments for cpi and gdp_forecast.
    QStringList args{country};
    if (cmd == QLatin1String("cpi")) {
        args << QStringLiteral("total") << freq << QStringLiteral("index");
    } else if (cmd != QLatin1String("gdp_forecast")) {
        args << freq;
    }

    show_loading(tr("Fetching OECD data…"));
    services::EconomicsService::instance().execute(kOecdSourceId, kOecdScript, cmd, args,
                                                   "oecd_" + cmd + "_" + country + "_" + freq);
}

void OecdPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != kOecdSourceId)
        return;
    if (!result.success) {
        show_error(result.error);
        return;
    }
    if (request_id.startsWith("oecd_")) {
        const QJsonArray arr = result.data["data"].toArray();
        const QString title = dataset_combo_->currentText() + " — " + country_combo_->currentText();
        display(arr, title);
        LOG_INFO("OecdPanel", QString("Displayed %1 rows").arg(arr.size()));
    }
}

// ── i18n ──────────────────────────────────────────────────────────────────────

void OecdPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    EconPanelBase::changeEvent(event);
}

void OecdPanel::retranslateUi() {
    if (dataset_lbl_)
        dataset_lbl_->setText(tr("DATASET"));
    if (country_lbl_)
        country_lbl_->setText(tr("COUNTRY"));
    if (freq_lbl_)
        freq_lbl_->setText(tr("FREQ"));
    if (frequency_combo_) {
        // Text follows the data key so per-dataset option sets stay correct.
        for (int i = 0; i < frequency_combo_->count(); ++i) {
            const QString key = frequency_combo_->itemData(i).toString();
            if (key == QLatin1String("monthly"))
                frequency_combo_->setItemText(i, tr("Monthly"));
            else if (key == QLatin1String("quarter"))
                frequency_combo_->setItemText(i, tr("Quarterly"));
            else if (key == QLatin1String("annual"))
                frequency_combo_->setItemText(i, tr("Annual"));
            else
                frequency_combo_->setItemText(i, tr("n/a"));
        }
    }
    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
