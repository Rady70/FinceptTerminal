// src/screens/economics/panels/CftcHeatmap.h
//
// Compact positioning heatmap for the R3 workspace: rows are participant
// classes, columns are report dates, and the cell tint encodes the selected
// metric. Cells are pooled, real widgets with their own tooltips so a missing
// metric reads as an explicit "—" rather than blending into a scale.
//
// Sign colours are only used for sign-valued metrics (weekly change, z-score);
// percentile-style metrics use a neutral sequential scale so a high reading is
// not painted as "good".
#pragma once

#include <QDate>
#include <QString>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QLabel;

namespace fincept::screens {

struct CftcHeatmapCell {
    bool has_value = false;
    double value = 0.0; // metric value; ignored when !has_value
    QString tooltip;    // exact readout
};

struct CftcHeatmapRow {
    QString label;                  // full participant label (tooltip context)
    QString short_label;            // row header text
    QVector<CftcHeatmapCell> cells; // aligned with the dates passed to set_data
};

class CftcHeatmap : public QWidget {
    Q_OBJECT
  public:
    enum class Scale {
        Sequential, // 0..100 (COT index, percentile): dim -> accent
        Diverging,  // -2.5..+2.5 (z-score): negative -> 0 -> positive
        Sign,       // weekly change: red below zero, green above, intensity by magnitude
    };

    explicit CftcHeatmap(QWidget* parent = nullptr);

    /// Replace the heatmap. `dates` must align with every row's cells.
    void set_data(const QVector<QDate>& dates, const QVector<CftcHeatmapRow>& rows, Scale scale);
    void set_scale_labels(const QString& low, const QString& high);
    void clear();
    void refresh_theme();

  private:
    void rebuild();
    QColor cell_color(const CftcHeatmapCell& cell) const;

    QGridLayout* grid_ = nullptr;
    QWidget* legend_row_ = nullptr;
    QLabel* empty_lbl_ = nullptr;
    QLabel* scale_low_lbl_ = nullptr;
    QLabel* scale_high_lbl_ = nullptr;

    QVector<QDate> dates_;
    QVector<CftcHeatmapRow> rows_;
    Scale scale_ = Scale::Sequential;
    QString scale_low_;
    QString scale_high_;

    QVector<QWidget*> cell_widgets_; // owned pooled cells, deleted on rebuild
    double magnitude_max_ = 0.0;     // for the Sign scale
};

} // namespace fincept::screens
