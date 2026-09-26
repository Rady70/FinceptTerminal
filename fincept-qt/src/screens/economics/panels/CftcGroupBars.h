// src/screens/economics/panels/CftcGroupBars.h
//
// Batch 6: the grouped cross-market positioning chart. One horizontal bar per
// market in an asset-class group, all drawn on one shared scale so bars are
// comparable across groups. The bar length is the selected qualified metric
// (Net %OI by default); the bar accent encodes the market's highest-priority
// descriptive attention class so a notable market is visible without reading
// the tables. A metric the engine could not form is never drawn as zero: the
// row shows an explicit dashed "unavailable" marker and a "—" value.
//
// The widget is presentation only: it reads CftcMonitorEntry-derived values
// and emits the exact market key on activation; it owns no analysis.
#pragma once

#include "screens/economics/panels/CftcMonitorVisualModel.h"
#include "services/economics/CftcMonitorModel.h"

#include <QString>
#include <QVector>
#include <QWidget>

namespace fincept::screens {

class CftcGroupBars : public QWidget {
    Q_OBJECT
  public:
    struct Row {
        QString market_key;
        QString label;
        bool has_value = false;
        double value = 0.0; // display units
        CftcMonitorTone tone = CftcMonitorTone::Ordinary;
        QString tooltip;
    };

    struct Scale {
        bool percentile = false; // bars span 0..extent instead of -extent..+extent
        double extent = 1.0;     // shared absolute scale across every group
        int decimals = 2;
    };

    explicit CftcGroupBars(QWidget* parent = nullptr);

    void set_scale(const Scale& scale);
    void set_rows(const QVector<Row>& rows);
    void refresh_theme();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void marketActivated(const QString& market_key);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    int row_at(const QPoint& pos) const;

    QVector<Row> rows_;
    Scale scale_;
    int hover_row_ = -1;
    int label_width_ = 174;
    int value_width_ = 74;
    int row_height_ = 22;
    int pad_top_ = 2;
    int axis_height_ = 14;
};

} // namespace fincept::screens
