// src/ui/charts/TimeSeriesChartView.cpp
#include "ui/charts/TimeSeriesChartView.h"

#include "ui/charts/ChartFactory.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QGraphicsLineItem>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMargins>
#include <QMouseEvent>
#include <QPen>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace fincept::ui {

namespace {

qreal point_x(const TimeSeriesPoint& p) {
    // Local midnight keeps QDateTimeAxis' local-time tick labels on the same
    // calendar date the provider reported in every timezone.
    return static_cast<qreal>(QDateTime(p.date, QTime(0, 0)).toMSecsSinceEpoch());
}

QString axis_date_format(qint64 span_days) {
    if (span_days <= 2)
        return QStringLiteral("dd MMM");
    if (span_days <= 2200)
        return QStringLiteral("MMM yyyy");
    return QStringLiteral("yyyy");
}

} // namespace

// ── Crosshair view ────────────────────────────────────────────────────────────
// Plain QChartView subclass: vertical crosshair + floating label that snap to
// the nearest returned observation so the exact period/value is inspectable.

class TimeSeriesCrosshairView : public QChartView {
  public:
    explicit TimeSeriesCrosshairView(QWidget* parent = nullptr) : QChartView(parent) {
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

        v_line_ = new QGraphicsLineItem(chart);
        v_line_->setPen(QPen(QColor(ui::colors::TEXT_TERTIARY()), 1, Qt::DashLine));
        v_line_->setVisible(false);
        v_line_->setZValue(10);
    }

    void set_points(const QVector<TimeSeriesPoint>& points, const QString& unit) {
        points_ = points;
        unit_ = unit;
        hide_crosshair();
    }

    void set_navigator(TimeAxisNavigator* navigator) { navigator_ = navigator; }
    void set_reset_handler(std::function<void()> handler) { reset_handler_ = std::move(handler); }
    void set_navigate_handler(std::function<void()> handler) { navigate_handler_ = std::move(handler); }

    void refresh_tooltip_style() {
        tooltip_->setStyleSheet(
            QString("QLabel { background:%1; color:%2; border:1px solid %3;"
                    " font-size:11px; font-weight:600; padding:3px 6px; }")
                .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_MED()));
    }

  protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        if (dragging_) {
            const qreal value_x = chart() ? chart()->mapToValue(QPointF(event->pos())).x() : 0.0;
            const auto delta = static_cast<long long>(drag_anchor_value_x_ - value_x);
            if (delta != 0 && navigator_ && navigator_->pan(delta)) {
                apply_navigator_range();
                if (navigate_handler_)
                    navigate_handler_();
            }
            hide_crosshair();
            event->accept();
            return;
        }
        QChartView::mouseMoveEvent(event);
        update_crosshair(event->pos());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && chart() && !points_.isEmpty()) {
            dragging_ = true;
            drag_anchor_value_x_ = chart()->mapToValue(QPointF(event->pos())).x();
        }
        QChartView::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton)
            dragging_ = false;
        QChartView::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && reset_handler_) {
            dragging_ = false;
            reset_handler_();
            event->accept();
            return;
        }
        QChartView::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override {
        if (!navigator_ || points_.isEmpty() || !chart() || chart()->axes(Qt::Horizontal).isEmpty()) {
            QChartView::wheelEvent(event);
            return;
        }
        const double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
        if (steps == 0.0) {
            QChartView::wheelEvent(event);
            return;
        }
        const double factor = std::pow(0.8, steps); // wheel up zooms in
        const long long anchor = static_cast<long long>(chart()->mapToValue(QPointF(event->position())).x());
        if (navigator_->zoom(factor, anchor)) {
            apply_navigator_range();
            if (navigate_handler_)
                navigate_handler_();
        }
        event->accept();
    }

    void leaveEvent(QEvent* event) override {
        dragging_ = false;
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

    bool apply_navigator_range() {
        if (!navigator_ || !navigator_->active() || !chart())
            return false;
        const auto axes = chart()->axes(Qt::Horizontal);
        if (axes.isEmpty())
            return false;
        auto* axis = qobject_cast<QDateTimeAxis*>(axes.first());
        if (!axis)
            return false;
        const long long span = navigator_->max() - navigator_->min();
        axis->setFormat(axis_date_format(span / 86400000LL));
        axis->setRange(QDateTime::fromMSecsSinceEpoch(navigator_->min(), QTimeZone::LocalTime),
                       QDateTime::fromMSecsSinceEpoch(navigator_->max(), QTimeZone::LocalTime));
        hide_crosshair();
        return true;
    }

    void update_crosshair(const QPoint& widget_pos) {
        if (points_.isEmpty() || !chart() || chart()->axes(Qt::Horizontal).isEmpty()) {
            hide_crosshair();
            return;
        }

        // Only observations inside the visible window can be inspected; a point
        // panned out of view must not snap the crosshair off-plot.
        long long window_min = 0;
        long long window_max = 0;
        if (!axis_window(window_min, window_max)) {
            hide_crosshair();
            return;
        }

        const QPointF chart_value = chart()->mapToValue(QPointF(widget_pos));

        int best = -1;
        double best_distance = std::numeric_limits<double>::max();
        for (int i = 0; i < points_.size(); ++i) {
            const double x = point_x(points_[i]);
            if (x < static_cast<double>(window_min) || x > static_cast<double>(window_max))
                continue;
            const double distance = std::abs(x - chart_value.x());
            if (distance < best_distance) {
                best_distance = distance;
                best = i;
            }
        }
        if (best < 0) {
            hide_crosshair();
            return;
        }

        const TimeSeriesPoint& snapped = points_[best];
        const QPointF scene_pos = chart()->mapToPosition(QPointF(point_x(snapped), snapped.value));
        const QRectF plot = chart()->plotArea();
        v_line_->setLine(scene_pos.x(), plot.top(), scene_pos.x(), plot.bottom());
        v_line_->setVisible(true);

        QString value = format_series_value(snapped.value);
        if (!unit_.isEmpty())
            value += QLatin1Char(' ') + unit_;
        tooltip_->setText(snapped.date_label + QLatin1Char('\n') + value);
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
        if (v_line_)
            v_line_->setVisible(false);
        if (tooltip_)
            tooltip_->hide();
    }

    QVector<TimeSeriesPoint> points_;
    QString unit_;
    QGraphicsLineItem* v_line_ = nullptr;
    QLabel* tooltip_ = nullptr;
    TimeAxisNavigator* navigator_ = nullptr;
    std::function<void()> reset_handler_;
    std::function<void()> navigate_handler_;
    bool dragging_ = false;
    qreal drag_anchor_value_x_ = 0.0;
};

// ── TimeSeriesChartView ───────────────────────────────────────────────────────

TimeSeriesChartView::TimeSeriesChartView(QWidget* parent) : QWidget(parent) {
    build_ui();
}

void TimeSeriesChartView::build_ui() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 6, 10, 6);
    root->setSpacing(4);

    auto* top = new QHBoxLayout;
    top->setSpacing(4);
    range_lbl_ = new QLabel(this);
    top->addWidget(range_lbl_);

    const struct {
        TimeRange value;
        const char* text;
    } kRanges[] = {{TimeRange::OneYear, "1Y"},
                   {TimeRange::ThreeYears, "3Y"},
                   {TimeRange::FiveYears, "5Y"},
                   {TimeRange::TenYears, "10Y"},
                   {TimeRange::Max, "MAX"}};
    for (const auto& spec : kRanges) {
        auto* button = new QPushButton(QString::fromLatin1(spec.text), this);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, [this, range = spec.value]() { apply_range(range); });
        range_btns_ << button;
        range_values_ << spec.value;
        top->addWidget(button);
    }
    top->addStretch(1);
    root->addLayout(top);

    chart_view_ = new TimeSeriesCrosshairView(this);
    chart_view_->set_navigator(&navigator_);
    chart_view_->set_reset_handler([this]() {
        navigator_.reset();
        apply_range(range_);
    });
    chart_view_->set_navigate_handler([this]() {
        // An interactive wheel/drag window no longer matches any preset range,
        // so no range tab may keep claiming it. Qt refuses setChecked(false) on
        // an auto-exclusive button, so exclusivity is lifted for the reset.
        for (auto* button : std::as_const(range_btns_)) {
            QSignalBlocker block(button);
            button->setAutoExclusive(false);
            button->setChecked(false);
            button->setAutoExclusive(true);
        }
    });
    root->addWidget(chart_view_, 1);

    empty_lbl_ = new QLabel(this);
    empty_lbl_->setAlignment(Qt::AlignCenter);
    empty_lbl_->hide();
    root->addWidget(empty_lbl_, 1);

    update_range_buttons();
    retranslateUi();
    refresh_theme();
}

void TimeSeriesChartView::set_series(const TimeSeries& series) {
    series_ = series;
    series_.points = sorted_time_series(series_.points);
    range_ = TimeRange::Max;
    navigator_.reset();
    update_range_buttons();
    rebuild_chart(true);
}

void TimeSeriesChartView::clear_series() {
    series_ = {};
    navigator_.reset();
    update_range_buttons();
    rebuild_chart(true);
}

void TimeSeriesChartView::set_accent_color(const QColor& color) {
    accent_ = color.isValid() ? color : QColor(ui::colors::AMBER());
    refresh_theme();
}

void TimeSeriesChartView::refresh_theme() {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));

    accent_ = accent_.isValid() ? accent_ : QColor(ui::colors::AMBER());

    const QString tab_style =
        QString("QPushButton { background:transparent; color:%1; border:1px solid %2;"
                " font-size:10px; font-weight:700; padding:3px 10px; }"
                "QPushButton:hover { color:%3; background:%4; }"
                "QPushButton:checked { background:%5; color:%6; border-color:%5; }"
                "QPushButton:disabled { color:%2; }")
            .arg(ui::colors::TEXT_SECONDARY(), ui::colors::BORDER_DIM(), ui::colors::TEXT_PRIMARY(),
                 ui::colors::BG_HOVER(), accent_.name(), ui::colors::BG_BASE());
    for (auto* button : std::as_const(range_btns_))
        button->setStyleSheet(tab_style);

    range_lbl_->setStyleSheet(
        QString("color:%1; font-size:9px; font-weight:700; background:transparent;").arg(ui::colors::TEXT_TERTIARY()));
    empty_lbl_->setStyleSheet(
        QString("color:%1; font-size:13px; background:transparent;").arg(ui::colors::TEXT_SECONDARY()));
    if (chart_view_)
        chart_view_->refresh_tooltip_style();
    rebuild_chart(false);
}

void TimeSeriesChartView::apply_range(TimeRange range) {
    range_ = range;
    navigator_.reset();
    update_range_buttons();
    rebuild_chart(true);
}

void TimeSeriesChartView::update_range_buttons() {
    for (int i = 0; i < range_btns_.size(); ++i) {
        const bool available = time_range_available(series_.points, range_values_[i]);
        if (!available && range_ == range_values_[i])
            range_ = TimeRange::Max;
        range_btns_[i]->setEnabled(available);
        range_btns_[i]->setToolTip(available ? QString() : tr("Not enough returned history for this range"));
    }
    const int index = range_values_.indexOf(range_);
    if (index >= 0 && !navigator_.active()) {
        // While the user's zoom/pan window is active no preset matches it;
        // re-checking here would make a language switch claim a window it does
        // not show.
        QSignalBlocker block(range_btns_[index]);
        range_btns_[index]->setChecked(true);
    }
}

void TimeSeriesChartView::rebuild_chart(bool reset_zoom) {
    if (!chart_view_)
        return;

    const QVector<TimeSeriesPoint> visible = filter_time_range(series_.points, range_);
    const bool has_points = !visible.isEmpty();
    empty_lbl_->setVisible(!has_points);
    chart_view_->setVisible(has_points);
    if (!has_points) {
        chart_view_->set_points({}, {});
        chart_view_->install_chart(new QChart);
        ChartFactory::apply_theme(chart_view_->chart());
        return;
    }

    const QColor accent = accent_.isValid() ? accent_ : QColor(ui::colors::AMBER());

    auto* chart = new QChart;
    chart->setAnimationOptions(QChart::NoAnimation);
    chart->legend()->setVisible(false);
    chart->setMargins(QMargins(4, 4, 4, 4));
    chart->setBackgroundRoundness(0);

    auto* axis_x = new QDateTimeAxis;
    chart->addAxis(axis_x, Qt::AlignBottom);

    auto* axis_y = new QValueAxis;
    chart->addAxis(axis_y, Qt::AlignLeft);

    double min_value = visible.first().value;
    double max_value = visible.first().value;
    for (const auto& p : visible) {
        min_value = std::min(min_value, p.value);
        max_value = std::max(max_value, p.value);
    }
    if (max_value - min_value <= 0.0) {
        const double pad = std::max(1.0, std::abs(max_value) * 0.05);
        axis_y->setRange(min_value - pad, max_value + pad);
    } else {
        const double pad = (max_value - min_value) * 0.06;
        axis_y->setRange(min_value - pad, max_value + pad);
    }
    axis_y->applyNiceNumbers();
    // Same 6-significant-digit convention the stat cards use: large national
    // accounts figures become "3.07697e+13" instead of a 14-digit wall of
    // zeros, while ordinary magnitudes (2.4, 90026.5) stay plain.
    axis_y->setLabelFormat(QStringLiteral("%.6g"));

    QString frequency = series_.meta.frequency;
    if (frequency.isEmpty())
        frequency = inferred_frequency_label(series_.points);
    const auto segments = split_time_series_gaps(visible, frequency);
    for (const auto& segment : segments) {
        if (segment.size() < 2)
            continue;
        auto* line = new QLineSeries;
        line->setPen(QPen(accent, 1.6));
        for (const auto& p : segment)
            line->append(point_x(p), p.value);
        chart->addSeries(line);
        line->attachAxis(axis_x);
        line->attachAxis(axis_y);
    }

    // Markers show every observation for short series. Isolated points between
    // two gaps carry no line, so they are always marked even on long series —
    // otherwise a real observation would vanish from the chart.
    QVector<TimeSeriesPoint> isolated;
    for (const auto& segment : segments) {
        if (segment.size() == 1)
            isolated << segment.first();
    }
    if (visible.size() <= 250 || !isolated.isEmpty()) {
        const QVector<TimeSeriesPoint>& marker_points = visible.size() <= 250 ? visible : isolated;
        auto* markers = new QScatterSeries;
        markers->setMarkerSize(5.0);
        markers->setColor(accent);
        markers->setBorderColor(QColor(ui::colors::BG_SURFACE()));
        for (const auto& p : marker_points)
            markers->append(point_x(p), p.value);
        chart->addSeries(markers);
        markers->attachAxis(axis_x);
        markers->attachAxis(axis_y);
    }

    const qreal first_x = point_x(visible.first());
    const qreal last_x = point_x(visible.last());
    qreal lower_x = first_x;
    qreal upper_x = last_x;
    if (upper_x <= lower_x) {
        lower_x -= 43200000.0; // half a day either side of a single observation
        upper_x += 43200000.0;
    }

    // The navigator owns the visible time window once the user zooms or pans;
    // a range-button click or a new series re-seeds it from the real
    // observations. Its slot size caps the pan/zoom margins so a coarse
    // (annual) cadence cannot scroll decades past the data.
    int step_days = frequency_step_days(frequency);
    if (step_days <= 0)
        step_days = inferred_step_days(visible);
    if (step_days <= 0)
        step_days = 1;
    const long long cadence_ms = static_cast<long long>(step_days) * 86400000LL;
    const long long data_span_ms =
        std::max<long long>(1, static_cast<long long>(last_x) - static_cast<long long>(first_x));
    const long long navigator_slot_ms = std::min(cadence_ms, std::max<long long>(86400000LL, data_span_ms / 80));
    navigator_.set_extent(static_cast<long long>(first_x), static_cast<long long>(last_x), navigator_slot_ms);
    if (reset_zoom)
        navigator_.reset();
    long long window_min = static_cast<long long>(lower_x);
    long long window_max = static_cast<long long>(upper_x);
    if (navigator_.active()) {
        window_min = navigator_.min();
        window_max = navigator_.max();
    } else {
        navigator_.seed(window_min, window_max);
    }
    axis_x->setFormat(axis_date_format((window_max - window_min) / 86400000LL));
    axis_x->setRange(QDateTime::fromMSecsSinceEpoch(window_min, QTimeZone::LocalTime),
                     QDateTime::fromMSecsSinceEpoch(window_max, QTimeZone::LocalTime));

    chart_view_->install_chart(chart);
    ChartFactory::apply_theme(chart);
    chart_view_->set_points(visible, series_.meta.unit);
}

void TimeSeriesChartView::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void TimeSeriesChartView::retranslateUi() {
    if (range_lbl_)
        range_lbl_->setText(tr("RANGE"));
    if (empty_lbl_)
        empty_lbl_->setText(tr("No observations to chart"));
    update_range_buttons(); // re-applies the translated availability tooltip
}

} // namespace fincept::ui
