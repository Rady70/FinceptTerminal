#pragma once
#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/etf/research/EtfResearchSnapshot.h"

#include <QFutureWatcher>
#include <QGridLayout>
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

  protected:
    void showEvent(QShowEvent* e) override;
    void on_theme_changed() override;
    void retranslateUi() override;

  private:
    void load();
    void render();
    void apply_styles();

    QWidget* body_ = nullptr;
    QLabel* tilt_ = nullptr;
    QLabel* measured_title_ = nullptr;
    QGridLayout* measured_ = nullptr;
    QLabel* estimated_title_ = nullptr;
    QGridLayout* estimated_ = nullptr;

    QFutureWatcher<void>* watcher_ = nullptr;
    std::shared_ptr<services::etf::research::ResearchSnapshot> snap_;
    bool loaded_once_ = false;
};

} // namespace fincept::screens::widgets
