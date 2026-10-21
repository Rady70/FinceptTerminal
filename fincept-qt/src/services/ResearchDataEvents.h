#pragma once
// ResearchDataEvents — "new data was saved" notices for the research tools
// whose dashboard widget and main screen read the same stored data (FedWatch,
// CFTC COT, ETF Flow). Whoever finishes a refresh emits; every other view of
// that tool re-reads the stored data, so the widget and the main screen never
// show different refreshes. `origin` lets the emitter skip its own notice.
#include <QObject>

namespace fincept::services {

class ResearchDataEvents : public QObject {
    Q_OBJECT
  public:
    static ResearchDataEvents& instance();

  signals:
    void fedwatch_saved(QObject* origin);
    void cot_saved(QObject* origin);
    void etf_saved(QObject* origin);

  private:
    ResearchDataEvents() = default;
};

} // namespace fincept::services
