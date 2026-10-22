#pragma once
// Dashboard widgets on FINRA data read with the owner's FINRA API key
// (finra_api.json next to sec_edgar.json; see services::ShortInterestFeed).
//
//  ShortInterestLookupWidget  one stock's short interest, read only when the
//                             owner asks for that symbol
//                             (scripts/finra_short_interest.py stock)
//  BondPulseWidget            daily breadth and customer flows of
//                             investment-grade and high-yield corporate bonds
//                             (scripts/finra_bonds.py pulse)
#include "screens/dashboard/widgets/BaseWidget.h"
#include "screens/dashboard/widgets/HighlightCharts.h"

#include <QDateTime>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>

namespace fincept::services {
struct EconomicsResult;
}

namespace fincept::screens::widgets {

class ShortInterestLookupWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit ShortInterestLookupWidget(const QJsonObject& cfg = {}, QWidget* parent = nullptr);

    QJsonObject config() const override;
    void apply_config(const QJsonObject& cfg) override;
    void request_bulk_refresh() override;

  protected:
    void on_theme_changed() override;
    void retranslateUi() override;

  private:
    void look_up(const QString& symbol);
    void on_result(const QString& request_id, const services::EconomicsResult& result);
    void render();
    void apply_styles();

    QWidget* body_ = nullptr;
    QLineEdit* input_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* pct_ = nullptr;
    QLabel* detail_ = nullptr;
    charts::SeriesLine* chart_ = nullptr;
    QLabel* note_ = nullptr;

    QString symbol_;
    QString pending_id_;
    int sequence_ = 0;
    QJsonObject data_;
    QString error_;
};

class BondPulseWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit BondPulseWidget(QWidget* parent = nullptr);

    void request_bulk_refresh() override;

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void retranslateUi() override;

  private:
    struct Block {
        QLabel* title = nullptr;
        charts::SplitBar* split = nullptr;
        QLabel* line = nullptr;
        charts::BarList* rows = nullptr;
    };
    Block make_block(QWidget* parent, QVBoxLayout* into);
    void fill_block(Block& b, const QJsonObject& c);
    void request();
    void on_result(const QString& request_id, const services::EconomicsResult& result);
    void render();
    void apply_styles();

    QWidget* body_ = nullptr;
    QLabel* basis_ = nullptr;
    Block hy_;
    Block ig_;
    QLabel* chart_title_ = nullptr;
    charts::SeriesLine* chart_ = nullptr;

    QString pending_id_;
    int sequence_ = 0;
    QJsonObject data_;
    QString error_;
    QDateTime last_success_;
};

} // namespace fincept::screens::widgets
