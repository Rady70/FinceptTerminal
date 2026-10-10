// src/screens/economics/panels/CftcPanel.cpp
// The CFTC Commitments of Traders research workspace (see CftcPanel.h).
#include "screens/economics/panels/CftcPanel.h"

#include "core/logging/Logger.h"
#include "datahub/DataHub.h"
#include "datahub/DataHubMetaTypes.h"
#include "screens/economics/panels/CftcResearchCharts.h"
#include "screens/economics/panels/CftcResearchModel.h"
#include "screens/economics/panels/CftcResearchText.h"
#include "screens/economics/panels/CftcSyncChartData.h"
#include "services/economics/CftcMarketCatalog.h"
#include "services/economics/EconomicsService.h"
#include "ui/theme/Theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

using services::CftcFamily;
using services::CftcMonitorEntry;
using services::CftcObservation;

namespace {

constexpr const char* kCotScript = "cftc_data.py";
constexpr const char* kCotSourceId = "cftc";
constexpr const char* kCotColor = "#FF5722";
constexpr const char* kCotMonitorMaxRows = "25000";

struct CotPriceSpec {
    QString symbol;
    bool spot_index = false; // a spot index, not the futures contract
};

// Retained free public price path (Yahoo Finance through the DataHub
// market:history topic). Continuous `=F` symbols are front-month proxies that
// roll between contracts without adjustment; they are drawn for reference
// only, and the engine derives no price relationship from them
// (PriceSeriesNotRollSafe). Spot indices have no roll and are evaluated.
const QHash<QString, CotPriceSpec>& cot_price_specs() {
    static const QHash<QString, CotPriceSpec> specs = {
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

QString cot_family_code(CftcFamily family) {
    return services::cftc_family_code(family);
}

/// Plain text with line breaks as rich text (the note labels render HTML).
QString cot_note_html(const QString& text) {
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
}

QString cot_basis_text(bool futures_only) {
    return futures_only ? QCoreApplication::translate("CftcPanel", "futures only")
                        : QCoreApplication::translate("CftcPanel", "futures and options combined");
}

QString cot_dataset_note(const QString& dataset) {
    return dataset.isEmpty() ? QString() : QStringLiteral(" (publicreporting.cftc.gov %1)").arg(dataset);
}

/// A CFTC script failure may arrive as the typed error envelope; show the
/// human message only.
QString cot_error_text(const QString& raw) {
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

/// When the CFTC is expected to have published the report after `latest`:
/// positions are dated Tuesday and released the following Friday at 3:30 pm
/// Eastern, so the next report is due ten days after the latest report date.
/// A holiday-delayed release simply finds nothing new.
bool cot_next_report_due(const QDate& latest, const QDateTime& now_utc) {
    if (!latest.isValid())
        return true;
    const QDateTime due(latest.addDays(10), QTime(21, 0), QTimeZone::utc());
    return now_utc >= due;
}

QString cot_date_text(const QDate& date) {
    return date.isValid() ? date.toString(QStringLiteral("ddd d MMM yyyy")) : QString();
}

QString cot_signed_number(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    const double rounded = std::round(value * scale) / scale;
    const QString text =
        decimals == 0 ? cftc_contracts(std::fabs(rounded)) : QString::number(std::fabs(rounded), 'f', decimals);
    if (rounded > 0.0)
        return QStringLiteral("+") + text;
    if (rounded < 0.0)
        return QStringLiteral("-") + text;
    return text;
}

QTableWidget* cot_make_table(QWidget* parent, const QString& object_name) {
    auto* table = new QTableWidget(parent);
    table->setObjectName(object_name);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(24);
    table->horizontalHeader()->setHighlightSections(false);
    table->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return table;
}

void cot_set_cell(QTableWidget* table, int row, int column, const QString& text, Qt::Alignment align = Qt::AlignRight,
                  const QString& tooltip = QString(), const QColor& color = QColor()) {
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(align | Qt::AlignVCenter);
    if (!tooltip.isEmpty())
        item->setToolTip(tooltip);
    if (color.isValid())
        item->setForeground(color);
    table->setItem(row, column, item);
}

void cot_fit_table(QTableWidget* table) {
    // The header's size hint covers two-line labels before the first layout.
    int height = std::max(table->horizontalHeader()->height(), table->horizontalHeader()->sizeHint().height()) + 6;
    for (int row = 0; row < table->rowCount(); ++row)
        height += table->rowHeight(row);
    if (table->horizontalScrollBar() && table->horizontalScrollBar()->isVisible())
        height += table->horizontalScrollBar()->height();
    table->setFixedHeight(height);
}

std::optional<double> cot_json_number(const QJsonObject& row, const QString& key) {
    const QJsonValue value = row.value(key);
    if (value.isDouble() && std::isfinite(value.toDouble()))
        return value.toDouble();
    if (value.isString()) {
        bool ok = false;
        const double parsed = value.toString().toDouble(&ok);
        if (ok && std::isfinite(parsed))
            return parsed;
    }
    return std::nullopt;
}

} // namespace

// ── Construction ────────────────────────────────────────────────────────────

CftcPanel::CftcPanel(QWidget* parent) : EconPanelBase(kCotSourceId, kCotColor, parent) {
    setObjectName("cftcPanel");
    build_base_ui(this);
    set_stats_visible(false);
    identity_bar_ = findChild<QWidget*>(QStringLiteral("econTitleBar"));
    if (auto* toolbar = findChild<QWidget*>(QStringLiteral("econToolbar")))
        toolbar->setFixedHeight(48);
    fetch_btn_->setAccessibleName(tr("Refresh from the CFTC"));
    build_nav_bar();
    build_overview_page();
    build_market_page();
    apply_texts();
    show_view(View::Overview);
    connect(&services::EconomicsService::instance(), &services::EconomicsService::result_ready, this,
            &CftcPanel::on_result);
    arrange();
    refresh_panel_theme();
}

QFrame* CftcPanel::card(QWidget* parent, const QString& object_name) {
    auto* frame = new QFrame(parent);
    frame->setObjectName(object_name);
    frame->setProperty("cftcCard", true);
    return frame;
}

QLabel* CftcPanel::heading(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    label->setProperty("cftcHeading", true);
    return label;
}

QLabel* CftcPanel::note(QWidget* parent, const QString& object_name) {
    auto* label = new QLabel(parent);
    label->setObjectName(object_name);
    label->setProperty("cftcNote", true);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    return label;
}

QPushButton* CftcPanel::segment(QWidget* parent, QButtonGroup* group, int id, const QString& object_name) {
    auto* button = new QPushButton(parent);
    button->setObjectName(object_name);
    button->setProperty("cftcSegment", true);
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    group->addButton(button, id);
    return button;
}

void CftcPanel::build_controls(QHBoxLayout* toolbar) {
    title_lbl_ = new QLabel(this);
    title_lbl_->setObjectName("cftcTitle");
    toolbar->addWidget(title_lbl_);
    toolbar->addSpacing(10);

    family_group_ = new QButtonGroup(this);
    family_group_->setExclusive(true);
    for (int id = 0; id < 3; ++id) {
        auto* button = segment(this, family_group_, id, QStringLiteral("cftcFamily%1").arg(id));
        family_btns_ << button;
        toolbar->addWidget(button);
    }
    connect(family_group_, &QButtonGroup::idClicked, this, &CftcPanel::select_family);
    toolbar->addSpacing(8);

    basis_group_ = new QButtonGroup(this);
    basis_group_->setExclusive(true);
    for (int id = 0; id < 2; ++id) {
        auto* button = segment(this, basis_group_, id, QStringLiteral("cftcBasis%1").arg(id));
        basis_btns_ << button;
        toolbar->addWidget(button);
    }
    connect(basis_group_, &QButtonGroup::idClicked, this, &CftcPanel::select_basis);
    toolbar->addSpacing(8);

    market_combo_ = new QComboBox(this);
    market_combo_->setObjectName("cftcMarketCombo");
    market_combo_->setMinimumWidth(190);
    market_combo_->setFixedHeight(28);
    market_combo_->setMaxVisibleItems(28);
    market_combo_->addItem(QString(), QString());
    QString previous_class;
    for (const auto& market : services::cftc_market_catalog()) {
        if (market.asset_class != previous_class) {
            market_combo_->insertSeparator(market_combo_->count());
            previous_class = market.asset_class;
        }
        market_combo_->addItem(market.label, market.key);
    }
    connect(market_combo_, &QComboBox::activated, this, [this](int index) {
        const QString key = market_combo_->itemData(index).toString();
        if (key.isEmpty())
            show_view(View::Overview);
        else
            open_market(key, false);
    });
    toolbar->addWidget(market_combo_);
    toolbar->addSpacing(12);

    as_of_lbl_ = new QLabel(this);
    as_of_lbl_->setObjectName("cftcAsOf");
    as_of_lbl_->setWordWrap(true);
    as_of_lbl_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    toolbar->addWidget(as_of_lbl_, 1);
    sync_selectors();
}

void CftcPanel::build_nav_bar() {
    nav_bar_ = new QWidget(this);
    nav_bar_->setObjectName("cftcNavBar");
    auto* layout = new QHBoxLayout(nav_bar_);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(4);
    auto* views = new QButtonGroup(this);
    nav_all_ = segment(nav_bar_, views, 0, QStringLiteral("cftcNavAll"));
    nav_market_ = segment(nav_bar_, views, 1, QStringLiteral("cftcNavMarket"));
    connect(nav_all_, &QPushButton::clicked, this, [this] { show_view(View::Overview); });
    connect(nav_market_, &QPushButton::clicked, this, [this] {
        if (market_loaded_)
            show_view(View::Market);
    });
    layout->addWidget(nav_all_);
    layout->addWidget(nav_market_);
    nav_note_ = new QLabel(nav_bar_);
    nav_note_->setObjectName("cftcNavNote");
    layout->addSpacing(12);
    layout->addWidget(nav_note_, 1);
    auto* modes = new QButtonGroup(this);
    nav_charts_ = segment(nav_bar_, modes, 0, QStringLiteral("cftcNavCharts"));
    nav_table_ = segment(nav_bar_, modes, 1, QStringLiteral("cftcNavTable"));
    connect(nav_charts_, &QPushButton::clicked, this, [this] { show_table_view(false); });
    connect(nav_table_, &QPushButton::clicked, this, [this] { show_table_view(true); });
    layout->addWidget(nav_charts_);
    layout->addWidget(nav_table_);
    if (auto* root = qobject_cast<QVBoxLayout*>(this->layout()))
        root->insertWidget(1, nav_bar_);
}

void CftcPanel::build_overview_page() {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    overview_content_ = new QWidget(scroll);
    overview_content_->setObjectName("cftcContent");
    auto* layout = new QVBoxLayout(overview_content_);
    layout->setContentsMargins(12, 10, 12, 16);
    layout->setSpacing(12);

    status_lbl_ = note(overview_content_, QStringLiteral("cftcStatus"));
    status_lbl_->hide();
    layout->addWidget(status_lbl_);

    kpi_row_ = new QWidget(overview_content_);
    kpi_layout_ = new QGridLayout(kpi_row_);
    kpi_layout_->setContentsMargins(0, 0, 0, 0);
    kpi_layout_->setSpacing(8);
    for (int i = 0; i < 5; ++i) {
        Kpi kpi;
        kpi.frame = card(kpi_row_, QStringLiteral("cftcKpi%1").arg(i));
        kpi.frame->setProperty("kpi", true);
        auto* v = new QVBoxLayout(kpi.frame);
        v->setContentsMargins(12, 8, 12, 9);
        v->setSpacing(2);
        kpi.caption = new QLabel(kpi.frame);
        kpi.caption->setObjectName("cftcKpiCaption");
        kpi.value = new QLabel(kpi.frame);
        kpi.value->setProperty("kpiValue", true);
        kpi.value->setObjectName(QStringLiteral("cftcKpi%1Value").arg(i));
        kpi.sub = new QLabel(kpi.frame);
        kpi.sub->setObjectName("cftcKpiSub");
        kpi.sub->setWordWrap(true);
        v->addWidget(kpi.caption);
        v->addWidget(kpi.value);
        v->addWidget(kpi.sub);
        v->addStretch(1);
        kpis_ << kpi;
    }
    layout->addWidget(kpi_row_);

    // Asset-class tabs: one row of segments; they filter the cards, map and board.
    class_bar_ = new QWidget(overview_content_);
    class_bar_->setObjectName("cftcClassBar");
    auto* class_layout = new QHBoxLayout(class_bar_);
    class_layout->setContentsMargins(0, 0, 0, 0);
    class_layout->setSpacing(4);
    class_group_ = new QButtonGroup(this);
    class_group_->setExclusive(true);
    const QStringList classes = services::cftc_asset_class_order();
    for (int id = 0; id <= classes.size(); ++id) {
        auto* button = segment(class_bar_, class_group_, id, QStringLiteral("cftcClass%1").arg(id));
        class_btns_ << button;
        class_layout->addWidget(button);
    }
    class_layout->addStretch(1);
    class_btns_.first()->setChecked(true);
    connect(class_group_, &QButtonGroup::idClicked, this, &CftcPanel::select_asset_class);
    layout->addWidget(class_bar_);

    map_card_ = card(overview_content_, QStringLiteral("cftcMapCard"));
    auto* map_layout = new QVBoxLayout(map_card_);
    map_layout->setContentsMargins(12, 10, 12, 12);
    map_layout->setSpacing(6);
    map_title_ = heading(map_card_, QString());
    map_hint_ = note(map_card_);
    map_ = new CftcPositioningMap(map_card_);
    map_->setObjectName("cftcMap");
    connect(map_, &CftcPositioningMap::marketActivated, this, [this](const QString& key) { open_market(key, false); });
    map_layout->addWidget(map_title_);
    map_layout->addWidget(map_hint_);
    map_layout->addWidget(map_);
    layout->addWidget(map_card_);

    auto* board_card = card(overview_content_, QStringLiteral("cftcBoardCard"));
    auto* board_layout = new QVBoxLayout(board_card);
    board_layout->setContentsMargins(12, 10, 12, 12);
    board_layout->setSpacing(6);
    board_title_ = heading(board_card, QString());
    board_hint_ = note(board_card);
    board_layout->addWidget(board_title_);
    board_layout->addWidget(board_hint_);
    board_scroll_ = new QScrollArea(board_card);
    board_scroll_->setObjectName("cftcBoardScroll");
    board_scroll_->setFrameShape(QFrame::NoFrame);
    board_scroll_->setWidgetResizable(true);
    board_scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    board_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    board_ = new CftcPositioningBoard(board_scroll_);
    board_->setObjectName("cftcBoard");
    board_scroll_->setWidget(board_);
    board_layout->addWidget(board_scroll_);
    connect(board_, &CftcPositioningBoard::marketActivated, this,
            [this](const QString& key) { open_market(key, false); });
    layout->addWidget(board_card);

    auto* notes_card = card(overview_content_, QStringLiteral("cftcOverviewNotesCard"));
    auto* notes_layout = new QVBoxLayout(notes_card);
    notes_layout->setContentsMargins(12, 10, 12, 10);
    notes_layout->setSpacing(6);
    overview_notes_ = note(notes_card, QStringLiteral("cftcOverviewNotes"));
    overview_notes_->setTextFormat(Qt::RichText);
    overview_source_ = note(notes_card, QStringLiteral("cftcOverviewSource"));
    overview_source_->setTextFormat(Qt::RichText);
    notes_layout->addWidget(overview_notes_);
    notes_layout->addWidget(overview_source_);
    auto* notes_row = new QHBoxLayout;
    notes_row->addStretch(1);
    backfill_btn_ = new QPushButton(notes_card);
    backfill_btn_->setObjectName("cftcBackfill");
    backfill_btn_->setCursor(Qt::PointingHandCursor);
    connect(backfill_btn_, &QPushButton::clicked, this, &CftcPanel::request_backfill);
    notes_row->addWidget(backfill_btn_);
    notes_layout->addLayout(notes_row);
    layout->addWidget(notes_card);
    layout->addStretch(1);

    scroll->setWidget(overview_content_);
    overview_page_ = add_content_page(scroll);
}

void CftcPanel::build_market_page() {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    market_content_ = new QWidget(scroll);
    market_content_->setObjectName("cftcContent");
    auto* layout = new QVBoxLayout(market_content_);
    layout->setContentsMargins(12, 10, 12, 16);
    layout->setSpacing(12);

    auto* header = card(market_content_, QStringLiteral("cftcMarketHeader"));
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(14, 10, 14, 10);
    auto* header_text = new QVBoxLayout;
    header_text->setSpacing(2);
    market_title_ = new QLabel(header);
    market_title_->setObjectName("cftcMarketTitle");
    market_meta_ = note(header, QStringLiteral("cftcMarketMeta"));
    market_tags_ = new QLabel(header);
    market_tags_->setObjectName("cftcMarketTags");
    market_tags_->setTextFormat(Qt::RichText);
    market_tags_->hide();
    header_text->addWidget(market_title_);
    header_text->addWidget(market_meta_);
    header_text->addWidget(market_tags_);
    header_layout->addLayout(header_text, 1);
    market_badge_ = new QLabel(header);
    market_badge_->setObjectName("cftcBadge");
    market_badge_->setWordWrap(true);
    market_badge_->setMaximumWidth(360);
    market_badge_->hide();
    header_layout->addWidget(market_badge_, 0, Qt::AlignTop);
    layout->addWidget(header);

    auto* top = new QWidget(market_content_);
    top_layout_ = new QGridLayout(top);
    top_layout_->setContentsMargins(0, 0, 0, 0);
    top_layout_->setSpacing(12);
    conclusion_card_ = card(top, QStringLiteral("cftcConclusionCard"));
    auto* conclusion = new QVBoxLayout(conclusion_card_);
    conclusion->setContentsMargins(14, 10, 14, 12);
    conclusion->setSpacing(8);
    conclusion_title_ = heading(conclusion_card_, QString());
    readings_scroll_ = new QScrollArea(conclusion_card_);
    readings_scroll_->setObjectName("cftcReadingsScroll");
    readings_scroll_->setFrameShape(QFrame::NoFrame);
    readings_scroll_->setWidgetResizable(true);
    readings_scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    readings_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    readings_ = new CftcReadingsGrid(readings_scroll_);
    readings_->setObjectName("cftcReadings");
    readings_scroll_->setWidget(readings_);
    conclusion_footnote_ = note(conclusion_card_, QStringLiteral("cftcFootnote"));
    conclusion->addWidget(conclusion_title_);
    conclusion->addWidget(readings_scroll_);
    conclusion->addWidget(conclusion_footnote_);

    participants_card_ = card(top, QStringLiteral("cftcParticipantsCard"));
    auto* participants = new QVBoxLayout(participants_card_);
    participants->setContentsMargins(14, 10, 14, 12);
    participants->setSpacing(8);
    participants_title_ = heading(participants_card_, QString());
    bars_ = new CftcParticipantBars(participants_card_);
    bars_->setObjectName("cftcParticipantBars");
    participants_meta_ = note(participants_card_, QStringLiteral("cftcParticipantsMeta"));
    participants->addWidget(participants_title_);
    participants->addWidget(bars_);
    participants->addWidget(participants_meta_);
    participants->addStretch(1);
    layout->addWidget(top);
    market_sections_ << top;

    auto* history_card = card(market_content_, QStringLiteral("cftcHistoryCard"));
    auto* history = new QVBoxLayout(history_card);
    history->setContentsMargins(14, 10, 14, 12);
    history->setSpacing(6);
    auto* history_head = new QHBoxLayout;
    history_title_ = heading(history_card, QString());
    history_head->addWidget(history_title_);
    history_head->addStretch(1);
    range_group_ = new QButtonGroup(this);
    for (int years : {1, 3, 5, 10, 0}) {
        auto* button = segment(history_card, range_group_, years, QStringLiteral("cftcRange%1").arg(years));
        range_btns_ << button;
        history_head->addWidget(button);
    }
    connect(range_group_, &QButtonGroup::idClicked, this, &CftcPanel::set_range);
    history->addLayout(history_head);
    history_chart_ = new CftcHistoryChart(history_card);
    history_chart_->setObjectName("cftcHistoryChart");
    history->addWidget(history_chart_);
    history_hint_ = note(history_card, QStringLiteral("cftcHistoryHint"));
    history->addWidget(history_hint_);
    layout->addWidget(history_card);
    market_sections_ << history_card;

    auto* flows_card = card(market_content_, QStringLiteral("cftcFlowsCard"));
    auto* flows = new QVBoxLayout(flows_card);
    flows->setContentsMargins(14, 10, 14, 12);
    flows->setSpacing(6);
    flows_title_ = heading(flows_card, QString());
    flows_hint_ = note(flows_card, QStringLiteral("cftcFlowsHint"));
    flow_strip_ = new CftcFlowStrip(flows_card);
    flow_strip_->setObjectName("cftcFlowStrip");
    flows->addWidget(flows_title_);
    flows->addWidget(flows_hint_);
    flows->addWidget(flow_strip_);
    layout->addWidget(flows_card);
    market_sections_ << flows_card;

    auto* tables = new QWidget(market_content_);
    tables_layout_ = new QGridLayout(tables);
    tables_layout_->setContentsMargins(0, 0, 0, 0);
    tables_layout_->setSpacing(12);
    report_card_ = card(tables, QStringLiteral("cftcReportCard"));
    auto* report = new QVBoxLayout(report_card_);
    report->setContentsMargins(14, 10, 14, 12);
    report_title_ = heading(report_card_, QString());
    report_table_ = cot_make_table(report_card_, QStringLiteral("cftcReportTable"));
    report->addWidget(report_title_);
    report->addWidget(report_table_);
    report->addStretch(1);
    context_card_ = card(tables, QStringLiteral("cftcContextCard"));
    auto* context = new QVBoxLayout(context_card_);
    context->setContentsMargins(14, 10, 14, 12);
    context_title_ = heading(context_card_, QString());
    context_table_ = cot_make_table(context_card_, QStringLiteral("cftcContextTable"));
    context->addWidget(context_title_);
    context->addWidget(context_table_);
    context->addStretch(1);
    layout->addWidget(tables);
    market_sections_ << tables;

    auto* notes_card = card(market_content_, QStringLiteral("cftcMarketNotesCard"));
    auto* notes = new QVBoxLayout(notes_card);
    notes->setContentsMargins(14, 10, 14, 12);
    notes->setSpacing(6);
    market_notes_title_ = heading(notes_card, QString());
    market_notes_ = note(notes_card, QStringLiteral("cftcMarketNotes"));
    market_notes_->setTextFormat(Qt::RichText);
    notes->addWidget(market_notes_title_);
    notes->addWidget(market_notes_);
    layout->addWidget(notes_card);
    market_sections_ << notes_card;
    layout->addStretch(1);

    scroll->setWidget(market_content_);
    market_page_ = add_content_page(scroll);
}

void CftcPanel::apply_texts() {
    if (title_lbl_)
        title_lbl_->setText(tr("COMMITMENTS OF TRADERS"));
    // A narrow window gets short labels so the market selector, REFRESH and
    // CSV stay on screen; the tooltips keep the full names.
    const QStringList families = compact_toolbar_
                                     ? QStringList{tr("LEGACY"), tr("DISAGG."), tr("TFF")}
                                     : QStringList{tr("LEGACY"), tr("DISAGGREGATED"), tr("FINANCIAL (TFF)")};
    const QStringList family_tips = {
        tr("Legacy report: commercial, non-commercial and non-reportable traders; every market"),
        tr("Disaggregated report: producers/merchants, swap dealers, managed money and other reportables; "
           "commodity markets"),
        tr("Traders in Financial Futures: dealers, asset managers, leveraged funds and other reportables; financial "
           "markets")};
    for (int i = 0; i < family_btns_.size(); ++i) {
        family_btns_[i]->setText(families.value(i));
        family_btns_[i]->setToolTip(family_tips.value(i));
    }
    const QStringList bases = compact_toolbar_ ? QStringList{tr("FUTURES"), tr("F + O")}
                                               : QStringList{tr("FUTURES"), tr("FUTURES + OPTIONS")};
    for (int i = 0; i < basis_btns_.size(); ++i)
        basis_btns_[i]->setText(bases.value(i));
    if (basis_btns_.size() == 2) {
        basis_btns_[0]->setToolTip(tr("Futures-only positions"));
        basis_btns_[1]->setToolTip(tr("Futures and delta-adjusted options positions combined"));
    }
    if (market_combo_ && market_combo_->count() > 0)
        market_combo_->setItemText(0, tr("All markets"));
    if (fetch_btn_) {
        fetch_btn_->setText(tr("REFRESH"));
        fetch_btn_->setToolTip(tr("Read the newest reports from the CFTC (publicreporting.cftc.gov) into the local "
                                  "archive"));
    }
    if (nav_all_)
        nav_all_->setText(tr("ALL MARKETS"));
    if (nav_market_)
        nav_market_->setText(market_loaded_ ? services::cftc_market_definition(market_key_).label.toUpper()
                                            : tr("MARKET"));
    if (nav_charts_)
        nav_charts_->setText(tr("CHARTS"));
    if (nav_table_) {
        nav_table_->setText(tr("TABLE"));
        nav_table_->setToolTip(tr("The exact rows behind this view, newest first; CSV exports them"));
    }
    const QStringList captions = {tr("LATEST REPORT"), tr("SPECULATOR BIAS"), tr("CROWDED"), tr("FLIPPED THIS WEEK"),
                                  tr("LARGE WEEKLY MOVES")};
    for (int i = 0; i < kpis_.size(); ++i)
        kpis_[i].caption->setText(captions.value(i));
    if (!class_btns_.isEmpty()) {
        class_btns_[0]->setText(tr("ALL"));
        const QStringList classes = services::cftc_asset_class_order();
        for (int i = 1; i < class_btns_.size(); ++i)
            class_btns_[i]->setText(services::cftc_asset_class_label(classes.value(i - 1)).toUpper());
    }
    // The map title names the family's speculators; render_overview() sets it.
    if (map_hint_)
        map_hint_->setText(tr("Each tile is a market, placed by where the speculators' net position sits among its "
                              "previous 52 weekly reports. Left edge: green BULLISH, red BEARISH, grey NEUTRAL. "
                              "Hover a tile for the details; click it to open the market."));
    if (board_title_)
        board_title_->setText(tr("ALL GROUPS"));
    if (board_hint_)
        board_hint_->setText(tr("Bias and 52-week range of the speculators, then every group's net position as a "
                                "share of open interest, shaded amber toward the top of its 52 weeks and cyan toward "
                                "the bottom. 1W Δ: the net change since the previous report, as a share of that "
                                "report's open interest."));
    if (backfill_btn_) {
        backfill_btn_->setText(tr("LOAD ANNUAL FILES"));
        backfill_btn_->setToolTip(tr("Download the CFTC's annual history files for this report into the local archive "
                                     "(an alternative to the full read the first REFRESH performs)"));
    }
    if (conclusion_title_)
        conclusion_title_->setText(tr("READINGS · LAST 52 WEEKS · CHANGES OVER 1, 4 AND 13 WEEKS"));
    if (participants_title_)
        participants_title_->setText(tr("POSITIONS BY GROUP"));
    if (history_title_)
        history_title_->setText(tr("HISTORY"));
    const QStringList ranges = {tr("1Y"), tr("3Y"), tr("5Y"), tr("10Y"), tr("ALL")};
    for (int i = 0; i < range_btns_.size(); ++i)
        range_btns_[i]->setText(ranges.value(i));
    if (flows_title_)
        flows_title_->setText(tr("WEEK BY WEEK"));
    if (flows_hint_)
        flows_hint_->setText(tr("Each cell is one report: the change in a group's net position since the previous "
                                "weekly report, as a share of open interest (open interest itself in % change). Amber "
                                "= toward long, cyan = toward short; dark = no one-week change could be formed."));
    if (report_title_)
        report_title_->setText(tr("LATEST REPORT IN CONTRACTS"));
    if (context_title_)
        context_title_->setText(tr("HISTORICAL CONTEXT · NET POSITION AS % OF OPEN INTEREST"));
    if (market_notes_title_)
        market_notes_title_->setText(tr("SOURCE AND METHOD"));
}

// ── Layout ──────────────────────────────────────────────────────────────────

void CftcPanel::arrange() {
    const int w = width();
    const bool compact = w < 1350;
    if (compact != compact_toolbar_) {
        compact_toolbar_ = compact;
        apply_texts();
        set_as_of(as_of_text_, QDate());
    }
    if (title_lbl_)
        title_lbl_->setVisible(w >= 1100);
    if (market_combo_)
        market_combo_->setMinimumWidth(compact ? 150 : 190);
    if (kpi_layout_) {
        const int columns = w >= 1300 ? 5 : w >= 820 ? 3 : 2;
        for (const auto& kpi : kpis_)
            kpi_layout_->removeWidget(kpi.frame);
        for (int i = 0; i < kpis_.size(); ++i)
            kpi_layout_->addWidget(kpis_[i].frame, i / columns, i % columns);
        for (int c = 0; c < 5; ++c)
            kpi_layout_->setColumnStretch(c, c < columns ? 1 : 0);
    }
    const bool wide = w >= 1250;
    if (top_layout_) {
        top_layout_->removeWidget(conclusion_card_);
        top_layout_->removeWidget(participants_card_);
        // The readings grid needs the full width; the group bars sit below it.
        top_layout_->addWidget(conclusion_card_, 0, 0);
        top_layout_->addWidget(participants_card_, 1, 0);
        top_layout_->setColumnStretch(0, 1);
        top_layout_->setColumnStretch(1, 0);
    }
    if (tables_layout_) {
        tables_layout_->removeWidget(report_card_);
        tables_layout_->removeWidget(context_card_);
        tables_layout_->addWidget(report_card_, 0, 0);
        tables_layout_->addWidget(context_card_, wide ? 0 : 1, wide ? 1 : 0);
        tables_layout_->setColumnStretch(0, 1);
        tables_layout_->setColumnStretch(1, wide ? 1 : 0);
    }
}

void CftcPanel::resizeEvent(QResizeEvent* event) {
    EconPanelBase::resizeEvent(event);
    arrange();
}

// ── Navigation ──────────────────────────────────────────────────────────────

void CftcPanel::sync_selectors() {
    const int family_id = family_ == CftcFamily::Disaggregated ? 1 : family_ == CftcFamily::Tff ? 2 : 0;
    if (family_group_ && family_group_->button(family_id))
        family_group_->button(family_id)->setChecked(true);
    if (basis_group_ && basis_group_->button(futures_only_ ? 0 : 1))
        basis_group_->button(futures_only_ ? 0 : 1)->setChecked(true);
    if (range_group_ && range_group_->button(range_years_))
        range_group_->button(range_years_)->setChecked(true);
}

void CftcPanel::show_view(View view) {
    view_ = view;
    if (view == View::Market && !market_loaded_ && pending_market_.isEmpty())
        view_ = View::Overview;
    if (nav_all_)
        nav_all_->setChecked(view_ == View::Overview);
    if (nav_market_) {
        nav_market_->setEnabled(market_loaded_ || !pending_market_.isEmpty());
        nav_market_->setChecked(view_ == View::Market);
        nav_market_->setText(!market_key_.isEmpty() ? services::cftc_market_definition(market_key_).label.toUpper()
                                                    : tr("MARKET"));
    }
    if (market_combo_) {
        const QSignalBlocker block(market_combo_);
        const int index = market_combo_->findData(view_ == View::Market ? market_key_ : QString());
        market_combo_->setCurrentIndex(std::max(0, index));
    }
    show_table_view(table_view_);
}

void CftcPanel::show_table_view(bool table) {
    table_view_ = table;
    if (nav_charts_)
        nav_charts_->setChecked(!table);
    if (nav_table_)
        nav_table_->setChecked(table);
    if (table) {
        const bool market = view_ == View::Market && market_loaded_;
        const QJsonArray rows = market ? market_table_rows() : overview_table_rows();
        const QString title = market ? tr("CFTC COT · %1 · %2 report, %3")
                                           .arg(services::cftc_market_definition(market_key_).label,
                                                cftc_family_name(family_), cot_basis_text(futures_only_))
                                     : tr("CFTC COT · all markets · %1 report, %2")
                                           .arg(cftc_family_name(family_), cot_basis_text(futures_only_));
        // shows the shared table page (or its empty state), columns grouped by participant
        display(rows, title, market ? market_table_columns() : overview_table_columns());
    } else {
        show_content_page(view_ == View::Market ? market_page_ : overview_page_);
    }
    if (identity_bar_)
        identity_bar_->setVisible(table);
}

void CftcPanel::select_family(int id) {
    const CftcFamily family = id == 1 ? CftcFamily::Disaggregated : id == 2 ? CftcFamily::Tff : CftcFamily::Legacy;
    if (family == family_)
        return;
    family_ = family;
    entries_.clear();
    rows_.clear();
    overview_loaded_ = false;
    request_overview(false);
    if (view_ == View::Market && !market_key_.isEmpty())
        open_market(market_key_, false);
    else
        render_overview();
}

void CftcPanel::select_basis(int id) {
    const bool futures_only = id == 0;
    if (futures_only == futures_only_)
        return;
    futures_only_ = futures_only;
    entries_.clear();
    rows_.clear();
    overview_loaded_ = false;
    request_overview(false);
    if (view_ == View::Market && !market_key_.isEmpty())
        open_market(market_key_, false);
    else
        render_overview();
}

void CftcPanel::activate() {
    if (!overview_loaded_ && pending_overview_.isEmpty())
        request_overview(false);
    sync_selectors();
    show_view(view_);
}

void CftcPanel::on_fetch() {
    if (view_ == View::Market && !market_key_.isEmpty())
        open_market(market_key_, true);
    else
        request_overview(true);
}

void CftcPanel::on_result(const QString& request_id, const services::EconomicsResult& result) {
    if (result.source_id != QLatin1String(kCotSourceId))
        return;
    if (!pending_overview_.isEmpty() && request_id == pending_overview_) {
        pending_overview_.clear();
        accept_overview(result, pending_overview_refresh_);
        return;
    }
    if (!pending_market_.isEmpty() && request_id == pending_market_) {
        pending_market_.clear();
        accept_market(result);
        return;
    }
    if (!pending_backfill_.isEmpty() && request_id == pending_backfill_) {
        pending_backfill_.clear();
        if (!result.success) {
            set_status(tr("Loading the annual files failed: %1").arg(cot_error_text(result.error)), false);
            return;
        }
        const QJsonObject data = result.data.value(QStringLiteral("data")).toObject();
        QStringList failed;
        for (const QJsonValue& year : data.value(QStringLiteral("failed_years")).toArray())
            failed << QString::number(year.toInt());
        QString text = tr("Annual files loaded: %1 new and %2 revised reports stored.")
                           .arg(data.value(QStringLiteral("rows_inserted")).toInt())
                           .arg(data.value(QStringLiteral("rows_updated")).toInt());
        if (!failed.isEmpty())
            text += QLatin1Char(' ') + tr("Years that could not be read: %1.").arg(failed.join(QStringLiteral(", ")));
        set_status(text, false);
        request_overview(false);
    }
}

// ── All markets ─────────────────────────────────────────────────────────────

void CftcPanel::set_status(const QString& text, bool busy) {
    if (!status_lbl_)
        return;
    status_lbl_->setText(text);
    status_lbl_->setProperty("busy", busy);
    status_lbl_->style()->unpolish(status_lbl_);
    status_lbl_->style()->polish(status_lbl_);
    status_lbl_->setVisible(!text.isEmpty());
}

void CftcPanel::request_overview(bool refresh) {
    pending_overview_ = QStringLiteral("cftc_overview_%1").arg(++request_seq_);
    pending_overview_refresh_ = refresh;
    if (refresh) {
        const bool nothing_stored = std::none_of(rows_.cbegin(), rows_.cend(), [](const CftcMarketRow& row) {
            return row.availability == CftcRowAvailability::Current ||
                   row.availability == CftcRowAvailability::Stored || row.availability == CftcRowAvailability::Old;
        });
        set_status(nothing_stored
                       ? tr("Downloading the official CFTC history of every market in this report into the local "
                            "archive for the first time. This takes a few minutes; later refreshes only fetch new "
                            "weeks.")
                       : tr("Refreshing from the CFTC… the stored reports stay on screen until the new ones arrive."),
                   true);
    } else if (!overview_loaded_) {
        set_status(tr("Opening the stored reports…"), true);
    }
    services::EconomicsService::instance().execute(kCotSourceId, kCotScript, QStringLiteral("cot_monitor"),
                                                   {cot_family_code(family_),
                                                    futures_only_ ? QStringLiteral("true") : QStringLiteral("false"),
                                                    QStringLiteral("all"), QString::fromLatin1(kCotMonitorMaxRows),
                                                    refresh ? QStringLiteral("true") : QStringLiteral("false")},
                                                   pending_overview_, /*bypass_cache=*/true);
}

bool CftcPanel::overview_refresh_due() const {
    QDate latest;
    for (const auto& row : rows_) {
        if (row.availability == CftcRowAvailability::NotCovered || row.availability == CftcRowAvailability::Unavailable)
            continue;
        if (row.report_date.isValid() && row.report_date > latest)
            latest = row.report_date;
    }
    return cot_next_report_due(latest, QDateTime::currentDateTimeUtc());
}

void CftcPanel::accept_overview(const services::EconomicsResult& result, bool refresh) {
    if (!result.success) {
        if (refresh)
            set_status(tr("The CFTC could not be reached: %1. The stored reports are shown.")
                           .arg(cot_error_text(result.error)),
                       false);
        else
            set_status(tr("The stored reports could not be opened: %1").arg(cot_error_text(result.error)), false);
        render_overview();
        return;
    }
    const QJsonObject data = result.data.value(QStringLiteral("data")).toObject();
    const QJsonObject parameters = result.data.value(QStringLiteral("parameters")).toObject();
    const services::CftcMonitorModel model = services::cftc_parse_monitor_payload(
        data, family_, futures_only_, QDateTime::currentDateTimeUtc().date(), cftc_principal_participant_key(family_));
    if (!model.error.isEmpty()) {
        set_status(model.error, false);
        render_overview();
        return;
    }
    entries_ = model.entries;
    rows_ = cftc_market_rows(entries_);
    archive_ = data.value(QStringLiteral("archive")).toObject();
    overview_dataset_ = parameters.value(QStringLiteral("dataset")).toString();
    overview_retrieved_ = parameters.value(QStringLiteral("retrieved_at")).toString();
    overview_loaded_ = true;
    overview_from_refresh_ = refresh;

    int interpreted = 0;
    for (const auto& entry : entries_)
        interpreted += entry.interpreted ? 1 : 0;
    const QString key = cot_family_code(family_) + (futures_only_ ? QStringLiteral("/fo") : QStringLiteral("/comb"));
    if (!refresh) {
        set_status(QString(), false);
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QDateTime last = auto_refreshed_.value(key);
        const bool recently = last.isValid() && last.secsTo(now) < 3600;
        if (!recently && (interpreted == 0 || overview_refresh_due())) {
            auto_refreshed_[key] = now;
            render_overview();
            request_overview(true);
            return;
        }
    } else {
        int updated = 0;
        int failed = 0;
        for (const auto& entry : entries_) {
            if (entry.provider_status == QLatin1String("updated"))
                ++updated;
            else if (entry.status == services::CftcMonitorStatus::ArchiveOnly ||
                     entry.status == services::CftcMonitorStatus::Unavailable)
                ++failed;
        }
        const QString when = QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm 'UTC'"));
        QString text =
            updated > 0
                ? tr("Refreshed from the CFTC at %1: new or revised reports for %2 markets.").arg(when).arg(updated)
                : tr("Refreshed from the CFTC at %1: the stored reports were already current.").arg(when);
        if (failed > 0)
            text +=
                QLatin1Char(' ') + tr("%1 markets could not be refreshed; their stored reports are shown.").arg(failed);
        set_status(text, false);
    }
    render_overview();
}

void CftcPanel::render_overview() {
    render_kpis();
    if (map_title_)
        map_title_->setText(tr("POSITIONING MAP · %1 · LEVEL IN THE LAST 52 WEEKS")
                                .arg(cftc_group_name(family_, cftc_principal_participant_key(family_)).toUpper()));
    // Until a market has a report (first download of a report family) there
    // is nothing to compare: the status line explains, and the KPI cards,
    // tabs, map and board stay hidden instead of listing empty rows.
    const bool any_report = std::any_of(rows_.cbegin(), rows_.cend(), [](const CftcMarketRow& row) {
        return row.availability == CftcRowAvailability::Current || row.availability == CftcRowAvailability::Stored ||
               row.availability == CftcRowAvailability::Old;
    });
    for (QWidget* section : {kpi_row_, class_bar_, static_cast<QWidget*>(map_card_),
                             overview_content_->findChild<QWidget*>(QStringLiteral("cftcBoardCard"))})
        if (section)
            section->setVisible(any_report);
    // Tabs for the asset classes this report has; a tab that empties falls back to all.
    const QStringList classes = services::cftc_asset_class_order();
    for (int i = 1; i < class_btns_.size(); ++i) {
        const QString code = classes.value(i - 1);
        const bool present = std::any_of(rows_.cbegin(), rows_.cend(), [&code](const CftcMarketRow& row) {
            return row.asset_class == code && row.availability != CftcRowAvailability::NotCovered &&
                   row.availability != CftcRowAvailability::Unavailable;
        });
        class_btns_[i]->setVisible(present);
        if (!present && asset_class_ == code) {
            asset_class_.clear();
            class_btns_[0]->setChecked(true);
        }
    }
    const QVector<CftcMarketRow> shown = visible_rows();
    if (map_)
        map_->set_rows(shown, cftc_group_name(family_, cftc_principal_participant_key(family_)));
    QVector<CftcBoardRow> board_rows;
    for (const QString& asset_class : services::cftc_asset_class_order()) {
        bool heading_added = false;
        for (const auto& row : shown) {
            // A market without a usable report has no row: it is named once in
            // the notes below, with the reason.
            if (row.asset_class != asset_class || row.availability == CftcRowAvailability::NotCovered ||
                row.availability == CftcRowAvailability::Unavailable)
                continue;
            if (!heading_added) {
                CftcBoardRow heading_row;
                heading_row.heading = true;
                heading_row.label = services::cftc_asset_class_label(asset_class);
                board_rows << heading_row;
                heading_added = true;
            }
            CftcBoardRow market_row;
            market_row.key = row.market_key;
            market_row.label = row.label;
            market_row.market = row;
            board_rows << market_row;
        }
    }
    QStringList columns;
    for (const auto& participant : services::cftc_family_participants(family_))
        columns << participant.key;
    if (board_) {
        board_->set_data(board_rows, columns, cftc_principal_participant_key(family_), family_);
        const int scrollbar = 14;
        board_scroll_->setFixedHeight(board_->height() + scrollbar);
    }
    render_overview_notes();
    // Markets the selected report does not publish cannot be opened.
    if (auto* model = qobject_cast<QStandardItemModel*>(market_combo_ ? market_combo_->model() : nullptr)) {
        for (int i = 0; i < market_combo_->count(); ++i) {
            const QString key = market_combo_->itemData(i).toString();
            if (key.isEmpty())
                continue;
            bool covered = true;
            for (const auto& row : rows_)
                if (row.market_key == key && row.availability == CftcRowAvailability::NotCovered)
                    covered = false;
            if (QStandardItem* item = model->item(i))
                item->setEnabled(covered);
        }
    }
    const CftcOverviewFigures figures = cftc_overview_figures(rows_);
    if (as_of_lbl_) {
        // The family and basis are the highlighted toolbar segments; the label
        // carries only the report date and how fresh the copy is.
        QStringList parts;
        if (figures.latest_report.isValid())
            parts << tr("Positions as of %1").arg(figures.latest_report.toString(QStringLiteral("d MMM yyyy")));
        if (!overview_retrieved_.isEmpty() && overview_from_refresh_) {
            const QDateTime retrieved = QDateTime::fromString(overview_retrieved_, Qt::ISODate);
            parts << tr("refreshed %1")
                         .arg(retrieved.isValid()
                                  ? retrieved.toUTC().toString(QStringLiteral("d MMM HH:mm 'UTC'"))
                                  : overview_retrieved_.left(16).replace(QLatin1Char('T'), QLatin1Char(' ')));
        } else if (overview_loaded_ && figures.latest_report.isValid())
            parts << tr("from stored data");
        set_as_of(parts.join(QLatin1Char('\n')), figures.latest_report);
    }
    if (table_view_ && view_ == View::Overview)
        show_table_view(true);
}

void CftcPanel::render_kpis() {
    if (kpis_.size() < 5)
        return;
    const CftcOverviewFigures figures = cftc_overview_figures(visible_rows());
    auto names = [](const QStringList& list) {
        if (list.size() <= 5)
            return list.join(QStringLiteral(", "));
        return QStringList(list.mid(0, 5)).join(QStringLiteral(", ")) +
               QCoreApplication::translate("CftcPanel", " and %1 more").arg(list.size() - 5);
    };
    auto set = [this](int i, const QString& value, const QString& sub) {
        kpis_[i].value->setText(value);
        kpis_[i].sub->setText(sub);
    };
    if (!overview_loaded_ || rows_.isEmpty()) {
        for (int i = 0; i < kpis_.size(); ++i)
            set(i, QStringLiteral("…"), QString());
        return;
    }
    const int with_report = figures.markets_current + figures.markets_stored + figures.markets_old;
    QString report_sub =
        tr("Tuesday positions, published by the CFTC each Friday · %1 markets with a report").arg(with_report);
    if (figures.markets_stored > 0)
        report_sub += QLatin1Char(' ') + tr("· refresh failed for %1").arg(figures.markets_stored);
    if (figures.markets_old > 0)
        report_sub += QLatin1Char(' ') + tr("· %1 with an old report").arg(figures.markets_old);
    set(0, figures.latest_report.isValid() ? figures.latest_report.toString(QStringLiteral("d MMM yyyy")) : tr("none"),
        report_sub);
    const QString group = cftc_group_name(family_, cftc_principal_participant_key(family_));
    set(1, tr("▲ %1   ▼ %2   = %3").arg(figures.bullish.size()).arg(figures.bearish.size()).arg(figures.neutral.size()),
        tr("%1 positioning over 52 weeks: BULLISH / BEARISH / NEUTRAL").arg(group));
    kpis_[1].frame->setToolTip(
        tr("BULLISH: %1\nBEARISH: %2\nNEUTRAL: %3\n\n%4")
            .arg(names(figures.bullish), names(figures.bearish), names(figures.neutral), cftc_method_note()));
    set(2, tr("L %1 · S %2").arg(figures.crowded_long.size()).arg(figures.crowded_short.size()),
        figures.crowded_long.isEmpty() && figures.crowded_short.isEmpty()
            ? tr("no market has %1 crowded long or crowded short").arg(group)
            : tr("long: %1 · short: %2")
                  .arg(figures.crowded_long.isEmpty() ? tr("none") : names(figures.crowded_long),
                       figures.crowded_short.isEmpty() ? tr("none") : names(figures.crowded_short)));
    set(3, QString::number(figures.flipped.size()),
        figures.flipped.isEmpty() ? tr("%1 kept its side in every market").arg(group)
                                  : tr("%1 net position changed side: %2").arg(group, names(figures.flipped)));
    set(4, QString::number(figures.large_move_markets.size()),
        figures.large_move_markets.isEmpty()
            ? tr("no LARGE one-week change of %1").arg(group)
            : tr("%1 one-week change LARGE: %2").arg(group, names(figures.large_move_markets)));
}

void CftcPanel::select_asset_class(int id) {
    const QStringList classes = services::cftc_asset_class_order();
    asset_class_ = id <= 0 ? QString() : classes.value(id - 1);
    render_overview();
}

QVector<CftcMarketRow> CftcPanel::visible_rows() const {
    if (asset_class_.isEmpty())
        return rows_;
    QVector<CftcMarketRow> out;
    for (const auto& row : rows_)
        if (row.asset_class == asset_class_)
            out << row;
    return out;
}

void CftcPanel::set_as_of(const QString& text, const QDate& report_date) {
    // A narrow toolbar keeps only the report date; the tooltip has the rest.
    as_of_text_ = text;
    if (report_date.isValid())
        as_of_short_ = report_date.toString(QStringLiteral("d MMM yyyy"));
    else if (text.isEmpty())
        as_of_short_.clear();
    if (!as_of_lbl_)
        return;
    as_of_lbl_->setText(compact_toolbar_ ? as_of_short_ : as_of_text_);
    as_of_lbl_->setToolTip(as_of_text_);
}

void CftcPanel::render_overview_notes() {
    if (!overview_notes_ || !overview_source_)
        return;
    QStringList notes;
    QStringList not_covered;
    QStringList stored;
    QStringList old_reports;
    QStringList unavailable;
    for (const auto& row : rows_) {
        switch (row.availability) {
            case CftcRowAvailability::NotCovered:
                not_covered << row.label;
                break;
            case CftcRowAvailability::Stored:
                stored << QStringLiteral("%1 (%2)").arg(row.label, row.availability_note);
                break;
            case CftcRowAvailability::Old:
                old_reports << QStringLiteral("%1 (%2)").arg(row.label, row.availability_note);
                break;
            case CftcRowAvailability::Unavailable:
                unavailable << QStringLiteral("%1 (%2)").arg(row.label, row.availability_note);
                break;
            case CftcRowAvailability::Current:
                break;
        }
    }
    // Before the first download of a report nothing is stored for any market;
    // the status line says so, and the markets are not listed one by one.
    const bool any_report = std::any_of(rows_.cbegin(), rows_.cend(), [](const CftcMarketRow& row) {
        return row.availability == CftcRowAvailability::Current || row.availability == CftcRowAvailability::Stored ||
               row.availability == CftcRowAvailability::Old;
    });
    if (!unavailable.isEmpty() && any_report)
        notes << tr("No usable report: %1.").arg(unavailable.join(QStringLiteral("; "))).toHtmlEscaped();
    if (!old_reports.isEmpty())
        notes << tr("Latest report older than two weeks (the contract may no longer be reported): %1.")
                     .arg(old_reports.join(QStringLiteral("; ")))
                     .toHtmlEscaped();
    if (!stored.isEmpty())
        notes << tr("The last refresh failed for these markets; their stored reports are shown: %1.")
                     .arg(stored.join(QStringLiteral("; ")))
                     .toHtmlEscaped();
    if (!not_covered.isEmpty())
        notes << tr("Not part of the %1 report (the CFTC does not publish it for these markets): %2.")
                     .arg(cftc_family_name(family_), not_covered.join(QStringLiteral(", ")))
                     .toHtmlEscaped();
    overview_notes_->setText(notes.join(QStringLiteral("<br/>")));
    overview_notes_->setVisible(!notes.isEmpty());

    const int total = archive_.value(QStringLiteral("total_observations")).toInt();
    QString source =
        tr("Source: CFTC Commitments of Traders, %1 report, %2%3.")
            .arg(cftc_family_name(family_), cot_basis_text(futures_only_), cot_dataset_note(overview_dataset_))
            .toHtmlEscaped();
    if (total > 0)
        source += QLatin1Char(' ') + tr("Local archive: %1 stored reports.").arg(cftc_contracts(total)).toHtmlEscaped();
    source += QLatin1Char(' ') +
              tr("Interpretation rule set %1.").arg(services::cftc_interpretation_rule_set_version()).toHtmlEscaped();
    source += QStringLiteral("<br/>") + cot_note_html(cftc_method_note());
    overview_source_->setText(source);
}

void CftcPanel::request_backfill() {
    pending_backfill_ = QStringLiteral("cftc_backfill_%1").arg(++request_seq_);
    set_status(tr("Downloading the CFTC's annual history files for this report… completed years are kept if a later "
                  "year fails."),
               true);
    services::EconomicsService::instance().execute(kCotSourceId, kCotScript, QStringLiteral("cot_backfill"),
                                                   {cot_family_code(family_),
                                                    futures_only_ ? QStringLiteral("true") : QStringLiteral("false"),
                                                    QString(), QString(), QStringLiteral("all")},
                                                   pending_backfill_, /*bypass_cache=*/true);
}

QJsonArray CftcPanel::overview_table_rows() const {
    QJsonArray out;
    const auto participants = services::cftc_family_participants(family_);
    for (const auto& row : rows_) {
        if (row.availability == CftcRowAvailability::NotCovered)
            continue;
        QJsonObject object;
        object[QStringLiteral("market")] = row.label;
        object[QStringLiteral("date")] = row.report_date.isValid() ? row.report_date.toString(Qt::ISODate) : QString();
        object[QStringLiteral("data")] = row.availability == CftcRowAvailability::Current  ? tr("current")
                                         : row.availability == CftcRowAvailability::Stored ? tr("stored")
                                         : row.availability == CftcRowAvailability::Old    ? tr("old report")
                                                                                           : tr("unavailable");
        for (const auto& cell : row.cells) {
            const QString name = cftc_group_name(family_, cell.participant_key);
            if (cell.has_value)
                object[name + tr(" net %OI")] = std::round(cell.net_pct_oi * 100.0) / 100.0;
            if (cell.percentile)
                object[name + tr(" percentile")] = std::round(*cell.percentile * 1000.0) / 10.0;
        }
        if (row.has_open_interest)
            object[tr("open interest")] = row.open_interest;
        if (row.oi_change_4r)
            object[tr("open interest 4w %")] = std::round(*row.oi_change_4r * 100.0) / 100.0;
        if (row.concentration_long)
            object[tr("top-4 long %OI")] = *row.concentration_long;
        out.append(object);
    }
    return out;
}

QStringList CftcPanel::overview_table_columns() const {
    QStringList columns{QStringLiteral("market"), QStringLiteral("date"), QStringLiteral("data")};
    for (const auto& participant : services::cftc_family_participants(family_)) {
        const QString name = cftc_group_name(family_, participant.key);
        columns << name + tr(" net %OI") << name + tr(" percentile");
    }
    columns << tr("open interest") << tr("open interest 4w %") << tr("top-4 long %OI");
    return columns;
}

// ── One market ──────────────────────────────────────────────────────────────

void CftcPanel::open_market(const QString& market_key, bool refresh) {
    if (market_key.isEmpty())
        return;
    // The page shows one market of one report family and basis. Any other
    // identity (another market, or the same market after a family or basis
    // switch) clears it: figures of another report are never left on screen.
    const QString identity = market_key + QLatin1Char('/') + cot_family_code(family_) +
                             (futures_only_ ? QStringLiteral("/fo") : QStringLiteral("/comb"));
    const bool switching = identity != market_identity_;
    market_identity_ = identity;
    market_key_ = market_key;
    pending_market_ = QStringLiteral("cftc_market_%1").arg(++request_seq_);
    pending_market_key_ = market_key;
    pending_market_refresh_ = refresh;
    const QString label = services::cftc_market_definition(market_key).label;
    if (switching) {
        market_loaded_ = false;
        history_ = {};
        result_ = {};
        trails_.clear();
        market_rows_json_ = QJsonArray();
        market_params_ = {};
        market_title_->setText(label);
        market_meta_->setText(tr("Opening the stored %1 reports…").arg(cftc_family_name(family_)));
        market_badge_->hide();
        for (QWidget* section : std::as_const(market_sections_))
            section->hide();
        readings_->set_rows({});
        readings_scroll_->setFixedHeight(readings_->height() + 14); // room for the sideways scrollbar
        market_tags_->clear();
        market_tags_->hide();
        conclusion_footnote_->clear();
        bars_->set_rows({});
        history_chart_->clear();
        flow_strip_->set_rows({});
        report_table_->setRowCount(0);
        context_table_->setRowCount(0);
        market_notes_->clear();
        ++price_token_;
        price_state_ = PriceState::None;
        price_.clear();
        price_market_key_.clear();
    } else if (refresh) {
        market_meta_->setText(market_meta_->text() + QStringLiteral("  ·  ") + tr("refreshing from the CFTC…"));
    }
    view_ = View::Market;
    show_view(View::Market);
    services::EconomicsService::instance().execute(kCotSourceId, kCotScript, QStringLiteral("cot_market"),
                                                   {market_key, cot_family_code(family_),
                                                    futures_only_ ? QStringLiteral("true") : QStringLiteral("false"),
                                                    refresh ? QStringLiteral("true") : QStringLiteral("false")},
                                                   pending_market_, /*bypass_cache=*/true);
}

void CftcPanel::accept_market(const services::EconomicsResult& result) {
    const QString label = services::cftc_market_definition(pending_market_key_).label;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QString auto_key = pending_market_key_ + QLatin1Char('/') + cot_family_code(family_) +
                             (futures_only_ ? QStringLiteral("/fo") : QStringLiteral("/comb"));
    if (!result.success) {
        if (!pending_market_refresh_) {
            // Nothing stored yet (or the archive could not be opened): read it
            // from the CFTC once.
            market_auto_refreshed_[auto_key] = now;
            open_market(pending_market_key_, true);
            market_meta_->setText(
                tr("Reading the official %1 history of %2 from the CFTC…").arg(cftc_family_name(family_), label));
            return;
        }
        if (!market_loaded_) {
            market_meta_->setText(tr("%1 could not be loaded: %2. ALL MARKETS lists the markets this report covers.")
                                      .arg(label, cot_error_text(result.error)));
        } else {
            market_badge_->setText(
                tr("Refresh failed: %1. The stored reports are shown.").arg(cot_error_text(result.error)));
            market_badge_->show();
        }
        return;
    }
    const QJsonArray rows = result.data.value(QStringLiteral("data")).toArray();
    services::CftcHistory history = services::cftc_parse_history(rows, family_);
    QString error = history.error;
    if (error.isEmpty())
        error = services::cftc_apply_report_basis(history, futures_only_);
    if (!error.isEmpty()) {
        market_meta_->setText(tr("%1: %2").arg(label, error));
        return;
    }
    market_rows_json_ = rows;
    market_params_ = result.data.value(QStringLiteral("parameters")).toObject();
    history_ = history;
    market_loaded_ = true;
    for (QWidget* section : std::as_const(market_sections_))
        section->show();
    interpret_market();
    render_market();
    if (table_view_)
        show_table_view(true);
    if (price_market_key_ != market_key_)
        request_price();
    if (!pending_market_refresh_ && cot_next_report_due(history_.observations.last().date, now)) {
        const QDateTime last = market_auto_refreshed_.value(auto_key);
        if (!last.isValid() || last.secsTo(now) >= 3600) {
            market_auto_refreshed_[auto_key] = now;
            open_market(market_key_, true);
        }
    }
}

void CftcPanel::interpret_market() {
    if (history_.observations.isEmpty())
        return;
    const bool price_ready = price_state_ == PriceState::Ready && price_market_key_ == market_key_;
    const services::CftcInterpretationInput input = cftc_make_interpretation_input(
        family_, history_.observations,
        futures_only_ ? QStringLiteral("futures_only") : QStringLiteral("futures_and_options_combined"),
        price_ready ? price_ : QVector<services::CftcPricePoint>{}, price_ready ? price_series_label() : QString(),
        price_ready && !price_spot_index_, price_spot_index_, QDateTime::currentDateTimeUtc().date());
    result_ = services::cftc_interpret(input);
    trails_.clear();
    const auto participants = services::cftc_family_participants(family_);
    for (int i = 0; i < participants.size(); ++i)
        trails_ << services::cftc_participant_trail(history_.observations, i, participants[i].key, result_.config);
}

void CftcPanel::render_market() {
    if (!market_loaded_)
        return;
    render_market_header();
    render_readings();
    render_participant_bars();
    render_history();
    render_flows();
    render_tables();
    render_market_notes();
    if (nav_market_) {
        nav_market_->setEnabled(true);
        nav_market_->setText(services::cftc_market_definition(market_key_).label.toUpper());
    }
    if (as_of_lbl_ && view_ == View::Market) {
        set_as_of(tr("Positions as of %1").arg(result_.report_date.toString(QStringLiteral("d MMM yyyy"))),
                  result_.report_date);
    }
}

void CftcPanel::render_market_header() {
    const CftcObservation& latest = history_.observations.last();
    const QString label = services::cftc_market_definition(market_key_).label;
    market_title_->setText(latest.market.isEmpty() ? label : QStringLiteral("%1  ·  %2").arg(label, latest.market));
    QStringList meta;
    if (!latest.contract_code.isEmpty())
        meta << tr("CFTC contract %1").arg(latest.contract_code);
    if (!latest.units.isEmpty())
        meta << latest.units.toLower();
    meta << tr("%1 report, %2").arg(cftc_family_name(family_), cot_basis_text(futures_only_));
    meta << tr("report of %1").arg(cot_date_text(result_.report_date));
    if (result_.report_age_available)
        meta << tr("%1 days ago").arg(result_.report_age_days);
    meta << tr("history since %1 (%2 reports)")
                .arg(history_.observations.first().date.toString(Qt::ISODate))
                .arg(cftc_contracts(history_.observations.size()));
    market_meta_->setText(meta.join(QStringLiteral("  ·  ")));
    const QString status = market_params_.value(QStringLiteral("status")).toString();
    QString badge;
    if (result_.report_outdated) {
        badge = tr("Old report: the latest report is %1 days old; the CFTC publishes weekly, so this contract may no "
                   "longer be reported.")
                    .arg(result_.report_age_days);
    } else if (status == QLatin1String("archive_only") && pending_market_refresh_) {
        badge = tr("Stored reports shown: the refresh failed (%1).")
                    .arg(market_params_.value(QStringLiteral("refresh_note")).toString());
    }
    market_badge_->setText(badge);
    market_badge_->setVisible(!badge.isEmpty());

    // The speculators' standard labels, as a line of tags under the title.
    const QString principal = cftc_principal_participant_key(family_);
    const services::CftcParticipantTrail* trail = nullptr;
    for (const auto& candidate : trails_)
        if (candidate.participant_key == principal)
            trail = &candidate;
    auto tag = [](const QString& text, const QColor& tone, bool filled) {
        return filled ? QStringLiteral("<span style='background-color:%1; color:%2; font-weight:700;'>&nbsp;%3&nbsp;"
                                       "</span>")
                            .arg(tone.name(), cftc_ink_on(tone).name(), text.toHtmlEscaped())
                      : QStringLiteral("<span style='color:%1; font-weight:700;'>%2</span>")
                            .arg(tone.name(), text.toHtmlEscaped());
    };
    QStringList tags;
    QStringList tips;
    for (const auto& p : result_.participants) {
        if (p.participant_key != principal)
            continue;
        const CftcBias bias = cftc_positioning_bias(p, result_.config);
        if (bias != CftcBias::Unavailable) {
            tags << tag(cftc_bias_label(bias), cftc_bias_color(bias), bias != CftcBias::Neutral);
            tips << cftc_bias_reason(bias, p, result_.config);
        }
        if (p.has_net_pct_oi && p.historical_percentile_available) {
            const CftcLevelBand band = cftc_level_band(p.percentile, result_.config);
            tags << tag(cftc_level_band_label(band), cftc_band_color(band), false);
        }
        const CftcCrowding crowding = cftc_crowding(p);
        if (crowding != CftcCrowding::None) {
            tags << tag(cftc_crowding_label(crowding), cftc_tag_color(cftc_crowding_label(crowding)), false);
            tips << cftc_crowding_reason(crowding, p, result_.config);
        }
        if (trail) {
            const CftcSideFlip flip = cftc_side_flip(trail->net_pct_oi);
            if (flip.direction != 0) {
                tags << tag(cftc_side_flip_label(flip), cftc_tag_color(cftc_side_flip_label(flip)), false);
                tips << cftc_side_flip_reason(flip);
            }
        }
    }
    if (!tags.isEmpty()) {
        market_tags_->setText(tr("%1:").arg(cftc_group_name(family_, principal)).toHtmlEscaped() +
                              QStringLiteral("&nbsp;&nbsp;") + tags.join(QStringLiteral("&nbsp;&nbsp;")));
        market_tags_->setToolTip(tips.join(QLatin1Char('\n')));
    }
    market_tags_->setVisible(!tags.isEmpty());
}

void CftcPanel::render_readings() {
    using namespace ui::colors;
    const services::CftcInterpretationConfig& config = result_.config;
    const QString principal = cftc_principal_participant_key(family_);
    const int horizons[3] = {1, 4, 13};
    QVector<CftcReadingsRow> rows;
    for (const auto& p : result_.participants) {
        CftcReadingsRow row;
        row.name = cftc_group_name(family_, p.participant_key);
        row.principal = p.participant_key == principal;
        const services::CftcParticipantTrail* trail = nullptr;
        for (const auto& candidate : trails_)
            if (candidate.participant_key == p.participant_key)
                trail = &candidate;
        if (p.has_net_pct_oi) {
            row.position.tag = p.net_pct_oi > 0.0 ? tr("NET LONG") : p.net_pct_oi < 0.0 ? tr("NET SHORT") : tr("FLAT");
            row.position.tag_color = p.net_pct_oi > 0.0 ? QColor(AMBER()) : QColor(CYAN());
            row.position.value = tr("%1 of OI").arg(cftc_abs_pct(p.net_pct_oi));
            row.position.tooltip =
                tr("%1: %2 (%3 contracts)")
                    .arg(row.name, cftc_net_position_text(p.net_pct_oi), cot_signed_number(p.net_position, 0));
        }
        if (p.has_net_pct_oi && p.historical_percentile_available) {
            const CftcLevelBand band = cftc_level_band(p.percentile, config);
            row.level.tag = cftc_level_band_label(band);
            row.level.tag_color = cftc_band_color(band);
            row.level.value = cftc_percentile_ordinal(p.percentile);
            row.level.tooltip = cftc_level_statement(family_, p, trail, config, 4, /*include_usual_range=*/true);
        } else if (p.has_net_pct_oi) {
            row.level.value = tr("—");
            row.level.tooltip = tr("Fewer than %1 earlier reports to compare with.").arg(config.history_window);
        }
        for (int i = 0; i < 3; ++i) {
            const services::CftcHorizonFlowReading* reading = nullptr;
            for (const auto& candidate : p.flow_readings)
                if (candidate.horizon_reports == horizons[i])
                    reading = &candidate;
            CftcReadingCell& cell = row.horizons[i];
            if (!reading || !reading->evaluated || !reading->has_net_flow) {
                if (reading && !reading->evaluated)
                    cell.tooltip = cftc_unavailable_reason_wording(reading->reason);
                continue;
            }
            const CftcFlowType type = cftc_flow_type(*reading, config);
            cell.tag = cftc_flow_label(type);
            cell.tag_color = cftc_flow_color(type);
            cell.value = tr("net %1").arg(cftc_signed_pct(reading->net_flow));
            cell.size = cftc_change_size_label(cftc_change_size(reading->has_net_rank, reading->net_rank, config));
            cell.tooltip = cftc_change_statement(family_, p, trail, horizons[i], config);
        }
        QStringList tips;
        if (row.principal) {
            const CftcBias bias = cftc_positioning_bias(p, config);
            if (bias != CftcBias::Unavailable) {
                row.tags << cftc_bias_label(bias);
                tips << cftc_bias_reason(bias, p, config);
            }
            const CftcCrowding crowding = cftc_crowding(p);
            if (crowding != CftcCrowding::None) {
                row.tags << cftc_crowding_label(crowding);
                tips << cftc_crowding_reason(crowding, p, config);
            }
            if (trail) {
                const CftcSideFlip flip = cftc_side_flip(trail->net_pct_oi);
                if (flip.direction != 0) {
                    row.tags << cftc_side_flip_label(flip);
                    tips << cftc_side_flip_reason(flip);
                }
            }
        }
        const QString transition = cftc_transition_statement(family_, p, trail);
        if (!transition.isEmpty()) {
            row.tags << tr("TURNING");
            tips << transition;
        }
        row.tags_tooltip = tips.join(QLatin1Char('\n'));
        rows << row;
    }
    // Open interest.
    CftcReadingsRow oi;
    oi.name = tr("Open interest");
    oi.open_interest = true;
    if (result_.open_interest_available) {
        oi.position.value = tr("%1 contracts").arg(cftc_contracts(result_.open_interest));
        oi.position.tooltip = cftc_open_interest_statement(result_, 1);
    }
    for (int i = 0; i < 3; ++i) {
        for (const auto& reading : result_.open_interest_readings) {
            if (reading.horizon_reports != horizons[i] || !reading.evaluated || !reading.has_oi_change)
                continue;
            CftcReadingCell& cell = oi.horizons[i];
            cell.tag = reading.oi_change >= 0.0 ? tr("RISING") : tr("FALLING");
            cell.tag_color = reading.oi_change >= 0.0 ? QColor(AMBER()) : QColor(CYAN());
            cell.value = cftc_signed_pct(reading.oi_change);
            cell.size = cftc_change_size_label(cftc_change_size(reading.has_rank, reading.rank, config));
            cell.tooltip = cftc_open_interest_statement(result_, horizons[i]);
        }
    }
    rows << oi;
    readings_->set_rows(rows);
    readings_scroll_->setFixedHeight(readings_->height() + 14); // room for the sideways scrollbar

    // Below the grid: the price relationship where one exists, and what could
    // not be assessed.
    CftcConclusionInput input;
    input.family = family_;
    input.result = &result_;
    input.trails = trails_;
    input.horizon = 4;
    input.price_state = price_context_state();
    input.price_unavailable_note = price_state_ == PriceState::Unavailable ? price_reason_ : QString();
    input.price_series_label = price_series_label();
    const CftcConclusion conclusion = cftc_compose_conclusion(input);
    QStringList footnote;
    const QString price = cftc_price_statement(family_, result_, principal, 4, price_series_label());
    if (!price.isEmpty())
        footnote << price;
    if (result_.report_outdated)
        footnote << tr("Old report: the latest available report is %1 days old.").arg(result_.report_age_days);
    if (!conclusion.not_assessed.isEmpty())
        footnote << tr("Not assessed — %1.").arg(conclusion.not_assessed.join(QStringLiteral("; ")));
    conclusion_footnote_->setText(footnote.join(QStringLiteral(" ")));
    conclusion_footnote_->setVisible(!footnote.isEmpty());
}

void CftcPanel::render_participant_bars() {
    const auto participants = services::cftc_family_participants(family_);
    const QString principal = cftc_principal_participant_key(family_);
    const CftcObservation& latest = history_.observations.last();
    QVector<CftcParticipantBarRow> rows;
    for (int i = 0; i < participants.size(); ++i) {
        const services::CftcPositionMetrics metrics = services::cftc_position_metrics(latest, i);
        CftcParticipantBarRow row;
        row.key = participants[i].key;
        row.name = cftc_group_name(family_, participants[i].key);
        row.has_long = metrics.has_long_pct_oi;
        row.long_pct = metrics.long_pct_oi;
        row.has_short = metrics.has_short_pct_oi;
        row.short_pct = metrics.short_pct_oi;
        row.has_net = metrics.has_net_pct_oi;
        row.net_pct = metrics.net_pct_oi;
        row.net_contracts = metrics.net_position;
        row.principal = participants[i].key == principal;
        if (i < trails_.size()) {
            row.percentile = trails_[i].percentile_now;
            row.percentile_4r = trails_[i].percentile_4r;
            row.percentile_13r = trails_[i].percentile_13r;
        }
        for (const auto& interpretation : result_.participants)
            if (interpretation.participant_key == row.key)
                row.net_flow_1w = cftc_one_week_net_flow(interpretation);
        rows << row;
    }
    bars_->set_rows(rows);
    participants_title_->setText(
        tr("POSITIONS BY GROUP · REPORT OF %1").arg(result_.report_date.toString(Qt::ISODate)));
    QStringList meta;
    if (result_.open_interest_available) {
        QString oi = tr("Open interest %1 contracts").arg(cftc_contracts(result_.open_interest));
        for (const auto& reading : result_.open_interest_readings)
            if (reading.horizon_reports == 1 && reading.evaluated && reading.has_oi_change)
                oi += tr(" (%1 this week)").arg(cftc_signed_pct(reading.oi_change));
        meta << oi;
    }
    if (latest.concentration_gross_4_long || latest.concentration_gross_4_short) {
        QStringList conc;
        if (latest.concentration_gross_4_long)
            conc << tr("long %1").arg(cftc_abs_pct(*latest.concentration_gross_4_long));
        if (latest.concentration_gross_4_short)
            conc << tr("short %1").arg(cftc_abs_pct(*latest.concentration_gross_4_short));
        meta << tr("4 largest traders: %1 of open interest").arg(conc.join(QStringLiteral(", ")));
    }
    if (latest.traders_total)
        meta << tr("%1 reportable traders").arg(cftc_contracts(*latest.traders_total));
    meta << tr("Bars: long and short positions as a share of open interest; diamond = net. Track: percentile vs the "
               "previous 52 weekly reports (hollow = 4 and 13 weeks ago). 1W Δ: net change since the previous "
               "report, as a share of that report's open interest.");
    participants_meta_->setText(meta.join(QStringLiteral("  ·  ")));
}

void CftcPanel::render_history() {
    const auto& observations = history_.observations;
    const QDate last = observations.last().date;
    const QDate first = observations.first().date;
    for (auto* button : range_btns_) {
        const int years = range_group_->id(button);
        button->setEnabled(years == 0 || first <= last.addYears(-years));
    }
    QDate from = range_years_ == 0 ? first : last.addYears(-range_years_);
    if (from < first)
        from = first;
    history_chart_->set_range(from, last);
    const auto participants = services::cftc_family_participants(family_);
    const QString principal = cftc_principal_participant_key(family_);
    int principal_index = -1;
    QVector<CftcHistorySeries> series;
    for (int i = 0; i < participants.size(); ++i) {
        CftcHistorySeries s;
        s.key = participants[i].key;
        s.name = cftc_group_name(family_, participants[i].key);
        s.color = cftc_group_color(i);
        s.points = cftc_net_pct_series(observations, i);
        s.emphasized = participants[i].key == principal;
        if (s.emphasized)
            principal_index = i;
        series << s;
    }
    history_chart_->set_positioning(series);
    history_chart_->set_percentile(principal_index >= 0 ? cftc_percentile_series(observations, principal_index)
                                                        : QVector<CftcSeriesPoint>{},
                                   cftc_group_name(family_, principal));
    history_chart_->set_open_interest(cftc_open_interest_points(observations));
    history_chart_->set_concentration(cftc_concentration_points(observations, true),
                                      cftc_concentration_points(observations, false));
    QVector<CftcSeriesPoint> price;
    if (price_state_ == PriceState::Ready && price_market_key_ == market_key_) {
        price.reserve(price_.size());
        for (const auto& point : price_)
            price.append({point.date, true, point.close});
    }
    history_chart_->set_price(price, price_series_label());
    QString hint;
    if (price_state_ == PriceState::Ready && price_market_key_ == market_key_) {
        hint = price_spot_index_
                   ? tr("Price: Yahoo Finance %1, a spot index (not the futures contract itself), daily closes.")
                         .arg(price_symbol_)
                   : tr("Price: Yahoo Finance %1, a continuous front-month futures series shown for reference. It "
                        "joins successive contracts without adjustment, so no price-positioning relationship is "
                        "calculated from it.")
                         .arg(price_symbol_);
    } else if (price_state_ == PriceState::Pending) {
        hint = tr("Loading the price series…");
    } else if (price_state_ == PriceState::Unavailable && !price_reason_.isEmpty()) {
        hint = tr("No price pane: %1.").arg(price_reason_);
    }
    hint += (hint.isEmpty() ? QString() : QStringLiteral(" ")) +
            tr("Hover for exact values; click a group in the legend to hide or show it.");
    history_hint_->setText(hint);
}

void CftcPanel::render_flows() {
    const auto participants = services::cftc_family_participants(family_);
    QVector<CftcFlowRow> rows;
    for (int i = 0; i < participants.size(); ++i)
        rows << CftcFlowRow{cftc_group_name(family_, participants[i].key),
                            cftc_weekly_flow_cells(history_.observations, i, 52), false};
    rows << CftcFlowRow{tr("Open interest"), cftc_weekly_flow_cells(history_.observations, -1, 52), true};
    flow_strip_->set_rows(rows);
}

void CftcPanel::render_tables() {
    const auto participants = services::cftc_family_participants(family_);
    const CftcObservation& latest = history_.observations.last();
    // The canonical row of the latest report, for the published spreading
    // positions and per-group trader counts the observation does not retain.
    QJsonObject latest_json;
    for (const QJsonValue& value : std::as_const(market_rows_json_)) {
        const QJsonObject row = value.toObject();
        if (services::cftc_parse_report_date(row.value(QStringLiteral("report_date_as_yyyy_mm_dd")).toString()) ==
            latest.date)
            latest_json = row;
    }
    report_table_->clear();
    report_table_->setColumnCount(8);
    report_table_->setHorizontalHeaderLabels({tr("GROUP"), tr("LONG"), tr("SHORT"), tr("SPREADING"), tr("NET"),
                                              tr("NET %OI"), tr("NET Δ 1W"), tr("TRADERS L / S")});
    report_table_->setRowCount(participants.size() + 1);
    const QColor muted(ui::colors::TEXT_SECONDARY());
    for (int i = 0; i < participants.size(); ++i) {
        const QString key = participants[i].key;
        const services::CftcPositionMetrics m = services::cftc_position_metrics(latest, i);
        cot_set_cell(report_table_, i, 0, cftc_group_name(family_, key), Qt::AlignLeft);
        if (m.has_long)
            cot_set_cell(report_table_, i, 1, cftc_contracts(m.long_leg));
        if (m.has_short)
            cot_set_cell(report_table_, i, 2, cftc_contracts(m.short_leg));
        if (const auto spread = cot_json_number(latest_json, key + QStringLiteral("_spread")))
            cot_set_cell(report_table_, i, 3, cftc_contracts(*spread));
        if (m.has_net)
            cot_set_cell(report_table_, i, 4, cot_signed_number(m.net_position, 0));
        if (m.has_net_pct_oi)
            cot_set_cell(report_table_, i, 5, cftc_signed_pct(m.net_pct_oi));
        const services::CftcChange change = services::cftc_weekly_change(
            services::cftc_metric_series(history_.observations, i, services::CftcMetricKind::Net), latest.date);
        if (change.has_value)
            cot_set_cell(report_table_, i, 6, cot_signed_number(change.value, 0), Qt::AlignRight,
                         tr("vs the report of %1").arg(change.previous_date.toString(Qt::ISODate)),
                         change.value >= 0 ? QColor(ui::colors::AMBER()) : QColor(ui::colors::CYAN()));
        const auto traders_long = cot_json_number(latest_json, key + QStringLiteral("_traders_long"));
        const auto traders_short = cot_json_number(latest_json, key + QStringLiteral("_traders_short"));
        if (traders_long || traders_short)
            cot_set_cell(report_table_, i, 7,
                         QStringLiteral("%1 / %2").arg(traders_long ? cftc_contracts(*traders_long) : QString(),
                                                       traders_short ? cftc_contracts(*traders_short) : QString()));
    }
    const int total = participants.size();
    cot_set_cell(report_table_, total, 0, tr("Open interest"), Qt::AlignLeft, QString(), muted);
    if (latest.open_interest)
        cot_set_cell(report_table_, total, 1, cftc_contracts(*latest.open_interest), Qt::AlignRight,
                     tr("Total open interest; every group's long and short positions (with spreading) add up to it"));
    const services::CftcChange oi_change =
        services::cftc_weekly_change(services::cftc_open_interest_series(history_.observations), latest.date);
    if (oi_change.has_value)
        cot_set_cell(report_table_, total, 6, cot_signed_number(oi_change.value, 0));
    if (latest.traders_total)
        cot_set_cell(report_table_, total, 7, tr("%1 total").arg(cftc_contracts(*latest.traders_total)));
    // Every column fits its content (group names are never elided); the last
    // column takes the remaining width.
    report_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    report_table_->horizontalHeader()->setStretchLastSection(true);
    cot_fit_table(report_table_);

    context_table_->clear();
    context_table_->setColumnCount(6);
    context_table_->setHorizontalHeaderLabels({tr("GROUP"), tr("NOW"), tr("PERCENTILE\nNOW · 4W · 13W AGO"),
                                               tr("52-WEEK\nRANGE"), tr("FULL HISTORY\nRANGE"),
                                               tr("LAST TIME\nBEYOND NOW")});
    context_table_->setRowCount(participants.size());
    for (int i = 0; i < participants.size(); ++i) {
        const CftcHistoricalContext context = cftc_historical_context(history_.observations, i);
        cot_set_cell(context_table_, i, 0, cftc_group_name(family_, participants[i].key), Qt::AlignLeft);
        if (!context.available)
            continue;
        cot_set_cell(context_table_, i, 1, cftc_signed_pct(context.now));
        if (i < trails_.size() && trails_[i].percentile_now) {
            auto pct = [](const std::optional<double>& v) {
                return v ? QString::number(std::lround(*v * 100.0)) : QStringLiteral("·");
            };
            cot_set_cell(
                context_table_, i, 2,
                QStringLiteral("%1 · %2 · %3")
                    .arg(pct(trails_[i].percentile_now), pct(trails_[i].percentile_4r), pct(trails_[i].percentile_13r)),
                Qt::AlignCenter,
                tr("Percentile of the net position among the previous 52 weekly reports: now %1, 4 weeks ago "
                   "%2, 13 weeks ago %3")
                    .arg(pct(trails_[i].percentile_now), pct(trails_[i].percentile_4r), pct(trails_[i].percentile_13r)),
                cftc_percentile_color(*trails_[i].percentile_now) == QColor(ui::colors::BG_RAISED())
                    ? QColor()
                    : (*trails_[i].percentile_now >= 0.9   ? QColor(ui::colors::AMBER())
                       : *trails_[i].percentile_now <= 0.1 ? QColor(ui::colors::CYAN())
                                                           : QColor()));
        }
        cot_set_cell(
            context_table_, i, 3,
            QStringLiteral("%1 … %2").arg(cftc_signed_pct(context.window_min), cftc_signed_pct(context.window_max)),
            Qt::AlignCenter,
            tr("Lowest %1 on %2; highest %3 on %4 (last %5 reports)")
                .arg(cftc_signed_pct(context.window_min), context.window_min_date.toString(Qt::ISODate),
                     cftc_signed_pct(context.window_max), context.window_max_date.toString(Qt::ISODate))
                .arg(context.window_reports));
        cot_set_cell(
            context_table_, i, 4,
            QStringLiteral("%1 … %2").arg(cftc_signed_pct(context.record_min), cftc_signed_pct(context.record_max)),
            Qt::AlignCenter,
            tr("Lowest %1 on %2; highest %3 on %4 (since %5)")
                .arg(cftc_signed_pct(context.record_min), context.record_min_date.toString(Qt::ISODate),
                     cftc_signed_pct(context.record_max), context.record_max_date.toString(Qt::ISODate),
                     context.history_start.toString(Qt::ISODate)));
        cot_set_cell(context_table_, i, 5,
                     context.beyond_found ? context.beyond_since.toString(Qt::ISODate)
                                          : tr("never (since %1)").arg(context.history_start.toString(Qt::ISODate)),
                     Qt::AlignCenter,
                     tr("The most recent earlier report whose net position was at least as far %1 as today's")
                         .arg(context.now >= (context.window_min + context.window_max) / 2.0 ? tr("up") : tr("down")));
    }
    context_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    context_table_->horizontalHeader()->setStretchLastSection(true);
    cot_fit_table(context_table_);
}

void CftcPanel::render_market_notes() {
    QStringList lines;
    const QString dataset = market_params_.value(QStringLiteral("dataset")).toString();
    lines << tr("Source: CFTC Commitments of Traders, %1 report, %2%3.")
                 .arg(cftc_family_name(family_), cot_basis_text(futures_only_), cot_dataset_note(dataset))
                 .toHtmlEscaped();
    const QString full_read = market_params_.value(QStringLiteral("full_history_fetched_at")).toString();
    const QString retrieved = market_params_.value(QStringLiteral("retrieved_at")).toString();
    QString archive = tr("Local archive: %1 reports from %2 to %3")
                          .arg(cftc_contracts(market_params_.value(QStringLiteral("count")).toDouble()),
                               market_params_.value(QStringLiteral("first_report_date")).toString(),
                               market_params_.value(QStringLiteral("last_report_date")).toString());
    if (!full_read.isEmpty())
        archive +=
            tr("; complete history last read %1").arg(full_read.left(16).replace(QLatin1Char('T'), QLatin1Char(' ')));
    if (!retrieved.isEmpty())
        archive += tr("; opened %1").arg(retrieved.left(16).replace(QLatin1Char('T'), QLatin1Char(' ')));
    lines << (archive + QLatin1Char('.')).toHtmlEscaped();
    if (!history_.set_aside.isEmpty()) {
        QStringList items;
        for (const auto& item : std::as_const(history_.set_aside))
            items << QStringLiteral("%1 (%2)").arg(item.date_label.left(10), item.reason);
        lines << tr("Reports set aside because their own published figures are inconsistent (all other reports are "
                    "used): %1.")
                     .arg(items.join(QStringLiteral("; ")))
                     .toHtmlEscaped();
    }
    if (history_.basis_inherited > 0)
        lines << tr("%1 stored reports did not state their report basis; they come from the %2 source and are treated "
                    "as %2.")
                     .arg(history_.basis_inherited)
                     .arg(cot_basis_text(futures_only_))
                     .toHtmlEscaped();
    lines << cot_note_html(cftc_method_note());
    lines << tr("Interpretation rule set %1 (deterministic; descriptive only).")
                 .arg(result_.rule_set_version)
                 .toHtmlEscaped();
    market_notes_->setText(lines.join(QStringLiteral("<br/>")));
}

void CftcPanel::set_range(int years) {
    range_years_ = years;
    if (market_loaded_)
        render_history();
}

QJsonArray CftcPanel::market_table_rows() const {
    QJsonArray out;
    const auto participants = services::cftc_family_participants(family_);
    for (int r = history_.observations.size() - 1; r >= 0; --r) {
        const CftcObservation& obs = history_.observations[r];
        QJsonObject row;
        row[QStringLiteral("date")] = obs.date.toString(Qt::ISODate);
        if (obs.open_interest)
            row[tr("open interest")] = *obs.open_interest;
        for (int p = 0; p < participants.size(); ++p) {
            const QString name = cftc_group_name(family_, participants[p].key);
            const services::CftcPositionMetrics m = services::cftc_position_metrics(obs, p);
            if (m.has_long)
                row[name + tr(" long")] = m.long_leg;
            if (m.has_short)
                row[name + tr(" short")] = m.short_leg;
            if (m.has_net_pct_oi)
                row[name + tr(" net %OI")] = std::round(m.net_pct_oi * 100.0) / 100.0;
        }
        if (obs.concentration_gross_4_long)
            row[tr("top-4 long %OI")] = *obs.concentration_gross_4_long;
        if (obs.concentration_gross_4_short)
            row[tr("top-4 short %OI")] = *obs.concentration_gross_4_short;
        if (obs.traders_total)
            row[tr("traders")] = *obs.traders_total;
        out.append(row);
    }
    return out;
}

QStringList CftcPanel::market_table_columns() const {
    QStringList columns{QStringLiteral("date"), tr("open interest")};
    for (const auto& participant : services::cftc_family_participants(family_)) {
        const QString name = cftc_group_name(family_, participant.key);
        columns << name + tr(" long") << name + tr(" short") << name + tr(" net %OI");
    }
    columns << tr("top-4 long %OI") << tr("top-4 short %OI") << tr("traders");
    return columns;
}

// ── Price context ───────────────────────────────────────────────────────────

void CftcPanel::request_price() {
    price_market_key_ = market_key_;
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
    const auto it = cot_price_specs().constFind(market_key_);
    if (it == cot_price_specs().constEnd()) {
        price_state_ = PriceState::Unavailable;
        price_reason_ = tr("no free price source is mapped for this market");
        if (market_loaded_)
            render_market();
        return;
    }
    price_symbol_ = it->symbol;
    price_spot_index_ = it->spot_index;
    price_state_ = PriceState::Pending;
    // Price context comes from the retained free public path through its
    // DataHub producer/topic (market:history:*), not a direct service call.
    price_topic_ = QStringLiteral("market:history:%1:max:1d").arg(price_symbol_);
    hub.subscribe<QVector<services::HistoryPoint>>(this, price_topic_,
                                                   [this, token](const QVector<services::HistoryPoint>& points) {
                                                       if (token != price_token_)
                                                           return;
                                                       accept_price(points);
                                                   });
    hub.subscribe_errors(this, price_topic_, [this, token](const QString& error) {
        if (token != price_token_)
            return;
        price_state_ = PriceState::Unavailable;
        price_reason_ = error.trimmed().isEmpty() ? tr("the Yahoo Finance request failed")
                                                  : tr("the Yahoo Finance request failed: %1").arg(error.trimmed());
        if (market_loaded_) {
            interpret_market();
            render_market();
        }
    });
    hub.request(price_topic_, /*force=*/true);
}

void CftcPanel::accept_price(const QVector<services::HistoryPoint>& points) {
    QVector<services::CftcPricePoint> prices;
    prices.reserve(points.size());
    for (const auto& point : points) {
        if (!std::isfinite(point.close) || point.close <= 0.0)
            continue;
        const QDate date = cftc_price_session_date(point.timestamp); // exchange session date
        if (date.isValid())
            prices.append({date, point.close});
    }
    std::stable_sort(
        prices.begin(), prices.end(),
        [](const services::CftcPricePoint& a, const services::CftcPricePoint& b) { return a.date < b.date; });
    price_ = prices;
    if (price_.isEmpty()) {
        price_state_ = PriceState::Unavailable;
        price_reason_ = tr("the price provider returned no usable closes");
    } else {
        price_state_ = PriceState::Ready;
    }
    if (market_loaded_) {
        interpret_market();
        render_market();
    }
}

CftcPriceContextState CftcPanel::price_context_state() const {
    if (price_state_ == PriceState::Pending)
        return CftcPriceContextState::Pending;
    if (price_state_ == PriceState::Ready && price_market_key_ == market_key_)
        return CftcPriceContextState::Ready;
    return CftcPriceContextState::Unavailable;
}

QString CftcPanel::price_series_label() const {
    if (price_symbol_.isEmpty())
        return {};
    return price_spot_index_ ? tr("%1 spot index").arg(price_symbol_)
                             : tr("%1 continuous front-month, reference only").arg(price_symbol_);
}

// ── State, theme, language ──────────────────────────────────────────────────

QVariantMap CftcPanel::save_panel_state() const {
    return {{QStringLiteral("family"), cot_family_code(family_)},
            {QStringLiteral("futures_only"), futures_only_},
            {QStringLiteral("market"), view_ == View::Market ? market_key_ : QString()},
            {QStringLiteral("range"), range_years_}};
}

void CftcPanel::restore_panel_state(const QVariantMap& state) {
    if (state.contains(QStringLiteral("family")))
        family_ = services::cftc_family_from_code(state.value(QStringLiteral("family")).toString());
    if (state.contains(QStringLiteral("futures_only")))
        futures_only_ = state.value(QStringLiteral("futures_only")).toBool();
    const int range = state.value(QStringLiteral("range"), range_years_).toInt();
    if (range == 0 || range == 1 || range == 3 || range == 5 || range == 10)
        range_years_ = range;
    sync_selectors();
    const QString market = state.value(QStringLiteral("market")).toString();
    if (services::cftc_market_is_known(market))
        open_market(market, false);
}

void CftcPanel::refresh_panel_theme() {
    using namespace ui::colors;
    EconPanelBase::refresh_panel_theme();
    setStyleSheet(
        panel_style() +
        QString(
            "#cftcContent { background:%1; }"
            "QFrame[cftcCard=\"true\"] { background:%2; border:1px solid %3; border-radius:3px; }"
            "QFrame[kpi=\"true\"] { background:%4; }"
            "QLabel { color:%5; font-size:12px; background:transparent; }"
            "QLabel#cftcTitle { color:%6; font-size:13px; font-weight:700; letter-spacing:1px; }"
            "QLabel#cftcAsOf { color:%7; font-size:11px; }"
            "QLabel[cftcHeading=\"true\"] { color:%6; font-size:11px; font-weight:700; letter-spacing:1px; }"
            "QLabel[cftcNote=\"true\"] { color:%7; font-size:11px; }"
            "QLabel#cftcStatus { color:%5; font-size:12px; background:%4; border:1px solid %3;"
            " border-left:3px solid %6; padding:6px 10px; }"
            "QLabel#cftcKpiCaption { color:%7; font-size:10px; font-weight:700; letter-spacing:1px; }"
            "QLabel[kpiValue=\"true\"] { color:%5; font-size:20px; font-weight:700; }"
            "QLabel#cftcKpiSub { color:%7; font-size:11px; }"
            "QLabel#cftcDevTitle { color:%6; font-size:10px; font-weight:700; letter-spacing:1px; }"
            "QLabel[cftcDevBody=\"true\"] { color:%5; font-size:12px; }"
            "QLabel#cftcMarketTitle { color:%5; font-size:17px; font-weight:700; letter-spacing:1px; }"
            "QLabel#cftcBadge { color:%8; font-size:11px; border:1px solid %8; border-radius:3px; padding:4px 8px; }"
            "QLabel#cftcHeadline { color:%5; font-size:15px; font-weight:700; }"
            "QLabel#cftcPoints { color:%5; font-size:13px; }"
            "QWidget#cftcNavBar { background:%2; border-bottom:1px solid %3; }"
            "QLabel#cftcNavNote { color:%7; font-size:11px; }"
            "QPushButton[cftcSegment=\"true\"] { background:transparent; color:%7; border:1px solid %3;"
            " font-size:11px; font-weight:700; padding:4px 10px; }"
            "QPushButton[cftcSegment=\"true\"]:hover { color:%5; background:%9; border-color:%10; }"
            "QPushButton[cftcSegment=\"true\"]:checked { background:%6; color:%11; border-color:%6; }"
            "QPushButton[cftcSegment=\"true\"]:disabled { color:%12; border-color:%3; }"
            "QPushButton#cftcBackfill { background:transparent; color:%7; border:1px solid %3; font-size:10px;"
            " font-weight:700; padding:4px 10px; }"
            "QPushButton#cftcBackfill:hover { color:%5; background:%9; border-color:%10; }"
            "QTableWidget#cftcReportTable, QTableWidget#cftcContextTable { background:transparent; color:%5;"
            " border:none; font-size:12px; }"
            "QTableWidget#cftcReportTable QHeaderView::section, QTableWidget#cftcContextTable QHeaderView::section"
            " { background:%4; color:%7; border:none; border-bottom:1px solid %3; font-size:10px; font-weight:700;"
            " padding:4px 6px; }"
            "QTableWidget#cftcReportTable::item, QTableWidget#cftcContextTable::item { padding:2px 6px; }"
            "QScrollArea#cftcBoardScroll { background:transparent; }"
            "QScrollArea#cftcBoardScroll > QWidget > QWidget { background:transparent; }")
            .arg(BG_BASE(), BG_SURFACE(), BORDER_DIM(), BG_RAISED(), TEXT_PRIMARY(), AMBER(), TEXT_SECONDARY(),
                 WARNING(), BG_HOVER())
            .arg(BORDER_BRIGHT(), TEXT_ON_ACCENT(), TEXT_TERTIARY()));
    for (QWidget* chart : std::initializer_list<QWidget*>{board_, map_, bars_, history_chart_, flow_strip_, readings_})
        if (chart)
            chart->update();
    if (market_loaded_)
        render_market_header();
}

void CftcPanel::retranslateUi() {
    apply_texts();
    if (overview_loaded_)
        render_overview();
    if (market_loaded_)
        render_market();
    EconPanelBase::retranslateUi();
}

} // namespace fincept::screens
