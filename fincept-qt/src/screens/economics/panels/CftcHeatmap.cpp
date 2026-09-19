// src/screens/economics/panels/CftcHeatmap.cpp
#include "screens/economics/panels/CftcHeatmap.h"

#include "ui/theme/Theme.h"

#include <QDate>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fincept::screens {

namespace {

QColor mix(const QColor& from, const QColor& to, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                            from.greenF() + (to.greenF() - from.greenF()) * t,
                            from.blueF() + (to.blueF() - from.blueF()) * t);
}

QString cell_style(const QColor& color) {
    return QString("background:%1; border:1px solid %2;").arg(color.name(QColor::HexArgb), ui::colors::BG_BASE());
}

} // namespace

CftcHeatmap::CftcHeatmap(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    grid_ = new QGridLayout;
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(1);
    grid_->setVerticalSpacing(1);
    root->addLayout(grid_);

    empty_lbl_ = new QLabel(tr("No heatmap observations for this selection"), this);
    empty_lbl_->setAlignment(Qt::AlignCenter);
    root->addWidget(empty_lbl_);

    auto* legend = new QWidget(this);
    auto* legend_hl = new QHBoxLayout(legend);
    legend_hl->setContentsMargins(0, 0, 0, 0);
    legend_hl->setSpacing(4);
    scale_low_lbl_ = new QLabel(legend);
    legend_hl->addWidget(scale_low_lbl_);
    legend_row_ = new QWidget(legend);
    legend_row_->setFixedHeight(8);
    auto* swatch_layout = new QHBoxLayout(legend_row_);
    swatch_layout->setContentsMargins(0, 0, 0, 0);
    swatch_layout->setSpacing(1);
    for (int i = 0; i < 12; ++i) {
        auto* swatch = new QFrame(legend_row_);
        swatch->setFixedSize(14, 8);
        swatch->setProperty("cftcSwatch", i);
        swatch_layout->addWidget(swatch);
    }
    legend_hl->addWidget(legend_row_);
    scale_high_lbl_ = new QLabel(legend);
    scale_high_lbl_->setText(QString());
    legend_hl->addWidget(scale_high_lbl_);
    legend_hl->addStretch(1);
    root->addWidget(legend);

    refresh_theme();
    rebuild();
}

void CftcHeatmap::set_data(const QVector<QDate>& dates, const QVector<CftcHeatmapRow>& rows, Scale scale) {
    dates_ = dates;
    rows_ = rows;
    scale_ = scale;
    magnitude_max_ = 0.0;
    for (const auto& row : std::as_const(rows_)) {
        for (const auto& cell : row.cells) {
            if (cell.has_value)
                magnitude_max_ = std::max(magnitude_max_, std::abs(cell.value));
        }
    }
    // refresh_theme() also recolours the legend swatches, which must follow a
    // metric switch (sequential vs diverging vs sign).
    refresh_theme();
}

void CftcHeatmap::set_scale_labels(const QString& low, const QString& high) {
    scale_low_ = low;
    scale_high_ = high;
    if (scale_low_lbl_)
        scale_low_lbl_->setText(scale_low_);
    if (scale_high_lbl_)
        scale_high_lbl_->setText(scale_high_);
}

void CftcHeatmap::clear() {
    dates_.clear();
    rows_.clear();
    magnitude_max_ = 0.0;
    rebuild();
}

void CftcHeatmap::refresh_theme() {
    const auto label_style =
        QString("color:%1; font-size:9px; background:transparent;").arg(ui::colors::TEXT_TERTIARY());
    if (scale_low_lbl_)
        scale_low_lbl_->setStyleSheet(label_style);
    if (scale_high_lbl_)
        scale_high_lbl_->setStyleSheet(label_style);
    if (empty_lbl_)
        empty_lbl_->setStyleSheet(
            QString("color:%1; font-size:11px; background:transparent;").arg(ui::colors::TEXT_SECONDARY()));

    // Legend swatches read the same scale the cells do.
    QColor low_color;
    QColor high_color;
    switch (scale_) {
        case Scale::Sequential:
            low_color = QColor(ui::colors::BG_RAISED());
            high_color = QColor(ui::colors::AMBER());
            break;
        case Scale::Diverging:
            low_color = QColor(ui::colors::NEGATIVE());
            high_color = QColor(ui::colors::POSITIVE());
            break;
        case Scale::Sign:
            low_color = QColor(ui::colors::NEGATIVE());
            high_color = QColor(ui::colors::POSITIVE());
            break;
    }
    const auto swatches = legend_row_ ? legend_row_->findChildren<QFrame*>() : QList<QFrame*>();
    for (auto* swatch : swatches) {
        const int index = swatch->property("cftcSwatch").toInt();
        const double t = static_cast<double>(index) / 11.0;
        QColor color;
        if (scale_ == Scale::Diverging || scale_ == Scale::Sign) {
            // Negative end -> neutral center -> positive end.
            color = t < 0.5 ? mix(QColor(ui::colors::BG_RAISED()), low_color, (0.5 - t) * 2.0)
                            : mix(QColor(ui::colors::BG_RAISED()), high_color, (t - 0.5) * 2.0);
        } else {
            color = mix(low_color, high_color, t);
        }
        swatch->setStyleSheet(QString("background:%1; border:none;").arg(color.name(QColor::HexRgb)));
    }
    rebuild();
}

QColor CftcHeatmap::cell_color(const CftcHeatmapCell& cell) const {
    if (!cell.has_value)
        return QColor(ui::colors::BG_BASE());

    switch (scale_) {
        case Scale::Sequential: {
            const double t = std::clamp(cell.value / 100.0, 0.0, 1.0);
            return mix(QColor(ui::colors::BG_RAISED()), QColor(ui::colors::AMBER()), t);
        }
        case Scale::Diverging: {
            const double t = std::clamp((cell.value + 2.5) / 5.0, 0.0, 1.0);
            if (t < 0.5)
                return mix(QColor(ui::colors::BG_RAISED()), QColor(ui::colors::NEGATIVE()), (0.5 - t) * 2.0);
            return mix(QColor(ui::colors::BG_RAISED()), QColor(ui::colors::POSITIVE()), (t - 0.5) * 2.0);
        }
        case Scale::Sign: {
            if (magnitude_max_ <= 0.0 || cell.value == 0.0)
                return QColor(ui::colors::BG_RAISED());
            const double intensity = std::clamp(std::abs(cell.value) / magnitude_max_, 0.0, 1.0);
            const QColor target(cell.value > 0.0 ? ui::colors::POSITIVE() : ui::colors::NEGATIVE());
            return mix(QColor(ui::colors::BG_RAISED()), target, 0.25 + 0.75 * intensity);
        }
    }
    return QColor(ui::colors::BG_RAISED());
}

void CftcHeatmap::rebuild() {
    if (!grid_)
        return;

    for (auto* widget : std::as_const(cell_widgets_)) {
        grid_->removeWidget(widget);
        widget->deleteLater();
    }
    cell_widgets_.clear();

    const bool has_data = !dates_.isEmpty() && !rows_.isEmpty();
    empty_lbl_->setVisible(!has_data);
    if (legend_row_)
        legend_row_->setVisible(has_data);
    if (scale_low_lbl_)
        scale_low_lbl_->setVisible(has_data);
    if (scale_high_lbl_)
        scale_high_lbl_->setVisible(has_data);

    if (!has_data)
        return;

    const auto header_style =
        QString("color:%1; font-size:9px; font-weight:700; background:transparent;").arg(ui::colors::TEXT_TERTIARY());
    const auto row_style =
        QString("color:%1; font-size:10px; background:transparent;").arg(ui::colors::TEXT_SECONDARY());

    // Date headers: sparse enough to stay readable at 52 columns.
    const int step = std::max(1, static_cast<int>(dates_.size()) / 5);
    for (int col = 0; col < dates_.size(); ++col) {
        // The last column always carries the end-label; suppress a periodic
        // label one column earlier so the two texts cannot overlap.
        const bool labelled = col == 0 || col == dates_.size() - 1 || (col % step == 0 && dates_.size() - 1 - col >= 2);
        auto* label = new QLabel(labelled ? dates_[col].toString(QStringLiteral("MMM yy")) : QString(), this);
        label->setStyleSheet(header_style);
        label->setAlignment(Qt::AlignCenter);
        grid_->addWidget(label, 0, col + 1);
        cell_widgets_.append(label);
    }

    for (int row = 0; row < rows_.size(); ++row) {
        auto* name = new QLabel(rows_[row].short_label, this);
        name->setStyleSheet(row_style);
        name->setToolTip(rows_[row].label);
        name->setMinimumWidth(110);
        grid_->addWidget(name, row + 1, 0);
        cell_widgets_.append(name);

        for (int col = 0; col < dates_.size(); ++col) {
            const CftcHeatmapCell cell = col < rows_[row].cells.size() ? rows_[row].cells[col] : CftcHeatmapCell{};
            auto* swatch = new QFrame(this);
            swatch->setMinimumSize(10, 16);
            swatch->setStyleSheet(cell_style(cell_color(cell)));
            if (!cell.tooltip.isEmpty())
                swatch->setToolTip(cell.tooltip);
            grid_->addWidget(swatch, row + 1, col + 1);
            cell_widgets_.append(swatch);
        }
    }

    grid_->setColumnStretch(0, 0);
    for (int col = 1; col <= dates_.size(); ++col)
        grid_->setColumnStretch(col, 1);
}

} // namespace fincept::screens
