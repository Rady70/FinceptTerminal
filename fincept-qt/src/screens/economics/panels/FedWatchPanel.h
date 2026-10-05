#pragma once
#include "screens/economics/panels/EconPanelBase.h"
#include "screens/economics/panels/FedWatchViewModel.h"

#include <QHash>

#include <functional>

class QFrame;
class QGridLayout;
class QPlainTextEdit;
class QScrollArea;
class QToolButton;

namespace fincept::screens {
class FedWatchMatrix;
class FedWatchPathChart;
class FedWatchBandBars;
class FedWatchOutcomeBars;
class FedWatchHeatStrip;

/// Economics -> FedWatch research workspace.
///
/// Opening the panel reads everything from local storage in one `workspace`
/// request and shows it immediately. The only acquisition control is REFRESH,
/// which collects current observations for every upcoming meeting, updates the
/// bounded Polymarket daily history for validated markets, and reloads the
/// local read. Meeting focus changes are local and need no request.
class FedWatchPanel : public EconPanelBase {
    Q_OBJECT
  public:
    using Dispatch = std::function<void(const QString&, const QStringList&, const QString&)>;
    explicit FedWatchPanel(QWidget* parent = nullptr);
    explicit FedWatchPanel(Dispatch dispatch, QWidget* parent = nullptr);
    void activate() override;
    QVariantMap save_panel_state() const override;
    void restore_panel_state(const QVariantMap& state) override;
    void accept_result(const QString& request_id, const services::EconomicsResult& result);

    const fedwatch::Workspace& workspace() const { return workspace_; }
    QString selected_meeting() const { return selected_; }
    void select_meeting(const QString& meeting);
    bool refreshing() const { return stage_ != Stage::Idle; }

  signals:
    void command_requested(const QString& command, const QStringList& args, const QString& request_id);

  protected:
    void build_controls(QHBoxLayout* toolbar) override;
    void on_fetch() override;
    void on_result(const QString& request_id, const services::EconomicsResult& result) override;
    void refresh_panel_theme() override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    enum class Stage { Idle, Loading, Collecting, Backfilling };
    void request(const QString& command, const QStringList& args = {});
    void load_workspace();
    void render();
    void render_kpis();
    void render_chips();
    void render_focus();
    void render_history(const fedwatch::Meeting& meeting);
    void render_changes(const fedwatch::Meeting& meeting);
    void render_sources();
    void render_status();
    void arrange();
    QFrame* card(const QString& object_name);
    QWidget* section(QWidget* parent, const QString& title, const QString& hint, QLabel** hint_label = nullptr);

    Dispatch dispatch_;
    int sequence_ = 0;
    QHash<QString, QString> pending_;
    QString latest_workspace_request_;
    Stage stage_ = Stage::Idle;
    bool activated_ = false;

    fedwatch::Workspace workspace_;
    QJsonObject raw_workspace_;
    QString selected_, restored_selection_;
    QString load_error_, refresh_summary_;
    QStringList refresh_issues_;

    QLabel *as_of_ = nullptr, *status_ = nullptr;
    QPushButton* open_cme_ = nullptr;
    struct Kpi {
        QFrame* frame = nullptr;
        QLabel *caption = nullptr, *value = nullptr, *sub = nullptr;
    };
    QVector<Kpi> kpis_;
    QWidget* kpi_row_ = nullptr;
    QGridLayout* kpi_layout_ = nullptr;
    QScrollArea* matrix_scroll_ = nullptr;
    FedWatchMatrix* matrix_ = nullptr;
    FedWatchPathChart* path_ = nullptr;
    QWidget* chips_ = nullptr;
    QLabel *focus_title_ = nullptr, *focus_meta_ = nullptr;
    QGridLayout* focus_layout_ = nullptr;
    QWidget *band_card_ = nullptr, *outcome_card_ = nullptr;
    FedWatchBandBars* bands_ = nullptr;
    FedWatchOutcomeBars* outcomes_ = nullptr;
    QLabel *band_note_ = nullptr, *outcome_note_ = nullptr;
    FedWatchHeatStrip *fed_mix_ = nullptr, *poly_mix_ = nullptr;
    QLabel *history_note_ = nullptr, *fed_mix_hint_ = nullptr, *poly_mix_hint_ = nullptr;
    QTableWidget *changes_ = nullptr, *sources_ = nullptr;
    QLabel *diagnostics_ = nullptr, *notes_ = nullptr;
    QToolButton* audit_toggle_ = nullptr;
    QPlainTextEdit* audit_ = nullptr;
    int workspace_page_ = -1;
};
} // namespace fincept::screens
