// src/screens/economics/panels/IlostatPanel.h
// ILO ILOSTAT — unemployment, labour force participation, employment-to-population.
// Script: ilostat_data.py  |  No API key required.
#pragma once

#include "screens/economics/panels/EconPanelBase.h"

#include <QComboBox>
#include <QLineEdit>

namespace fincept::screens {

class IlostatPanel : public EconPanelBase {
    Q_OBJECT
  public:
    explicit IlostatPanel(QWidget* parent = nullptr);
    void activate() override;

  protected:
    void build_controls(QHBoxLayout* thl) override;
    void on_fetch() override;
    void on_result(const QString& request_id, const services::EconomicsResult& result) override;
    void changeEvent(QEvent* event) override;

  private:
    void retranslateUi() override;

    QComboBox* series_combo_ = nullptr;
    QLineEdit* country_edit_ = nullptr;
    QLineEdit* start_edit_ = nullptr;
    QLineEdit* end_edit_ = nullptr;

    /// Request id of the in-flight fetch; a late response (success or failure)
    /// for an older request must not overwrite the current one. The descriptor
    /// below matches that id so the result is labelled from the request, not
    /// from a later user selection.
    QString pending_request_;
    int pending_series_index_ = -1;
    QString pending_country_;

    // Cached for retranslateUi
    QLabel* series_lbl_ = nullptr;
    QLabel* country_lbl_ = nullptr;
    QLabel* from_lbl_ = nullptr;
    QLabel* to_lbl_ = nullptr;
};

} // namespace fincept::screens
