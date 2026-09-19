// src/screens/economics/panels/CftcPanel.cpp
// R3 — the dedicated CFTC Commitments of Traders analytical workspace.
//
// Two result tabs: Analysis (one vertically scrollable page) and Raw Data
// (the shared newest-first table + CSV over the same observations). All
// analytics are computed locally from the authoritative CFTC history returned
// by cftc_data.py's cot_history command; nothing is fetched from a third-party
// COT service.
#include "screens/economics/panels/CftcPanel.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/economics/panels/CftcHeatmap.h"
#include "screens/economics/panels/CftcNetFormat.h"
#include "screens/economics/panels/CftcPositioningChart.h"
#include "services/economics/EconomicsService.h"
#include "ui/charts/TimeSeriesData.h"
#include "ui/theme/Theme.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace fincept::screens {

// The COT metric core lives in fincept::services. Name the specific types and
// functions this translation unit uses instead of importing the whole
// namespace, so a future same-name addition elsewhere stays a clear error.
using services::cftc_change_since_days;
using services::cftc_direction;
using services::cftc_directions_aligned;
using services::cftc_family_code;
using services::cftc_family_from_code;
using services::cftc_family_participants;
using services::cftc_filter_range;
using services::cftc_heatmap_series;
using services::cftc_metric_series;
using services::cftc_net_extreme_dates;
using services::cftc_open_interest_series;
using services::cftc_parse_history;
using services::cftc_position_metrics;
using services::cftc_price_change_since_days;
using services::cftc_price_on_or_before;
using services::cftc_range_available;
using services::cftc_range_label;
using services::cftc_report_dates;
using services::cftc_speculative_index;
using services::cftc_weekly_change;
using services::cftc_window_stats;
using services::CftcChange;
using services::CftcDatedValue;
using services::CftcDirection;
using services::CftcFamily;
using services::CftcHeatmapPoint;
using services::CftcHistory;
using services::CftcMetricKind;
using services::CftcObservation;
using services::CftcPositionMetrics;
using services::CftcPricePoint;
using services::CftcRange;
using services::CftcWindowStats;
using services::kCftcHeatmapMinObservations;
using services::kCftcWeeklyGapDays;

namespace {

static constexpr const char* kCftcScript = "cftc_data.py";
static constexpr const char* kCftcSourceId = "cftc";
static constexpr const char* kCftcColor = "#FF5722"; // deep orange
static constexpr const char* kCftcMaxRows = "20000";
static constexpr int kCftcHeatmapColumns = 52;
static constexpr int kCftcMobileBreakpoint = 980;

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

struct PriceSpec {
    QString symbol;
    bool spot_index = false; // true: a spot index, not the futures contract
};

// Retained free public price path (Yahoo Finance via MarketDataService, no
// account, no key). A market without a mapping explicitly reports that price
// analysis is unavailable instead of fabricating a series. Continuous symbols
// are front-month proxies that roll between contracts; spot indices are
// labelled as such in the divergence section.
static const QHash<QString, PriceSpec>& price_specs() {
    static const QHash<QString, PriceSpec> specs = {
        {QStringLiteral("gold"), {QStringLiteral("GC=F"), false}},
        {QStringLiteral("silver"), {QStringLiteral("SI=F"), false}},
        {QStringLiteral("copper"), {QStringLiteral("HG=F"), false}},
        {QStringLiteral("platinum"), {QStringLiteral("PL=F"), false}},
        {QStringLiteral("palladium"), {QStringLiteral("PA=F"), false}},
        {QStringLiteral("crude_oil"), {QStringLiteral("CL=F"), false}},
        {QStringLiteral("natural_gas"), {QStringLiteral("NG=F"), false}},
        {QStringLiteral("gasoline"), {QStringLiteral("RB=F"), false}},
        {QStringLiteral("heating_oil"), {QStringLiteral("HO=F"), false}},
        {QStringLiteral("corn"), {QStringLiteral("ZC=F"), false}},
        {QStringLiteral("wheat"), {QStringLiteral("ZW=F"), false}},
        {QStringLiteral("soybeans"), {QStringLiteral("ZS=F"), false}},
        {QStringLiteral("cotton"), {QStringLiteral("CT=F"), false}},
        {QStringLiteral("coffee"), {QStringLiteral("KC=F"), false}},
        {QStringLiteral("sugar"), {QStringLiteral("SB=F"), false}},
        {QStringLiteral("cocoa"), {QStringLiteral("CC=F"), false}},
        {QStringLiteral("live_cattle"), {QStringLiteral("LE=F"), false}},
        {QStringLiteral("lean_hogs"), {QStringLiteral("HE=F"), false}},
        {QStringLiteral("euro"), {QStringLiteral("6E=F"), false}},
        {QStringLiteral("jpy"), {QStringLiteral("6J=F"), false}},
        {QStringLiteral("british_pound"), {QStringLiteral("6B=F"), false}},
        {QStringLiteral("swiss_franc"), {QStringLiteral("6S=F"), false}},
        {QStringLiteral("canadian_dollar"), {QStringLiteral("6C=F"), false}},
        {QStringLiteral("australian_dollar"), {QStringLiteral("6A=F"), false}},
        {QStringLiteral("s&p_500"), {QStringLiteral("ES=F"), false}},
        {QStringLiteral("nasdaq_100"), {QStringLiteral("NQ=F"), false}},
        {QStringLiteral("dow_jones"), {QStringLiteral("YM=F"), false}},
        {QStringLiteral("nikkei"), {QStringLiteral("NIY=F"), false}},
        {QStringLiteral("vix"), {QStringLiteral("^VIX"), true}},
        {QStringLiteral("treasury_bonds"), {QStringLiteral("ZB=F"), false}},
        {QStringLiteral("treasury_notes_10y"), {QStringLiteral("ZN=F"), false}},
        {QStringLiteral("treasury_notes_5y"), {QStringLiteral("ZF=F"), false}},
        {QStringLiteral("treasury_notes_2y"), {QStringLiteral("ZT=F"), false}},
        {QStringLiteral("fed_funds"), {QStringLiteral("ZQ=F"), false}},
        {QStringLiteral("bitcoin"), {QStringLiteral("BTC=F"), false}},
        {QStringLiteral("ether"), {QStringLiteral("ETH=F"), false}},
        {QStringLiteral("us_dollar_index"), {QStringLiteral("DX-Y.NYB"), true}},
    };
    return specs;
}

QString family_label(CftcFamily family) {
    switch (family) {
        case CftcFamily::Disaggregated:
            return QCoreApplication::translate("CftcPanel", "Disaggregated");
        case CftcFamily::Tff:
            return QCoreApplication::translate("CftcPanel", "Traders in Financial Futures (TFF)");
        case CftcFamily::Legacy:
            break;
    }
    return QCoreApplication::translate("CftcPanel", "Legacy");
}

QString participant_short_label(const QString& key) {
    if (key == QLatin1String("commercial"))
        return QCoreApplication::translate("CftcPanel", "COMMERCIAL");
    if (key == QLatin1String("non_commercial"))
        return QCoreApplication::translate("CftcPanel", "NON-COMMERCIAL");
    if (key == QLatin1String("producer_merchant"))
        return QCoreApplication::translate("CftcPanel", "PROD/MERCH");
    if (key == QLatin1String("swap_dealer"))
        return QCoreApplication::translate("CftcPanel", "SWAP DEALERS");
    if (key == QLatin1String("managed_money"))
        return QCoreApplication::translate("CftcPanel", "MANAGED MONEY");
    if (key == QLatin1String("dealer"))
        return QCoreApplication::translate("CftcPanel", "DEALER");
    if (key == QLatin1String("asset_manager"))
        return QCoreApplication::translate("CftcPanel", "ASSET MGR");
    if (key == QLatin1String("leveraged_funds"))
        return QCoreApplication::translate("CftcPanel", "LEVERAGED FUNDS");
    if (key == QLatin1String("other_reportable"))
        return QCoreApplication::translate("CftcPanel", "OTHER REPT");
    if (key == QLatin1String("non_reportable"))
        return QCoreApplication::translate("CftcPanel", "NON-REPT");
    return key.toUpper();
}

QVector<QColor> series_palette() {
    QVector<QColor> palette;
    const auto& colors = ui::ThemeManager::instance().tokens().chart_colors;
    for (const auto* color : colors)
        palette.append(QColor(QString::fromLatin1(color)));
    return palette;
}

CftcChartMetric cftc_chart_metric_from_code(const QString& code) {
    if (code == QLatin1String("long"))
        return CftcChartMetric::Long;
    if (code == QLatin1String("short"))
        return CftcChartMetric::Short;
    return CftcChartMetric::Net;
}

CftcMetricKind metric_kind(CftcChartMetric metric) {
    switch (metric) {
        case CftcChartMetric::Long:
            return CftcMetricKind::Long;
        case CftcChartMetric::Short:
            return CftcMetricKind::Short;
        case CftcChartMetric::Net:
            break;
    }
    return CftcMetricKind::Net;
}

QVector<CftcDatedValue> metric_series(const QVector<CftcObservation>& observations, int index, CftcChartMetric metric) {
    return cftc_metric_series(observations, index, metric_kind(metric));
}

/// UI-side adapter from the core's dated series to the shared chart point
/// type, so the specialist chart reuses the tested gap/format rules without
/// the analytical core depending on the chart types.
QVector<ui::TimeSeriesPoint> cftc_to_time_series(const QVector<CftcDatedValue>& series) {
    QVector<ui::TimeSeriesPoint> out;
    out.reserve(series.size());
    for (const auto& point : series)
        out.append({point.date, point.date_label, point.value});
    return out;
}

QString position_text(double value) {
    if (value == std::floor(value) && std::abs(value) < 1e15)
        return QString::number(static_cast<qint64>(value));
    return QString::number(value, 'g', 10);
}

QString signed_decimal(double value, int decimals) {
    const QString text = QString::number(value, 'f', decimals);
    return value > 0.0 ? QStringLiteral("+") + text : text;
}

QString stat_unavailable_reason(const CftcWindowStats& stats, const QDate& as_of) {
    if (!stats.at_latest_report)
        return QCoreApplication::translate("CftcPanel", "the latest report (%1) does not carry this class")
            .arg(as_of.toString(Qt::ISODate));
    if (stats.count == 0)
        return QCoreApplication::translate("CftcPanel", "no observations in the selected window");
    if (stats.count < 2)
        return QCoreApplication::translate("CftcPanel", "fewer than 2 observations in the selected window");
    if (stats.zero_variance)
        return QCoreApplication::translate("CftcPanel", "the window has no variance");
    return QCoreApplication::translate("CftcPanel", "not available");
}

/// A CFTC script failure may reach the panel as the raw typed error envelope
/// ({"endpoint":..., "error":"message", ...}), depending on which layer
/// classified it. Surface the human message, never the internal JSON.
QString cftc_error_text(const QString& raw) {
    const QString trimmed = raw.trimmed();
    if (trimmed.startsWith(QLatin1Char('{'))) {
        const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8());
        if (doc.isObject()) {
            const QJsonValue error = doc.object().value(QStringLiteral("error"));
            if (error.isString() && !error.toString().trimmed().isEmpty())
                return error.toString();
            if (error.isObject()) {
                const QString nested = error.toObject().value(QStringLiteral("error")).toString();
                if (!nested.trimmed().isEmpty())
                    return nested;
            }
        }
    }
    return raw;
}

QString weekly_unavailable_reason(const CftcChange& change, const QDate& as_of) {
    if (change.stale)
        return QCoreApplication::translate("CftcPanel", "the latest report (%1) does not carry this class")
            .arg(as_of.toString(Qt::ISODate));
    if (!change.has_pair)
        return QCoreApplication::translate("CftcPanel", "no previous report in the returned history");
    return QCoreApplication::translate("CftcPanel", "previous report is %1 days earlier").arg(change.gap_days);
}

enum class StatMetric { Latest, Max, Min, Avg, CotIndex, Percentile, ZScore, FromHigh, FromLow, Change4W, Change13W };
constexpr int kStatMetricCount = 11;

StatMetric stat_metric_at(int row) {
    switch (row) {
        case 0:
            return StatMetric::Latest;
        case 1:
            return StatMetric::Max;
        case 2:
            return StatMetric::Min;
        case 3:
            return StatMetric::Avg;
        case 4:
            return StatMetric::CotIndex;
        case 5:
            return StatMetric::Percentile;
        case 6:
            return StatMetric::ZScore;
        case 7:
            return StatMetric::FromHigh;
        case 8:
            return StatMetric::FromLow;
        case 9:
            return StatMetric::Change4W;
        default:
            return StatMetric::Change13W;
    }
}

QString stat_metric_label(StatMetric metric) {
    switch (metric) {
        case StatMetric::Latest:
            return QCoreApplication::translate("CftcPanel", "LATEST");
        case StatMetric::Max:
            return QCoreApplication::translate("CftcPanel", "MAX");
        case StatMetric::Min:
            return QCoreApplication::translate("CftcPanel", "MIN");
        case StatMetric::Avg:
            return QCoreApplication::translate("CftcPanel", "AVERAGE");
        case StatMetric::CotIndex:
            return QCoreApplication::translate("CftcPanel", "COT INDEX");
        case StatMetric::Percentile:
            return QCoreApplication::translate("CftcPanel", "PERCENTILE");
        case StatMetric::ZScore:
            return QCoreApplication::translate("CftcPanel", "Z-SCORE");
        case StatMetric::FromHigh:
            return QCoreApplication::translate("CftcPanel", "FROM HIGH");
        case StatMetric::FromLow:
            return QCoreApplication::translate("CftcPanel", "FROM LOW");
        case StatMetric::Change4W:
            return QCoreApplication::translate("CftcPanel", "Δ 4 WEEK");
        case StatMetric::Change13W:
            break;
    }
    return QCoreApplication::translate("CftcPanel", "Δ 13 WEEK");
}

bool stat_metric_is_window_scoped(StatMetric metric) {
    switch (metric) {
        case StatMetric::Max:
        case StatMetric::Min:
        case StatMetric::Avg:
        case StatMetric::CotIndex:
        case StatMetric::Percentile:
        case StatMetric::ZScore:
        case StatMetric::FromHigh:
        case StatMetric::FromLow:
            return true;
        case StatMetric::Latest:
        case StatMetric::Change4W:
        case StatMetric::Change13W:
            break;
    }
    return false;
}

QString stat_metric_tooltip(StatMetric metric) {
    switch (metric) {
        case StatMetric::Latest:
            return QCoreApplication::translate("CftcPanel", "Net position of the newest report in the window.");
        case StatMetric::Max:
            return QCoreApplication::translate("CftcPanel", "Highest net position in the selected window.");
        case StatMetric::Min:
            return QCoreApplication::translate("CftcPanel", "Lowest net position in the selected window.");
        case StatMetric::Avg:
            return QCoreApplication::translate("CftcPanel",
                                               "Mean net position over the selected window's real observations.");
        case StatMetric::CotIndex:
            return QCoreApplication::translate(
                "CftcPanel", "COT Index = 100 × (latest − window min) / (window max − window min).\n"
                             "0 = window low, 100 = window high. Undefined when the window has no variance.");
        case StatMetric::Percentile:
            return QCoreApplication::translate("CftcPanel",
                                               "Share of window observations at or below the latest reading (0–100%).");
        case StatMetric::ZScore:
            return QCoreApplication::translate("CftcPanel",
                                               "Z-score = (latest − window mean) / sample standard deviation (n−1).");
        case StatMetric::FromHigh:
            return QCoreApplication::translate("CftcPanel", "Latest minus the window's highest reading.");
        case StatMetric::FromLow:
            return QCoreApplication::translate("CftcPanel", "Latest minus the window's lowest reading.");
        case StatMetric::Change4W:
            return QCoreApplication::translate(
                "CftcPanel", "Latest value minus the most recent observation at least 28 days older.");
        case StatMetric::Change13W:
            break;
    }
    return QCoreApplication::translate("CftcPanel",
                                       "Latest value minus the most recent observation at least 91 days older.");
}

bool stat_metric_readout(StatMetric metric, const CftcWindowStats& stats, QString& text) {
    switch (metric) {
        case StatMetric::Latest:
            if (!stats.has_latest)
                return false;
            text = cftc_signed_net(stats.latest);
            return true;
        case StatMetric::Max:
            if (!stats.has_max)
                return false;
            text = cftc_signed_net(stats.max_value);
            return true;
        case StatMetric::Min:
            if (!stats.has_min)
                return false;
            text = cftc_signed_net(stats.min_value);
            return true;
        case StatMetric::Avg:
            if (!stats.has_avg)
                return false;
            text = signed_decimal(stats.avg, 1);
            return true;
        case StatMetric::CotIndex:
            if (!stats.has_cot_index)
                return false;
            text = QString::number(stats.cot_index, 'f', 1);
            return true;
        case StatMetric::Percentile:
            if (!stats.has_percentile)
                return false;
            text = QString::number(stats.percentile, 'f', 1) + QLatin1Char('%');
            return true;
        case StatMetric::ZScore:
            if (!stats.has_zscore)
                return false;
            text = QString::number(stats.zscore, 'f', 2);
            return true;
        case StatMetric::FromHigh:
            if (!stats.has_distance_high)
                return false;
            text = cftc_signed_net(stats.distance_high);
            return true;
        case StatMetric::FromLow:
            if (!stats.has_distance_low)
                return false;
            text = cftc_signed_net(stats.distance_low);
            return true;
        case StatMetric::Change4W:
            if (!stats.has_change_4w)
                return false;
            text = cftc_signed_net(stats.change_4w);
            return true;
        case StatMetric::Change13W:
            break;
    }
    if (!stats.has_change_13w)
        return false;
    text = cftc_signed_net(stats.change_13w);
    return true;
}

QString percentile_state(double percentile) {
    if (percentile >= 80.0)
        return QCoreApplication::translate("CftcPanel", "UPPER RANGE");
    if (percentile <= 20.0)
        return QCoreApplication::translate("CftcPanel", "LOWER RANGE");
    return QCoreApplication::translate("CftcPanel", "MIDDLE RANGE");
}

void apply_value_state(QLabel* label, int sign) {
    const char* name = sign > 0 ? "econStatPos" : sign < 0 ? "econStatNeg" : "econStatVal";
    if (label->objectName() == QLatin1String(name))
        return;
    label->setObjectName(name);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

void fit_table_height(QTableWidget* table) {
    table->setFixedHeight(24 + table->rowCount() * 20 + 8);
}

} // namespace

// ── Construction ────────────────────────────────────────────────────────────

CftcPanel::CftcPanel(QWidget* parent) : EconPanelBase(kCftcSourceId, kCftcColor, parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    build_result_tabs(root);

    auto* base_host = new QWidget(this);
    root->addWidget(base_host, 1);
    build_base_ui(base_host);
    set_stats_visible(false);
    build_analysis_page();

    result_tabs_->hide();
    retranslateUi();

    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &CftcPanel::on_result);
}

void CftcPanel::activate() {
    clear_workspace();
    show_empty(tr("Select a futures market and report family, then click FETCH\n"
                  "CFTC data is free — no API key required\n"
                  "Analysis is computed locally from the official CFTC weekly reports"));
}

// ── Controls ────────────────────────────────────────────────────────────────

void CftcPanel::build_controls(QHBoxLayout* thl) {
    auto make_lbl = [](const QString& text) {
        auto* label = new QLabel(text);
        label->setStyleSheet(ctrl_label_style());
        return label;
    };

    market_combo_ = new QComboBox;
    for (const auto& market : kMarkets)
        market_combo_->addItem(market.first, market.second);
    market_combo_->setFixedHeight(26);
    market_combo_->setMinimumWidth(160);

    report_combo_ = new QComboBox;
    report_combo_->addItem(QString(), QStringLiteral("legacy"));
    report_combo_->addItem(QString(), QStringLiteral("disaggregated"));
    report_combo_->addItem(QString(), QStringLiteral("tff"));
    report_combo_->setFixedHeight(26);
    report_combo_->setMinimumWidth(120);

    type_combo_ = new QComboBox;
    type_combo_->addItem(QString(), QStringLiteral("futures_only"));
    type_combo_->addItem(QString(), QStringLiteral("combined"));
    type_combo_->setFixedHeight(26);
    type_combo_->setMinimumWidth(120);

    thl->addWidget(market_lbl_ = make_lbl(tr("MARKET")));
    thl->addWidget(market_combo_);
    thl->addWidget(report_lbl_ = make_lbl(tr("REPORT")));
    thl->addWidget(report_combo_);
    thl->addWidget(type_lbl_ = make_lbl(tr("TYPE")));
    thl->addWidget(type_combo_);
}

// ── Result tabs ─────────────────────────────────────────────────────────────

void CftcPanel::build_result_tabs(QVBoxLayout* root) {
    result_tabs_ = new QWidget(this);
    result_tabs_->setObjectName("cftcResultTabs");
    auto* layout = new QHBoxLayout(result_tabs_);
    layout->setContentsMargins(10, 4, 10, 0);
    layout->setSpacing(4);

    analysis_tab_ = new QPushButton(tr("Analysis"), result_tabs_);
    raw_tab_ = new QPushButton(tr("Raw Data"), result_tabs_);
    for (auto* button : {analysis_tab_, raw_tab_}) {
        button->setObjectName("econViewTab");
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setCursor(Qt::PointingHandCursor);
        layout->addWidget(button);
    }
    layout->addStretch(1);
    connect(analysis_tab_, &QPushButton::clicked, this, &CftcPanel::show_analysis_tab);
    connect(raw_tab_, &QPushButton::clicked, this, &CftcPanel::show_raw_tab);
    root->addWidget(result_tabs_);
}

void CftcPanel::show_analysis_tab() {
    if (analysis_page_ >= 0)
        show_content_page(analysis_page_);
    if (analysis_tab_)
        analysis_tab_->setChecked(true);
}

void CftcPanel::show_raw_tab() {
    show_content_page(1);
    if (raw_tab_)
        raw_tab_->setChecked(true);
}

// ── Analysis page construction ──────────────────────────────────────────────

CftcPanel::Section CftcPanel::make_section(const QString& title) {
    Section section;
    section.frame = new QWidget(analysis_content_);
    section.frame->setObjectName("cftcSection");
    section.body = new QVBoxLayout(section.frame);
    section.body->setContentsMargins(12, 10, 12, 10);
    section.body->setSpacing(8);
    section.title = new QLabel(title, section.frame);
    section.title->setObjectName("cftcSectionTitle");
    section.body->addWidget(section.title);
    return section;
}

QTableWidget* CftcPanel::make_table(QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setObjectName("cftcTable");
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(20);
    table->horizontalHeader()->setFixedHeight(20);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setSectionsMovable(false);
    table->horizontalHeader()->setMinimumSectionSize(36);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return table;
}

CftcPanel::SnapshotCard CftcPanel::make_snapshot_card() {
    SnapshotCard card;
    card.frame = new QWidget(analysis_content_);
    card.frame->setObjectName("cftcCard");
    card.frame->setMinimumWidth(118);
    auto* layout = new QVBoxLayout(card.frame);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(2);

    card.caption = new QLabel(card.frame);
    card.caption->setObjectName("cftcCardLabel");
    card.value = new QLabel(QStringLiteral("—"), card.frame);
    card.value->setObjectName("econStatVal");
    card.sub = new QLabel(card.frame);
    card.sub->setObjectName("cftcCardSub");
    card.sub->setWordWrap(true);

    layout->addWidget(card.caption);
    layout->addWidget(card.value);
    layout->addWidget(card.sub);
    return card;
}

void CftcPanel::build_analysis_page() {
    analysis_scroll_ = new QScrollArea(this);
    analysis_scroll_->setWidgetResizable(true);
    analysis_scroll_->setFrameShape(QFrame::NoFrame);
    analysis_scroll_->setObjectName("cftcAnalysisScroll");
    analysis_content_ = new QWidget;
    analysis_content_->setObjectName("cftcAnalysis");
    auto* content_layout = new QVBoxLayout(analysis_content_);
    content_layout->setContentsMargins(12, 10, 12, 12);
    content_layout->setSpacing(10);

    // 1. Header and controls: identity, report/source context and the
    //    analytical history window.
    auto* header = new QWidget(analysis_content_);
    header->setObjectName("cftcSection");
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(12, 10, 12, 10);
    header_layout->setSpacing(6);
    hdr_title_ = new QLabel(QStringLiteral("—"), header);
    hdr_title_->setObjectName("cftcHeaderTitle");
    hdr_meta_ = new QLabel(QStringLiteral("—"), header);
    hdr_meta_->setObjectName("cftcHeaderMeta");
    hdr_meta_->setWordWrap(true);
    hdr_meta_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header_layout->addWidget(hdr_title_);
    header_layout->addWidget(hdr_meta_);

    auto* range_row = new QHBoxLayout;
    range_row->setSpacing(4);
    range_lbl_ = new QLabel(header);
    range_lbl_->setObjectName("cftcSectionTitle");
    range_row->addWidget(range_lbl_);
    const struct {
        CftcRange value;
        const char* text;
    } kRanges[] = {{CftcRange::OneYear, "1Y"},   {CftcRange::TwoYears, "2Y"},     {CftcRange::FiveYears, "5Y"},
                   {CftcRange::TenYears, "10Y"}, {CftcRange::TwentyYears, "20Y"}, {CftcRange::Max, "MAX"}};
    for (const auto& spec : kRanges) {
        auto* button = new QPushButton(QString::fromLatin1(spec.text), header);
        button->setObjectName("cftcRangeBtn");
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, [this, range = spec.value]() { apply_range(range); });
        range_row->addWidget(button);
        range_btns_ << button;
        range_values_ << spec.value;
    }
    range_row->addStretch(1);
    range_info_lbl_ = new QLabel(header);
    range_info_lbl_->setObjectName("cftcSectionMeta");
    range_row->addWidget(range_info_lbl_);
    header_layout->addLayout(range_row);
    content_layout->addWidget(header);

    // 2. Current snapshot.
    auto snapshot = make_section(tr("CURRENT SNAPSHOT"));
    snapshot_title_ = snapshot.title;
    snapshot_grid_ = new QGridLayout;
    snapshot_grid_->setSpacing(8);
    for (int i = 0; i < 7; ++i) {
        snapshot_cards_.append(make_snapshot_card());
        snapshot_grid_->addWidget(snapshot_cards_.last().frame, i / 4, i % 4);
    }
    snapshot.body->addLayout(snapshot_grid_);
    content_layout->addWidget(snapshot.frame);

    // 3 + 4. Positioning and weekly changes share a row when width allows.
    pair_layout_ = new QGridLayout;
    pair_layout_->setSpacing(10);

    auto positioning = make_section(tr("POSITIONING"));
    positioning_frame_ = positioning.frame;
    positioning_title_ = positioning.title;
    positioning_table_ = make_table(positioning.frame);
    positioning.body->addWidget(positioning_table_);

    auto weekly = make_section(tr("WEEKLY CHANGES"));
    weekly_frame_ = weekly.frame;
    weekly_title_ = weekly.title;
    auto* weekly_oi_row = new QHBoxLayout;
    weekly_oi_row->setSpacing(6);
    weekly_oi_caption_ = new QLabel(weekly.frame);
    weekly_oi_caption_->setObjectName("cftcSectionMeta");
    weekly_oi_lbl_ = new QLabel(QStringLiteral("—"), weekly.frame);
    weekly_oi_lbl_->setObjectName("econStatVal");
    weekly_oi_row->addWidget(weekly_oi_caption_);
    weekly_oi_row->addWidget(weekly_oi_lbl_);
    weekly_oi_row->addStretch(1);
    weekly.body->addLayout(weekly_oi_row);
    weekly_table_ = make_table(weekly.frame);
    weekly.body->addWidget(weekly_table_);

    pair_layout_->addWidget(positioning_frame_, 0, 0);
    pair_layout_->addWidget(weekly_frame_, 0, 1);
    content_layout->addLayout(pair_layout_);

    // 5. Historical positioning: the principal large chart.
    auto chart_section = make_section(tr("HISTORICAL POSITIONING"));
    chart_title_ = chart_section.title;
    auto* chart_controls = new QHBoxLayout;
    chart_controls->setSpacing(6);
    chart_metric_lbl_ = new QLabel(chart_section.frame);
    chart_metric_lbl_->setObjectName("cftcSectionMeta");
    chart_metric_combo_ = new QComboBox(chart_section.frame);
    chart_metric_combo_->addItem(QString(), QStringLiteral("net"));
    chart_metric_combo_->addItem(QString(), QStringLiteral("long"));
    chart_metric_combo_->addItem(QString(), QStringLiteral("short"));
    chart_metric_combo_->setFixedHeight(22);
    connect(chart_metric_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { update_chart(); });
    chart_oi_check_ = new QCheckBox(chart_section.frame);
    connect(chart_oi_check_, &QCheckBox::toggled, this, [this]() {
        if (chart_)
            chart_->set_open_interest_visible(chart_oi_check_->isChecked());
    });
    chart_controls->addWidget(chart_metric_lbl_);
    chart_controls->addWidget(chart_metric_combo_);
    chart_controls->addWidget(chart_oi_check_);
    chart_controls->addStretch(1);
    chart_section.body->addLayout(chart_controls);

    auto* participant_row = new QHBoxLayout;
    participant_row->setSpacing(10);
    participant_check_layout_ = participant_row;
    auto* participant_holder = new QWidget(chart_section.frame);
    participant_holder->setLayout(participant_row);
    chart_section.body->addWidget(participant_holder);

    chart_ = new CftcPositioningChart(chart_section.frame);
    chart_section.body->addWidget(chart_);
    content_layout->addWidget(chart_section.frame);

    // 6. Window statistics & extremes; 7. price + positioning divergence.
    auto stats = make_section(tr("POSITIONING STATISTICS & EXTREMES"));
    stats_frame_ = stats.frame;
    stats_title_ = stats.title;
    stats_meta_lbl_ = new QLabel(stats.frame);
    stats_meta_lbl_->setObjectName("cftcSectionMeta");
    stats_meta_lbl_->setWordWrap(true);
    stats.body->addWidget(stats_meta_lbl_);
    stats_table_ = make_table(stats.frame);
    stats_table_->verticalHeader()->setVisible(true);
    stats_table_->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    stats_table_->verticalHeader()->setMinimumWidth(110);
    stats.body->addWidget(stats_table_);

    auto divergence = make_section(tr("PRICE + POSITIONING / DIVERGENCE"));
    divergence_frame_ = divergence.frame;
    divergence_title_ = divergence.title;
    divergence_source_lbl_ = new QLabel(divergence.frame);
    divergence_source_lbl_->setObjectName("cftcSectionMeta");
    divergence_source_lbl_->setWordWrap(true);
    divergence.body->addWidget(divergence_source_lbl_);
    divergence_table_ = make_table(divergence.frame);
    divergence.body->addWidget(divergence_table_);
    divergence_extremes_lbl_ = new QLabel(divergence.frame);
    divergence_extremes_lbl_->setObjectName("cftcSectionMeta");
    divergence_extremes_lbl_->setWordWrap(true);
    divergence.body->addWidget(divergence_extremes_lbl_);

    // Statistics/extremes and divergence follow the principal chart, sharing a
    // row when width allows.
    stats_pair_layout_ = new QGridLayout;
    stats_pair_layout_->setSpacing(10);
    stats_pair_layout_->addWidget(stats_frame_, 0, 0);
    stats_pair_layout_->addWidget(divergence_frame_, 0, 1);
    content_layout->addLayout(stats_pair_layout_);

    // 8. Positioning heatmap.
    auto heatmap = make_section(tr("POSITIONING HEATMAP"));
    heatmap_title_ = heatmap.title;
    auto* heat_controls = new QHBoxLayout;
    heat_controls->setSpacing(6);
    heatmap_metric_lbl_ = new QLabel(heatmap.frame);
    heatmap_metric_lbl_->setObjectName("cftcSectionMeta");
    heatmap_metric_combo_ = new QComboBox(heatmap.frame);
    for (const char* code : {"cot_index", "percentile", "zscore", "weekly_change"})
        heatmap_metric_combo_->addItem(QString(), QString::fromLatin1(code));
    heatmap_metric_combo_->setFixedHeight(22);
    connect(heatmap_metric_combo_, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this]() { update_heatmap(); });
    heat_controls->addWidget(heatmap_metric_lbl_);
    heat_controls->addWidget(heatmap_metric_combo_);
    heat_controls->addStretch(1);
    heatmap.body->addLayout(heat_controls);
    heatmap_meta_lbl_ = new QLabel(heatmap.frame);
    heatmap_meta_lbl_->setObjectName("cftcSectionMeta");
    heatmap_meta_lbl_->setWordWrap(true);
    heatmap.body->addWidget(heatmap_meta_lbl_);
    heatmap_ = new CftcHeatmap(heatmap.frame);
    heatmap.body->addWidget(heatmap_);
    content_layout->addWidget(heatmap.frame);

    content_layout->addStretch(1);
    analysis_scroll_->setWidget(analysis_content_);
    analysis_page_ = add_content_page(analysis_scroll_);
    apply_responsive_layout();
    // build_base_ui() ran before this page existed, so its refresh_panel_theme()
    // could not style it; apply the workspace QSS now (theme changes re-apply
    // it through refresh_panel_theme()).
    analysis_content_->setStyleSheet(workspace_style());
}

void CftcPanel::apply_responsive_layout() {
    if (!snapshot_grid_ || !pair_layout_ || !stats_pair_layout_)
        return;

    const int snapshot_columns = narrow_layout_ ? 2 : 4;
    for (int i = 0; i < snapshot_cards_.size(); ++i) {
        snapshot_grid_->removeWidget(snapshot_cards_[i].frame);
        snapshot_grid_->addWidget(snapshot_cards_[i].frame, i / snapshot_columns, i % snapshot_columns);
    }
    for (int col = 0; col < 4; ++col)
        snapshot_grid_->setColumnStretch(col, col < snapshot_columns ? 1 : 0);

    const QVector<QWidget*> first_row = {positioning_frame_, weekly_frame_};
    const QVector<QWidget*> second_row = {stats_frame_, divergence_frame_};
    for (auto* frame : first_row) {
        if (frame)
            pair_layout_->removeWidget(frame);
    }
    for (auto* frame : second_row) {
        if (frame)
            stats_pair_layout_->removeWidget(frame);
    }
    if (narrow_layout_) {
        for (int i = 0; i < first_row.size(); ++i) {
            if (first_row[i])
                pair_layout_->addWidget(first_row[i], i, 0, 1, 2);
        }
        for (int i = 0; i < second_row.size(); ++i) {
            if (second_row[i])
                stats_pair_layout_->addWidget(second_row[i], i, 0, 1, 2);
        }
    } else {
        if (positioning_frame_)
            pair_layout_->addWidget(positioning_frame_, 0, 0);
        if (weekly_frame_)
            pair_layout_->addWidget(weekly_frame_, 0, 1);
        if (stats_frame_)
            stats_pair_layout_->addWidget(stats_frame_, 0, 0);
        if (divergence_frame_)
            stats_pair_layout_->addWidget(divergence_frame_, 0, 1);
    }
    pair_layout_->setColumnStretch(0, 1);
    pair_layout_->setColumnStretch(1, 1);
    stats_pair_layout_->setColumnStretch(0, 1);
    stats_pair_layout_->setColumnStretch(1, 1);
}

void CftcPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const bool narrow = event->size().width() < kCftcMobileBreakpoint;
    if (narrow != narrow_layout_) {
        narrow_layout_ = narrow;
        apply_responsive_layout();
    }
}

// ── Fetch ───────────────────────────────────────────────────────────────────

void CftcPanel::on_fetch() {
    const QString market = market_combo_->currentData().toString();
    if (market.isEmpty()) {
        clear_workspace();
        show_empty(tr("Select a futures market"));
        return;
    }

    market_key_ = market;
    market_label_ = market_combo_->currentText();
    family_ = cftc_family_from_code(report_combo_->currentData().toString());
    futures_only_ = type_combo_->currentData().toString() == QLatin1String("futures_only");

    clear_workspace();
    show_loading(tr("Fetching full CFTC history for %1…").arg(market_combo_->currentText()));

    pending_request_ = QStringLiteral("cftc_hist_%1").arg(++request_seq_);
    services::EconomicsService::instance().execute(kCftcSourceId, kCftcScript, "cot_history",
                                                   {market, cftc_family_code(family_),
                                                    futures_only_ ? QStringLiteral("true") : QStringLiteral("false"),
                                                    QString::fromLatin1(kCftcMaxRows)},
                                                   pending_request_);
}

void CftcPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != kCftcSourceId)
        return;
    // A newer fetch supersedes an older response, including a stale failure
    // that must not overwrite a newer success.
    if (request_id != pending_request_)
        return;

    if (!result.success) {
        clear_workspace();
        show_error(cftc_error_text(result.error));
        return;
    }

    result_params_ = result.data.value(QStringLiteral("parameters")).toObject();
    dataset_ = result_params_.value(QStringLiteral("dataset")).toString();
    family_ = cftc_family_from_code(result_params_.value(QStringLiteral("report_family")).toString());
    market_key_ = result_params_.value(QStringLiteral("identifier")).toString(market_key_);
    futures_only_ = result_params_.value(QStringLiteral("futures_only")).toBool(futures_only_);

    const CftcHistory history = cftc_parse_history(result.data.value(QStringLiteral("data")).toArray(), family_);
    if (!history.error.isEmpty()) {
        clear_workspace();
        show_error(history.error);
        return;
    }

    history_ = history;
    participants_ = cftc_family_participants(family_);
    speculative_index_ = cftc_speculative_index(participants_);
    range_ = cftc_range_available(history_.observations, CftcRange::TwoYears) ? CftcRange::TwoYears : CftcRange::Max;

    // Raw Data is the same observations, newest first, through the shared
    // table + CSV path. Analysis is shown by default with Raw Data one click
    // away.
    QString title =
        tr("CFTC: %1 — %2 · %3")
            .arg(history_.observations.last().market.isEmpty() ? market_label_ : history_.observations.last().market,
                 family_label(family_), futures_only_ ? tr("Futures Only") : tr("Combined"));
    // The shared result title is visible on both tabs, so it carries the
    // contract identifier too — Raw Data keeps at least that source context.
    if (!history_.observations.last().contract_code.isEmpty())
        title += QStringLiteral(" · ") + history_.observations.last().contract_code;
    display(build_raw_rows(), title);
    build_participant_controls();
    rebuild_workspace();
    result_tabs_->show();
    show_analysis_tab();
    request_price(market_key_);

    LOG_INFO("CftcPanel", QString("Displayed %1 CFTC observations (%2)")
                              .arg(history_.observations.size())
                              .arg(cftc_family_code(family_)));
}

// ── Workspace state ─────────────────────────────────────────────────────────

void CftcPanel::clear_workspace() {
    // Invalidate every in-flight response bound to this workspace state: an
    // older CFTC request must not repopulate a reset/ reactivated panel, and
    // an older price delivery must not attach to a newer market.
    pending_request_.clear();
    ++price_token_;
    if (!price_topic_.isEmpty()) {
        auto& hub = datahub::DataHub::instance();
        hub.unsubscribe(this, price_topic_);
        hub.unsubscribe_errors(this, price_topic_);
        price_topic_.clear();
    }
    price_state_ = PriceState::None;
    price_reason_.clear();
    price_symbol_.clear();
    price_spot_index_ = false;
    price_.clear();
    history_ = {};
    window_.clear();
    participants_.clear();
    speculative_index_ = -1;
    dataset_.clear();
    result_params_ = {};
    if (result_tabs_)
        result_tabs_->hide();
    if (hdr_title_)
        hdr_title_->setText(QStringLiteral("—"));
    if (hdr_meta_)
        hdr_meta_->setText(QStringLiteral("—"));
    if (range_info_lbl_)
        range_info_lbl_->clear();

    for (auto& card : snapshot_cards_) {
        if (card.value)
            card.value->setText(QStringLiteral("—"));
        if (card.sub)
            card.sub->clear();
        if (card.frame)
            card.frame->setToolTip(QString());
    }
    if (positioning_table_) {
        positioning_table_->clearContents();
        positioning_table_->setRowCount(0);
        positioning_table_->setColumnCount(0);
    }
    if (weekly_table_) {
        weekly_table_->clearContents();
        weekly_table_->setRowCount(0);
        weekly_table_->setColumnCount(0);
    }
    if (weekly_oi_lbl_) {
        weekly_oi_lbl_->setText(QStringLiteral("—"));
        apply_value_state(weekly_oi_lbl_, 0);
    }
    if (chart_)
        chart_->clear();
    if (stats_table_) {
        stats_table_->clearContents();
        stats_table_->setRowCount(0);
        stats_table_->setColumnCount(0);
    }
    if (stats_meta_lbl_)
        stats_meta_lbl_->clear();
    if (divergence_table_) {
        divergence_table_->clearContents();
        divergence_table_->setRowCount(0);
        divergence_table_->setColumnCount(0);
    }
    if (divergence_source_lbl_)
        divergence_source_lbl_->clear();
    if (divergence_extremes_lbl_)
        divergence_extremes_lbl_->clear();
    if (heatmap_)
        heatmap_->clear();
    if (heatmap_meta_lbl_)
        heatmap_meta_lbl_->clear();
}

void CftcPanel::build_participant_controls() {
    if (!participant_check_layout_)
        return;
    while (QLayoutItem* item = participant_check_layout_->takeAt(0)) {
        if (auto* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    participant_checks_.clear();

    const auto palette = series_palette();
    QWidget* parent = participant_check_layout_->parentWidget();
    for (int i = 0; i < participants_.size(); ++i) {
        auto* check = new QCheckBox(participants_[i].label, parent);
        check->setChecked(participants_[i].key != QLatin1String("non_reportable"));
        check->setStyleSheet(QStringLiteral("color:%1; font-size:10px; background:transparent;")
                                 .arg(palette[i % palette.size()].name()));
        check->setToolTip(tr("Show %1 in the historical chart").arg(participants_[i].label));
        connect(check, &QCheckBox::toggled, this, [this]() { update_chart(); });
        participant_check_layout_->addWidget(check);
        participant_checks_.append(check);
    }
}

void CftcPanel::apply_range(CftcRange range) {
    if (history_.observations.isEmpty() || !cftc_range_available(history_.observations, range))
        return;
    range_ = range;
    rebuild_workspace();
}

void CftcPanel::rebuild_workspace() {
    if (history_.observations.isEmpty())
        return;
    if (!cftc_range_available(history_.observations, range_))
        range_ = CftcRange::Max;
    window_ = cftc_filter_range(history_.observations, range_);
    refresh_range_buttons();
    update_header();
    update_snapshot();
    update_positioning();
    update_weekly();
    update_chart();
    update_statistics();
    update_divergence();
    update_heatmap();
}

void CftcPanel::refresh_range_buttons() {
    for (int i = 0; i < range_btns_.size(); ++i) {
        const bool available = cftc_range_available(history_.observations, range_values_[i]);
        range_btns_[i]->setEnabled(available);
        range_btns_[i]->setToolTip(available ? QString() : tr("Not enough returned history for this range"));
        if (range_values_[i] == range_) {
            QSignalBlocker block(range_btns_[i]);
            range_btns_[i]->setChecked(true);
        }
    }
}

QVector<CftcDatedValue> CftcPanel::participant_metric_series(int participant_index, CftcChartMetric metric) const {
    return metric_series(window_, participant_index, metric);
}

QDate CftcPanel::latest_report_date() const {
    return history_.observations.isEmpty() ? QDate() : history_.observations.last().date;
}

QJsonArray CftcPanel::build_raw_rows() const {
    QJsonArray rows;
    for (int i = history_.observations.size() - 1; i >= 0; --i) {
        const CftcObservation& obs = history_.observations.at(i);
        QJsonObject row;
        row[QStringLiteral("date")] = obs.date_label;
        row[QStringLiteral("market")] = obs.market;
        row[QStringLiteral("open_interest")] =
            obs.open_interest ? QJsonValue(*obs.open_interest) : QJsonValue(QJsonValue::Null);
        for (int p = 0; p < participants_.size(); ++p) {
            row[participants_[p].key + QStringLiteral("_long")] =
                (p < obs.longs.size() && obs.longs[p]) ? QJsonValue(*obs.longs[p]) : QJsonValue(QJsonValue::Null);
            row[participants_[p].key + QStringLiteral("_short")] =
                (p < obs.shorts.size() && obs.shorts[p]) ? QJsonValue(*obs.shorts[p]) : QJsonValue(QJsonValue::Null);
        }
        rows.append(row);
    }
    return rows;
}

// ── Analysis sections ───────────────────────────────────────────────────────

void CftcPanel::update_header() {
    if (history_.observations.isEmpty())
        return;
    const CftcObservation& latest = history_.observations.last();
    hdr_title_->setText(latest.market.isEmpty() ? market_label_ : latest.market);

    QStringList meta;
    meta << tr("Report %1").arg(latest.date_label);
    meta << family_label(family_);
    meta << (futures_only_ ? tr("Futures Only") : tr("Combined (futures + options)"));
    if (!latest.contract_code.isEmpty()) {
        meta << (latest.units.isEmpty() ? tr("CFTC contract %1").arg(latest.contract_code)
                                        : tr("CFTC contract %1 — %2").arg(latest.contract_code, latest.units));
    }
    QString source = tr("Source: CFTC Commitments of Traders — publicreporting.cftc.gov");
    if (!dataset_.isEmpty())
        source += QStringLiteral(" (%1)").arg(dataset_);
    meta << source;
    const QString retrieved = result_params_.value(QStringLiteral("retrieved_at")).toString();
    if (!retrieved.isEmpty())
        meta << tr("retrieved %1").arg(retrieved);
    hdr_meta_->setText(meta.join(QStringLiteral("  ·  ")));

    if (window_.isEmpty()) {
        range_info_lbl_->clear();
    } else {
        range_info_lbl_->setText(
            tr("%1 reports · %2 → %3").arg(window_.size()).arg(window_.first().date_label, window_.last().date_label));
    }
}

void CftcPanel::update_snapshot() {
    if (snapshot_cards_.size() < 7 || speculative_index_ < 0)
        return;
    const QString range_label = cftc_range_label(range_);
    const QString spec_short = participant_short_label(participants_[speculative_index_].key);
    const QDate as_of_date = latest_report_date();
    const auto spec_series = participant_metric_series(speculative_index_, CftcChartMetric::Net);
    const CftcWindowStats stats = cftc_window_stats(spec_series, as_of_date);
    const CftcObservation& latest = history_.observations.last();

    auto set_card = [&](int index, const QString& caption, const QString& value, int sign, const QString& sub,
                        const QString& tooltip) {
        SnapshotCard& card = snapshot_cards_[index];
        card.caption->setText(caption);
        card.value->setText(value);
        apply_value_state(card.value, sign);
        card.sub->setText(sub);
        card.frame->setToolTip(tooltip);
    };

    // NET. Anchored at the actual latest official report: if that report does
    // not carry this class' legs, the current reading is unavailable — an
    // older value is never presented as current.
    if (stats.has_latest) {
        set_card(0, tr("NET (%1)").arg(spec_short), cftc_signed_net(stats.latest),
                 stats.latest > 0   ? 1
                 : stats.latest < 0 ? -1
                                    : 0,
                 tr("as of %1").arg(as_of_date.toString(Qt::ISODate)),
                 tr("Latest net position of %1 (%2 − %3) as of the newest CFTC report.")
                     .arg(participants_[speculative_index_].label, tr("long"), tr("short")));
    } else {
        set_card(0, tr("NET (%1)").arg(spec_short), QStringLiteral("—"), 0, stat_unavailable_reason(stats, as_of_date),
                 QString());
    }

    // WEEKLY CHANGE
    const CftcChange weekly = cftc_weekly_change(spec_series, as_of_date);
    if (weekly.has_value) {
        set_card(1, tr("WEEKLY CHANGE (%1)").arg(spec_short), cftc_signed_net(weekly.value),
                 weekly.value > 0   ? 1
                 : weekly.value < 0 ? -1
                                    : 0,
                 tr("vs previous report %1").arg(weekly.previous_date.toString(Qt::ISODate)),
                 tr("Difference from the previous report; a gap longer than %1 days is not a weekly change.")
                     .arg(kCftcWeeklyGapDays));
    } else {
        set_card(1, tr("WEEKLY CHANGE (%1)").arg(spec_short), QStringLiteral("—"), 0,
                 weekly_unavailable_reason(weekly, as_of_date),
                 tr("A weekly change requires the latest report and a previous report within %1 days.")
                     .arg(kCftcWeeklyGapDays));
    }

    // OPEN INTEREST
    if (latest.open_interest) {
        set_card(2, tr("OPEN INTEREST"), position_text(*latest.open_interest), 0,
                 tr("contracts · as of %1").arg(latest.date_label),
                 tr("Total open interest reported by CFTC (all contracts), not a participant position."));
    } else {
        set_card(2, tr("OPEN INTEREST"), QStringLiteral("—"), 0, tr("not carried by the latest report"),
                 tr("Open interest is the total contracts reported by CFTC."));
    }

    // OI WEEKLY CHANGE
    const auto oi_series = cftc_open_interest_series(history_.observations);
    const CftcChange oi_weekly = cftc_weekly_change(oi_series, as_of_date);
    if (oi_weekly.has_value) {
        set_card(3, tr("OPEN INTEREST Δ WEEK"), cftc_signed_net(oi_weekly.value),
                 oi_weekly.value > 0   ? 1
                 : oi_weekly.value < 0 ? -1
                                       : 0,
                 tr("vs previous report %1").arg(oi_weekly.previous_date.toString(Qt::ISODate)),
                 tr("Open interest change between the two newest reports."));
    } else {
        set_card(3, tr("OPEN INTEREST Δ WEEK"), QStringLiteral("—"), 0,
                 weekly_unavailable_reason(oi_weekly, as_of_date), QString());
    }

    // COT INDEX / Z-SCORE / PERCENTILE (window-dependent labels)
    const QString cot_caption = tr("COT INDEX (%1)").arg(range_label);
    if (stats.has_cot_index) {
        set_card(4, cot_caption, QString::number(stats.cot_index, 'f', 1), 0,
                 tr("window %1 → %2").arg(cftc_signed_net(stats.min_value), cftc_signed_net(stats.max_value)),
                 stat_metric_tooltip(StatMetric::CotIndex));
    } else {
        set_card(4, cot_caption, QStringLiteral("—"), 0, stat_unavailable_reason(stats, as_of_date),
                 stat_metric_tooltip(StatMetric::CotIndex));
    }

    const QString z_caption = tr("Z-SCORE (%1)").arg(range_label);
    if (stats.has_zscore) {
        set_card(5, z_caption, QString::number(stats.zscore, 'f', 2), 0, tr("vs window mean / σ"),
                 stat_metric_tooltip(StatMetric::ZScore));
    } else {
        set_card(5, z_caption, QStringLiteral("—"), 0, stat_unavailable_reason(stats, as_of_date),
                 stat_metric_tooltip(StatMetric::ZScore));
    }

    const QString pct_caption = tr("PERCENTILE (%1)").arg(range_label);
    if (stats.has_percentile) {
        set_card(6, pct_caption, QString::number(stats.percentile, 'f', 1) + QLatin1Char('%'), 0,
                 percentile_state(stats.percentile), stat_metric_tooltip(StatMetric::Percentile));
    } else {
        set_card(6, pct_caption, QStringLiteral("—"), 0, stat_unavailable_reason(stats, as_of_date),
                 stat_metric_tooltip(StatMetric::Percentile));
    }
}

void CftcPanel::set_plain_cell(QTableWidget* table, int row, int column, const QString& text, int alignment) {
    auto* item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    item->setText(text);
    item->setTextAlignment(static_cast<Qt::Alignment>(alignment));
    item->setForeground(QColor(ui::colors::TEXT_PRIMARY()));
    item->setToolTip(QString());
}

void CftcPanel::set_signed_cell(QTableWidget* table, int row, int column, const std::optional<double>& value,
                                const QString& missing) {
    auto* item = table->item(row, column);
    if (!item) {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    if (!value) {
        item->setText(missing);
        item->setForeground(QColor(ui::colors::TEXT_TERTIARY()));
    } else {
        item->setText(cftc_signed_net(*value));
        if (*value > 0.0)
            item->setForeground(QColor(ui::colors::POSITIVE()));
        else if (*value < 0.0)
            item->setForeground(QColor(ui::colors::NEGATIVE()));
        else
            item->setForeground(QColor(ui::colors::TEXT_PRIMARY()));
    }
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    item->setToolTip(QString());
}

void CftcPanel::update_positioning() {
    if (!positioning_table_ || history_.observations.isEmpty())
        return;
    const CftcObservation& latest = history_.observations.last();
    const int columns = 8;
    positioning_table_->setColumnCount(columns);
    positioning_table_->setRowCount(participants_.size());
    positioning_table_->setHorizontalHeaderLabels({tr("PARTICIPANT"), tr("LONG"), tr("SHORT"), tr("NET"), tr("% LONG"),
                                                   tr("% SHORT"), tr("COT IDX (%1)").arg(cftc_range_label(range_)),
                                                   tr("%ILE (%1)").arg(cftc_range_label(range_))});
    positioning_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int col = 1; col < columns; ++col)
        positioning_table_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Stretch);

    for (int p = 0; p < participants_.size(); ++p) {
        set_plain_cell(positioning_table_, p, 0, participants_[p].label, Qt::AlignLeft | Qt::AlignVCenter);

        const CftcPositionMetrics metrics = cftc_position_metrics(latest, p);
        set_plain_cell(positioning_table_, p, 1,
                       metrics.has_long ? position_text(metrics.long_leg) : QStringLiteral("—"));
        set_plain_cell(positioning_table_, p, 2,
                       metrics.has_short ? position_text(metrics.short_leg) : QStringLiteral("—"));
        set_signed_cell(positioning_table_, p, 3,
                        metrics.has_net ? std::optional<double>(metrics.net_position) : std::nullopt);

        if (metrics.has_long_pct_oi)
            set_plain_cell(positioning_table_, p, 4, QString::number(metrics.long_pct_oi, 'f', 1) + QLatin1Char('%'));
        else
            set_plain_cell(positioning_table_, p, 4, QStringLiteral("—"));
        if (metrics.has_short_pct_oi)
            set_plain_cell(positioning_table_, p, 5, QString::number(metrics.short_pct_oi, 'f', 1) + QLatin1Char('%'));
        else
            set_plain_cell(positioning_table_, p, 5, QStringLiteral("—"));

        const QDate as_of_date = latest_report_date();
        const CftcWindowStats stats = cftc_window_stats(participant_metric_series(p, CftcChartMetric::Net), as_of_date);
        if (stats.has_cot_index)
            set_plain_cell(positioning_table_, p, 6, QString::number(stats.cot_index, 'f', 1));
        else {
            set_plain_cell(positioning_table_, p, 6, QStringLiteral("—"));
            positioning_table_->item(p, 6)->setToolTip(stat_unavailable_reason(stats, as_of_date));
        }
        if (stats.has_percentile)
            set_plain_cell(positioning_table_, p, 7, QString::number(stats.percentile, 'f', 1) + QLatin1Char('%'));
        else {
            set_plain_cell(positioning_table_, p, 7, QStringLiteral("—"));
            positioning_table_->item(p, 7)->setToolTip(stat_unavailable_reason(stats, as_of_date));
        }
    }
    fit_table_height(positioning_table_);
}

void CftcPanel::update_weekly() {
    if (!weekly_table_ || history_.observations.isEmpty())
        return;
    const int columns = 4;
    weekly_table_->setColumnCount(columns);
    weekly_table_->setRowCount(participants_.size());
    weekly_table_->setHorizontalHeaderLabels({tr("PARTICIPANT"), tr("Δ LONG"), tr("Δ SHORT"), tr("Δ NET")});
    weekly_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int col = 1; col < columns; ++col)
        weekly_table_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Stretch);

    const QDate as_of_date = latest_report_date();
    const auto oi_series = cftc_open_interest_series(history_.observations);
    const CftcChange oi_change = cftc_weekly_change(oi_series, as_of_date);
    if (weekly_oi_caption_)
        weekly_oi_caption_->setText(tr("OPEN INTEREST Δ"));
    if (weekly_oi_lbl_) {
        if (oi_change.has_value) {
            weekly_oi_lbl_->setText(cftc_signed_net(oi_change.value));
            apply_value_state(weekly_oi_lbl_, oi_change.value > 0.0 ? 1 : oi_change.value < 0.0 ? -1 : 0);
            weekly_oi_lbl_->setToolTip(tr("vs previous report %1").arg(oi_change.previous_date.toString(Qt::ISODate)));
        } else {
            weekly_oi_lbl_->setText(QStringLiteral("—"));
            apply_value_state(weekly_oi_lbl_, 0);
            weekly_oi_lbl_->setToolTip(weekly_unavailable_reason(oi_change, as_of_date));
        }
    }

    for (int p = 0; p < participants_.size(); ++p) {
        set_plain_cell(weekly_table_, p, 0, participants_[p].label, Qt::AlignLeft | Qt::AlignVCenter);
        const CftcChange long_change =
            cftc_weekly_change(metric_series(history_.observations, p, CftcChartMetric::Long), as_of_date);
        const CftcChange short_change =
            cftc_weekly_change(metric_series(history_.observations, p, CftcChartMetric::Short), as_of_date);
        const CftcChange net_change =
            cftc_weekly_change(metric_series(history_.observations, p, CftcChartMetric::Net), as_of_date);
        for (int col = 0; col < 3; ++col) {
            const CftcChange& change = col == 0 ? long_change : col == 1 ? short_change : net_change;
            set_signed_cell(weekly_table_, p, col + 1,
                            change.has_value ? std::optional<double>(change.value) : std::nullopt);
            if (!change.has_value) {
                if (auto* item = weekly_table_->item(p, col + 1))
                    item->setToolTip(weekly_unavailable_reason(change, as_of_date));
            }
        }
    }
    fit_table_height(weekly_table_);
}

void CftcPanel::update_chart() {
    if (!chart_)
        return;
    const CftcChartMetric metric = cftc_chart_metric_from_code(chart_metric_combo_->currentData().toString());
    const auto palette = series_palette();

    QVector<CftcChartSeries> series;
    for (int p = 0; p < participants_.size(); ++p) {
        const bool visible = p < participant_checks_.size() && participant_checks_[p]->isChecked();
        if (!visible)
            continue;
        CftcChartSeries chart_series;
        chart_series.key = participants_[p].key;
        chart_series.label = participants_[p].label;
        chart_series.color = palette[p % palette.size()];
        chart_series.points = cftc_to_time_series(participant_metric_series(p, metric));
        series.append(chart_series);
    }

    const QVector<ui::TimeSeriesPoint> open_interest = cftc_to_time_series(cftc_open_interest_series(window_));
    chart_->set_series(series, open_interest);
    chart_->set_open_interest_visible(chart_oi_check_ && chart_oi_check_->isChecked());
}

void CftcPanel::update_statistics() {
    if (!stats_table_ || window_.isEmpty())
        return;
    stats_table_->setColumnCount(participants_.size());
    stats_table_->setRowCount(kStatMetricCount);
    QStringList headers;
    for (const auto& participant : participants_)
        headers << participant_short_label(participant.key);
    stats_table_->setHorizontalHeaderLabels(headers);
    stats_table_->verticalHeader()->setVisible(true);
    stats_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    const QDate as_of_date = latest_report_date();
    QVector<CftcWindowStats> stats;
    stats.reserve(participants_.size());
    for (int p = 0; p < participants_.size(); ++p)
        stats.append(cftc_window_stats(participant_metric_series(p, CftcChartMetric::Net), as_of_date));

    for (int row = 0; row < kStatMetricCount; ++row) {
        const StatMetric metric = stat_metric_at(row);
        const QString label = stat_metric_is_window_scoped(metric)
                                  ? tr("%1 (%2)").arg(stat_metric_label(metric), cftc_range_label(range_))
                                  : stat_metric_label(metric);
        auto* header = new QTableWidgetItem(label);
        header->setToolTip(stat_metric_tooltip(metric));
        stats_table_->setVerticalHeaderItem(row, header);
        for (int p = 0; p < participants_.size(); ++p) {
            QString text;
            if (stat_metric_readout(metric, stats[p], text))
                set_plain_cell(stats_table_, row, p, text);
            else {
                set_plain_cell(stats_table_, row, p, QStringLiteral("—"));
                stats_table_->item(row, p)->setToolTip(stat_unavailable_reason(stats[p], as_of_date));
            }
        }
    }
    fit_table_height(stats_table_);

    if (stats_meta_lbl_) {
        stats_meta_lbl_->setText(
            tr("Window %1 · %2 reports · %3 → %4 — statistical summaries of real observations; an extreme "
               "reading is not a prediction. Latest-window measures are anchored at the %5 report.")
                .arg(cftc_range_label(range_))
                .arg(window_.size())
                .arg(window_.first().date_label, window_.last().date_label)
                .arg(as_of_date.toString(Qt::ISODate)));
        stats_meta_lbl_->setToolTip(stat_metric_tooltip(StatMetric::CotIndex));
    }
}

void CftcPanel::request_price(const QString& market_key) {
    price_market_key_ = market_key;
    price_.clear();
    price_reason_.clear();
    price_symbol_.clear();
    price_spot_index_ = false;
    const int token = ++price_token_;

    auto& hub = datahub::DataHub::instance();
    if (!price_topic_.isEmpty()) {
        hub.unsubscribe(this, price_topic_);
        hub.unsubscribe_errors(this, price_topic_);
        price_topic_.clear();
    }

    const auto it = price_specs().constFind(market_key);
    if (it == price_specs().constEnd()) {
        price_state_ = PriceState::Unavailable;
        price_reason_ = tr("no retained free price source is mapped for this market");
        update_divergence();
        return;
    }
    price_symbol_ = it->symbol;
    price_spot_index_ = it->spot_index;
    price_state_ = PriceState::Pending;
    update_divergence();

    // Price context comes from the retained free public path through its
    // DataHub producer/topic (market:history:*), not a direct service call;
    // see the D4 rule in the lint workflow. The subscription is owned by this
    // panel (auto-cancelled on destruction) and re-pointed on every fetch.
    price_topic_ = QStringLiteral("market:history:%1:max:1d").arg(price_symbol_);
    hub.subscribe<QVector<services::HistoryPoint>>(
        this, price_topic_, [this, token](const QVector<services::HistoryPoint>& points) {
            if (token != price_token_)
                return; // superseded by a newer fetch or a cleared workspace
            if (points.isEmpty()) {
                price_state_ = PriceState::Unavailable;
                price_reason_ = tr("the price provider returned no observations for this symbol");
                update_divergence();
                return;
            }
            update_price_points(points);
        });
    hub.subscribe_errors(this, price_topic_, [this, token](const QString& error) {
        if (token != price_token_)
            return;
        price_state_ = PriceState::Unavailable;
        price_reason_ = error.trimmed().isEmpty()
                            ? tr("the Yahoo Finance history request failed")
                            : tr("the Yahoo Finance history request failed: %1").arg(error.trimmed());
        update_divergence();
    });
    hub.request(price_topic_, /*force=*/true);
}

void CftcPanel::update_price_points(const QVector<services::HistoryPoint>& points) {
    QVector<CftcPricePoint> prices;
    prices.reserve(points.size());
    int dropped = 0;
    for (const auto& point : points) {
        if (!std::isfinite(point.close) || point.close <= 0.0) {
            ++dropped;
            continue;
        }
        const QDate date = QDateTime::fromSecsSinceEpoch(point.timestamp).date();
        if (!date.isValid()) {
            ++dropped;
            continue;
        }
        prices.append({date, point.close});
    }
    std::stable_sort(prices.begin(), prices.end(),
                     [](const CftcPricePoint& a, const CftcPricePoint& b) { return a.date < b.date; });
    if (dropped > 0)
        LOG_WARN("CftcPanel", QString("Dropped %1 unusable price rows for %2").arg(dropped).arg(price_symbol_));

    price_ = prices;
    if (price_.isEmpty()) {
        price_state_ = PriceState::Unavailable;
        price_reason_ = tr("all returned price rows were unusable");
    } else {
        price_state_ = PriceState::Ready;
    }
    update_divergence();
}

QString CftcPanel::price_source_text() const {
    if (price_symbol_.isEmpty())
        return {};
    if (price_spot_index_) {
        return tr("Price: Yahoo Finance — %1 (spot index, not the CFTC-report futures contract). "
                  "Separate from CFTC data. Alignment below is descriptive (same sign), not a trading signal.")
            .arg(price_symbol_);
    }
    return tr("Price: Yahoo Finance — %1 front-month continuous futures (rolls between contracts; not an "
              "individual deliverable contract). Separate from CFTC data. Alignment below is descriptive "
              "(same sign), not a trading signal.")
        .arg(price_symbol_);
}

void CftcPanel::update_divergence() {
    if (!divergence_table_ || speculative_index_ < 0)
        return;
    const QString spec_label = participants_[speculative_index_].label;

    if (price_state_ == PriceState::Pending) {
        divergence_source_lbl_->setText(tr("Loading price context from Yahoo Finance…"));
        divergence_table_->setRowCount(0);
        divergence_table_->setColumnCount(0);
        divergence_extremes_lbl_->clear();
        return;
    }
    if (price_state_ != PriceState::Ready) {
        divergence_source_lbl_->setText(tr("Price analysis unavailable: %1.").arg(price_reason_));
        divergence_table_->setRowCount(0);
        divergence_table_->setColumnCount(0);
        divergence_extremes_lbl_->clear();
        return;
    }
    divergence_source_lbl_->setText(price_source_text());

    const QDate report_date = history_.observations.last().date;
    QVector<CftcPricePoint> aligned;
    for (const auto& point : std::as_const(price_)) {
        if (point.date <= report_date)
            aligned.append(point);
        else
            break;
    }
    if (aligned.isEmpty()) {
        divergence_source_lbl_->setText(
            tr("Price analysis unavailable: no price close on or before the latest report date."));
        divergence_table_->setRowCount(0);
        divergence_table_->setColumnCount(0);
        divergence_extremes_lbl_->clear();
        return;
    }

    const auto net_series = participant_metric_series(speculative_index_, CftcChartMetric::Net);
    if (net_series.size() < 2) {
        divergence_source_lbl_->setText(
            tr("Price analysis unavailable: fewer than two positioning observations in the window."));
        divergence_table_->setRowCount(0);
        divergence_table_->setColumnCount(0);
        divergence_extremes_lbl_->clear();
        return;
    }
    // The positioning series must reach the same latest official report the
    // price series is truncated to; otherwise a comparison would silently pair
    // this report's price with an older positioning snapshot.
    if (net_series.last().date != report_date) {
        divergence_source_lbl_->setText(
            tr("Price analysis unavailable: the latest report (%1) does not carry %2 positions.")
                .arg(report_date.toString(Qt::ISODate), spec_label));
        divergence_table_->setRowCount(0);
        divergence_table_->setColumnCount(0);
        divergence_extremes_lbl_->clear();
        return;
    }

    const std::optional<double> net_4w = cftc_change_since_days(net_series, 28, report_date);
    const std::optional<double> net_13w = cftc_change_since_days(net_series, 91, report_date);
    const std::optional<double> price_4w = cftc_price_change_since_days(aligned, 28);
    const std::optional<double> price_13w = cftc_price_change_since_days(aligned, 91);

    const double net_window = net_series.last().value - net_series.first().value;
    const std::optional<double> price_start = cftc_price_on_or_before(aligned, net_series.first().date);
    const std::optional<double> price_window =
        price_start ? std::optional<double>(aligned.last().close - *price_start) : std::nullopt;

    divergence_table_->setColumnCount(4);
    divergence_table_->setRowCount(3);
    divergence_table_->setHorizontalHeaderLabels(
        {tr("MEASURE"), tr("PRICE"), tr("NET (%1)").arg(participant_short_label(participants_[speculative_index_].key)),
         tr("ALIGNMENT")});
    divergence_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int col = 1; col < 4; ++col)
        divergence_table_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Stretch);

    struct Row {
        QString label;
        std::optional<double> price;
        std::optional<double> net;
    };
    const Row rows[3] = {
        {tr("Δ 4 WEEK"), price_4w, net_4w},
        {tr("Δ 13 WEEK"), price_13w, net_13w},
        {tr("WINDOW Δ"), price_window, std::optional<double>(net_window)},
    };

    for (int row = 0; row < 3; ++row) {
        set_plain_cell(divergence_table_, row, 0, rows[row].label, Qt::AlignLeft | Qt::AlignVCenter);
        if (rows[row].price) {
            auto* item = divergence_table_->item(row, 1);
            if (!item) {
                item = new QTableWidgetItem;
                divergence_table_->setItem(row, 1, item);
            }
            item->setText(signed_decimal(*rows[row].price, 2));
            item->setForeground(QColor(*rows[row].price > 0.0   ? ui::colors::POSITIVE()
                                       : *rows[row].price < 0.0 ? ui::colors::NEGATIVE()
                                                                : ui::colors::TEXT_PRIMARY()));
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        } else {
            set_signed_cell(divergence_table_, row, 1, std::nullopt);
        }
        set_signed_cell(divergence_table_, row, 2, rows[row].net);

        const CftcDirection price_direction = cftc_direction(rows[row].price);
        const CftcDirection net_direction = cftc_direction(rows[row].net);
        const bool comparison_unavailable =
            price_direction == CftcDirection::Unavailable || net_direction == CftcDirection::Unavailable;
        QString alignment;
        if (comparison_unavailable)
            alignment = QStringLiteral("—");
        else if (!cftc_directions_aligned(price_direction, net_direction))
            alignment = tr("Diverging");
        else if (price_direction == CftcDirection::Up)
            alignment = tr("Aligned up");
        else if (price_direction == CftcDirection::Down)
            alignment = tr("Aligned down");
        else
            alignment = tr("Both flat");
        set_plain_cell(divergence_table_, row, 3, alignment);
        if (comparison_unavailable)
            divergence_table_->item(row, 3)->setToolTip(
                tr("Price or positioning history does not reach far enough back for this comparison."));
    }
    fit_table_height(divergence_table_);

    if (speculative_index_ >= 0) {
        QDate high_date;
        QDate low_date;
        QStringList extremes;
        if (cftc_net_extreme_dates(net_series, high_date, low_date)) {
            const auto high_price = cftc_price_on_or_before(aligned, high_date);
            const auto low_price = cftc_price_on_or_before(aligned, low_date);
            extremes << tr("NET HIGH %1 at %2")
                            .arg(high_date.toString(Qt::ISODate),
                                 high_price ? QString::number(*high_price, 'f', 2) : tr("price unavailable"));
            extremes << tr("NET LOW %1 at %2")
                            .arg(low_date.toString(Qt::ISODate),
                                 low_price ? QString::number(*low_price, 'f', 2) : tr("price unavailable"));
        }
        divergence_extremes_lbl_->setText(
            tr("Extremes (price is the most recent close on or before the report date): %1")
                .arg(extremes.join(QStringLiteral("  ·  "))));
    }
}

void CftcPanel::update_heatmap() {
    if (!heatmap_ || window_.isEmpty() || speculative_index_ < 0)
        return;
    const QString metric_code =
        heatmap_metric_combo_ ? heatmap_metric_combo_->currentData().toString() : QStringLiteral("cot_index");

    // The horizontal axis is the official report dates of the window, not one
    // participant's valid observations: a report stays a column even when a
    // class is missing on it (that row's cell is blank).
    const QVector<QDate> dates = cftc_report_dates(window_, kCftcHeatmapColumns);

    CftcHeatmap::Scale scale = CftcHeatmap::Scale::Sequential;
    if (metric_code == QLatin1String("zscore"))
        scale = CftcHeatmap::Scale::Diverging;
    else if (metric_code == QLatin1String("weekly_change"))
        scale = CftcHeatmap::Scale::Sign;

    QVector<CftcHeatmapRow> rows;
    double max_change = 0.0;
    for (int p = 0; p < participants_.size(); ++p) {
        const auto points =
            cftc_heatmap_series(participant_metric_series(p, CftcChartMetric::Net), kCftcHeatmapColumns);
        QHash<QDate, CftcHeatmapPoint> by_date;
        for (const auto& point : points)
            by_date.insert(point.date, point);

        CftcHeatmapRow row;
        row.label = participants_[p].label;
        row.short_label = participant_short_label(participants_[p].key);
        for (const QDate& date : std::as_const(dates)) {
            const auto it = by_date.constFind(date);
            CftcHeatmapCell cell;
            if (it == by_date.constEnd()) {
                cell.tooltip = tr("%1 — %2\nNo net position observation at this report.")
                                   .arg(participants_[p].label, date.toString(Qt::ISODate));
            } else {
                const CftcHeatmapPoint& point = *it;
                bool has_metric = false;
                QString metric_text;
                if (metric_code == QLatin1String("cot_index")) {
                    has_metric = point.has_cot_index;
                    cell.value = point.cot_index;
                    metric_text = tr("COT Index %1").arg(QString::number(point.cot_index, 'f', 1));
                } else if (metric_code == QLatin1String("percentile")) {
                    has_metric = point.has_percentile;
                    cell.value = point.percentile;
                    metric_text = tr("Percentile %1").arg(QString::number(point.percentile, 'f', 1) + QLatin1Char('%'));
                } else if (metric_code == QLatin1String("zscore")) {
                    has_metric = point.has_zscore;
                    cell.value = point.zscore;
                    metric_text = tr("Z-score %1").arg(QString::number(point.zscore, 'f', 2));
                } else {
                    has_metric = point.has_change;
                    cell.value = point.change;
                    metric_text = tr("Weekly Δ %1").arg(cftc_signed_net(point.change));
                    max_change = std::max(max_change, std::abs(point.change));
                }
                cell.has_value = has_metric;
                cell.tooltip = has_metric ? tr("%1 — %2\nNet %3\n%4")
                                                .arg(participants_[p].label, date.toString(Qt::ISODate),
                                                     cftc_signed_net(point.net), metric_text)
                                          : tr("%1 — %2\nNet %3\nNot available for this report.")
                                                .arg(participants_[p].label, date.toString(Qt::ISODate),
                                                     cftc_signed_net(point.net));
            }
            row.cells.append(cell);
        }
        rows.append(row);
    }

    if (metric_code == QLatin1String("cot_index"))
        heatmap_->set_scale_labels(QStringLiteral("0"), QStringLiteral("100"));
    else if (metric_code == QLatin1String("percentile"))
        heatmap_->set_scale_labels(QStringLiteral("0%"), QStringLiteral("100%"));
    else if (metric_code == QLatin1String("zscore"))
        heatmap_->set_scale_labels(QStringLiteral("-2.5σ"), QStringLiteral("+2.5σ"));
    else
        heatmap_->set_scale_labels(cftc_signed_net(-max_change), cftc_signed_net(max_change));
    heatmap_->set_data(dates, rows, scale);

    if (heatmap_meta_lbl_) {
        if (dates.isEmpty()) {
            heatmap_meta_lbl_->clear();
        } else {
            heatmap_meta_lbl_->setText(
                tr("Last %1 reports in the window. Each column uses the window's observations up to that report "
                   "(no lookahead); fewer than %2 observations stays blank.")
                    .arg(dates.size())
                    .arg(kCftcHeatmapMinObservations));
        }
    }
}

// ── Theme / i18n ────────────────────────────────────────────────────────────

void CftcPanel::refresh_panel_theme() {
    EconPanelBase::refresh_panel_theme();
    if (result_tabs_)
        result_tabs_->setStyleSheet(panel_style());
    if (analysis_content_)
        analysis_content_->setStyleSheet(workspace_style());
    if (chart_)
        chart_->refresh_theme();
    if (heatmap_)
        heatmap_->refresh_theme();
    if (participant_check_layout_) {
        const auto palette = series_palette();
        for (int i = 0; i < participant_checks_.size(); ++i) {
            participant_checks_[i]->setStyleSheet(QStringLiteral("color:%1; font-size:10px; background:transparent;")
                                                      .arg(palette[i % palette.size()].name()));
        }
    }
}

QString CftcPanel::workspace_style() const {
    using namespace ui::colors;
    return QString("#cftcSection { background:%1; border:1px solid %2; border-radius:3px; }"
                   "#cftcSectionTitle { color:%3; font-size:9px; font-weight:700; letter-spacing:1px;"
                   " background:transparent; }"
                   "#cftcSectionMeta { color:%4; font-size:9px; background:transparent; }"
                   "#cftcHeaderTitle { color:%5; font-size:15px; font-weight:700; background:transparent; }"
                   "#cftcHeaderMeta { color:%4; font-size:10px; background:transparent; }"
                   "#cftcRangeBtn { background:transparent; color:%3; border:1px solid %2;"
                   " font-size:10px; font-weight:700; padding:3px 9px; }"
                   "#cftcRangeBtn:hover { color:%5; background:%6; }"
                   "#cftcRangeBtn:checked { background:%7; color:%8; border-color:%7; }"
                   "#cftcRangeBtn:disabled { color:%4; }"
                   "#cftcCard { background:%9; border:1px solid %2; border-radius:3px; }"
                   "#cftcCardLabel { color:%3; font-size:8px; font-weight:700; letter-spacing:1px;"
                   " background:transparent; }"
                   "#cftcCardSub { color:%4; font-size:9px; background:transparent; }"
                   "#cftcTable { background:transparent; color:%5; border:none; font-size:10px; }"
                   "#cftcTable QHeaderView::section { background:%9; color:%3; border:none;"
                   " border-bottom:1px solid %2; font-size:9px; font-weight:700; padding:3px; }"
                   "#cftcTable::item { padding:2px 4px; }")
        .arg(BG_SURFACE())    // %1
        .arg(BORDER_DIM())    // %2
        .arg(TEXT_TERTIARY()) // %3
        .arg(TEXT_DIM())      // %4
        .arg(TEXT_PRIMARY())  // %5
        .arg(BG_HOVER())      // %6
        .arg(color_)          // %7
        .arg(BG_BASE())       // %8
        .arg(BG_RAISED());    // %9
}

void CftcPanel::retranslateUi() {
    if (market_lbl_)
        market_lbl_->setText(tr("MARKET"));
    if (report_lbl_)
        report_lbl_->setText(tr("REPORT"));
    if (type_lbl_)
        type_lbl_->setText(tr("TYPE"));
    if (report_combo_ && report_combo_->count() == 3) {
        report_combo_->setItemText(0, tr("Legacy"));
        report_combo_->setItemText(1, tr("Disaggregated"));
        report_combo_->setItemText(2, tr("Financial (TFF)"));
    }
    if (type_combo_ && type_combo_->count() == 2) {
        type_combo_->setItemText(0, tr("Futures Only"));
        type_combo_->setItemText(1, tr("Combined"));
    }
    if (analysis_tab_)
        analysis_tab_->setText(tr("Analysis"));
    if (raw_tab_)
        raw_tab_->setText(tr("Raw Data"));
    if (snapshot_title_)
        snapshot_title_->setText(tr("CURRENT SNAPSHOT"));
    if (positioning_title_)
        positioning_title_->setText(tr("POSITIONING"));
    if (weekly_title_)
        weekly_title_->setText(tr("WEEKLY CHANGES"));
    if (chart_title_)
        chart_title_->setText(tr("HISTORICAL POSITIONING"));
    if (stats_title_)
        stats_title_->setText(tr("POSITIONING STATISTICS & EXTREMES"));
    if (divergence_title_)
        divergence_title_->setText(tr("PRICE + POSITIONING / DIVERGENCE"));
    if (heatmap_title_)
        heatmap_title_->setText(tr("POSITIONING HEATMAP"));
    if (range_lbl_)
        range_lbl_->setText(tr("HISTORY"));
    if (chart_metric_lbl_)
        chart_metric_lbl_->setText(tr("SERIES"));
    if (chart_metric_combo_ && chart_metric_combo_->count() == 3) {
        chart_metric_combo_->setItemText(0, tr("Net"));
        chart_metric_combo_->setItemText(1, tr("Long"));
        chart_metric_combo_->setItemText(2, tr("Short"));
    }
    if (chart_oi_check_)
        chart_oi_check_->setText(tr("Open Interest (right axis)"));
    if (heatmap_metric_lbl_)
        heatmap_metric_lbl_->setText(tr("METRIC"));
    if (heatmap_metric_combo_ && heatmap_metric_combo_->count() == 4) {
        heatmap_metric_combo_->setItemText(0, tr("COT Index"));
        heatmap_metric_combo_->setItemText(1, tr("Percentile"));
        heatmap_metric_combo_->setItemText(2, tr("Z-Score"));
        heatmap_metric_combo_->setItemText(3, tr("Weekly Δ Net"));
    }

    if (!history_.observations.isEmpty())
        rebuild_workspace();

    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
