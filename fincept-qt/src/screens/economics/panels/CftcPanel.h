// src/screens/economics/panels/CftcPanel.h
// CFTC Commitments of Traders — research workspace.
//
// Two views on one report family and basis (Legacy / Disaggregated / TFF,
// futures only or futures + options):
//
//   * ALL MARKETS opens immediately from the local canonical archive and
//     refreshes from the CFTC when a newer weekly report is due: key figures,
//     asset-class tabs, the positioning map (every market as a tile by the
//     speculators' 52-week level) and the positioning board (every market's
//     place in its 52-week range and every participant group's net position).
//   * One MARKET: the readings grid (every group's position, 52-week level and
//     1 / 4 / 13-week change as standard labels, sentences in the tooltips),
//     positions by group, a synchronized history (price for reference, net
//     positions, percentile, open interest, concentration), the week-by-week
//     change strip and the exact report tables.
//
// A TABLE switch shows the exact rows behind either view (newest first, CSV).
// Scripts: cftc_data.py (cot_monitor, cot_market, cot_backfill).
#pragma once

#include "screens/economics/panels/CftcInterpretationPresentation.h"
#include "screens/economics/panels/CftcResearchModel.h"
#include "screens/economics/panels/EconPanelBase.h"
#include "services/economics/CftcInterpretationModel.h"
#include "services/economics/CftcMetricModel.h"
#include "services/economics/CftcMonitorModel.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

class QButtonGroup;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QTableWidget;
class QVBoxLayout;

namespace fincept::services {
struct HistoryPoint;
}

namespace fincept::screens {

class CftcPositioningBoard;
class CftcPositioningMap;
class CftcReadingsGrid;
class CftcParticipantBars;
class CftcHistoryChart;
class CftcFlowStrip;

class CftcPanel : public EconPanelBase {
    Q_OBJECT
  public:
    explicit CftcPanel(QWidget* parent = nullptr);
    void activate() override;
    QVariantMap save_panel_state() const override;
    void restore_panel_state(const QVariantMap& state) override;

  protected:
    void build_controls(QHBoxLayout* toolbar) override;
    void on_fetch() override;
    void on_result(const QString& request_id, const services::EconomicsResult& result) override;
    void refresh_panel_theme() override;
    void resizeEvent(QResizeEvent* event) override;
    void retranslateUi() override;

  private:
    enum class View { Overview, Market };
    enum class PriceState { None, Pending, Ready, Unavailable };
    struct Kpi {
        QFrame* frame = nullptr;
        QLabel* caption = nullptr;
        QLabel* value = nullptr;
        QLabel* sub = nullptr;
    };

    // ── construction ───────────────────────────────────────────────────────
    QFrame* card(QWidget* parent, const QString& object_name);
    QLabel* heading(QWidget* parent, const QString& text);
    QLabel* note(QWidget* parent, const QString& object_name = QStringLiteral("cftcNote"));
    QPushButton* segment(QWidget* parent, QButtonGroup* group, int id, const QString& object_name);
    void build_nav_bar();
    void build_overview_page();
    void build_market_page();
    void apply_texts();
    void arrange();

    // ── navigation ─────────────────────────────────────────────────────────
    void show_view(View view);
    void show_table_view(bool table);
    void select_family(int id);
    void select_basis(int id);
    void sync_selectors();

    // ── all markets ────────────────────────────────────────────────────────
    void request_overview(bool refresh);
    void accept_overview(const services::EconomicsResult& result, bool refresh);
    bool overview_refresh_due() const;
    void render_overview();
    void render_kpis();
    void set_as_of(const QString& text, const QDate& report_date);
    void select_asset_class(int id);
    QVector<CftcMarketRow> visible_rows() const;
    void render_overview_notes();
    void set_status(const QString& text, bool busy);
    void request_backfill();
    QJsonArray overview_table_rows() const;
    QStringList overview_table_columns() const;

    // ── one market ─────────────────────────────────────────────────────────
    void open_market(const QString& market_key, bool refresh);
    void accept_market(const services::EconomicsResult& result);
    void interpret_market();
    void render_market();
    void render_market_header();
    void render_readings();
    void render_participant_bars();
    void render_history();
    void render_flows();
    void render_tables();
    void render_market_notes();
    void set_range(int years);
    QJsonArray market_table_rows() const;
    QStringList market_table_columns() const;

    // ── price context ──────────────────────────────────────────────────────
    void request_price();
    void accept_price(const QVector<services::HistoryPoint>& points);
    CftcPriceContextState price_context_state() const;
    QString price_series_label() const;

    // ── selection ──────────────────────────────────────────────────────────
    services::CftcFamily family_ = services::CftcFamily::Legacy;
    bool futures_only_ = true;
    View view_ = View::Overview;
    bool table_view_ = false;
    bool compact_toolbar_ = false; ///< short family/basis labels in a narrow window
    int request_seq_ = 0;

    // ── all-markets state ──────────────────────────────────────────────────
    QVector<services::CftcMonitorEntry> entries_;
    QVector<CftcMarketRow> rows_;
    QJsonObject archive_;
    QString overview_dataset_;
    QString overview_retrieved_;
    bool overview_loaded_ = false;
    bool overview_from_refresh_ = false;
    QString pending_overview_;
    bool pending_overview_refresh_ = false;
    QString asset_class_; ///< the selected asset-class tab; empty = all
    QString pending_backfill_;
    QHash<QString, QDateTime> auto_refreshed_; ///< family/basis -> last automatic refresh

    // ── one-market state ───────────────────────────────────────────────────
    QString market_key_;
    QString market_identity_; ///< market/family/basis of the content on the market page
    QString pending_market_;
    QString pending_market_key_;
    bool pending_market_refresh_ = false;
    QHash<QString, QDateTime> market_auto_refreshed_; ///< market/family/basis -> last automatic refresh
    bool market_loaded_ = false;
    QJsonArray market_rows_json_;
    QJsonObject market_params_;
    services::CftcHistory history_;
    services::CftcInterpretationResult result_;
    QVector<services::CftcParticipantTrail> trails_;
    int range_years_ = 3;

    PriceState price_state_ = PriceState::None;
    QString price_reason_;
    QString price_symbol_;
    bool price_spot_index_ = false;
    QString price_market_key_;
    QString price_topic_;
    QVector<services::CftcPricePoint> price_;
    int price_token_ = 0;

    // ── toolbar ────────────────────────────────────────────────────────────
    QLabel* title_lbl_ = nullptr;
    QLabel* as_of_lbl_ = nullptr;
    QString as_of_text_;  ///< report date and freshness
    QString as_of_short_; ///< the report date alone, for a narrow window
    QButtonGroup* family_group_ = nullptr;
    QVector<QPushButton*> family_btns_;
    QButtonGroup* basis_group_ = nullptr;
    QVector<QPushButton*> basis_btns_;
    QComboBox* market_combo_ = nullptr;

    // ── navigation bar ─────────────────────────────────────────────────────
    QWidget* nav_bar_ = nullptr;
    QPushButton* nav_all_ = nullptr;
    QPushButton* nav_market_ = nullptr;
    QPushButton* nav_charts_ = nullptr;
    QPushButton* nav_table_ = nullptr;
    QLabel* nav_note_ = nullptr;
    QWidget* identity_bar_ = nullptr;

    // ── all-markets page ───────────────────────────────────────────────────
    int overview_page_ = -1;
    QWidget* overview_content_ = nullptr;
    QLabel* status_lbl_ = nullptr;
    QWidget* kpi_row_ = nullptr;
    QGridLayout* kpi_layout_ = nullptr;
    QVector<Kpi> kpis_;
    QWidget* class_bar_ = nullptr;
    QButtonGroup* class_group_ = nullptr;
    QVector<QPushButton*> class_btns_; ///< id 0 = all, i = cftc_asset_class_order()[i - 1]
    QFrame* map_card_ = nullptr;
    QLabel* map_title_ = nullptr;
    QLabel* map_hint_ = nullptr;
    CftcPositioningMap* map_ = nullptr;
    QLabel* board_title_ = nullptr;
    QLabel* board_hint_ = nullptr;
    QScrollArea* board_scroll_ = nullptr;
    CftcPositioningBoard* board_ = nullptr;
    QLabel* overview_notes_ = nullptr;
    QLabel* overview_source_ = nullptr;
    QPushButton* backfill_btn_ = nullptr;

    // ── market page ────────────────────────────────────────────────────────
    int market_page_ = -1;
    QWidget* market_content_ = nullptr;
    QVector<QWidget*> market_sections_; ///< everything below the header; shown once a report is loaded
    QLabel* market_title_ = nullptr;
    QLabel* market_meta_ = nullptr;
    QLabel* market_badge_ = nullptr;
    QLabel* market_tags_ = nullptr; ///< the speculators' labels for this market
    QGridLayout* top_layout_ = nullptr;
    QFrame* conclusion_card_ = nullptr; ///< the readings card
    QLabel* conclusion_title_ = nullptr;
    CftcReadingsGrid* readings_ = nullptr;
    QScrollArea* readings_scroll_ = nullptr;
    QLabel* conclusion_footnote_ = nullptr;
    QFrame* participants_card_ = nullptr;
    QLabel* participants_title_ = nullptr;
    CftcParticipantBars* bars_ = nullptr;
    QLabel* participants_meta_ = nullptr;
    QLabel* history_title_ = nullptr;
    QButtonGroup* range_group_ = nullptr;
    QVector<QPushButton*> range_btns_;
    CftcHistoryChart* history_chart_ = nullptr;
    QLabel* history_hint_ = nullptr;
    QLabel* flows_title_ = nullptr;
    QLabel* flows_hint_ = nullptr;
    CftcFlowStrip* flow_strip_ = nullptr;
    QGridLayout* tables_layout_ = nullptr;
    QFrame* report_card_ = nullptr;
    QFrame* context_card_ = nullptr;
    QLabel* report_title_ = nullptr;
    QLabel* context_title_ = nullptr;
    QTableWidget* report_table_ = nullptr;
    QTableWidget* context_table_ = nullptr;
    QLabel* market_notes_title_ = nullptr;
    QLabel* market_notes_ = nullptr;
};

} // namespace fincept::screens
