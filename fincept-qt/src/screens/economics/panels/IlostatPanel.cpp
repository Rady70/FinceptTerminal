// src/screens/economics/panels/IlostatPanel.cpp
// ILO ILOSTAT — unemployment rate, labour force participation, employment-to-population.
// Script: ilostat_data.py  |  No API key required.
//
// Response shape: { success, indicator, dataflow, key, data:[{TIME_PERIOD, OBS_VALUE, REF_AREA, ...}] }
#include "screens/economics/panels/IlostatPanel.h"

#include "core/logging/Logger.h"
#include "screens/economics/panels/IlostatSeriesData.h"
#include "services/economics/EconomicsService.h"
#include "ui/charts/TimeSeriesData.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>

namespace fincept::screens {
namespace {

static constexpr const char* kIlostatScript = "ilostat_data.py";
static constexpr const char* kIlostatSourceId = "ilostat";
static constexpr const char* kIlostatColor = "#3B82F6"; // ILO blue
} // namespace

struct IloSeries {
    QString label;
    QString command;
    QString unit;
    QString description;
};

static const QList<IloSeries> kIlostatSeries = {
    {"Unemployment Rate", "unemployment", "Percent", "UNE_DEAP_SEX_AGE_RT — annual, total, 15+"},
    {"Labour Force Participation Rate", "lfpr", "Percent", "EAP_DWAP_SEX_AGE_RT — annual, total, 15+"},
    {"Employment-to-Population Ratio", "emp_to_pop", "Percent", "EMP_DWAP_SEX_AGE_RT — annual, total, 15+"},
};

IlostatPanel::IlostatPanel(QWidget* parent) : EconPanelBase(kIlostatSourceId, kIlostatColor, parent) {
    build_base_ui(this);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &IlostatPanel::on_result);
}

void IlostatPanel::activate() {
    show_empty(tr("Select a series, enter a country code, then click FETCH\n"
                  "Source: ILO ILOSTAT SDMX REST API (sdmx.ilo.org) — no API key required\n"
                  "Series are ANNUAL, both sexes, age 15+, harmonised (modelled) estimates — percent.\n"
                  "Country codes (ISO-3): USA, GBR, DEU, FRA, IND, CHN, JPN, BRA, ZAF · "
                  "G7 = CAN+USA+GBR+DEU+FRA+ITA+JPN"));
}

void IlostatPanel::build_controls(QHBoxLayout* thl) {
    auto mk_lbl = [](const QString& t) {
        auto* l = new QLabel(t);
        l->setStyleSheet(ctrl_label_style());
        return l;
    };

    series_combo_ = new QComboBox;
    for (const auto& s : kIlostatSeries)
        series_combo_->addItem(s.label, s.command);
    series_combo_->setFixedHeight(26);
    series_combo_->setMinimumWidth(220);

    for (int i = 0; i < kIlostatSeries.size(); ++i)
        series_combo_->setItemData(i, kIlostatSeries[i].description, Qt::ToolTipRole);

    country_edit_ = new QLineEdit("USA");
    country_edit_->setFixedHeight(26);
    country_edit_->setFixedWidth(120);
    // ILOSTAT SDMX uses ISO-3166 alpha-3 (USA/GBR/DEU) — the field was
    // mislabelled "ISO-2" while every example it gave was alpha-3.
    country_edit_->setPlaceholderText(tr("ISO-3 code…"));
    country_edit_->setToolTip(tr("ISO-3 country code, e.g. USA, GBR, DEU\n"
                                 "Multiple: CAN+USA+GBR\n"
                                 "All countries: ALL"));
    country_edit_->setAccessibleName(tr("ILOSTAT country code"));

    start_edit_ = new QLineEdit("2010");
    start_edit_->setFixedHeight(26);
    start_edit_->setFixedWidth(52);

    end_edit_ = new QLineEdit("2023");
    end_edit_->setFixedHeight(26);
    end_edit_->setFixedWidth(52);

    thl->addWidget(series_lbl_ = mk_lbl(tr("SERIES")));
    thl->addWidget(series_combo_);
    thl->addSpacing(6);
    thl->addWidget(country_lbl_ = mk_lbl(tr("COUNTRY")));
    thl->addWidget(country_edit_);
    thl->addSpacing(6);
    thl->addWidget(from_lbl_ = mk_lbl(tr("FROM")));
    thl->addWidget(start_edit_);
    thl->addWidget(to_lbl_ = mk_lbl(tr("TO")));
    thl->addWidget(end_edit_);
}

void IlostatPanel::on_fetch() {
    const int idx = series_combo_->currentIndex();
    const auto& series = kIlostatSeries[idx];

    const QString country = country_edit_->text().trimmed().toUpper();
    const QString start = start_edit_->text().trimmed();
    const QString end = end_edit_->text().trimmed();

    if (country.isEmpty()) {
        show_error(tr("Please enter a country code (e.g. USA)"));
        return;
    }

    show_loading(tr("Fetching ILO: %1 — %2…").arg(series.label, country));

    // The id must identify the full request: two fetches for the same series
    // with different country/year windows are different operations, and a late
    // response for one must not replace the other.
    pending_request_ = "ilostat_" + series.command + "_" + country + "_" + start + "_" + end;
    pending_series_index_ = idx;
    pending_country_ = country;
    services::EconomicsService::instance().execute(kIlostatSourceId, kIlostatScript, series.command,
                                                   {country, "A", "SEX_T", "AGE_YTHADULT_YGE15", start, end},
                                                   pending_request_);
}

void IlostatPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != kIlostatSourceId)
        return;
    if (!request_id.startsWith("ilostat_"))
        return;

    // A newer fetch supersedes an older response that arrives late — including
    // a stale failure, which must not overwrite a newer success.
    if (request_id != pending_request_)
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

    // Response: { success, indicator, dataflow, key, data:[{TIME_PERIOD, OBS_VALUE, REF_AREA, ...}] }
    const QJsonArray rows = ilostat_normalize_periods(result.data["data"].toArray());

    if (rows.isEmpty()) {
        show_error(tr("No data returned — try a different country code or year range"));
        return;
    }

    // Label the result from the request that produced it, never from a later
    // user selection.
    const bool have_series = pending_series_index_ >= 0 && pending_series_index_ < kIlostatSeries.size();
    const QString series_label = have_series ? kIlostatSeries[pending_series_index_].label : QString();
    const QString title = "ILO: " + (series_label.isEmpty() ? tr("Series") : series_label) +
                          (pending_country_.isEmpty() ? QString() : " — " + pending_country_);

    // One chart means one series. It is offered only when the request named a
    // single country and every returned row carries that same area; multi-
    // country lists, ALL and any area mismatch stay tabular because one line
    // cannot represent them. Their pooled LATEST/CHANGE/... would mix
    // countries, so the stat cards are hidden for that display and restored
    // for a real series.
    const bool single_country_request = !pending_country_.isEmpty() && !pending_country_.contains(QLatin1Char('+')) &&
                                        pending_country_ != QLatin1String("ALL");
    const QString area = ilostat_single_series_area(rows);
    if (!single_country_request || area.isEmpty() || area != pending_country_) {
        set_stats_visible(false);
        display(rows, title);
    } else {
        set_stats_visible(true);
        ui::TimeSeriesMeta meta;
        meta.source = QStringLiteral("ILO ILOSTAT");
        // Prefer the provider's own unit when it states one; the three retained
        // series are rates, so the panel's descriptor is the fallback.
        meta.unit = rows.first().toObject().value(QStringLiteral("UNIT_MEASURE")).toString().trimmed();
        if (meta.unit.isEmpty() && have_series)
            meta.unit = kIlostatSeries[pending_series_index_].unit;
        meta.frequency = QStringLiteral("Annual");
        display_time_series(rows, title, QStringLiteral("TIME_PERIOD"), QStringLiteral("OBS_VALUE"), meta);
    }
    LOG_INFO("IlostatPanel", QString("Displayed %1 rows: %2").arg(rows.size()).arg(title));
}

// ── i18n ──────────────────────────────────────────────────────────────────────

void IlostatPanel::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    EconPanelBase::changeEvent(event);
}

void IlostatPanel::retranslateUi() {
    if (series_lbl_)
        series_lbl_->setText(tr("SERIES"));
    if (country_lbl_)
        country_lbl_->setText(tr("COUNTRY"));
    if (from_lbl_)
        from_lbl_->setText(tr("FROM"));
    if (to_lbl_)
        to_lbl_->setText(tr("TO"));
    if (country_edit_) {
        country_edit_->setPlaceholderText(tr("ISO-3 code…"));
        country_edit_->setToolTip(tr("ISO-3 country code, e.g. USA, GBR, DEU\n"
                                     "Multiple: CAN+USA+GBR\n"
                                     "All countries: ALL"));
    }
    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
