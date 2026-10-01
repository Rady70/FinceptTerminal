#pragma once
#include <QJsonArray>
#include <QWidget>

namespace fincept::screens {
class EtfMonthlyChart : public QWidget {
    Q_OBJECT
  public:
    explicit EtfMonthlyChart(QWidget* parent = nullptr);
    void set_months(const QJsonArray& months);
    QJsonArray displayed_months() const;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QJsonArray months_;
};
} // namespace fincept::screens
