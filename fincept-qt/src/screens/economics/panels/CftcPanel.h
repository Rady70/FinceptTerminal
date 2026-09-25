// src/screens/economics/panels/CftcPanel.h
// CFTC (Commodity Futures Trading Commission) — Commitments of Traders.
// No API key required. Authoritative source: publicreporting.cftc.gov.
//
// R3 turns the retained CFTC capability into a dedicated analytical workspace:
// two result tabs (Analysis | Raw Data). Analysis is one vertically scrollable
// page — current snapshot, participant positioning, weekly changes, the
// principal historical positioning chart, window statistics/extremes, price +
// positioning divergence and a compact positioning heatmap. Raw Data is the
// shared newest-first table + CSV export over the same CFTC observations.
//
// Script: cftc_data.py (command: cot_history)
//
// Market Structure / Forward Curve is deliberately deferred (R3 item 9).
// The CFTC COT report carries no term structure, and the retained free price
// path (Yahoo Finance via MarketDataService) returns only a front-month
// continuous series that rolls between contracts. A truthful per-expiration
// futures strip would need another market-data source: CME/COMEX scripts in
// this repository require an API key, the only keyless curve endpoint
// (CBOE VIX futures) covers a single market, and constructing a curve from a
// continuous ticker is explicitly not allowed. Rather than fake a curve, the
// workspace reports the price path truthfully everywhere else and leaves this
// section deferred until a supported free strip source exists.
#pragma once

#include "screens/economics/panels/CftcWorkspaceContract.h"
#include "screens/economics/panels/EconPanelBase.h"
#include "services/economics/CftcInterpretationModel.h"
#include "services/economics/CftcMetricModel.h"
#include "services/economics/CftcMonitorModel.h"

#include <QComboBox>
#include <QJsonObject>
#include <QLabel>
#include <QString>
#include <QVector>

class QCheckBox;
class QGridLayout;
class QHBoxLayout;
class QPushButton;
class QScrollArea;
class QTableWidget;
class QVBoxLayout;

namespace fincept::services {
struct HistoryPoint;
}

namespace fincept::screens {

enum class CftcPriceContextState;

class CftcHeatmap;
class CftcPositioningChart;
class CftcPricePositioningChart;

/// Which participant leg the historical chart and statistics plot.
enum class CftcChartMetric { Net, Long, Short };

class CftcPanel : public EconPanelBase {
    Q_OBJECT
  public:
    explicit CftcPanel(QWidget* parent = nullptr);
    void activate() override;

  protected:
    void build_controls(QHBoxLayout* thl) override;
    void on_fetch() override;
    void on_result(const QString& request_id, const services::EconomicsResult& result) override;
    void refresh_panel_theme() override;
    void resizeEvent(QResizeEvent* event) override;
    void retranslateUi() override;

  private:
    enum class PriceState { None, Pending, Ready, Unavailable };

    struct SnapshotCard {
        QWidget* frame = nullptr;
        QLabel* caption = nullptr;
        QLabel* value = nullptr;
        QLabel* sub = nullptr;
    };

    // ── construction ────────────────────────────────────────────────────────
    void build_result_tabs(QVBoxLayout* root);
    void build_analysis_page();
    void build_monitor_page();
    struct Section {
        QWidget* frame = nullptr;
        QVBoxLayout* body = nullptr;
        QLabel* title = nullptr;
    };
    Section make_section(const QString& title);
    static QTableWidget* make_table(QWidget* parent);
    static QTableWidget* make_monitor_table(QWidget* parent);
    SnapshotCard make_snapshot_card();
    void apply_responsive_layout();

    // ── workspace state ─────────────────────────────────────────────────────
    void clear_workspace();
    void build_participant_controls();
    void show_monitor_tab();
    void show_analysis_tab();
    void show_raw_tab();
    void apply_range(services::CftcRange range);
    void rebuild_workspace(bool range_only = false);
    void update_header();
    void refresh_interpretation();
    void render_interpretation();
    void set_interpretation_horizon(int horizon_reports);
    void refresh_horizon_buttons();
    void update_evidence_visibility();
    void update_sync_chart();
    void update_snapshot();
    void update_positioning();
    void update_weekly();
    void update_chart();
    void update_statistics();
    void update_divergence();
    void update_heatmap();
    void refresh_range_buttons();

    // ── cross-market monitor ────────────────────────────────────────────────
    void on_markets_button();
    void on_scan_markets();
    void on_backfill_history();
    void render_monitor(const services::CftcMonitorModel& model);
    void set_monitor_status(const QString& text);
    void clear_monitor_display(const QString& status);
    void open_monitor_market(int entry_index);
    QString monitor_alert_text(const services::CftcAlert& alert, const QString& participant_label) const;
    QString monitor_descriptive_text(const services::CftcMonitorEntry& entry) const;

    // ── price context ───────────────────────────────────────────────────────
    void request_price(const QString& market_key);
    void update_price_points(const QVector<services::HistoryPoint>& points);
    void update_price_views();
    CftcPriceContextState price_context_state() const;
    QString price_unavailable_note() const;
    QString price_source_text() const;
    QString concise_price_source_text() const;

    // ── helpers ─────────────────────────────────────────────────────────────
    QDate latest_report_date() const;
    QJsonArray build_raw_rows() const;
    QVector<services::CftcDatedValue> participant_metric_series(int participant_index, CftcChartMetric metric) const;
    QString workspace_style() const;
    static void set_plain_cell(QTableWidget* table, int row, int column, const QString& text,
                               int alignment = Qt::AlignRight | Qt::AlignVCenter);
    void set_signed_cell(QTableWidget* table, int row, int column, const std::optional<double>& value,
                         const QString& missing = QStringLiteral("—"));

    // ── controls ────────────────────────────────────────────────────────────
    QComboBox* market_combo_ = nullptr;
    QComboBox* report_combo_ = nullptr;
    QComboBox* type_combo_ = nullptr;
    QLabel* market_lbl_ = nullptr;
    QLabel* report_lbl_ = nullptr;
    QLabel* type_lbl_ = nullptr;
    QPushButton* markets_btn_ = nullptr;

    // ── result tabs ─────────────────────────────────────────────────────────
    QWidget* result_tabs_ = nullptr;
    QPushButton* monitor_tab_ = nullptr;
    QPushButton* analysis_tab_ = nullptr;
    QPushButton* raw_tab_ = nullptr;
    int analysis_page_ = -1;
    int monitor_page_ = -1;

    // ── monitor page ────────────────────────────────────────────────────────
    QWidget* monitor_content_ = nullptr;
    QComboBox* monitor_family_combo_ = nullptr;
    QComboBox* monitor_type_combo_ = nullptr;
    QLabel* monitor_family_lbl_ = nullptr;
    QLabel* monitor_type_lbl_ = nullptr;
    QPushButton* monitor_scan_btn_ = nullptr;
    QPushButton* monitor_backfill_btn_ = nullptr;
    QLabel* monitor_status_lbl_ = nullptr;
    QLabel* monitor_archive_lbl_ = nullptr;
    QLabel* monitor_attention_title_ = nullptr;
    QLabel* monitor_all_title_ = nullptr;
    QTableWidget* monitor_attention_table_ = nullptr;
    QTableWidget* monitor_table_ = nullptr;
    QVector<services::CftcMonitorEntry> monitor_entries_; // provider order
    bool monitor_rendered_ = false;
    QString pending_monitor_request_;
    QString pending_backfill_request_;
    // The request-time identity: a selector change while a scan/backfill is in
    // flight must not reinterpret the response against the new selection.
    QString monitor_request_family_code_;
    bool monitor_request_futures_only_ = false;

    // ── analysis page chrome ────────────────────────────────────────────────
    QScrollArea* analysis_scroll_ = nullptr;
    QWidget* analysis_content_ = nullptr;
    QLabel* hdr_title_ = nullptr;
    QLabel* hdr_meta_ = nullptr;
    QLabel* range_lbl_ = nullptr;
    QLabel* range_hint_lbl_ = nullptr;
    QLabel* range_info_lbl_ = nullptr;
    QVector<QPushButton*> range_btns_;
    QVector<services::CftcRange> range_values_;
    bool narrow_layout_ = false;

    // ── sections ────────────────────────────────────────────────────────────
    QGridLayout* snapshot_grid_ = nullptr;
    QVector<SnapshotCard> snapshot_cards_;
    QGridLayout* top_row_layout_ = nullptr;    // interpretation | positioning + weekly changes
    QGridLayout* stats_pair_layout_ = nullptr; // statistics | divergence evidence
    QWidget* interpretation_frame_ = nullptr;
    QWidget* positioning_frame_ = nullptr;
    QWidget* weekly_frame_ = nullptr;
    QWidget* stats_frame_ = nullptr;
    QWidget* divergence_frame_ = nullptr;
    QLabel* interpretation_title_ = nullptr;
    QLabel* interpretation_headline_ = nullptr;
    QLabel* interpretation_body_ = nullptr;
    QLabel* interpretation_context_ = nullptr;
    QLabel* horizon_lbl_ = nullptr;
    QVector<QPushButton*> horizon_btns_;
    QVector<int> horizon_values_;
    QPushButton* interpretation_evidence_toggle_ = nullptr;
    bool evidence_expanded_ = false;
    int horizon_reports_ = cftc_default_interpretation_horizon(); // 1W | 4W | 13W selection
    QTableWidget* interpretation_evidence_ = nullptr;
    QLabel* sync_chart_title_ = nullptr;
    CftcPricePositioningChart* sync_chart_ = nullptr;
    QLabel* positioning_title_ = nullptr;
    QLabel* weekly_title_ = nullptr;
    QLabel* chart_title_ = nullptr;
    QLabel* stats_title_ = nullptr;
    QLabel* divergence_title_ = nullptr;
    QLabel* heatmap_title_ = nullptr;
    QLabel* snapshot_title_ = nullptr;

    QTableWidget* positioning_table_ = nullptr;
    QTableWidget* weekly_table_ = nullptr;
    QLabel* weekly_oi_caption_ = nullptr;
    QLabel* weekly_oi_lbl_ = nullptr;

    QComboBox* chart_metric_combo_ = nullptr;
    QLabel* chart_metric_lbl_ = nullptr;
    QCheckBox* chart_oi_check_ = nullptr;
    QHBoxLayout* participant_check_layout_ = nullptr;
    QVector<QCheckBox*> participant_checks_;
    CftcPositioningChart* chart_ = nullptr;

    QTableWidget* stats_table_ = nullptr;
    QLabel* stats_meta_lbl_ = nullptr;

    QLabel* divergence_source_lbl_ = nullptr;
    QLabel* divergence_extremes_lbl_ = nullptr;
    QTableWidget* divergence_table_ = nullptr;

    QComboBox* heatmap_metric_combo_ = nullptr;
    QLabel* heatmap_metric_lbl_ = nullptr;
    QLabel* heatmap_meta_lbl_ = nullptr;
    CftcHeatmap* heatmap_ = nullptr;

    // ── data state ──────────────────────────────────────────────────────────
    services::CftcHistory history_;
    QVector<services::CftcObservation> window_; // history_ filtered to the active range
    services::CftcInterpretationResult interpretation_;
    services::CftcFamily family_ = services::CftcFamily::Legacy;
    QVector<services::CftcParticipant> participants_;
    int speculative_index_ = -1; // historical R3 numerical sections only
    int principal_index_ = -1;   // Batch 4B semantic path: finalized terminology contract
    QString market_key_;
    QString market_label_;
    bool futures_only_ = false;
    services::CftcRange range_ = services::CftcRange::TwoYears;
    QJsonObject result_params_;
    QString dataset_;

    PriceState price_state_ = PriceState::None;
    QString price_reason_;
    QString price_symbol_;
    bool price_spot_index_ = false;
    QString price_market_key_;
    QString price_topic_; // DataHub market:history:<sym>:<period>:<interval> subscription
    QVector<services::CftcPricePoint> price_;
    int price_token_ = 0;

    QString pending_request_;
    int request_seq_ = 0;
};

} // namespace fincept::screens
