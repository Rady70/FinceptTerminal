#pragma once
#include "screens/common/IStatefulScreen.h"
#include "services/etf/EtfGroupAnalytics.h"
#include "services/etf/EtfTaxonomy.h"

#include <QFutureWatcher>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QDateTimeEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTableView;
class QTabWidget;
class QScrollArea;

namespace fincept::screens {
class EtfMonthlyChart;
class EtfGroupBoard;
class EtfSessionChart;

class EtfFlowsScreen : public QWidget, public IStatefulScreen {
    Q_OBJECT
  public:
    explicit EtfFlowsScreen(QWidget* parent = nullptr);
    void restore_state(const QVariantMap& state) override;
    QVariantMap save_state() const override;
    QString state_key() const override { return QStringLiteral("etf_flows"); }

  signals:
    void research_loaded(bool success);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    void populate_groups();
    void recompute();
    void invalidate();
    void set_busy(bool busy);
    void clear_results();
    void render_groups();
    void select_group();
    void select_month();
    void inspect_subject(QTableWidget* table);
    void render_subject(const QJsonObject& subject, const QJsonObject& result, const QJsonObject& group);
    void show_provenance(const QJsonObject& object);
    void refresh_theme();
    void navigate(const QString& level, const QString& group);
    void update_browse_choices();
    void update_research_status();
    void update_detail_layout();
    void reset_individual_detail();
    void materialize_individual_detail();
    void materialize_provenance();
    QString subject_name(const QJsonObject& row) const;

    std::optional<services::etf::TaxonomySnapshot> taxonomy_;
    QString taxonomy_error_;
    services::etf::GroupRunRequest loaded_request_;
    QJsonObject result_;
    QJsonObject group_result_;
    QJsonObject selected_subject_;
    QJsonObject pending_provenance_;
    QJsonObject individual_research_;
    QJsonArray individual_periods_;
    QJsonArray individual_attribution_;
    QString individual_time_key_;
    bool provenance_dirty_ = false;
    QFutureWatcher<Result<QJsonObject>>* watcher_ = nullptr;
    quint64 generation_ = 0;
    bool busy_ = false;

    QWidget* controls_ = nullptr;
    QWidget* primary_controls_ = nullptr;
    QWidget* browse_choices_ = nullptr;
    QWidget* rotation_choices_ = nullptr;
    QComboBox* selected_month_ = nullptr;
    EtfGroupBoard* board_ = nullptr;
    EtfMonthlyChart* individual_flow_chart_ = nullptr;
    EtfSessionChart* individual_rotation_chart_ = nullptr;
    QWidget* individual_family_choices_ = nullptr;
    QScrollArea* board_scroll_ = nullptr;
    QComboBox* level_ = nullptr;
    QComboBox* group_ = nullptr;
    QDateEdit* from_ = nullptr;
    QDateEdit* to_ = nullptr;
    QDateTimeEdit* as_of_ = nullptr;
    QDateTimeEdit* known_at_ = nullptr;
    QCheckBox* leveraged_ = nullptr;
    QPushButton* recompute_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* context_ = nullptr;
    QLabel* selection_ = nullptr;
    QLabel* month_status_ = nullptr;
    QLabel* individual_status_ = nullptr;
    QLabel* rotation_status_ = nullptr;
    QLabel* individual_detail_status_ = nullptr;
    QPushButton* exact_individual_ = nullptr;
    QPushButton* exact_months_ = nullptr;
    QPushButton* exact_rotation_ = nullptr;
    QSplitter* splitter_ = nullptr;
    int restored_tab_ = -1;
    QByteArray restored_splitter_state_;
    QTabWidget* tabs_ = nullptr;
    QTableWidget* overview_ = nullptr;
    QTableWidget* months_ = nullptr;
    QTableWidget* constituents_ = nullptr;
    QTableWidget* rotation_ = nullptr;
    QTableWidget* unresolved_ = nullptr;
    QTableView* individual_ = nullptr;
    QPlainTextEdit* provenance_ = nullptr;
    EtfMonthlyChart* chart_ = nullptr;
};
} // namespace fincept::screens
