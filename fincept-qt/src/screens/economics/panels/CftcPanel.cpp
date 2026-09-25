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
#include "screens/economics/panels/CftcInterpretationPresentation.h"
#include "screens/economics/panels/CftcNetFormat.h"
#include "screens/economics/panels/CftcPositioningChart.h"
#include "screens/economics/panels/CftcPricePositioningChart.h"
#include "screens/economics/panels/CftcSyncChartData.h"
#include "screens/economics/panels/CftcWorkspaceContract.h"
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
using services::cftc_family_code;
using services::cftc_family_from_code;
using services::cftc_family_participants;
using services::cftc_filter_range;
using services::cftc_heatmap_series;
using services::cftc_history_basis_error;
using services::cftc_interpret;
using services::cftc_metric_series;
using services::cftc_net_extreme_dates;
using services::cftc_open_interest_series;
using services::cftc_parse_history;
using services::cftc_position_metrics;
using services::cftc_price_on_or_before;
using services::cftc_range_available;
using services::cftc_range_label;
using services::cftc_report_dates;
using services::cftc_speculative_index;
using services::cftc_weekly_change;
using services::cftc_window_stats;
using services::CftcChange;
using services::CftcDatedValue;
using services::CftcFamily;
using services::CftcHeatmapPoint;
using services::CftcHistory;
using services::CftcInterpretationInput;
using services::CftcInterpretationResult;
using services::CftcMetricKind;
using services::CftcObservation;
using services::CftcPositionMetrics;
using services::CftcPricePoint;
using services::CftcRange;
using services::CftcWindowStats;
using services::kCftcHeatmapMinObservations;
using services::kCftcWeeklyGapDays;
using services::kCftcWeeklyMinGapDays;

namespace {

static constexpr const char* kCftcScript = "cftc_data.py";
static constexpr const char* kCftcSourceId = "cftc";
static constexpr const char* kCftcColor = "#FF5722"; // deep orange
static constexpr const char* kCftcMaxRows = "20000";
static constexpr const char* kCftcMonitorMaxRows = "25000";
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
    {"Nikkei 225 (Yen)", "nikkei"},
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
// are front-month proxies that roll between contracts without roll adjustment,
// and the provider does not identify the roll dates, so the interpretation
// engine derives no price move or relationship from them
// (PriceSeriesNotRollSafe); the chart still shows the series. Spot indices
// have no contract roll, are evaluated, and are labelled as spot indices.
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
    if (change.gap_days < kCftcWeeklyMinGapDays)
        return QCoreApplication::translate("CftcPanel",
                                           "previous report is only %1 days earlier (an extra report, not a weekly "
                                           "interval of %2-%3 days)")
            .arg(change.gap_days)
            .arg(kCftcWeeklyMinGapDays)
            .arg(kCftcWeeklyGapDays);
    return QCoreApplication::translate("CftcPanel",
                                       "previous report is %1 days earlier (longer than a weekly interval of %2-%3 "
                                       "days)")
        .arg(change.gap_days)
        .arg(kCftcWeeklyMinGapDays)
        .arg(kCftcWeeklyGapDays);
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
            return QCoreApplication::translate("CftcPanel", "WINDOW PERCENTILE");
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
                "CftcPanel", "COT Index = 100 × (latest − window min) / (window max − window min), on raw net "
                             "contracts within the selected history range, including the latest report.\n"
                             "0 = window low, 100 = window high. Undefined when the window has no variance.\n"
                             "Not the COT interpretation's historical percentile, which ranks Net %OI against the "
                             "previous 156 reports.");
        case StatMetric::Percentile:
            return cftc_window_percentile_tooltip();
        case StatMetric::ZScore:
            return QCoreApplication::translate(
                "CftcPanel", "Z-score = (latest − window mean) / sample standard deviation (n−1), on raw net contracts "
                             "within the selected history range, including the latest report.");
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
    build_monitor_page();

    result_tabs_->hide();
    retranslateUi();

    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &CftcPanel::on_result);
}

void CftcPanel::activate() {
    clear_workspace();
    // A completed cross-market scan is kept across activations; the monitored
    // workspace is the natural landing view once it exists. Without one, the
    // established single-market empty state is unchanged.
    if (monitor_rendered_ && !monitor_entries_.isEmpty()) {
        result_tabs_->show();
        show_monitor_tab();
        return;
    }
    show_empty(tr("Select a futures market and report family, then click FETCH\n"
                  "CFTC data is free — no API key required\n"
                  "Analysis is computed locally from the official CFTC weekly reports\n"
                  "Use the MARKETS button for the cross-market monitor"));
}

// ── Controls ────────────────────────────────────────────────────────────────

void CftcPanel::build_controls(QHBoxLayout* thl) {
    auto make_lbl = [](const QString& text) {
        auto* label = new QLabel(text);
        // The toolbar identity labels sit on the dark workspace background; the
        // neutral secondary token keeps them readable without hardcoding a
        // one-off color.
        label->setStyleSheet(QStringLiteral("color:%1; font-size:10px; font-weight:700; letter-spacing:1px;")
                                 .arg(ui::colors::TEXT_SECONDARY()));
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

    markets_btn_ = new QPushButton(tr("MARKETS"), this);
    markets_btn_->setObjectName("cftcRangeBtn");
    markets_btn_->setCursor(Qt::PointingHandCursor);
    markets_btn_->setToolTip(tr("Cross-market COT monitor: inspect the current validated state of every supported "
                                "market and open any market's detailed workspace."));
    connect(markets_btn_, &QPushButton::clicked, this, &CftcPanel::on_markets_button);

    thl->addWidget(market_lbl_ = make_lbl(tr("MARKET")));
    thl->addWidget(market_combo_);
    thl->addWidget(report_lbl_ = make_lbl(tr("REPORT")));
    thl->addWidget(report_combo_);
    thl->addWidget(type_lbl_ = make_lbl(tr("TYPE")));
    thl->addWidget(type_combo_);
    thl->addWidget(markets_btn_);
}

// ── Result tabs ─────────────────────────────────────────────────────────────

void CftcPanel::build_result_tabs(QVBoxLayout* root) {
    result_tabs_ = new QWidget(this);
    result_tabs_->setObjectName("cftcResultTabs");
    auto* layout = new QHBoxLayout(result_tabs_);
    layout->setContentsMargins(10, 4, 10, 0);
    layout->setSpacing(4);

    monitor_tab_ = new QPushButton(tr("Markets"), result_tabs_);
    analysis_tab_ = new QPushButton(tr("Analysis"), result_tabs_);
    raw_tab_ = new QPushButton(tr("Raw Data"), result_tabs_);
    for (auto* button : {monitor_tab_, analysis_tab_, raw_tab_}) {
        button->setObjectName("econViewTab");
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setCursor(Qt::PointingHandCursor);
        layout->addWidget(button);
    }
    layout->addStretch(1);
    connect(monitor_tab_, &QPushButton::clicked, this, &CftcPanel::show_monitor_tab);
    connect(analysis_tab_, &QPushButton::clicked, this, &CftcPanel::show_analysis_tab);
    connect(raw_tab_, &QPushButton::clicked, this, &CftcPanel::show_raw_tab);
    root->addWidget(result_tabs_);
}

void CftcPanel::show_monitor_tab() {
    if (monitor_page_ >= 0)
        show_content_page(monitor_page_);
    if (monitor_tab_)
        monitor_tab_->setChecked(true);
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

QTableWidget* CftcPanel::make_monitor_table(QWidget* parent) {
    auto* table = new QTableWidget(parent);
    table->setObjectName("cftcTable");
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(21);
    table->horizontalHeader()->setFixedHeight(20);
    table->horizontalHeader()->setHighlightSections(false);
    table->horizontalHeader()->setSectionsMovable(false);
    table->horizontalHeader()->setMinimumSectionSize(48);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table->setCursor(Qt::PointingHandCursor);
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

    // The Analysis-page information hierarchy is governed by
    // CftcWorkspaceContract.h; this construction order must stay in sync with
    // cftc_workspace_section_order() (guarded below).
    int last_section_index = -1;
    const QVector<CftcWorkspaceSection> section_order = cftc_workspace_section_order();
    const auto note_section = [&last_section_index, &section_order](CftcWorkspaceSection section) {
        const int index = section_order.indexOf(section);
        if (index < 0 || !cftc_section_order_stays_monotonic(last_section_index, index)) {
            Q_ASSERT_X(false, "CftcPanel", "CFTC analysis section order violates the workspace contract");
            LOG_ERROR("CftcPanel", QStringLiteral("CFTC analysis section order violates the workspace contract"));
            return;
        }
        last_section_index = index;
    };

    // 1. Header and controls: identity, report/source context and the
    //    analytical history-display range with an explicit purpose statement.
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
    range_hint_lbl_ = new QLabel(header);
    range_hint_lbl_->setObjectName("cftcSectionMeta");
    range_hint_lbl_->setWordWrap(true);
    header_layout->addWidget(range_hint_lbl_);
    content_layout->addWidget(header);
    note_section(CftcWorkspaceSection::Header);

    // 2. Current snapshot: the latest official report at a glance, before any
    //    graph or deeper table.
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
    note_section(CftcWorkspaceSection::CurrentSnapshot);

    // 3 + 4 + 5. The compact interpretation card and the participant detail
    //    tables share the top row: the interpretation never spans the full
    //    content width, and Positioning | Weekly Changes sit beside it instead
    //    of below the charts.
    top_row_layout_ = new QGridLayout;
    top_row_layout_->setSpacing(10);

    auto interpretation = make_section(tr("COT INTERPRETATION"));
    interpretation_frame_ = interpretation.frame;
    interpretation_title_ = interpretation.title;
    auto* interpretation_controls = new QHBoxLayout;
    interpretation_controls->setSpacing(4);
    horizon_lbl_ = new QLabel(interpretation.frame);
    horizon_lbl_->setObjectName("cftcSectionMeta");
    interpretation_controls->addWidget(horizon_lbl_);
    for (int horizon : cftc_interpretation_horizons()) {
        auto* button = new QPushButton(interpretation.frame);
        button->setObjectName("cftcHorizonBtn");
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, [this, horizon]() { set_interpretation_horizon(horizon); });
        interpretation_controls->addWidget(button);
        horizon_btns_ << button;
        horizon_values_ << horizon;
    }
    interpretation_controls->addStretch(1);
    interpretation_evidence_toggle_ = new QPushButton(interpretation.frame);
    interpretation_evidence_toggle_->setObjectName("cftcEvidenceToggle");
    interpretation_evidence_toggle_->setCheckable(true);
    interpretation_evidence_toggle_->setCursor(Qt::PointingHandCursor);
    connect(interpretation_evidence_toggle_, &QPushButton::toggled, this, [this](bool expanded) {
        evidence_expanded_ = expanded;
        update_evidence_visibility();
    });
    interpretation_controls->addWidget(interpretation_evidence_toggle_);
    interpretation.body->addLayout(interpretation_controls);
    interpretation_headline_ = new QLabel(interpretation.frame);
    interpretation_headline_->setObjectName("cftcInterpretationHeadline");
    interpretation_headline_->setWordWrap(true);
    interpretation_headline_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    interpretation.body->addWidget(interpretation_headline_);
    interpretation_body_ = new QLabel(interpretation.frame);
    interpretation_body_->setObjectName("cftcInterpretationBody");
    interpretation_body_->setWordWrap(true);
    interpretation_body_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    interpretation.body->addWidget(interpretation_body_);
    interpretation_evidence_ = make_table(interpretation.frame);
    interpretation_evidence_->setWordWrap(true);
    interpretation_evidence_->setColumnCount(2);
    interpretation.body->addWidget(interpretation_evidence_);
    interpretation_context_ = new QLabel(interpretation.frame);
    interpretation_context_->setObjectName("cftcSectionMeta");
    interpretation_context_->setWordWrap(true);
    interpretation.body->addWidget(interpretation_context_);
    note_section(CftcWorkspaceSection::Interpretation);

    auto positioning = make_section(tr("POSITIONING"));
    positioning_frame_ = positioning.frame;
    positioning_title_ = positioning.title;
    positioning_table_ = make_table(positioning.frame);
    positioning.body->addWidget(positioning_table_);
    note_section(CftcWorkspaceSection::Positioning);

    auto weekly = make_section(tr("WEEKLY CHANGES"));
    weekly_frame_ = weekly.frame;
    weekly_title_ = weekly.title;
    auto* weekly_oi_row = new QHBoxLayout;
    weekly_oi_row->setSpacing(6);
    weekly_oi_caption_ = new QLabel(weekly.frame);
    weekly_oi_caption_->setObjectName("cftcSectionMeta");
    weekly_oi_lbl_ = new QLabel(QStringLiteral("-"), weekly.frame);
    weekly_oi_lbl_->setObjectName("econStatVal");
    weekly_oi_row->addWidget(weekly_oi_caption_);
    weekly_oi_row->addWidget(weekly_oi_lbl_);
    weekly_oi_row->addStretch(1);
    weekly.body->addLayout(weekly_oi_row);
    weekly_table_ = make_table(weekly.frame);
    weekly.body->addWidget(weekly_table_);
    note_section(CftcWorkspaceSection::WeeklyChanges);

    top_row_layout_->addWidget(interpretation_frame_, 0, 0, 2, 1);
    top_row_layout_->addWidget(positioning_frame_, 0, 1);
    top_row_layout_->addWidget(weekly_frame_, 1, 1);
    content_layout->addLayout(top_row_layout_);

    // 6 + 7. Window statistics & extremes and the price + positioning evidence.
    //    These detail cards sit before the large graphs; the evidence section
    //    carries the Batch 4A price/positioning assessments as raw numbers and
    //    the user-facing conclusion lives only in the interpretation above.
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
    note_section(CftcWorkspaceSection::Statistics);

    auto divergence = make_section(tr("PRICE + POSITIONING — EVIDENCE"));
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
    note_section(CftcWorkspaceSection::PricePositioningEvidence);

    stats_pair_layout_ = new QGridLayout;
    stats_pair_layout_->setSpacing(10);
    stats_pair_layout_->addWidget(stats_frame_, 0, 0);
    stats_pair_layout_->addWidget(divergence_frame_, 0, 1);
    content_layout->addLayout(stats_pair_layout_);

    // 8. The large synchronized Price + Positioning chart: two semantically
    //    separate panes on one report-date axis with a shared crosshair.
    auto sync = make_section(tr("PRICE + POSITIONING — SYNCHRONIZED"));
    sync_chart_title_ = sync.title;
    sync_chart_ = new CftcPricePositioningChart(sync.frame);
    sync.body->addWidget(sync_chart_);
    content_layout->addWidget(sync.frame);
    note_section(CftcWorkspaceSection::SyncChart);

    // 9. Historical positioning: the principal multi-series chart.
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
    note_section(CftcWorkspaceSection::HistoricalPositioning);

    // 10. Positioning heatmap.
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
    note_section(CftcWorkspaceSection::Heatmap);

    content_layout->addStretch(1);
    analysis_scroll_->setWidget(analysis_content_);
    analysis_page_ = add_content_page(analysis_scroll_);
    apply_responsive_layout();
    refresh_horizon_buttons();
    update_evidence_visibility();
    // build_base_ui() ran before this page existed, so its refresh_panel_theme()
    // could not style it; apply the workspace QSS now (theme changes re-apply
    // it through refresh_panel_theme()).
    analysis_content_->setStyleSheet(workspace_style());
}

// ── Cross-market monitor page ───────────────────────────────────────────────

namespace {

const services::CftcHorizonFlowReading* monitor_flow_at(const services::CftcMonitorEntry& entry, int horizon) {
    for (const auto& reading : entry.flow_readings) {
        if (reading.horizon_reports == horizon)
            return &reading;
    }
    return nullptr;
}

const services::CftcOpenInterestReading* monitor_oi_at(const services::CftcMonitorEntry& entry, int horizon) {
    for (const auto& reading : entry.open_interest_readings) {
        if (reading.horizon_reports == horizon)
            return &reading;
    }
    return nullptr;
}

QString monitor_status_label(services::CftcMonitorStatus status) {
    switch (status) {
        case services::CftcMonitorStatus::Ok:
            return QCoreApplication::translate("CftcPanel", "OK");
        case services::CftcMonitorStatus::ArchiveOnly:
            return QCoreApplication::translate("CftcPanel", "Stored history");
        case services::CftcMonitorStatus::NoData:
            return QCoreApplication::translate("CftcPanel", "No data");
        case services::CftcMonitorStatus::NoLocalHistory:
            return QCoreApplication::translate("CftcPanel", "No stored history");
        case services::CftcMonitorStatus::Unavailable:
            return QCoreApplication::translate("CftcPanel", "Unavailable");
        case services::CftcMonitorStatus::UnknownMarket:
            return QCoreApplication::translate("CftcPanel", "Unknown market");
    }
    return QCoreApplication::translate("CftcPanel", "Unavailable");
}

QString monitor_archive_text(const QJsonObject& archive) {
    if (archive.isEmpty())
        return {};
    const int total = archive.value(QStringLiteral("total_observations")).toInt();
    QStringList groups;
    for (const QJsonValue& value : archive.value(QStringLiteral("groups")).toArray()) {
        const QJsonObject group = value.toObject();
        groups << QStringLiteral("%1/%2: %3 (%4 → %5)")
                      .arg(group.value(QStringLiteral("report_family")).toString(),
                           group.value(QStringLiteral("report_basis")).toString())
                      .arg(group.value(QStringLiteral("rows")).toInt())
                      .arg(group.value(QStringLiteral("first_report_date")).toString(),
                           group.value(QStringLiteral("last_report_date")).toString());
    }
    QString text = QCoreApplication::translate("CftcPanel", "Local canonical archive: %1 observations").arg(total);
    if (!groups.isEmpty())
        text += QStringLiteral(" · ") + groups.join(QStringLiteral(" · "));
    return text;
}

} // namespace

void CftcPanel::build_monitor_page() {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setObjectName("cftcAnalysisScroll");
    monitor_content_ = new QWidget;
    monitor_content_->setObjectName("cftcAnalysis");
    auto* content_layout = new QVBoxLayout(monitor_content_);
    content_layout->setContentsMargins(12, 10, 12, 12);
    content_layout->setSpacing(10);

    // 1. Header, scope statement and controls.
    auto* header = new QWidget(monitor_content_);
    header->setObjectName("cftcSection");
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(12, 10, 12, 10);
    header_layout->setSpacing(6);
    auto* title = new QLabel(tr("CROSS-MARKET COT MONITOR"), header);
    title->setObjectName("cftcHeaderTitle");
    auto* meta = new QLabel(
        tr("Descriptive COT state across the supported futures markets, computed locally from the official CFTC "
           "weekly reports with the same cftc-descriptive-interpretation-v2 engine as the detailed workspace. "
           "A market is shown as requiring attention only when an existing engine state (or an explicit "
           "data-quality condition) fired; there is no score, ranking, forecast or recommendation. "
           "Price/positioning relationships are evaluated only in the detailed workspace."),
        header);
    meta->setObjectName("cftcHeaderMeta");
    meta->setWordWrap(true);
    header_layout->addWidget(title);
    header_layout->addWidget(meta);

    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    auto make_control_label = [&](const QString& text) {
        auto* label = new QLabel(text, header);
        label->setObjectName("cftcSectionMeta");
        return label;
    };
    monitor_family_combo_ = new QComboBox(header);
    monitor_family_combo_->addItem(QString(), QStringLiteral("legacy"));
    monitor_family_combo_->addItem(QString(), QStringLiteral("disaggregated"));
    monitor_family_combo_->addItem(QString(), QStringLiteral("tff"));
    monitor_family_combo_->setFixedHeight(24);
    monitor_type_combo_ = new QComboBox(header);
    monitor_type_combo_->addItem(QString(), QStringLiteral("futures_only"));
    monitor_type_combo_->addItem(QString(), QStringLiteral("combined"));
    monitor_type_combo_->setFixedHeight(24);
    monitor_scan_btn_ = new QPushButton(tr("SCAN MARKETS"), header);
    monitor_scan_btn_->setObjectName("cftcHorizonBtn");
    monitor_scan_btn_->setCursor(Qt::PointingHandCursor);
    monitor_backfill_btn_ = new QPushButton(tr("BACKFILL HISTORY"), header);
    monitor_backfill_btn_->setObjectName("cftcRangeBtn");
    monitor_backfill_btn_->setCursor(Qt::PointingHandCursor);
    connect(monitor_scan_btn_, &QPushButton::clicked, this, &CftcPanel::on_scan_markets);
    connect(monitor_backfill_btn_, &QPushButton::clicked, this, &CftcPanel::on_backfill_history);
    controls->addWidget(monitor_family_lbl_ = make_control_label(tr("REPORT FAMILY")));
    controls->addWidget(monitor_family_combo_);
    controls->addWidget(monitor_type_lbl_ = make_control_label(tr("TYPE")));
    controls->addWidget(monitor_type_combo_);
    controls->addWidget(monitor_scan_btn_);
    controls->addWidget(monitor_backfill_btn_);
    controls->addStretch(1);
    header_layout->addLayout(controls);

    monitor_status_lbl_ = new QLabel(header);
    monitor_status_lbl_->setObjectName("cftcSectionMeta");
    monitor_status_lbl_->setWordWrap(true);
    monitor_status_lbl_->setText(tr("Press SCAN MARKETS to read the current validated COT state across the "
                                    "supported universe."));
    monitor_archive_lbl_ = new QLabel(header);
    monitor_archive_lbl_->setObjectName("cftcSectionMeta");
    monitor_archive_lbl_->setWordWrap(true);
    header_layout->addWidget(monitor_status_lbl_);
    header_layout->addWidget(monitor_archive_lbl_);
    content_layout->addWidget(header);

    // 2. Markets requiring attention.
    auto* attention = new QWidget(monitor_content_);
    attention->setObjectName("cftcSection");
    auto* attention_layout = new QVBoxLayout(attention);
    attention_layout->setContentsMargins(12, 10, 12, 10);
    attention_layout->setSpacing(8);
    monitor_attention_title_ = new QLabel(tr("MARKETS REQUIRING ATTENTION"), attention);
    monitor_attention_title_->setObjectName("cftcSectionTitle");
    auto* attention_meta = new QLabel(
        tr("Grouped by descriptive class in the fixed order data quality, historical extreme, extreme transition, "
           "repositioning, Open Interest, concentration; markets inside a class are ordered by name. This is a "
           "review list, not a ranking. Double-click a market to open its detailed COT workspace."),
        attention);
    attention_meta->setObjectName("cftcSectionMeta");
    attention_meta->setWordWrap(true);
    monitor_attention_table_ = make_monitor_table(attention);
    monitor_attention_table_->setColumnCount(5);
    monitor_attention_table_->setHorizontalHeaderLabels(
        {tr("MARKET"), tr("GROUP"), tr("CLASS"), tr("DEVELOPMENT"), tr("REPORT")});
    monitor_attention_table_->horizontalHeader()->setStretchLastSection(true);
    monitor_attention_table_->setColumnWidth(0, 170);
    monitor_attention_table_->setColumnWidth(1, 110);
    monitor_attention_table_->setColumnWidth(2, 140);
    monitor_attention_table_->setColumnWidth(3, 620);
    connect(monitor_attention_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = monitor_attention_table_->item(row, 0);
        if (item)
            open_monitor_market(item->data(Qt::UserRole).toInt());
    });
    attention_layout->addWidget(monitor_attention_title_);
    attention_layout->addWidget(attention_meta);
    attention_layout->addWidget(monitor_attention_table_);
    content_layout->addWidget(attention);

    // 3. Every market in the scanned universe.
    auto* all = new QWidget(monitor_content_);
    all->setObjectName("cftcSection");
    auto* all_layout = new QVBoxLayout(all);
    all_layout->setContentsMargins(12, 10, 12, 10);
    all_layout->setSpacing(8);
    monitor_all_title_ = new QLabel(tr("ALL MARKETS"), all);
    monitor_all_title_->setObjectName("cftcSectionTitle");
    auto* all_meta = new QLabel(
        tr("Latest validated report per market. Net %OI is the principal participant's net position as a share of "
           "current Open Interest; the percentile is against the previous 156 valid reports; the 1R/4R/13R columns "
           "are that participant's net flow as a share of prior Open Interest. “—” always means the engine could "
           "not form that measure (missing cell, broken weekly sequence or insufficient history) — never zero."),
        all);
    all_meta->setObjectName("cftcSectionMeta");
    all_meta->setWordWrap(true);
    monitor_table_ = make_monitor_table(all);
    monitor_table_->setColumnCount(12);
    monitor_table_->setHorizontalHeaderLabels({tr("MARKET"), tr("GROUP"), tr("LATEST REPORT"), tr("NET %OI"),
                                               tr("%ILE (156R)"), tr("NET 1R"), tr("NET 4R"), tr("NET 13R"),
                                               tr("OPEN INTEREST"), tr("OI 1R"), tr("DEVELOPMENTS"), tr("STATUS")});
    monitor_table_->horizontalHeader()->setStretchLastSection(true);
    monitor_table_->setColumnWidth(0, 170);
    monitor_table_->setColumnWidth(1, 110);
    monitor_table_->setColumnWidth(2, 105);
    for (int column = 3; column <= 9; ++column)
        monitor_table_->setColumnWidth(column, 92);
    monitor_table_->setColumnWidth(10, 460);
    connect(monitor_table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        auto* item = monitor_table_->item(row, 0);
        if (item)
            open_monitor_market(item->data(Qt::UserRole).toInt());
    });
    all_layout->addWidget(monitor_all_title_);
    all_layout->addWidget(all_meta);
    all_layout->addWidget(monitor_table_);
    content_layout->addWidget(all);

    content_layout->addStretch(1);
    scroll->setWidget(monitor_content_);
    monitor_page_ = add_content_page(scroll);
    // build_base_ui() ran before this page existed; apply the workspace QSS now
    // (theme changes re-apply it through refresh_panel_theme()).
    monitor_content_->setStyleSheet(workspace_style());
}

void CftcPanel::set_monitor_status(const QString& text) {
    if (monitor_status_lbl_)
        monitor_status_lbl_->setText(text);
}

void CftcPanel::clear_monitor_display(const QString& status) {
    monitor_entries_.clear();
    monitor_rendered_ = false;
    if (monitor_attention_table_) {
        monitor_attention_table_->clearSpans();
        monitor_attention_table_->clearContents();
        monitor_attention_table_->setRowCount(0);
    }
    if (monitor_table_) {
        monitor_table_->clearContents();
        monitor_table_->setRowCount(0);
    }
    if (monitor_archive_lbl_)
        monitor_archive_lbl_->clear();
    set_monitor_status(status);
}

void CftcPanel::on_markets_button() {
    // An existing scan is a view, not a re-fetch; only the first entry scans,
    // and it uses the toolbar's current report-family/basis selection.
    if (monitor_rendered_) {
        if (result_tabs_)
            result_tabs_->show();
        show_monitor_tab();
        return;
    }
    if (monitor_family_combo_ && report_combo_) {
        const int family_index = monitor_family_combo_->findData(report_combo_->currentData().toString());
        if (family_index >= 0)
            monitor_family_combo_->setCurrentIndex(family_index);
    }
    if (monitor_type_combo_ && type_combo_) {
        const int type_index = monitor_type_combo_->findData(type_combo_->currentData().toString());
        if (type_index >= 0)
            monitor_type_combo_->setCurrentIndex(type_index);
    }
    on_scan_markets();
}

void CftcPanel::on_scan_markets() {
    monitor_request_family_code_ = monitor_family_combo_ ? monitor_family_combo_->currentData().toString()
                                                         : QStringLiteral("legacy");
    monitor_request_futures_only_ = monitor_type_combo_ &&
                                    monitor_type_combo_->currentData().toString() == QLatin1String("futures_only");
    pending_monitor_request_ = QStringLiteral("cftc_monitor_%1").arg(++request_seq_);
    clear_monitor_display(tr("Scanning the supported markets against the current CFTC API and the local canonical "
                             "archive… the first scan reads each contract's full official history, so it can take "
                             "up to a minute."));
    if (result_tabs_)
        result_tabs_->show();
    show_monitor_tab();
    services::EconomicsService::instance().execute(
        kCftcSourceId, kCftcScript, QStringLiteral("cot_monitor"),
        {monitor_request_family_code_,
         monitor_request_futures_only_ ? QStringLiteral("true") : QStringLiteral("false"), QStringLiteral("all"),
         QString::fromLatin1(kCftcMonitorMaxRows)},
        pending_monitor_request_, /*bypass_cache=*/true);
}

void CftcPanel::on_backfill_history() {
    const QString family_code = monitor_family_combo_ ? monitor_family_combo_->currentData().toString()
                                                      : QStringLiteral("legacy");
    const bool futures_only = monitor_type_combo_ &&
                              monitor_type_combo_->currentData().toString() == QLatin1String("futures_only");
    pending_backfill_request_ = QStringLiteral("cftc_backfill_%1").arg(++request_seq_);
    set_monitor_status(tr("Downloading and storing the official CFTC annual files for this family and basis… "
                          "completed years are kept if a later year fails."));
    services::EconomicsService::instance().execute(
        kCftcSourceId, kCftcScript, QStringLiteral("cot_backfill"),
        {family_code, futures_only ? QStringLiteral("true") : QStringLiteral("false"), QString(), QString(),
         QStringLiteral("all")},
        pending_backfill_request_, /*bypass_cache=*/true);
}

QString CftcPanel::monitor_alert_text(const services::CftcAlert& alert, const QString& participant_label) const {
    // Market-level records (Open Interest, concentration, data quality) are
    // never attributed to a participant category.
    return cftc_alert_summary(alert, alert.participant_key.isEmpty() ? QString() : participant_label);
}

QString CftcPanel::monitor_descriptive_text(const services::CftcMonitorEntry& entry) const {
    if (entry.alerts.isEmpty())
        return QStringLiteral("—");
    // The finalized Batch 4A terminology contract governs the monitor wording
    // too: Legacy Non-Commercial is never rendered as "Speculators".
    const QString participant = cftc_metric_participant_display_name(
        entry.family, entry.principal_participant_key, entry.principal_label);
    QStringList parts;
    for (const auto& alert : entry.alerts)
        parts << monitor_alert_text(alert, participant);
    return parts.join(QStringLiteral(" · "));
}

void CftcPanel::render_monitor(const services::CftcMonitorModel& model) {
    monitor_entries_ = model.entries;
    monitor_rendered_ = !monitor_entries_.isEmpty();
    if (result_tabs_)
        result_tabs_->show();

    const QString family_code = monitor_family_combo_ ? monitor_family_combo_->currentData().toString()
                                                      : QStringLiteral("legacy");
    const bool futures_only = monitor_type_combo_ &&
                              monitor_type_combo_->currentData().toString() == QLatin1String("futures_only");
    set_monitor_status(tr("Scan complete: %1 markets read for %2 · %3.")
                           .arg(monitor_entries_.size())
                           .arg(family_label(cftc_family_from_code(family_code)),
                                futures_only ? tr("Futures Only") : tr("Combined")));

    // Markets requiring attention.
    const QVector<int> order = services::cftc_attention_order(monitor_entries_);
    monitor_attention_table_->clearSpans();
    monitor_attention_table_->clearContents();
    if (order.isEmpty()) {
        monitor_attention_table_->setRowCount(1);
        monitor_attention_table_->setColumnCount(5);
        monitor_attention_table_->setSpan(0, 0, 1, 5);
        set_plain_cell(monitor_attention_table_, 0, 0,
                       tr("No scanned market currently carries a notable descriptive development or data-quality "
                          "condition."),
                       Qt::AlignLeft | Qt::AlignVCenter);
        monitor_attention_table_->setMinimumHeight(21 + 8);
    } else {
        monitor_attention_table_->setRowCount(order.size());
        for (int row = 0; row < order.size(); ++row) {
            const services::CftcMonitorEntry& entry = monitor_entries_.at(order[row]);
            QStringList classification_parts;
            for (const auto& alert : entry.alerts) {
                const QString label = services::cftc_attention_class_label(alert.attention_class);
                if (!classification_parts.contains(label))
                    classification_parts << label;
            }
            const QString classification = classification_parts.join(QStringLiteral(", "));
            auto* market_item = new QTableWidgetItem(entry.label);
            market_item->setData(Qt::UserRole, order[row]);
            market_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            monitor_attention_table_->setItem(row, 0, market_item);
            set_plain_cell(monitor_attention_table_, row, 1, services::cftc_asset_class_label(entry.asset_class),
                           Qt::AlignLeft | Qt::AlignVCenter);
            set_plain_cell(monitor_attention_table_, row, 2, classification, Qt::AlignLeft | Qt::AlignVCenter);
            if (auto* item = monitor_attention_table_->item(row, 2))
                item->setToolTip(classification);
            const QString development = monitor_descriptive_text(entry);
            set_plain_cell(monitor_attention_table_, row, 3, development, Qt::AlignLeft | Qt::AlignVCenter);
            if (auto* item = monitor_attention_table_->item(row, 3))
                item->setToolTip(development);
            set_plain_cell(monitor_attention_table_, row, 4,
                           entry.report_date.isValid() ? entry.report_date.toString(Qt::ISODate)
                                                       : QStringLiteral("—"),
                           Qt::AlignLeft | Qt::AlignVCenter);
        }
        monitor_attention_table_->setMinimumHeight(20 + order.size() * 21 + 8);
    }

    // Every market.
    monitor_table_->clearContents();
    monitor_table_->setRowCount(monitor_entries_.size());
    for (int row = 0; row < monitor_entries_.size(); ++row) {
        const services::CftcMonitorEntry& entry = monitor_entries_.at(row);
        auto* market_item = new QTableWidgetItem(entry.label);
        market_item->setData(Qt::UserRole, row);
        market_item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        if (!entry.status_detail.isEmpty())
            market_item->setToolTip(entry.status_detail);
        monitor_table_->setItem(row, 0, market_item);
        set_plain_cell(monitor_table_, row, 1, services::cftc_asset_class_label(entry.asset_class),
                       Qt::AlignLeft | Qt::AlignVCenter);

        QString report_text = QStringLiteral("—");
        if (entry.report_date.isValid()) {
            report_text = entry.report_date.toString(Qt::ISODate);
            if (entry.report_outdated)
                report_text += QStringLiteral(" (%1d)").arg(entry.report_age_days);
        }
        set_plain_cell(monitor_table_, row, 2, report_text, Qt::AlignLeft | Qt::AlignVCenter);

        const QString net_pct = entry.has_net_pct_oi
                                    ? cftc_signed_decimal(entry.net_pct_oi, 2) + QLatin1Char('%')
                                    : QStringLiteral("—");
        set_plain_cell(monitor_table_, row, 3, net_pct, Qt::AlignRight | Qt::AlignVCenter);
        if (auto* item = monitor_table_->item(row, 3)) {
            item->setToolTip(entry.has_net_pct_oi
                                 ? tr("Net position of the principal participant as % of current Open Interest.")
                                 : tr("Net %OI was not formed for this report."));
        }

        const QString percentile_text =
            entry.has_percentile
                ? QString::number(entry.percentile * 100.0, 'f', 1) + QLatin1Char('%') + QStringLiteral(" (n=%1)")
                                                                                         .arg(entry.percentile_reference_count)
                : QStringLiteral("—");
        set_plain_cell(monitor_table_, row, 4, percentile_text, Qt::AlignRight | Qt::AlignVCenter);
        if (auto* item = monitor_table_->item(row, 4)) {
            item->setToolTip(entry.has_percentile
                                 ? tr("Net %OI against the previous %1 valid reports (midpoint-tie percentile).")
                                       .arg(entry.percentile_reference_count)
                                 : tr("The 156-prior-report historical reference is unavailable for this market."));
        }

        const QString principal_name = cftc_metric_participant_display_name(
            entry.family, entry.principal_participant_key, entry.principal_label);
        for (int column = 5; column <= 7; ++column) {
            const int horizon = column == 5 ? 1 : column == 6 ? 4 : 13;
            const auto* reading = monitor_flow_at(entry, horizon);
            QString text = QStringLiteral("—");
            QString tooltip = tr("No %1-report net flow could be formed.").arg(horizon);
            if (reading && reading->evaluated && reading->has_net_flow) {
                text = cftc_signed_decimal(reading->net_flow, 2) + QLatin1Char('%');
                if (reading->has_net_rank) {
                    tooltip = principal_name.isEmpty()
                                  ? tr("%1-report net flow as % of prior Open Interest; materiality rank %2 of the "
                                       "previous %3 comparable moves.")
                                        .arg(horizon)
                                        .arg(QString::number(reading->net_rank, 'f', 2))
                                        .arg(reading->net_rank_reference_count)
                                  : tr("%4: %1-report net flow as % of prior Open Interest; materiality rank %2 of "
                                       "the previous %3 comparable moves.")
                                        .arg(horizon)
                                        .arg(QString::number(reading->net_rank, 'f', 2))
                                        .arg(reading->net_rank_reference_count)
                                        .arg(principal_name);
                } else {
                    tooltip = principal_name.isEmpty()
                                  ? tr("%1-report net flow as % of prior Open Interest.").arg(horizon)
                                  : tr("%2: %1-report net flow as % of prior Open Interest.")
                                        .arg(horizon)
                                        .arg(principal_name);
                }
            }
            set_plain_cell(monitor_table_, row, column, text, Qt::AlignRight | Qt::AlignVCenter);
            if (auto* item = monitor_table_->item(row, column))
                item->setToolTip(tooltip);
        }

        const QString oi_text = entry.has_open_interest ? position_text(entry.open_interest) : QStringLiteral("—");
        set_plain_cell(monitor_table_, row, 8, oi_text, Qt::AlignRight | Qt::AlignVCenter);
        if (auto* item = monitor_table_->item(row, 8))
            item->setToolTip(tr("Total Open Interest reported by CFTC for the latest report."));

        const auto* oi_reading = monitor_oi_at(entry, 1);
        const QString oi_change_text =
            (oi_reading && oi_reading->evaluated && oi_reading->has_oi_change)
                ? cftc_signed_decimal(oi_reading->oi_change, 2) + QLatin1Char('%')
                : QStringLiteral("—");
        set_plain_cell(monitor_table_, row, 9, oi_change_text, Qt::AlignRight | Qt::AlignVCenter);
        if (auto* item = monitor_table_->item(row, 9))
            item->setToolTip(tr("Open Interest change versus the prior report, as % of prior Open Interest."));

        const QString developments = monitor_descriptive_text(entry);
        set_plain_cell(monitor_table_, row, 10, developments, Qt::AlignLeft | Qt::AlignVCenter);
        if (auto* item = monitor_table_->item(row, 10))
            item->setToolTip(developments);

        set_plain_cell(monitor_table_, row, 11, monitor_status_label(entry.status), Qt::AlignLeft | Qt::AlignVCenter);
        if (auto* item = monitor_table_->item(row, 11))
            item->setToolTip(entry.status_detail.isEmpty() ? tr("Current canonical data.") : entry.status_detail);
    }
    monitor_table_->setMinimumHeight(20 + monitor_entries_.size() * 21 + 8);
}

void CftcPanel::open_monitor_market(int entry_index) {
    if (entry_index < 0 || entry_index >= monitor_entries_.size())
        return;
    const services::CftcMonitorEntry& entry = monitor_entries_.at(entry_index);
    if (!entry.known_market) {
        set_monitor_status(tr("This market is not part of the supported MarketLab CFTC universe."));
        return;
    }
    const int market_index = market_combo_ ? market_combo_->findData(entry.market_key) : -1;
    if (market_index < 0) {
        set_monitor_status(tr("The detailed workspace does not carry this market."));
        return;
    }
    market_combo_->setCurrentIndex(market_index);
    const int family_index = report_combo_->findData(services::cftc_family_code(entry.family));
    if (family_index >= 0)
        report_combo_->setCurrentIndex(family_index);
    const int type_index = type_combo_->findData(entry.futures_only ? QStringLiteral("futures_only")
                                                                    : QStringLiteral("combined"));
    if (type_index >= 0)
        type_combo_->setCurrentIndex(type_index);
    on_fetch();
}

void CftcPanel::apply_responsive_layout() {
    if (!snapshot_grid_ || !top_row_layout_ || !stats_pair_layout_)
        return;

    const int snapshot_columns = narrow_layout_ ? 2 : 7;
    for (int i = 0; i < snapshot_cards_.size(); ++i) {
        snapshot_grid_->removeWidget(snapshot_cards_[i].frame);
        snapshot_grid_->addWidget(snapshot_cards_[i].frame, i / snapshot_columns, i % snapshot_columns);
    }
    for (int col = 0; col < 8; ++col)
        snapshot_grid_->setColumnStretch(col, col < snapshot_columns ? 1 : 0);

    // Interpretation and the participant tables: side by side when width
    // allows, stacked otherwise.
    const QVector<QWidget*> top_frames = {interpretation_frame_, positioning_frame_, weekly_frame_};
    for (auto* frame : top_frames) {
        if (frame)
            top_row_layout_->removeWidget(frame);
    }
    if (narrow_layout_) {
        for (int i = 0; i < top_frames.size(); ++i) {
            if (top_frames[i])
                top_row_layout_->addWidget(top_frames[i], i, 0, 1, 2);
        }
        top_row_layout_->setColumnStretch(0, 1);
        top_row_layout_->setColumnStretch(1, 0);
    } else {
        if (interpretation_frame_)
            top_row_layout_->addWidget(interpretation_frame_, 0, 0, 2, 1);
        if (positioning_frame_)
            top_row_layout_->addWidget(positioning_frame_, 0, 1);
        if (weekly_frame_)
            top_row_layout_->addWidget(weekly_frame_, 1, 1);
        top_row_layout_->setColumnStretch(0, 3);
        top_row_layout_->setColumnStretch(1, 2);
    }

    const QVector<QWidget*> second_row = {stats_frame_, divergence_frame_};
    for (auto* frame : second_row) {
        if (frame)
            stats_pair_layout_->removeWidget(frame);
    }
    if (narrow_layout_) {
        for (int i = 0; i < second_row.size(); ++i) {
            if (second_row[i])
                stats_pair_layout_->addWidget(second_row[i], i, 0, 1, 2);
        }
    } else {
        if (stats_frame_)
            stats_pair_layout_->addWidget(stats_frame_, 0, 0);
        if (divergence_frame_)
            stats_pair_layout_->addWidget(divergence_frame_, 0, 1);
    }
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

    // A cross-market scan and a backfill are independent of the single-market
    // workspace, so they are matched before the single-market pending request.
    if (!pending_monitor_request_.isEmpty() && request_id == pending_monitor_request_) {
        pending_monitor_request_.clear();
        if (!result.success) {
            clear_monitor_display(cftc_error_text(result.error));
        } else {
            const QJsonObject data = result.data.value(QStringLiteral("data")).toObject();
            const CftcFamily family = cftc_family_from_code(monitor_request_family_code_);
            const bool futures_only = monitor_request_futures_only_;
            const services::CftcMonitorModel model = services::cftc_parse_monitor_payload(
                data, family, futures_only, QDateTime::currentDateTimeUtc().date(),
                cftc_principal_participant_key(family));
            if (!model.error.isEmpty()) {
                clear_monitor_display(model.error);
            } else {
                if (monitor_archive_lbl_)
                    monitor_archive_lbl_->setText(
                        monitor_archive_text(data.value(QStringLiteral("archive")).toObject()));
                render_monitor(model);
                const QString retrieved_at =
                    result.data.value(QStringLiteral("parameters")).toObject()
                        .value(QStringLiteral("retrieved_at")).toString();
                if (!retrieved_at.isEmpty()) {
                    set_monitor_status(tr("Scan complete: %1 markets read for %2 · %3 · retrieved %4.")
                                           .arg(model.entries.size())
                                           .arg(family_label(family), futures_only ? tr("Futures Only")
                                                                                   : tr("Combined"),
                                                retrieved_at));
                }
            }
        }
        show_monitor_tab();
        return;
    }
    if (!pending_backfill_request_.isEmpty() && request_id == pending_backfill_request_) {
        pending_backfill_request_.clear();
        if (!result.success) {
            set_monitor_status(cftc_error_text(result.error));
        } else {
            const QJsonObject data = result.data.value(QStringLiteral("data")).toObject();
            const QJsonObject parameters = result.data.value(QStringLiteral("parameters")).toObject();
            QStringList failed_years;
            for (const QJsonValue& value : data.value(QStringLiteral("failed_years")).toArray())
                failed_years << QString::number(value.toInt());
            QStringList skipped_years;
            for (const QJsonValue& value : data.value(QStringLiteral("skipped_years")).toArray())
                skipped_years << QString::number(value.toInt());
            QStringList no_data_years;
            for (const QJsonValue& value : data.value(QStringLiteral("no_data_years")).toArray())
                no_data_years << QString::number(value.toInt());
            const QString headline =
                failed_years.isEmpty()
                    ? tr("Backfill complete (%1–%2): %3 new observations, %4 revised, %5 rejected rows.")
                    : tr("Backfill finished with failures (%1–%2): %3 new observations, %4 revised, "
                         "%5 rejected rows.");
            QString text = headline
                               .arg(parameters.value(QStringLiteral("start_year")).toInt())
                               .arg(parameters.value(QStringLiteral("end_year")).toInt())
                               .arg(data.value(QStringLiteral("rows_inserted")).toInt())
                               .arg(data.value(QStringLiteral("rows_updated")).toInt())
                               .arg(data.value(QStringLiteral("rows_rejected")).toInt());
            if (!skipped_years.isEmpty())
                text += tr(" No official annual file: %1.").arg(skipped_years.join(QStringLiteral(", ")));
            if (!no_data_years.isEmpty())
                text += tr(" No data for the selected markets: %1.")
                            .arg(no_data_years.join(QStringLiteral(", ")));
            if (!failed_years.isEmpty())
                text += tr(" Failed or malformed years: %1.").arg(failed_years.join(QStringLiteral(", ")));
            text += tr(" Press SCAN MARKETS to refresh the monitor.");
            set_monitor_status(text);
            if (monitor_archive_lbl_)
                monitor_archive_lbl_->setText(
                    monitor_archive_text(data.value(QStringLiteral("archive")).toObject()));
        }
        show_monitor_tab();
        return;
    }

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
    // The rows' own published basis must be the requested basis: a Combined
    // history labelled Futures Only (or a splice of the two) is refused.
    const QString basis_error = cftc_history_basis_error(history.observations, futures_only_);
    if (!basis_error.isEmpty()) {
        clear_workspace();
        show_error(basis_error);
        return;
    }

    history_ = history;
    participants_ = cftc_family_participants(family_);
    speculative_index_ = cftc_speculative_index(participants_);
    // The Batch 4B semantic path (interpretation, synchronized chart and the
    // price/positioning evidence) selects its principal participant from the
    // finalized Batch 4A terminology contract, never from the speculative flag.
    principal_index_ = -1;
    const QString principal_key = cftc_principal_participant_key(family_);
    for (int i = 0; i < participants_.size(); ++i) {
        if (participants_[i].key == principal_key) {
            principal_index_ = i;
            break;
        }
    }
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
    // CFTC-only conclusions render immediately from the full validated
    // history; the price context arrives asynchronously and only refreshes the
    // price-dependent parts afterwards.
    refresh_interpretation();
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
    price_market_key_.clear();
    price_.clear();
    history_ = {};
    window_.clear();
    participants_.clear();
    speculative_index_ = -1;
    principal_index_ = -1;
    dataset_.clear();
    result_params_ = {};
    // The result tabs stay available while a cross-market scan exists: the
    // monitored view survives a single-market fetch and re-activation.
    if (result_tabs_ && !monitor_rendered_)
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

    interpretation_ = {};
    if (interpretation_headline_)
        interpretation_headline_->setText(QStringLiteral("—"));
    if (interpretation_body_)
        interpretation_body_->clear();
    if (interpretation_evidence_) {
        interpretation_evidence_->clearContents();
        interpretation_evidence_->setRowCount(0);
        interpretation_evidence_->setColumnCount(0);
    }
    if (interpretation_context_)
        interpretation_context_->clear();
    evidence_expanded_ = false;
    update_evidence_visibility();
    if (interpretation_evidence_toggle_)
        interpretation_evidence_toggle_->setVisible(false);
    if (sync_chart_)
        sync_chart_->clear();
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
        const QString display =
            cftc_metric_participant_display_name(family_, participants_[i].key, participants_[i].label);
        auto* check = new QCheckBox(display, parent);
        check->setChecked(participants_[i].key != QLatin1String("non_reportable"));
        check->setStyleSheet(QStringLiteral("color:%1; font-size:10px; background:transparent;")
                                 .arg(palette[i % palette.size()].name()));
        check->setToolTip(tr("Show %1 in the historical chart").arg(display));
        connect(check, &QCheckBox::toggled, this, [this]() { update_chart(); });
        participant_check_layout_->addWidget(check);
        participant_checks_.append(check);
    }
}

void CftcPanel::apply_range(CftcRange range) {
    if (history_.observations.isEmpty() || !cftc_range_available(history_.observations, range))
        return;
    range_ = range;
    // The history-display range refreshes only the sections its contract marks
    // as range-driven; the interpretation is never re-evaluated from a range.
    rebuild_workspace(/*range_only=*/true);
}

void CftcPanel::rebuild_workspace(bool range_only) {
    if (history_.observations.isEmpty())
        return;
    if (!cftc_range_available(history_.observations, range_))
        range_ = CftcRange::Max;
    window_ = cftc_filter_range(history_.observations, range_);
    refresh_range_buttons();
    // The refresh order follows the page hierarchy contract, and a range-only
    // refresh skips the sections the visible range does not change.
    for (CftcWorkspaceSection section : cftc_workspace_section_order()) {
        if (range_only && !cftc_section_refreshed_by_visible_range(section))
            continue;
        switch (section) {
            case CftcWorkspaceSection::Header:
                update_header();
                break;
            case CftcWorkspaceSection::CurrentSnapshot:
                update_snapshot();
                break;
            case CftcWorkspaceSection::Interpretation:
                // Re-renders the stored Batch 4A result; it never re-evaluates a
                // range-filtered history.
                render_interpretation();
                break;
            case CftcWorkspaceSection::SyncChart:
                update_sync_chart();
                break;
            case CftcWorkspaceSection::Positioning:
                update_positioning();
                break;
            case CftcWorkspaceSection::WeeklyChanges:
                update_weekly();
                break;
            case CftcWorkspaceSection::HistoricalPositioning:
                update_chart();
                break;
            case CftcWorkspaceSection::Statistics:
                update_statistics();
                break;
            case CftcWorkspaceSection::PricePositioningEvidence:
                update_divergence();
                break;
            case CftcWorkspaceSection::Heatmap:
                update_heatmap();
                break;
        }
    }
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

void CftcPanel::set_interpretation_horizon(int horizon_reports) {
    if (!cftc_is_interpretation_horizon(horizon_reports) || horizon_reports == horizon_reports_)
        return;
    horizon_reports_ = horizon_reports;
    refresh_horizon_buttons();
    // Only the horizon-scoped presentation changes. The stored Batch 4A result —
    // including the strict 156-prior-report crowding/extreme state — is never
    // re-evaluated, and the synchronized chart caption follows the selection.
    render_interpretation();
    update_sync_chart();
}

void CftcPanel::refresh_horizon_buttons() {
    for (int i = 0; i < horizon_btns_.size() && i < horizon_values_.size(); ++i) {
        QPushButton* button = horizon_btns_[i];
        if (!button)
            continue;
        const int horizon = horizon_values_[i];
        button->setText(cftc_horizon_button_label(horizon));
        button->setToolTip(tr("Interpret the CFTC report over the last %1").arg(cftc_horizon_phrase(horizon)));
        QSignalBlocker block(button);
        button->setChecked(horizon == horizon_reports_);
    }
}

void CftcPanel::update_evidence_visibility() {
    if (!interpretation_evidence_)
        return;
    interpretation_evidence_->setVisible(evidence_expanded_);
    if (interpretation_evidence_toggle_) {
        QSignalBlocker block(interpretation_evidence_toggle_);
        interpretation_evidence_toggle_->setChecked(evidence_expanded_);
        interpretation_evidence_toggle_->setText(evidence_expanded_ ? tr("Hide numerical evidence")
                                                                    : tr("Show numerical evidence"));
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
    if (interpretation_.report_outdated) {
        meta << tr("OUT OF DATE — %1 days old; CFTC publishes weekly, so this contract may be discontinued")
                    .arg(interpretation_.report_age_days);
    }
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

void CftcPanel::refresh_interpretation() {
    if (history_.observations.isEmpty() || principal_index_ < 0) {
        interpretation_ = {};
        render_interpretation();
        return;
    }
    // The interpretation always evaluates the full validated official history.
    // window_ (the visible chart range) is presentation-only and is never
    // passed here: the strict 156-prior-report reference needs the complete
    // history, and changing the visible range must not change any state.
    const bool price_ready = price_state_ == PriceState::Ready && price_market_key_ == market_key_;
    const QVector<CftcPricePoint> prices = price_ready ? price_ : QVector<CftcPricePoint>{};
    // Today's UTC calendar date is passed only for the report-freshness check,
    // so a discontinued contract's last report is flagged as out of date.
    const CftcInterpretationInput input = cftc_make_interpretation_input(
        family_, history_.observations,
        futures_only_ ? QStringLiteral("futures_only") : QStringLiteral("futures_and_options_combined"), prices,
        price_ready ? concise_price_source_text() : QString(), price_ready && !price_spot_index_, price_spot_index_,
        QDateTime::currentDateTimeUtc().date());
    interpretation_ = cftc_interpret(input);
    render_interpretation();
}

void CftcPanel::render_interpretation() {
    if (!interpretation_headline_)
        return;
    // One actual price state feeds the prose, the evidence rows and the
    // synchronized chart; a provider failure or an unmapped market carries its
    // concrete reason instead of the engine's generic missing-context wording.
    const CftcPriceContextState price_context = price_context_state();
    const QString price_note = price_unavailable_note();
    // The user's explicit 1W | 4W | 13W selection composes one horizon. The
    // historical level, persistence and strict 156-prior-report crowding state
    // come from the same stored full-history result at every selection.
    const CftcInterpretationView view =
        cftc_compose_horizon_interpretation(interpretation_, horizon_reports_, price_context, price_note);
    interpretation_headline_->setText(view.headline);
    // One conclusion per bullet-ish line keeps the interpretation scannable at
    // a glance instead of a dense paragraph.
    QStringList conclusion_lines;
    conclusion_lines.reserve(view.sentences.size());
    for (const QString& sentence : view.sentences)
        conclusion_lines << QStringLiteral("•  ") + sentence;
    interpretation_body_->setText(conclusion_lines.join(QStringLiteral("\n")));
    interpretation_context_->setText(view.context_text);

    // Status colours stay within the theme token system: an available readout
    // keeps its normal colour, an evaluated-not-material row is de-emphasised
    // to the readable secondary tone, unavailable data uses the readable
    // warning tone, and a pending request is secondary. The value text itself
    // always states which of the three states applies.
    const auto apply_status = [](QTableWidgetItem* item, CftcEvidenceStatus status) {
        if (!item)
            return;
        switch (status) {
            case CftcEvidenceStatus::Available:
                break;
            case CftcEvidenceStatus::NoMaterialState:
            case CftcEvidenceStatus::Pending:
                item->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
                break;
            case CftcEvidenceStatus::Unavailable:
                item->setForeground(QColor(ui::colors::WARNING()));
                break;
        }
    };
    const auto set_evidence_row = [this, &apply_status](const CftcEvidenceItem& item) {
        const int row = interpretation_evidence_->rowCount();
        interpretation_evidence_->insertRow(row);
        set_plain_cell(interpretation_evidence_, row, 0, item.label, Qt::AlignLeft | Qt::AlignVCenter);
        set_plain_cell(interpretation_evidence_, row, 1, item.value, Qt::AlignRight | Qt::AlignVCenter);
        apply_status(interpretation_evidence_->item(row, 1), item.status);
        if (auto* cell = interpretation_evidence_->item(row, 0))
            cell->setToolTip(item.label);
        if (auto* cell = interpretation_evidence_->item(row, 1))
            cell->setToolTip(item.value);
    };

    interpretation_evidence_->clearSpans();
    interpretation_evidence_->setRowCount(0);
    if (view.horizon_reports == 0) {
        // Combined contract view (the page renders a selected horizon; this
        // path is retained for the composition contract and its tests).
        interpretation_evidence_->setColumnCount(3);
        interpretation_evidence_->setHorizontalHeaderLabels({tr("EVIDENCE"), tr("4 REPORTS"), tr("13 REPORTS")});
        interpretation_evidence_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        interpretation_evidence_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        interpretation_evidence_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        QHash<QString, int> horizon_rows;
        for (const CftcEvidenceItem& item : std::as_const(view.evidence)) {
            if (item.horizon_reports == 4 || item.horizon_reports == 13) {
                int row = horizon_rows.value(item.label, -1);
                if (row < 0) {
                    row = interpretation_evidence_->rowCount();
                    interpretation_evidence_->insertRow(row);
                    horizon_rows.insert(item.label, row);
                    set_plain_cell(interpretation_evidence_, row, 0, item.label, Qt::AlignLeft | Qt::AlignVCenter);
                    // A horizon that has not reported a value yet shows an
                    // explicit dash; a real gap must not look like a zero.
                    set_plain_cell(interpretation_evidence_, row, item.horizon_reports == 4 ? 2 : 1,
                                   QStringLiteral("—"));
                    for (int col : {1, 2}) {
                        if (auto* cell = interpretation_evidence_->item(row, col))
                            cell->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
                    }
                }
                const int column = item.horizon_reports == 4 ? 1 : 2;
                set_plain_cell(interpretation_evidence_, row, column, item.value, Qt::AlignRight | Qt::AlignVCenter);
                apply_status(interpretation_evidence_->item(row, column), item.status);
                if (auto* cell = interpretation_evidence_->item(row, column))
                    cell->setToolTip(item.value);
            } else {
                const int row = interpretation_evidence_->rowCount();
                interpretation_evidence_->insertRow(row);
                set_plain_cell(interpretation_evidence_, row, 0, item.label, Qt::AlignLeft | Qt::AlignVCenter);
                set_plain_cell(interpretation_evidence_, row, 1, item.value, Qt::AlignRight | Qt::AlignVCenter);
                apply_status(interpretation_evidence_->item(row, 1), item.status);
                interpretation_evidence_->setSpan(row, 1, 1, 2);
                if (auto* cell = interpretation_evidence_->item(row, 0))
                    cell->setToolTip(item.label);
                if (auto* cell = interpretation_evidence_->item(row, 1))
                    cell->setToolTip(item.value);
            }
        }
    } else {
        // Single-horizon view: one label/value matrix with the selected
        // horizon named in the value column header.
        interpretation_evidence_->setColumnCount(2);
        interpretation_evidence_->setHorizontalHeaderLabels(
            {tr("EVIDENCE"), cftc_horizon_evidence_header(view.horizon_reports)});
        interpretation_evidence_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        interpretation_evidence_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        for (const CftcEvidenceItem& item : std::as_const(view.evidence))
            set_evidence_row(item);
    }
    interpretation_evidence_->resizeRowsToContents();
    int evidence_height = interpretation_evidence_->horizontalHeader()->height();
    for (int row = 0; row < interpretation_evidence_->rowCount(); ++row)
        evidence_height += interpretation_evidence_->rowHeight(row);
    interpretation_evidence_->setFixedHeight(evidence_height + 10);
    if (interpretation_evidence_toggle_)
        interpretation_evidence_toggle_->setVisible(!view.evidence.isEmpty());
    update_evidence_visibility();
}

void CftcPanel::update_sync_chart() {
    if (!sync_chart_)
        return;
    if (history_.observations.isEmpty() || principal_index_ < 0) {
        sync_chart_->clear();
        return;
    }
    const CftcPriceContextState price_context = price_context_state();
    const bool price_ready = price_context == CftcPriceContextState::Ready && price_market_key_ == market_key_;
    const QString price_note = price_unavailable_note();
    const CftcSyncChartData data = cftc_build_sync_chart_data(
        interpretation_, history_.observations, price_ready ? price_ : QVector<CftcPricePoint>{}, market_key_,
        price_market_key_, range_, price_context, price_note, price_ready ? concise_price_source_text() : QString(),
        price_ready && !price_spot_index_, price_spot_index_, horizon_reports_);
    sync_chart_->set_data(data);
}

CftcPriceContextState CftcPanel::price_context_state() const {
    if (price_state_ == PriceState::Pending)
        return CftcPriceContextState::Pending;
    if (price_state_ == PriceState::Ready)
        return CftcPriceContextState::Ready;
    return CftcPriceContextState::Unavailable;
}

QString CftcPanel::price_unavailable_note() const {
    return price_state_ == PriceState::Unavailable ? price_reason_ : QString();
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
                     .arg(cftc_metric_participant_display_name(family_, participants_[speculative_index_].key,
                                                               participants_[speculative_index_].label),
                          tr("long"), tr("short")));
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
                 tr("Difference from the previous report; only a previous report %1-%2 days earlier is a weekly "
                    "change.")
                     .arg(kCftcWeeklyMinGapDays)
                     .arg(kCftcWeeklyGapDays));
    } else {
        set_card(1, tr("WEEKLY CHANGE (%1)").arg(spec_short), QStringLiteral("—"), 0,
                 weekly_unavailable_reason(weekly, as_of_date),
                 tr("A weekly change requires the latest report and a previous report %1-%2 days earlier.")
                     .arg(kCftcWeeklyMinGapDays)
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

    // COT INDEX / Z-SCORE / PERCENTILE (window-dependent labels). These are
    // raw-net-contract statistics of the selected history range with the
    // latest report included; they are a different measure from the
    // interpretation's Net %OI percentile, so every card's subtitle names its
    // metric, window and inclusion, and none carries a categorical range
    // verdict.
    const QString cot_caption = tr("COT INDEX (%1)").arg(range_label);
    if (stats.has_cot_index) {
        set_card(4, cot_caption, QString::number(stats.cot_index, 'f', 1), 0,
                 tr("raw net contracts, window %1 → %2, incl. latest")
                     .arg(cftc_signed_net(stats.min_value), cftc_signed_net(stats.max_value)),
                 stat_metric_tooltip(StatMetric::CotIndex));
    } else {
        set_card(4, cot_caption, QStringLiteral("—"), 0, stat_unavailable_reason(stats, as_of_date),
                 stat_metric_tooltip(StatMetric::CotIndex));
    }

    const QString z_caption = tr("Z-SCORE (%1)").arg(range_label);
    if (stats.has_zscore) {
        set_card(5, z_caption, QString::number(stats.zscore, 'f', 2), 0,
                 tr("raw net contracts vs window mean / σ, incl. latest"), stat_metric_tooltip(StatMetric::ZScore));
    } else {
        set_card(5, z_caption, QStringLiteral("—"), 0, stat_unavailable_reason(stats, as_of_date),
                 stat_metric_tooltip(StatMetric::ZScore));
    }

    // The selected-window percentile is a different measure from the COT
    // interpretation's strict 156-prior-report Net %OI percentile; the caption
    // names the window explicitly so the two can never look contradictory.
    const QString pct_caption = cftc_window_percentile_label(range_label);
    if (stats.has_percentile) {
        set_card(6, pct_caption, QString::number(stats.percentile, 'f', 1) + QLatin1Char('%'), 0,
                 tr("raw net contracts, selected range, incl. latest"), cftc_window_percentile_tooltip());
    } else {
        set_card(6, pct_caption, QStringLiteral("—"), 0, stat_unavailable_reason(stats, as_of_date),
                 cftc_window_percentile_tooltip());
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
        item->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
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
    // Latest-report legs and shares only. The selected-window COT index and raw
    // percentile columns that used to sit here duplicated the Current Snapshot
    // cards and the Positioning Statistics section, so they were removed to
    // keep one authoritative presentation of the selected-window statistic.
    const int columns = 6;
    positioning_table_->setColumnCount(columns);
    positioning_table_->setRowCount(participants_.size());
    positioning_table_->setHorizontalHeaderLabels(
        {tr("PARTICIPANT"), tr("LONG"), tr("SHORT"), tr("NET"), tr("% LONG"), tr("% SHORT")});
    positioning_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int col = 1; col < columns; ++col)
        positioning_table_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Stretch);

    for (int p = 0; p < participants_.size(); ++p) {
        set_plain_cell(positioning_table_, p, 0,
                       cftc_metric_participant_display_name(family_, participants_[p].key, participants_[p].label),
                       Qt::AlignLeft | Qt::AlignVCenter);

        const CftcPositionMetrics metrics = cftc_position_metrics(latest, p);
        set_plain_cell(positioning_table_, p, 1,
                       metrics.has_long ? position_text(metrics.long_leg) : QStringLiteral("-"));
        set_plain_cell(positioning_table_, p, 2,
                       metrics.has_short ? position_text(metrics.short_leg) : QStringLiteral("-"));
        set_signed_cell(positioning_table_, p, 3,
                        metrics.has_net ? std::optional<double>(metrics.net_position) : std::nullopt);

        if (metrics.has_long_pct_oi)
            set_plain_cell(positioning_table_, p, 4, QString::number(metrics.long_pct_oi, 'f', 1) + QLatin1Char('%'));
        else
            set_plain_cell(positioning_table_, p, 4, QStringLiteral("-"));
        if (metrics.has_short_pct_oi)
            set_plain_cell(positioning_table_, p, 5, QString::number(metrics.short_pct_oi, 'f', 1) + QLatin1Char('%'));
        else
            set_plain_cell(positioning_table_, p, 5, QStringLiteral("-"));
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
        set_plain_cell(weekly_table_, p, 0,
                       cftc_metric_participant_display_name(family_, participants_[p].key, participants_[p].label),
                       Qt::AlignLeft | Qt::AlignVCenter);
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
        chart_series.label =
            cftc_metric_participant_display_name(family_, participants_[p].key, participants_[p].label);
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
            tr("Window %1 · %2 reports · %3 → %4 — statistical summaries of raw net contracts in the selected "
               "range, including the latest report; they are not the COT interpretation's Net %OI percentile, and an "
               "extreme reading is not a prediction. Latest-window measures are anchored at the %5 report.")
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
        update_price_views();
        return;
    }
    price_symbol_ = it->symbol;
    price_spot_index_ = it->spot_index;
    price_state_ = PriceState::Pending;
    update_price_views();

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
                update_price_views();
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
        update_price_views();
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
        // Exchange-session date, independent of the viewer's time zone.
        const QDate date = cftc_price_session_date(point.timestamp);
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
    update_price_views();
}

void CftcPanel::update_price_views() {
    // The correct current-market price context updates only the price-dependent
    // interpretation, the synchronized price pane and the price evidence; every
    // CFTC-only conclusion comes from the same deterministic full-history result.
    refresh_interpretation();
    update_sync_chart();
    update_divergence();
}

QString CftcPanel::concise_price_source_text() const {
    if (price_symbol_.isEmpty())
        return {};
    if (price_spot_index_)
        return tr("Yahoo Finance — %1 spot index").arg(price_symbol_);
    return tr("Yahoo Finance — %1 front-month continuous futures").arg(price_symbol_);
}

QString CftcPanel::price_source_text() const {
    if (price_symbol_.isEmpty())
        return {};
    if (price_spot_index_) {
        return tr("Price: Yahoo Finance — %1 (spot index, not the CFTC-report futures contract). "
                  "Separate from CFTC data. Relationship wording comes from the descriptive COT "
                  "interpretation engine and describes a contemporaneous relationship only.")
            .arg(price_symbol_);
    }
    return tr("Price: Yahoo Finance — %1 front-month continuous futures (rolls between contracts without roll "
              "adjustment, and the provider does not identify the roll dates, so a price move could include the gap "
              "between contracts; not an individual deliverable contract). Price moves and the price/positioning "
              "relationship are therefore not evaluated for this series; the chart shows it for reference only. "
              "Separate from CFTC data.")
        .arg(price_symbol_);
}

void CftcPanel::update_divergence() {
    if (!divergence_table_ || principal_index_ < 0)
        return;
    const QString spec_label = cftc_metric_participant_display_name(family_, participants_[principal_index_].key,
                                                                    participants_[principal_index_].label);

    // This section is raw numeric evidence only. Any user-facing statement that
    // price and positioning are aligned or diverging comes from the Batch 4A
    // assessments rendered in the COT interpretation section above; no second
    // algorithm computes an alignment verdict here.
    if (price_state_ == PriceState::Pending) {
        divergence_source_lbl_->setText(tr("Loading price context from Yahoo Finance…"));
    } else if (price_state_ != PriceState::Ready) {
        divergence_source_lbl_->setText(tr("Price evidence unavailable: %1.").arg(price_reason_));
    } else {
        divergence_source_lbl_->setText(price_source_text());
    }

    const auto net_series = participant_metric_series(principal_index_, CftcChartMetric::Net);
    const QString spec_short = participant_short_label(participants_[principal_index_].key);
    divergence_table_->setColumnCount(4);
    divergence_table_->setRowCount(3);
    // Batch 4A net_flow is normalized by the anchor report's Open Interest, so
    // the column states that exact unit rather than implying contracts. There is
    // deliberately no relationship column: the one authoritative relationship
    // conclusion is the COT interpretation result above, and this section keeps
    // only the numeric deltas and a neutral evaluation status.
    divergence_table_->setHorizontalHeaderLabels({tr("HORIZON"), tr("PRICE Δ (quoted units)"),
                                                  tr("NET Δ, % of prior OI (%1)").arg(spec_short),
                                                  tr("EVIDENCE STATUS")});
    divergence_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    divergence_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    divergence_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    divergence_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    // The CFTC-only net move comes from the emitted per-horizon flow reading
    // first, then the assessment, then a net-shift state. None of these depend
    // on price availability.
    const QString primary_state_key = participants_[principal_index_].key;
    auto net_flow_reading = [this, &primary_state_key](int horizon) -> std::optional<double> {
        for (const auto& participant : interpretation_.participants) {
            if (participant.participant_key != primary_state_key)
                continue;
            for (const auto& reading : participant.flow_readings) {
                if (reading.horizon_reports == horizon && reading.evaluated && reading.has_net_flow)
                    return reading.net_flow;
            }
        }
        return std::nullopt;
    };
    auto state_net_flow = [this, &primary_state_key](int horizon) -> std::optional<double> {
        for (const auto& participant : interpretation_.participants) {
            if (participant.participant_key != primary_state_key)
                continue;
            for (const auto& state : participant.states) {
                if (!state.has_horizon || state.horizon_reports != horizon)
                    continue;
                if (state.state_id != QLatin1String("NET_LONGWARD_SHIFT") &&
                    state.state_id != QLatin1String("NET_SHORTWARD_SHIFT"))
                    continue;
                for (const auto& metric : state.metrics) {
                    if (metric.key == QLatin1String("net_flow") && metric.has_value)
                        return metric.value;
                }
            }
        }
        return std::nullopt;
    };

    // The NET Δ unavailable reason is a CFTC-only statement (NET_SHIFT record
    // or the participant's horizon reading); the price assessment's reason
    // never labels a positioning cell.
    auto cftc_net_unavailable_reason = [this, &primary_state_key](int horizon) -> QString {
        return cftc_net_delta_unavailable_reason(interpretation_, primary_state_key, horizon);
    };

    const auto unavailable_cell = [this](int row, int column, const QString& reason) {
        set_plain_cell(divergence_table_, row, column, tr("unavailable — %1").arg(reason));
        if (auto* item = divergence_table_->item(row, column))
            item->setForeground(QColor(ui::colors::WARNING()));
    };
    // The caller's actual price failure wins over the engine's generic
    // missing-context wording, exactly as in the interpretation evidence.
    const auto concrete_price_reason = [this](const services::CftcPricePositionAssessment* assessment) {
        const QString engine_reason = assessment ? cftc_unavailable_reason_wording(assessment->reason)
                                                 : tr("no price context was requested for this horizon");
        if (price_state_ == PriceState::Unavailable && assessment &&
            assessment->reason == services::CftcUnavailableReason::MissingPriceContext &&
            !price_reason_.trimmed().isEmpty()) {
            return price_reason_;
        }
        return engine_reason;
    };

    int row = 0;
    for (int horizon : {1, 4, 13}) {
        set_plain_cell(divergence_table_, row, 0, horizon == 1 ? tr("1 REPORT") : tr("%1 REPORTS").arg(horizon),
                       Qt::AlignLeft | Qt::AlignVCenter);
        const services::CftcPricePositionAssessment* assessment = nullptr;
        for (const auto& candidate : interpretation_.price_context) {
            if (candidate.participant_key == participants_[principal_index_].key &&
                candidate.horizon_reports == horizon) {
                assessment = &candidate;
                break;
            }
        }

        if (assessment && assessment->has_price_move) {
            set_plain_cell(divergence_table_, row, 1, cftc_price_move_evidence_text(*assessment));
            if (auto* item = divergence_table_->item(row, 1)) {
                item->setForeground(QColor(assessment->price_move > 0.0   ? ui::colors::POSITIVE()
                                           : assessment->price_move < 0.0 ? ui::colors::NEGATIVE()
                                                                          : ui::colors::TEXT_PRIMARY()));
                item->setToolTip(tr("Closes %1 → %2 of %3.")
                                     .arg(assessment->price_anchor_date.toString(Qt::ISODate),
                                          assessment->price_latest_date.toString(Qt::ISODate), price_symbol_));
            }
        } else if (price_state_ == PriceState::Pending) {
            set_plain_cell(divergence_table_, row, 1, tr("pending — price context is loading"));
            if (auto* item = divergence_table_->item(row, 1))
                item->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
        } else {
            unavailable_cell(row, 1, concrete_price_reason(assessment));
        }
        std::optional<double> net_move = net_flow_reading(horizon);
        if (!net_move && assessment && assessment->has_positioning_move)
            net_move = assessment->positioning_move;
        if (!net_move)
            net_move = state_net_flow(horizon);
        if (net_move) {
            // The NET Δ column is a normalized percentage, not a contract
            // count: it keeps two decimals (a negative sub-percent move must
            // never become "0") and the signed-decimal formatter.
            set_plain_cell(divergence_table_, row, 2, signed_decimal(*net_move, 2));
            if (auto* item = divergence_table_->item(row, 2))
                item->setForeground(QColor(*net_move > 0.0   ? ui::colors::POSITIVE()
                                           : *net_move < 0.0 ? ui::colors::NEGATIVE()
                                                             : ui::colors::TEXT_PRIMARY()));
        } else {
            const QString cftc_reason = cftc_net_unavailable_reason(horizon);
            if (!cftc_reason.isEmpty()) {
                unavailable_cell(row, 2, cftc_reason);
            } else if (assessment && assessment->evaluated) {
                set_plain_cell(divergence_table_, row, 2, tr("not emitted for this horizon"));
                if (auto* item = divergence_table_->item(row, 2))
                    item->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
            } else if (assessment && assessment->reason != services::CftcUnavailableReason::MissingPriceContext) {
                unavailable_cell(row, 2, cftc_unavailable_reason_wording(assessment->reason));
            } else {
                unavailable_cell(row, 2, tr("the CFTC net measurement is unavailable for this horizon"));
            }
        }

        // Neutral evaluation status only: the relationship conclusion itself
        // lives in the COT interpretation result.
        if (price_state_ == PriceState::Pending) {
            set_plain_cell(divergence_table_, row, 3, tr("pending — price context is loading"));
            if (auto* item = divergence_table_->item(row, 3))
                item->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
        } else if (assessment && !assessment->evaluated) {
            unavailable_cell(row, 3, concrete_price_reason(assessment));
        } else if (assessment && assessment->has_state) {
            set_plain_cell(divergence_table_, row, 3, tr("evaluated — see COT INTERPRETATION"));
            if (auto* item = divergence_table_->item(row, 3))
                item->setForeground(QColor(ui::colors::TEXT_PRIMARY()));
        } else {
            set_plain_cell(divergence_table_, row, 3, tr("evaluated, below the materiality threshold"));
            if (auto* item = divergence_table_->item(row, 3))
                item->setForeground(QColor(ui::colors::TEXT_SECONDARY()));
        }
        ++row;
    }
    fit_table_height(divergence_table_);

    const QDate report_date = history_.observations.isEmpty() ? QDate() : history_.observations.last().date;
    QVector<CftcPricePoint> aligned;
    if (price_state_ == PriceState::Ready) {
        for (const auto& point : std::as_const(price_)) {
            if (point.date <= report_date)
                aligned.append(point);
            else
                break;
        }
    }
    QDate high_date;
    QDate low_date;
    QStringList extremes;
    if (net_series.size() >= 2 && cftc_net_extreme_dates(net_series, high_date, low_date)) {
        const auto high_price = cftc_price_on_or_before(aligned, high_date);
        const auto low_price = cftc_price_on_or_before(aligned, low_date);
        extremes << tr("NET HIGH %1 at %2")
                        .arg(high_date.toString(Qt::ISODate),
                             high_price ? QString::number(*high_price, 'f', 2) : tr("price unavailable"));
        extremes << tr("NET LOW %1 at %2")
                        .arg(low_date.toString(Qt::ISODate),
                             low_price ? QString::number(*low_price, 'f', 2) : tr("price unavailable"));
    }
    if (extremes.isEmpty()) {
        divergence_extremes_lbl_->clear();
    } else {
        divergence_extremes_lbl_->setText(
            tr("Window extremes for %1 (price is the most recent close on or before the report date): %2")
                .arg(spec_label, extremes.join(QStringLiteral("  ·  "))));
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
        row.label = cftc_metric_participant_display_name(family_, participants_[p].key, participants_[p].label);
        row.short_label = participant_short_label(participants_[p].key);
        for (const QDate& date : std::as_const(dates)) {
            const auto it = by_date.constFind(date);
            CftcHeatmapCell cell;
            if (it == by_date.constEnd()) {
                cell.tooltip = tr("%1 — %2\nNo net position observation at this report.")
                                   .arg(row.label, date.toString(Qt::ISODate));
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
                    metric_text =
                        tr("Window percentile %1").arg(QString::number(point.percentile, 'f', 1) + QLatin1Char('%'));
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
                                                .arg(row.label, date.toString(Qt::ISODate), cftc_signed_net(point.net),
                                                     metric_text)
                                          : tr("%1 — %2\nNet %3\nNot available for this report.")
                                                .arg(row.label, date.toString(Qt::ISODate), cftc_signed_net(point.net));
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
                tr("Last %1 reports in the window, on raw net contracts. Each column uses the window's observations "
                   "up to that report (no lookahead); fewer than %2 observations stays blank.")
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
    if (monitor_content_)
        monitor_content_->setStyleSheet(workspace_style());
    if (chart_)
        chart_->refresh_theme();
    if (heatmap_)
        heatmap_->refresh_theme();
    if (sync_chart_)
        sync_chart_->refresh_theme();
    render_interpretation();
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
    // Every muted label, metadata line, table header and control uses the
    // readable secondary token; tertiary/dim stay only for disabled controls.
    // The history-display buttons deliberately use a quiet checked state while
    // the interpretation-horizon buttons use the panel accent, so the two
    // selectors never look alike.
    return QString("#cftcSection { background:%1; border:1px solid %2; border-radius:3px; }"
                   "#cftcSectionTitle { color:%3; font-size:9px; font-weight:700; letter-spacing:1px;"
                   " background:transparent; }"
                   "#cftcSectionMeta { color:%3; font-size:9px; background:transparent; }"
                   "#cftcHeaderTitle { color:%5; font-size:15px; font-weight:700; background:transparent; }"
                   "#cftcHeaderMeta { color:%3; font-size:10px; background:transparent; }"
                   "#cftcRangeBtn { background:transparent; color:%3; border:1px solid %2;"
                   " font-size:10px; font-weight:700; padding:3px 9px; }"
                   "#cftcRangeBtn:hover { color:%5; background:%6; }"
                   "#cftcRangeBtn:checked { background:%6; color:%5; border-color:%10; }"
                   "#cftcRangeBtn:disabled { color:%4; }"
                   "#cftcHorizonBtn { background:transparent; color:%3; border:1px solid %2;"
                   " font-size:10px; font-weight:700; padding:3px 9px; }"
                   "#cftcHorizonBtn:hover { color:%5; background:%6; }"
                   "#cftcHorizonBtn:checked { background:%7; color:%8; border-color:%7; }"
                   "#cftcEvidenceToggle { background:transparent; color:%3; border:1px solid %2;"
                   " font-size:9px; padding:3px 8px; }"
                   "#cftcEvidenceToggle:hover { color:%5; background:%6; }"
                   "#cftcEvidenceToggle:checked { background:%6; color:%5; border-color:%10; }"
                   "#cftcCard { background:%9; border:1px solid %2; border-radius:3px; }"
                   "#cftcCardLabel { color:%3; font-size:8px; font-weight:700; letter-spacing:1px;"
                   " background:transparent; }"
                   "#cftcCardSub { color:%3; font-size:9px; background:transparent; }"
                   "#cftcInterpretationHeadline { color:%5; font-size:14px; font-weight:700;"
                   " background:transparent; }"
                   "#cftcInterpretationBody { color:%5; font-size:11px; background:transparent; }"
                   "#cftcTable { background:transparent; color:%5; border:none; font-size:10px; }"
                   "#cftcTable QHeaderView::section { background:%9; color:%3; border:none;"
                   " border-bottom:1px solid %2; font-size:9px; font-weight:700; padding:3px; }"
                   "#cftcTable::item { padding:2px 4px; }")
        .arg(BG_SURFACE())     // %1
        .arg(BORDER_DIM())     // %2
        .arg(TEXT_SECONDARY()) // %3 muted-but-readable labels, metadata, headers
        .arg(TEXT_TERTIARY())  // %4 disabled only
        .arg(TEXT_PRIMARY())   // %5
        .arg(BG_HOVER())       // %6
        .arg(color_)           // %7 accent
        .arg(BG_BASE())        // %8
        .arg(BG_RAISED())      // %9
        .arg(BORDER_BRIGHT()); // %10
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
    if (monitor_tab_)
        monitor_tab_->setText(tr("Markets"));
    if (markets_btn_)
        markets_btn_->setText(tr("MARKETS"));
    if (monitor_family_lbl_)
        monitor_family_lbl_->setText(tr("REPORT FAMILY"));
    if (monitor_type_lbl_)
        monitor_type_lbl_->setText(tr("TYPE"));
    if (monitor_family_combo_ && monitor_family_combo_->count() == 3) {
        monitor_family_combo_->setItemText(0, tr("Legacy"));
        monitor_family_combo_->setItemText(1, tr("Disaggregated"));
        monitor_family_combo_->setItemText(2, tr("Financial (TFF)"));
    }
    if (monitor_type_combo_ && monitor_type_combo_->count() == 2) {
        monitor_type_combo_->setItemText(0, tr("Futures Only"));
        monitor_type_combo_->setItemText(1, tr("Combined"));
    }
    if (monitor_scan_btn_)
        monitor_scan_btn_->setText(tr("SCAN MARKETS"));
    if (monitor_backfill_btn_)
        monitor_backfill_btn_->setText(tr("BACKFILL HISTORY"));
    if (monitor_attention_title_)
        monitor_attention_title_->setText(tr("MARKETS REQUIRING ATTENTION"));
    if (monitor_all_title_)
        monitor_all_title_->setText(tr("ALL MARKETS"));
    if (monitor_attention_table_ && monitor_attention_table_->columnCount() == 5) {
        monitor_attention_table_->setHorizontalHeaderLabels(
            {tr("MARKET"), tr("GROUP"), tr("CLASS"), tr("DEVELOPMENT"), tr("REPORT")});
    }
    if (monitor_table_ && monitor_table_->columnCount() == 12) {
        monitor_table_->setHorizontalHeaderLabels(
            {tr("MARKET"), tr("GROUP"), tr("LATEST REPORT"), tr("NET %OI"), tr("%ILE (156R)"), tr("NET 1R"),
             tr("NET 4R"), tr("NET 13R"), tr("OPEN INTEREST"), tr("OI 1R"), tr("DEVELOPMENTS"), tr("STATUS")});
    }
    if (snapshot_title_)
        snapshot_title_->setText(tr("CURRENT SNAPSHOT"));
    if (interpretation_title_)
        interpretation_title_->setText(tr("COT INTERPRETATION"));
    if (sync_chart_title_)
        sync_chart_title_->setText(tr("PRICE + POSITIONING — SYNCHRONIZED"));
    if (positioning_title_)
        positioning_title_->setText(tr("POSITIONING"));
    if (weekly_title_)
        weekly_title_->setText(tr("WEEKLY CHANGES"));
    if (chart_title_)
        chart_title_->setText(tr("HISTORICAL POSITIONING"));
    if (stats_title_)
        stats_title_->setText(tr("POSITIONING STATISTICS & EXTREMES"));
    if (divergence_title_)
        divergence_title_->setText(tr("PRICE + POSITIONING — EVIDENCE"));
    if (heatmap_title_)
        heatmap_title_->setText(tr("POSITIONING HEATMAP"));
    if (range_lbl_)
        range_lbl_->setText(tr("HISTORY DISPLAY"));
    if (range_hint_lbl_)
        range_hint_lbl_->setText(tr("Charts and window statistics only — the interpretation always evaluates the "
                                    "full validated history."));
    if (horizon_lbl_)
        horizon_lbl_->setText(tr("INTERPRETATION HORIZON"));
    refresh_horizon_buttons();
    update_evidence_visibility();
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
        heatmap_metric_combo_->setItemText(1, tr("Window percentile"));
        heatmap_metric_combo_->setItemText(2, tr("Z-Score"));
        heatmap_metric_combo_->setItemText(3, tr("Weekly Δ Net"));
    }

    if (!history_.observations.isEmpty())
        rebuild_workspace();

    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
