#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/dashboard/widgets/HighlightCharts.h"
#include "services/economics/CftcMetricModel.h"
#include "services/economics/EconomicsService.h"

#include <QGridLayout>
#include <QLabel>
#include <QPair>
#include <QVector>

namespace fincept::screens::widgets {

/// CFTC COT highlights — the speculators' positioning across the markets of
/// the Legacy futures-only report: how many lean long/short, which are
/// CROWDED (in the top/bottom 10% of their last 52 weeks), which flipped side
/// this week, and the largest weekly changes.
///
/// Uses the same stored archive and the same rules as Economics → CFTC
/// (`cftc_data.py cot_monitor`, cftc_market_rows / cftc_overview_figures /
/// cftc_weekly_move_ranking). Opening the dashboard reads the archive only;
/// the refresh button fetches new reports from the CFTC.
class CotHighlightsWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit CotHighlightsWidget(QWidget* parent = nullptr);

    /// Dashboard REFRESH ALL: re-read the stored archive only (no CFTC download).
    void request_bulk_refresh() override { request(false); }

  protected:
    void showEvent(QShowEvent* e) override;
    void on_theme_changed() override;
    void retranslateUi() override;

  private:
    void request(bool refresh);
    /// The next report of a full refresh (other families / bases), if any.
    void refresh_next_report();
    void on_result(const QString& request_id, const services::EconomicsResult& result);
    void render(const QJsonObject& data);
    void apply_styles();

    QWidget* body_ = nullptr;
    QLabel* report_ = nullptr;
    QLabel* lean_title_ = nullptr;
    charts::SplitBar* lean_ = nullptr; ///< leaning long / neutral / short counts
    QLabel* extremes_title_ = nullptr;
    charts::BarList* extremes_ = nullptr; ///< crowded markets, net position % of OI
    QLabel* flipped_ = nullptr;
    QLabel* moves_title_ = nullptr;
    charts::BarList* moves_ = nullptr; ///< largest one-week net changes

    QString pending_id_;
    bool pending_refresh_ = false;
    int sequence_ = 0;
    bool loaded_once_ = false;
    QJsonObject last_data_;
    QVector<QPair<services::CftcFamily, bool>> refresh_queue_; ///< (family, futures only) still to refresh
    int refresh_total_ = 0;
    QString pending_other_;
    QString other_status_;
    QStringList other_failures_;
};

} // namespace fincept::screens::widgets
