#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/economics/EconomicsService.h"

#include <QGridLayout>
#include <QLabel>

namespace fincept::screens::widgets {

/// FedWatch highlights — the current target range, the next FOMC meeting's
/// most likely outcomes (fed-funds-futures side and Polymarket) and the
/// expected-rate path over the next meetings.
///
/// Reads the same stored workspace as Economics → FedWatch
/// (`fedwatch_data.py workspace`, the profile's fedwatch_history.db). Opening
/// the dashboard performs no acquisition; the refresh button collects current
/// observations first, exactly like the FedWatch screen's REFRESH.
class FedWatchHighlightsWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit FedWatchHighlightsWidget(QWidget* parent = nullptr);

    /// Dashboard REFRESH ALL: re-read the stored workspace only (no collection).
    void request_bulk_refresh() override { request(QStringLiteral("workspace")); }

  protected:
    void showEvent(QShowEvent* e) override;
    void on_theme_changed() override;
    void retranslateUi() override;

  private:
    void request(const QString& command);
    void on_result(const QString& request_id, const services::EconomicsResult& result);
    void render(const QJsonObject& data);
    void apply_styles();

    QLabel* target_ = nullptr;
    QLabel* next_ = nullptr;
    QLabel* expected_ = nullptr;
    QGridLayout* outcomes_ = nullptr;
    QGridLayout* path_ = nullptr;
    QLabel* outcomes_title_ = nullptr;
    QLabel* path_title_ = nullptr;
    QWidget* body_ = nullptr;

    QString pending_id_;
    QString pending_command_;
    int sequence_ = 0;
    bool loaded_once_ = false;
    QJsonObject last_data_;
    QString collect_error_; ///< the last failed collection, kept in the footer until one succeeds
};

} // namespace fincept::screens::widgets
