#pragma once
#include "screens/economics/panels/EconPanelBase.h"

#include <QHash>

#include <functional>

class QPlainTextEdit;
class QToolButton;
class QGridLayout;
namespace fincept::screens {
class FedWatchHistoryChart;
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
  signals:
    void command_requested(const QString& command, const QStringList& args, const QString& request_id);

  protected:
    void build_controls(QHBoxLayout*) override;
    void on_fetch() override;
    void on_result(const QString&, const services::EconomicsResult&) override;
    void refresh_panel_theme() override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    // Panel-owned selection state. Visible buttons are the only user controls.
    struct Selection : QObject {
        explicit Selection(QObject* parent) : QObject(parent) {}
        QVector<QPair<QString, QVariant>> items;
        int index = -1;
        std::function<void()> changed;
        void clear() {
            items.clear();
            index = -1;
        }
        void addItem(const QString& label, const QVariant& value) { items.push_back({label, value}); }
        int count() const { return items.size(); }
        int currentIndex() const { return index; }
        QString itemText(int i) const { return i >= 0 && i < count() ? items[i].first : QString{}; }
        QVariant itemData(int i) const { return i >= 0 && i < count() ? items[i].second : QVariant{}; }
        QString currentText() const { return itemText(index); }
        QVariant currentData() const { return itemData(index); }
        int findData(const QVariant& value) const {
            for (int i = 0; i < count(); ++i)
                if (itemData(i) == value)
                    return i;
            return -1;
        }
        void setCurrentIndex(int i) {
            i = i >= 0 && i < count() ? i : -1;
            if (i == index)
                return;
            index = i;
            if (!signalsBlocked() && changed)
                changed();
        }
    };
    void request(const QString& command, const QStringList& args = {});
    void rebuild_meetings();
    void load_meeting();
    void rebuild_outcomes();
    void load_analytics();
    void render();
    void render_history();
    QString backfill_status() const;
    void arrange_charts();
    QWidget* make_chip_row(const QString& kind, Selection* model, int height);
    void sync_chips();
    bool resolved() const;
    QJsonObject meeting() const;
    QJsonObject current_meeting() const;
    Dispatch dispatch_;
    int sequence_ = 0;
    int generation_ = 0;
    struct Pending {
        QString command;
        int generation;
        QString meeting;
    };
    QHash<QString, Pending> pending_;
    QWidget* controls_ = nullptr;
    QWidget *meeting_chips_ = nullptr, *outcome_chips_ = nullptr, *method_chips_ = nullptr, *range_chips_ = nullptr;
    QPushButton* update_upcoming_ = nullptr;
    QPushButton* load_history_ = nullptr;
    Selection *meetings_ = nullptr, *outcomes_ = nullptr, *methods_ = nullptr, *ranges_ = nullptr;
    QLabel *summary_ = nullptr, *status_ = nullptr, *coverage_ = nullptr;
    QLabel* diagnostic_status_ = nullptr;
    QLabel *selected_current_ = nullptr, *source_status_ = nullptr;
    QWidget *probability_section_ = nullptr, *polymarket_section_ = nullptr, *diagnostics_ = nullptr;
    QGridLayout* charts_layout_ = nullptr;
    QTableWidget *distribution_ = nullptr, *indicators_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    FedWatchHistoryChart *probability_ = nullptr, *polymarket_ = nullptr, *divergence_ = nullptr;
    QJsonObject snapshot_, overview_, series_, analytics_;
    QString selected_meeting_, restored_outcome_;
    QString selected_outcome_;
    bool activated_ = false;
    bool current_ok_ = false;
    bool collect_after_overview_ = false;
    bool collect_in_flight_ = false;
    bool backfill_in_flight_ = false;
    bool meeting_explicitly_selected_ = false;
    QString current_error_, inventory_error_, series_error_, analytics_error_;
    struct BackfillResult {
        QJsonObject data;
        QString error;
    };
    QHash<QString, BackfillResult> backfill_results_;
    QString backfill_meeting_;
    int workspace_page_ = -1;
};
} // namespace fincept::screens
