#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/dashboard/widgets/HighlightCharts.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <QFutureWatcher>
#include <QLabel>

#include <memory>

namespace fincept::screens::widgets {

/// ETF flow highlights — the largest fund inflows and outflows with their
/// evidence class (SEC N-PORT MEASURED monthly flow, or ESTIMATED daily flow),
/// the sector tilt and breadth, from the same stored research snapshot as
/// ETF Flow & Sector Rotation. Only dollar flows are ranked: a 1M relative
/// return (PROXY) is market behaviour, never shown as flow.
///
/// Reads stored data only (no acquisition); the full refresh lives on the
/// ETF Flow screen.
class EtfFlowHighlightsWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit EtfFlowHighlightsWidget(QWidget* parent = nullptr);

    /// Dashboard REFRESH ALL: re-read the stored snapshot (same as its button).
    void request_bulk_refresh() override { load(); }

  protected:
    void showEvent(QShowEvent* e) override;
    void on_theme_changed() override;
    void retranslateUi() override;

  private:
    void load();
    /// The ETF Flow screen's full source refresh, then load().
    void refresh_sources();
    void render();
    void apply_styles();

    QWidget* body_ = nullptr;
    QLabel* tilt_caption_ = nullptr;
    QLabel* tilt_ = nullptr;
    QLabel* breadth_caption_ = nullptr;
    QLabel* breadth_ = nullptr;
    QLabel* sectors_title_ = nullptr;
    charts::BarList* sectors_ = nullptr; ///< U.S. sector funds, 1M total return minus SPY
    QLabel* flows_title_ = nullptr;
    charts::BarList* flows_ = nullptr; ///< measured (else estimated) fund flows, USD

    QFutureWatcher<void>* watcher_ = nullptr;
    std::shared_ptr<services::etf::research::ResearchSnapshot> snap_;
    bool loaded_once_ = false;
    bool refreshing_ = false;
    QString refresh_error_; ///< the last source refresh's error (empty after a success)
};

} // namespace fincept::screens::widgets
