// src/screens/economics/panels/CftcPositioningChart.cpp
#include "screens/economics/panels/CftcPositioningChart.h"

#include "ui/charts/ChartFactory.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QGraphicsLineItem>
#include <QGuiApplication>
#include <QLabel>
#include <QMargins>
#include <QMouseEvent>
#include <QPen>
#include <QScreen>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace fincept::screens {

namespace {

qreal point_x(const ui::TimeSeriesPoint& point) {
    return static_cast<qreal>(QDateTime(point.date, QTime(0, 0)).toMSecsSinceEpoch());
}

QString axis_date_format(qint64 span_days) {
    if (span_days <= 2)
        return QStringLiteral("dd MMM");
    if (span_days <= 2200)
        return QStringLiteral("MMM yyyy");
    return QStringLiteral("yyyy");
}

QString series_value_text(double value) {
    return ui::format_series_value(value);
}

} // namespace

// ── Canvas: chart + multi-series crosshair ───────────────────────────────────

class CftcPositioningChart::Canvas : public QChartView {
  public:
    explicit Canvas(QWidget* parent = nullptr) : QChartView(parent) {
        setMouseTracking(true);
        setRenderHint(QPainter::Antialiasing);
        setStyleSheet(QStringLiteral("background: transparent; border: none;"));

        tooltip_ = new QLabel(this);
        tooltip_->setWindowFlags(Qt::ToolTip);
        tooltip_->hide();
        refresh_tooltip_style();
    }

    void install_chart(QChart* chart) {
        QChart* previous = this->chart();
        setChart(chart);
        if (previous)
            previous->deleteLater();

        crosshair_ = new QGraphicsLineItem(chart);
        crosshair_->setPen(QPen(QColor(ui::colors::TEXT_SECONDARY()), 1, Qt::DashLine));
        crosshair_->setVisible(false);
        crosshair_->setZValue(10);
    }

    void set_hover_data(QVector<CftcChartSeries> series, const QVector<ui::TimeSeriesPoint>& open_interest,
                        bool show_open_interest) {
        hover_series_ = std::move(series);
        hover_open_interest_ = open_interest;
        hover_show_open_interest_ = show_open_interest;
        hide_crosshair();
    }

    void refresh_tooltip_style() {
        tooltip_->setStyleSheet(
            QString("QLabel { background:%1; color:%2; border:1px solid %3;"
                    " font-size:11px; font-weight:600; padding:3px 6px; }")
                .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_MED()));
    }

  protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        QChartView::mouseMoveEvent(event);
        update_crosshair(event->pos());
    }

    void leaveEvent(QEvent* event) override {
        QChartView::leaveEvent(event);
        hide_crosshair();
    }

  private:
    bool axis_window(long long& min_ms, long long& max_ms) const {
        if (!chart())
            return false;
        const auto axes = chart()->axes(Qt::Horizontal);
        if (axes.isEmpty())
            return false;
        if (auto* axis = qobject_cast<QDateTimeAxis*>(axes.first())) {
            min_ms = axis->min().toMSecsSinceEpoch();
            max_ms = axis->max().toMSecsSinceEpoch();
            return max_ms > min_ms;
        }
        return false;
    }

    void update_crosshair(const QPoint& widget_pos) {
        if (!chart() || chart()->axes(Qt::Horizontal).isEmpty()) {
            hide_crosshair();
            return;
        }

        long long window_min = 0;
        long long window_max = 0;
        if (!axis_window(window_min, window_max)) {
            hide_crosshair();
            return;
        }

        const QPointF chart_value = chart()->mapToValue(QPointF(widget_pos));

        // Snap to the nearest report date that carries at least one visible
        // observation inside the window; every series present at that date is
        // reported, and a series without one is simply absent (a real gap is
        // not a zero).
        qreal best_x = 0.0;
        double best_distance = std::numeric_limits<double>::max();
        bool found = false;
        for (const auto& series : std::as_const(hover_series_)) {
            for (const auto& point : series.points) {
                const double x = point_x(point);
                if (x < static_cast<double>(window_min) || x > static_cast<double>(window_max))
                    continue;
                const double distance = std::abs(x - chart_value.x());
                if (distance < best_distance) {
                    best_distance = distance;
                    best_x = x;
                    found = true;
                }
            }
        }
        if (hover_show_open_interest_) {
            for (const auto& point : std::as_const(hover_open_interest_)) {
                const double x = point_x(point);
                if (x < static_cast<double>(window_min) || x > static_cast<double>(window_max))
                    continue;
                const double distance = std::abs(x - chart_value.x());
                if (distance < best_distance) {
                    best_distance = distance;
                    best_x = x;
                    found = true;
                }
            }
        }
        if (!found) {
            hide_crosshair();
            return;
        }

        const qint64 snapped_ms = static_cast<qint64>(best_x);
        const QDate snapped_date = QDateTime::fromMSecsSinceEpoch(snapped_ms, QTimeZone::LocalTime).date();
        const QPointF scene_pos = chart()->mapToPosition(QPointF(best_x, 0.0));
        const QRectF plot = chart()->plotArea();
        crosshair_->setLine(scene_pos.x(), plot.top(), scene_pos.x(), plot.bottom());
        crosshair_->setVisible(true);

        QStringList lines;
        lines << snapped_date.toString(Qt::ISODate);
        for (const auto& series : std::as_const(hover_series_)) {
            for (const auto& point : series.points) {
                if (point.date != snapped_date)
                    continue;
                lines << QStringLiteral("%1  %2").arg(series.label, series_value_text(point.value));
                break;
            }
        }
        if (hover_show_open_interest_) {
            for (const auto& point : std::as_const(hover_open_interest_)) {
                if (point.date != snapped_date)
                    continue;
                lines << QStringLiteral("%1  %2").arg(QCoreApplication::translate("CftcPanel", "Open Interest"),
                                                      series_value_text(point.value));
                break;
            }
        }
        tooltip_->setText(lines.join(QLatin1Char('\n')));
        tooltip_->adjustSize();

        const QPoint global_pos = mapToGlobal(widget_pos) + QPoint(12, -tooltip_->height() - 4);
        QPoint shown = global_pos;
        if (QScreen* screen = QGuiApplication::screenAt(global_pos)) {
            const QRect bounds = screen->availableGeometry();
            if (shown.x() + tooltip_->width() > bounds.right())
                shown.rx() -= tooltip_->width() + 24;
            if (shown.y() < bounds.top())
                shown.ry() = mapToGlobal(widget_pos).y() + 12;
        }
        tooltip_->move(shown);
        tooltip_->show();
        tooltip_->raise();
    }

    void hide_crosshair() {
        if (crosshair_)
            crosshair_->setVisible(false);
        if (tooltip_)
            tooltip_->hide();
    }

    QVector<CftcChartSeries> hover_series_;
    QVector<ui::TimeSeriesPoint> hover_open_interest_;
    bool hover_show_open_interest_ = false;
    QGraphicsLineItem* crosshair_ = nullptr;
    QLabel* tooltip_ = nullptr;
};

// ── Chart widget ────────────────────────────────────────────────────────────

CftcPositioningChart::CftcPositioningChart(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    canvas_ = new Canvas(this);
    canvas_->setMinimumHeight(260);
    root->addWidget(canvas_, 1);

    empty_lbl_ = new QLabel(this);
    empty_lbl_->setAlignment(Qt::AlignCenter);
    root->addWidget(empty_lbl_, 1);

    retranslateUi();
    refresh_theme();
}

void CftcPositioningChart::set_series(const QVector<CftcChartSeries>& series,
                                      const QVector<ui::TimeSeriesPoint>& open_interest) {
    series_ = series;
    open_interest_ = open_interest;
    rebuild();
}

void CftcPositioningChart::set_open_interest_visible(bool visible) {
    show_open_interest_ = visible;
    rebuild();
}

void CftcPositioningChart::clear() {
    series_.clear();
    open_interest_.clear();
    rebuild();
}

void CftcPositioningChart::refresh_theme() {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));
    if (canvas_) {
        canvas_->refresh_tooltip_style();
        if (canvas_->chart())
            ui::ChartFactory::apply_theme(canvas_->chart());
    }
    empty_lbl_->setStyleSheet(
        QString("color:%1; font-size:12px; background:transparent;").arg(ui::colors::TEXT_SECONDARY()));
    rebuild();
}

void CftcPositioningChart::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void CftcPositioningChart::retranslateUi() {
    if (empty_lbl_)
        empty_lbl_->setText(tr("No chartable observations for the selected range"));
}

void CftcPositioningChart::rebuild() {
    if (!canvas_)
        return;

    bool has_points = show_open_interest_ && !open_interest_.isEmpty();
    for (const auto& series : std::as_const(series_)) {
        if (!series.points.isEmpty()) {
            has_points = true;
            break;
        }
    }

    canvas_->setVisible(has_points);
    empty_lbl_->setVisible(!has_points);
    if (!has_points) {
        canvas_->set_hover_data({}, {}, false);
        canvas_->install_chart(new QChart);
        ui::ChartFactory::apply_theme(canvas_->chart());
        setAccessibleName(tr("Historical positioning chart, no observations for the selected range"));
        return;
    }

    auto* chart = new QChart;
    chart->setAnimationOptions(QChart::NoAnimation);
    chart->legend()->setVisible(false);
    chart->setMargins(QMargins(4, 4, 4, 4));
    chart->setBackgroundRoundness(0);

    auto* axis_x = new QDateTimeAxis;
    chart->addAxis(axis_x, Qt::AlignBottom);

    auto* axis_y = new QValueAxis;
    chart->addAxis(axis_y, Qt::AlignLeft);
    axis_y->setLabelFormat(QStringLiteral("%.6g"));
    axis_y->applyNiceNumbers();

    double min_value = 0.0;
    double max_value = 0.0;
    bool have_range = false;
    qreal first_x = 0.0;
    qreal last_x = 0.0;
    bool have_x = false;

    for (const auto& series : std::as_const(series_)) {
        if (series.points.isEmpty())
            continue;
        const auto segments = ui::split_time_series_gaps(series.points, QStringLiteral("Weekly"));
        for (const auto& segment : segments) {
            if (segment.size() < 2)
                continue;
            auto* line = new QLineSeries;
            line->setPen(QPen(series.color, 1.6));
            for (const auto& point : segment)
                line->append(point_x(point), point.value);
            chart->addSeries(line);
            line->attachAxis(axis_x);
            line->attachAxis(axis_y);
        }

        // Isolated observations between two gaps carry no line; they are always
        // marked so a real report cannot vanish from the chart.
        QVector<ui::TimeSeriesPoint> isolated;
        for (const auto& segment : segments) {
            if (segment.size() == 1)
                isolated << segment.first();
        }
        if (series.points.size() <= 250 || !isolated.isEmpty()) {
            const QVector<ui::TimeSeriesPoint>& marker_points = series.points.size() <= 250 ? series.points : isolated;
            auto* markers = new QScatterSeries;
            markers->setMarkerSize(4.5);
            markers->setColor(series.color);
            markers->setBorderColor(QColor(ui::colors::BG_SURFACE()));
            for (const auto& point : marker_points)
                markers->append(point_x(point), point.value);
            chart->addSeries(markers);
            markers->attachAxis(axis_x);
            markers->attachAxis(axis_y);
        }

        for (const auto& point : series.points) {
            min_value = have_range ? std::min(min_value, point.value) : point.value;
            max_value = have_range ? std::max(max_value, point.value) : point.value;
            have_range = true;
            if (!have_x) {
                first_x = point_x(point);
                last_x = first_x;
                have_x = true;
            } else {
                first_x = std::min(first_x, point_x(point));
                last_x = std::max(last_x, point_x(point));
            }
        }
    }

    if (have_range) {
        if (max_value - min_value <= 0.0) {
            const double pad = std::max(1.0, std::abs(max_value) * 0.05);
            axis_y->setRange(min_value - pad, max_value + pad);
        } else {
            const double pad = (max_value - min_value) * 0.06;
            axis_y->setRange(min_value - pad, max_value + pad);
        }
    }

    // Open interest rides a dashed secondary axis: it is a different quantity
    // (total contracts) from participant positions and must not share their
    // scale.
    if (show_open_interest_ && !open_interest_.isEmpty()) {
        auto* axis_oi = new QValueAxis;
        chart->addAxis(axis_oi, Qt::AlignRight);
        axis_oi->setLabelFormat(QStringLiteral("%.6g"));
        axis_oi->applyNiceNumbers();
        double oi_min = open_interest_.first().value;
        double oi_max = oi_min;
        for (const auto& point : std::as_const(open_interest_)) {
            oi_min = std::min(oi_min, point.value);
            oi_max = std::max(oi_max, point.value);
        }
        if (oi_max - oi_min <= 0.0) {
            const double pad = std::max(1.0, std::abs(oi_max) * 0.05);
            axis_oi->setRange(oi_min - pad, oi_max + pad);
        } else {
            const double pad = (oi_max - oi_min) * 0.06;
            axis_oi->setRange(oi_min - pad, oi_max + pad);
        }
        const auto segments = ui::split_time_series_gaps(open_interest_, QStringLiteral("Weekly"));
        for (const auto& segment : segments) {
            if (segment.size() < 2)
                continue;
            auto* line = new QLineSeries;
            line->setPen(QPen(QColor(ui::colors::TEXT_SECONDARY()), 1.2, Qt::DashLine));
            for (const auto& point : segment)
                line->append(point_x(point), point.value);
            chart->addSeries(line);
            line->attachAxis(axis_x);
            line->attachAxis(axis_oi);
        }
        for (const auto& point : std::as_const(open_interest_)) {
            if (!have_x) {
                first_x = point_x(point);
                last_x = first_x;
                have_x = true;
            } else {
                first_x = std::min(first_x, point_x(point));
                last_x = std::max(last_x, point_x(point));
            }
        }
    }

    if (last_x <= first_x) {
        first_x -= 43200000.0; // half a day either side of a single observation
        last_x += 43200000.0;
    }
    axis_x->setFormat(axis_date_format(static_cast<qint64>((last_x - first_x) / 86400000.0)));
    axis_x->setRange(QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(first_x), QTimeZone::LocalTime),
                     QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(last_x), QTimeZone::LocalTime));
    // The plotted report span is part of the chart's accessible description, so
    // assistive technology (and the UI verification harness) can tell which
    // window is actually on screen without reading painted axis labels.
    setAccessibleName(tr("Historical positioning chart, %1 to %2")
                          .arg(QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(first_x), QTimeZone::LocalTime)
                                   .date()
                                   .toString(Qt::ISODate),
                               QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(last_x), QTimeZone::LocalTime)
                                   .date()
                                   .toString(Qt::ISODate)));

    QVector<CftcChartSeries> hover_series;
    for (const auto& series : std::as_const(series_)) {
        if (!series.points.isEmpty())
            hover_series << series;
    }
    canvas_->install_chart(chart);
    ui::ChartFactory::apply_theme(chart);
    canvas_->set_hover_data(hover_series, open_interest_, show_open_interest_ && !open_interest_.isEmpty());
}

} // namespace fincept::screens
