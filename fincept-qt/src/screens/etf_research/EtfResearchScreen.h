// src/screens/etf_research/EtfResearchScreen.h
//
// MarketLab ETF Flow & Sector Rotation workspace (consolidated Batch E).
//
// Universe-first, terminal-dense native Qt workspace modelled on the
// interaction style of triphopp/bloomberg-terminal (rotation table, RRG map,
// sector regime heatmap, sector tilt, heatmaps) without copying its web UI:
//
//   command bar   view switch · search · horizon · Refresh ETF Research Data · status
//   summary strip evidence counts · tilt · RRG quadrants · breadth · cycle · regime · freshness · sources
//   views         UNIVERSE (whole universe table) · SECTORS · THEMES · COUNTRIES · FLOW ·
//                 RRG · REGIME · MODELS · INTL · SOURCES
//   detail        opens on selection only: overview, chart, flow, holdings, provenance
//
// Opening the screen reads the stored observations on a worker thread and
// never starts an acquisition. The only acquisition path is the explicit
// Refresh ETF Research Data button. Views are populated on first show; the
// detail panel is built for the selected row only.
#pragma once
#include "screens/common/IStatefulScreen.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <memory>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QTableView;
class QToolButton;
class QFrame;

namespace fincept::screens {

namespace etfr {
class HeatmapWidget;
class RrgWidget;
class MatrixWidget;
class PcaWidget;
class LineChartWidget;
class BarChartWidget;
class ResearchTableModel;
class ResearchSortProxy;
} // namespace etfr

class EtfResearchScreen : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit EtfResearchScreen(QWidget* parent = nullptr);
    ~EtfResearchScreen() override;

    // IStatefulScreen
    void restore_state(const QVariantMap& state) override;
    QVariantMap save_state() const override;
    QString state_key() const override { return QStringLiteral("etf_research"); }
    int state_version() const override { return 1; }

    /// Display a computed snapshot (tests, capture, and the async load).
    void set_snapshot(const services::etf::research::ResearchSnapshot& s);
    void show_view(const QString& id);
    QString current_view() const;
    void select_symbol(const QString& symbol);
    QString selected_symbol() const { return selected_; }
    bool detail_visible() const;
    void show_detail_tab(const QString& id);
    /// When false, showEvent does not start a store read (tests/capture inject snapshots).
    void set_autoload(bool on) { autoload_ = on; }
    int load_requests() const { return load_requests_; }
    QWidget* view_widget(const QString& id) const;
    QTableView* table(const QString& id) const;
    QString status_text() const;
    QStringList view_ids() const;

  signals:
    void snapshot_applied();

  protected:
    void showEvent(QShowEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;

  private:
    void build_ui();
    QWidget* build_universe_view();
    QWidget* build_sectors_view();
    QWidget* build_themes_view();
    QWidget* build_countries_view();
    QWidget* build_flow_view();
    QWidget* build_rrg_view();
    QWidget* build_regime_view();
    QWidget* build_models_view();
    QWidget* build_intl_view();
    QWidget* build_sources_view();
    QWidget* build_detail();
    QTableView* make_table(const QString& id, etfr::ResearchTableModel** model, etfr::ResearchSortProxy** proxy);
    void apply_style();
    void start_load();
    void on_refresh();
    void populate_summary();
    void populate_view(const QString& id);
    void populate_universe();
    void populate_sectors();
    void populate_themes();
    void populate_countries();
    void populate_flow();
    void populate_rrg();
    void populate_regime();
    void populate_models();
    void populate_intl();
    void populate_sources();
    void populate_detail();
    void close_detail();
    void on_row_activated(const QString& key);
    void update_layout_mode();

    std::shared_ptr<services::etf::research::ResearchSnapshot> snap_;
    QSet<QString> populated_;
    QString selected_;
    QString current_view_ = QStringLiteral("universe");
    QString horizon_ = QStringLiteral("m1");
    bool autoload_ = true;
    bool loaded_once_ = false;
    int load_requests_ = 0;
    QFutureWatcher<void>* watcher_ = nullptr;

    QWidget* header_ = nullptr;
    QLabel* title_ = nullptr;
    QButtonGroup* view_group_ = nullptr;
    QHash<QString, QToolButton*> view_buttons_;
    QLineEdit* search_ = nullptr;
    QComboBox* horizon_box_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QLabel* status_ = nullptr;
    QWidget* summary_ = nullptr;
    QHash<QString, QLabel*> tiles_;
    QSplitter* body_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QHash<QString, QWidget*> views_;
    QHash<QString, QTableView*> tables_;
    QHash<QString, etfr::ResearchTableModel*> models_;
    QHash<QString, etfr::ResearchSortProxy*> proxies_;
    QButtonGroup* role_group_ = nullptr;

    etfr::HeatmapWidget* sector_heat_ = nullptr;
    etfr::RrgWidget* sector_rrg_ = nullptr;
    QLabel* tilt_label_ = nullptr;
    etfr::HeatmapWidget* theme_heat_ = nullptr;
    etfr::RrgWidget* theme_rrg_ = nullptr;
    etfr::HeatmapWidget* country_heat_ = nullptr;
    etfr::RrgWidget* country_rrg_ = nullptr;
    etfr::HeatmapWidget* flow_heat_ = nullptr;
    QComboBox* flow_mode_ = nullptr;
    QLabel* flow_note_ = nullptr;
    etfr::RrgWidget* big_rrg_ = nullptr;
    QComboBox* rrg_set_ = nullptr;
    QComboBox* rrg_tail_ = nullptr;
    etfr::MatrixWidget* matrix_ = nullptr;
    etfr::PcaWidget* pca_ = nullptr;
    QComboBox* regime_period_ = nullptr;
    QComboBox* regime_mode_ = nullptr;
    QLabel* regime_stats_ = nullptr;
    etfr::BarChartWidget* regime_probs_ = nullptr;
    etfr::BarChartWidget* cycle_bars_ = nullptr;
    QLabel* cycle_label_ = nullptr;
    QComboBox* intl_market_ = nullptr;
    etfr::HeatmapWidget* intl_heat_ = nullptr;
    QLabel* sources_note_ = nullptr;

    QFrame* detail_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QToolButton* detail_close_ = nullptr;
    QTabWidget* detail_tabs_ = nullptr;
    QLabel* detail_overview_ = nullptr;
    etfr::LineChartWidget* detail_chart_ = nullptr;
    etfr::RrgWidget* detail_rrg_ = nullptr;
    etfr::BarChartWidget* detail_measured_ = nullptr;
    QLabel* detail_flow_text_ = nullptr;
    QLabel* detail_holdings_ = nullptr;
    etfr::BarChartWidget* detail_weights_ = nullptr;
    QLabel* detail_provenance_ = nullptr;
};

/// Render every workspace view off screen (QWidget::grab with
/// WA_DontShowOnScreen) from a computed snapshot and write PNGs into `dir`.
/// No store read and no acquisition happen here. Returns the written files.
QJsonArray capture_etf_research_views(const services::etf::research::ResearchSnapshot& s, const QString& dir, int width,
                                      int height);

} // namespace fincept::screens
