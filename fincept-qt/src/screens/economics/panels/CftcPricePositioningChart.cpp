// src/screens/economics/panels/CftcPricePositioningChart.cpp
#include "screens/economics/panels/CftcPricePositioningChart.h"

#include "ui/charts/ChartFactory.h"
#include "ui/theme/Theme.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QGraphicsLineItem>
#include <QGuiApplication>
#include <QLabel>
#include <QMargins>
#include <QMouseEvent>
#include <QPen>
#include <QScreen>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QLineSeries>
#include <QtCharts/QScatterSeries>
#include <QtCharts/QValueAxis>

#include <algorithm>
#include <cmath>
#include <functional>
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

/// Width of the widest value-axis label the axis itself would draw, measured
/// with the chart's actual label font so both stacked panes can reserve the
/// same left inset and neither pane's labels are elided.
int axis_label_width(const QValueAxis* axis, const QFontMetrics& fm) {
    if (!axis)
        return 0;
    const QByteArray format = axis->labelFormat().toUtf8();
    int width = 0;
    for (int i = 0; i <= 4; ++i) {
        const double value = axis->min() + (axis->max() - axis->min()) * static_cast<double>(i) / 4.0;
        width = std::max(width, fm.horizontalAdvance(QString::asprintf(format.constData(), value)));
    }
    return width + fm.horizontalAdvance(QStringLiteral("-00"));
}

QString pane_value_text(double value) {
    return ui::format_series_value(value);
}

QColor pane_palette_color(int index) {
    const auto& colors = ui::ThemeManager::instance().tokens().chart_colors;
    const int count = static_cast<int>(colors.size());
    if (count <= 0)
        return QColor(ui::colors::POSITIVE());
    return QColor(QString::fromLatin1(colors[static_cast<std::size_t>(index) % static_cast<std::size_t>(count)]));
}

} // namespace

// ── Canvas: one pane with a date-snapped crosshair ──────────────────────────

class CftcPricePositioningChart::Canvas : public QChartView {
  public:
    explicit Canvas(QWidget* parent = nullptr) : QChartView(parent) {
        setMouseTracking(true);
        setRenderHint(QPainter::Antialiasing);
        // The two panes must stay on one report-date axis. Disabling view
        // interaction stops a wheel zoom from changing only one pane's range
        // and silently desynchronizing the shared crosshair; the visible range
        // is controlled by the panel's report-range buttons.
        setInteractive(false);
        setRubberBand(QChartView::NoRubberBand);
        setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    }

    std::function<void(const QDate&, const QPoint&)> hover_handler;
    std::function<void()> leave_handler;

    void install_chart(QChart* chart) {
        QChart* previous = this->chart();
        setChart(chart);
        if (previous)
            previous->deleteLater();

        crosshair_ = new QGraphicsLineItem(chart);
        crosshair_->setPen(QPen(QColor(ui::colors::TEXT_TERTIARY()), 1, Qt::DashLine));
        crosshair_->setVisible(false);
        crosshair_->setZValue(10);
        snap_dates_.clear();
    }

    void set_snap_dates(const QVector<QDate>& dates) { snap_dates_ = dates; }

    void refresh_crosshair_style() {
        if (crosshair_)
            crosshair_->setPen(QPen(QColor(ui::colors::TEXT_TERTIARY()), 1, Qt::DashLine));
    }

    void set_crosshair(const QDate& date, bool visible) {
        if (!crosshair_ || !visible || !date.isValid() || !chart() || chart()->plotArea().isEmpty()) {
            if (crosshair_)
                crosshair_->setVisible(false);
            return;
        }
        const QPointF position =
            chart()->mapToPosition(QPointF(static_cast<qreal>(QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch()), 0.0));
        const QRectF plot = chart()->plotArea();
        crosshair_->setLine(position.x(), plot.top(), position.x(), plot.bottom());
        crosshair_->setVisible(true);
    }

  protected:
    void mouseMoveEvent(QMouseEvent* event) override {
        QChartView::mouseMoveEvent(event);
        if (!hover_handler || !chart() || snap_dates_.isEmpty()) {
            if (leave_handler)
                leave_handler();
            return;
        }
        long long window_min = 0;
        long long window_max = 0;
        if (!axis_window(window_min, window_max)) {
            if (leave_handler)
                leave_handler();
            return;
        }
        const QPointF chart_value = chart()->mapToValue(event->pos());
        const QDate snapped =
            cftc_sync_nearest_snap_date(snap_dates_, static_cast<qint64>(chart_value.x()), window_min, window_max);
        if (!snapped.isValid()) {
            if (leave_handler)
                leave_handler();
            return;
        }
        hover_handler(snapped, mapToGlobal(event->pos()));
    }

    void leaveEvent(QEvent* event) override {
        QChartView::leaveEvent(event);
        if (leave_handler)
            leave_handler();
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

    QVector<QDate> snap_dates_;
    QGraphicsLineItem* crosshair_ = nullptr;
};

// ── Widget ──────────────────────────────────────────────────────────────────

CftcPricePositioningChart::CftcPricePositioningChart(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(3);

    auto make_header = [this](const char* object_name) {
        auto* label = new QLabel(this);
        label->setObjectName(QString::fromLatin1(object_name));
        return label;
    };
    auto make_note = [this]() {
        auto* label = new QLabel(this);
        label->setObjectName(QStringLiteral("cftcPaneNote"));
        label->setWordWrap(true);
        return label;
    };

    price_header_ = make_header("cftcPaneTitle");
    price_unavailable_lbl_ = make_note();
    price_canvas_ = new Canvas(this);
    price_canvas_->setMinimumHeight(220);

    position_header_ = make_header("cftcPaneTitle");
    position_unavailable_lbl_ = make_note();
    position_canvas_ = new Canvas(this);
    position_canvas_->setMinimumHeight(170);

    context_lbl_ = make_note();
    empty_lbl_ = new QLabel(this);
    empty_lbl_->setAlignment(Qt::AlignCenter);

    root->addWidget(price_header_);
    root->addWidget(price_unavailable_lbl_);
    root->addWidget(price_canvas_);
    root->addWidget(position_header_);
    root->addWidget(position_unavailable_lbl_);
    root->addWidget(position_canvas_);
    root->addWidget(context_lbl_);
    root->addWidget(empty_lbl_);

    tooltip_ = new QLabel(this);
    tooltip_->setWindowFlags(Qt::ToolTip);
    tooltip_->hide();

    price_canvas_->hover_handler = [this](const QDate& date, const QPoint& pos) { handle_hover(date, pos); };
    position_canvas_->hover_handler = [this](const QDate& date, const QPoint& pos) { handle_hover(date, pos); };
    price_canvas_->leave_handler = [this]() { hide_hover(); };
    position_canvas_->leave_handler = [this]() { hide_hover(); };

    retranslateUi();
    refresh_theme();
}

void CftcPricePositioningChart::set_data(const CftcSyncChartData& data) {
    data_ = data;
    rebuild();
}

void CftcPricePositioningChart::clear() {
    data_ = {};
    rebuild();
}

void CftcPricePositioningChart::refresh_theme() {
    const QString label_style = QStringLiteral("background:transparent;");
    price_header_->setStyleSheet(QStringLiteral("color:%1; font-size:9px; font-weight:700; letter-spacing:1px; ")
                                     .arg(ui::colors::TEXT_TERTIARY()) +
                                 label_style);
    position_header_->setStyleSheet(QStringLiteral("color:%1; font-size:9px; font-weight:700; letter-spacing:1px; ")
                                        .arg(ui::colors::TEXT_TERTIARY()) +
                                    label_style);
    const QString note_style = QStringLiteral("color:%1; font-size:9px; ").arg(ui::colors::TEXT_DIM()) + label_style;
    price_unavailable_lbl_->setStyleSheet(note_style);
    position_unavailable_lbl_->setStyleSheet(note_style);
    context_lbl_->setStyleSheet(note_style);
    empty_lbl_->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px; background:transparent;").arg(ui::colors::TEXT_SECONDARY()));
    tooltip_->setStyleSheet(QStringLiteral("QLabel { background:%1; color:%2; border:1px solid %3;"
                                           " font-size:11px; font-weight:600; padding:3px 6px; }")
                                .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_MED()));
    price_canvas_->refresh_crosshair_style();
    position_canvas_->refresh_crosshair_style();
    rebuild();
}

void CftcPricePositioningChart::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    QWidget::changeEvent(event);
}

void CftcPricePositioningChart::retranslateUi() {
    if (empty_lbl_)
        empty_lbl_->setText(tr("No synchronized price and positioning observations for the selected range"));
    rebuild();
}

namespace {

struct PaneYRange {
    double min_value = 0.0;
    double max_value = 0.0;
    bool has_range = false;
};

PaneYRange pane_y_range(const QVector<ui::TimeSeriesPoint>& points) {
    PaneYRange range;
    for (const auto& point : points) {
        if (!range.has_range) {
            range.min_value = point.value;
            range.max_value = point.value;
            range.has_range = true;
        } else {
            range.min_value = std::min(range.min_value, point.value);
            range.max_value = std::max(range.max_value, point.value);
        }
    }
    if (!range.has_range) {
        range.min_value = 0.0;
        range.max_value = 1.0;
        return range;
    }
    if (range.max_value - range.min_value <= 0.0) {
        const double pad = std::max(1.0, std::abs(range.max_value) * 0.05);
        range.min_value -= pad;
        range.max_value += pad;
    } else {
        const double pad = (range.max_value - range.min_value) * 0.06;
        range.min_value -= pad;
        range.max_value += pad;
    }
    return range;
}

QChart* make_pane_chart(const QVector<ui::TimeSeriesPoint>& points, const QColor& color, const QDate& x_first,
                        const QDate& x_last, const QString& axis_format) {
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
    const PaneYRange y_range = pane_y_range(points);
    axis_y->setRange(y_range.min_value, y_range.max_value);
    axis_y->applyNiceNumbers();

    const auto segments = ui::split_time_series_gaps(points, QStringLiteral("Weekly"));
    QVector<ui::TimeSeriesPoint> isolated;
    for (const auto& segment : segments) {
        if (segment.size() == 1) {
            isolated << segment.first();
            continue;
        }
        auto* line = new QLineSeries;
        line->setPen(QPen(color, 1.6));
        for (const auto& point : segment)
            line->append(point_x(point), point.value);
        chart->addSeries(line);
        line->attachAxis(axis_x);
        line->attachAxis(axis_y);
    }
    if (!points.isEmpty()) {
        const QVector<ui::TimeSeriesPoint>& marker_points = points.size() <= 250 ? points : isolated;
        if (!marker_points.isEmpty()) {
            auto* markers = new QScatterSeries;
            markers->setMarkerSize(4.5);
            markers->setColor(color);
            markers->setBorderColor(QColor(ui::colors::BG_SURFACE()));
            for (const auto& point : marker_points)
                markers->append(point_x(point), point.value);
            chart->addSeries(markers);
            markers->attachAxis(axis_x);
            markers->attachAxis(axis_y);
        }
    }

    qreal first_x = static_cast<qreal>(QDateTime(x_first, QTime(0, 0)).toMSecsSinceEpoch());
    qreal last_x = static_cast<qreal>(QDateTime(x_last, QTime(0, 0)).toMSecsSinceEpoch());
    if (!x_first.isValid() || !x_last.isValid()) {
        first_x = 0.0;
        last_x = 1.0;
    } else if (last_x <= first_x) {
        first_x -= 43200000.0; // half a day either side of a single report
        last_x += 43200000.0;
    }
    axis_x->setFormat(axis_format);
    axis_x->setRange(QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(first_x), QTimeZone::LocalTime),
                     QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(last_x), QTimeZone::LocalTime));
    return chart;
}

} // namespace

void CftcPricePositioningChart::rebuild() {
    if (!price_canvas_ || !position_canvas_)
        return;
    hide_hover();

    const bool has_price = data_.price.available && !data_.price.points.isEmpty();
    const bool has_position = data_.positioning.available && !data_.positioning.points.isEmpty();
    const bool has_any = has_price || has_position;

    empty_lbl_->setVisible(!has_any);
    price_header_->setVisible(has_any);
    position_header_->setVisible(has_any);
    context_lbl_->setVisible(has_any && !data_.horizon_context.isEmpty());
    price_canvas_->setVisible(has_price);
    position_canvas_->setVisible(has_position);
    price_unavailable_lbl_->setVisible(has_any && !has_price);
    position_unavailable_lbl_->setVisible(has_any && !has_position);

    if (!has_any) {
        price_canvas_->install_chart(new QChart);
        ui::ChartFactory::apply_theme(price_canvas_->chart());
        position_canvas_->install_chart(new QChart);
        ui::ChartFactory::apply_theme(position_canvas_->chart());
        return;
    }

    price_header_->setText(has_price ? tr("PRICE — %1").arg(data_.price_source) : tr("PRICE — unavailable"));
    position_header_->setText(tr("NET POSITIONING — %1 · contracts").arg(data_.positioning_label));
    price_unavailable_lbl_->setText(tr("Price pane unavailable: %1.").arg(data_.price.unavailable_reason));
    position_unavailable_lbl_->setText(
        tr("Positioning pane unavailable: %1.").arg(data_.positioning.unavailable_reason));
    if (!data_.horizon_context.isEmpty())
        context_lbl_->setText(
            tr("Interpretation context — %1").arg(data_.horizon_context.join(QStringLiteral("  ·  "))));

    QDate first_date;
    QDate last_date;
    auto extend_extent = [&first_date, &last_date](const QVector<ui::TimeSeriesPoint>& points) {
        for (const auto& point : points) {
            if (!point.date.isValid())
                continue;
            if (!first_date.isValid() || point.date < first_date)
                first_date = point.date;
            if (!last_date.isValid() || point.date > last_date)
                last_date = point.date;
        }
    };
    extend_extent(data_.price.points);
    extend_extent(data_.positioning.points);
    const qint64 span_days = first_date.isValid() && last_date.isValid() ? first_date.daysTo(last_date) : 0;
    const QString format = axis_date_format(span_days);

    QChart* price_chart =
        has_price ? make_pane_chart(data_.price.points, QColor(ui::colors::AMBER()), first_date, last_date, format)
                  : new QChart;
    QChart* position_chart =
        has_position ? make_pane_chart(data_.positioning.points, pane_palette_color(data_.positioning_palette_index),
                                       first_date, last_date, format)
                     : new QChart;

    auto vertical_axis = [](QChart* chart) -> QValueAxis* {
        const auto axes = chart->axes(Qt::Vertical);
        return axes.isEmpty() ? nullptr : qobject_cast<QValueAxis*>(axes.first());
    };
    // ChartFactory::apply_theme resets the chart margins, so theme first and
    // then reserve the shared left inset for the value-axis labels. The inset
    // is measured with the canvas font (the font the labels are actually drawn
    // with) and floored generously so a long net-position label is never elided.
    ui::ChartFactory::apply_theme(price_chart);
    ui::ChartFactory::apply_theme(position_chart);
    const QFontMetrics fm(font());
    const int label_width =
        std::max(axis_label_width(vertical_axis(price_chart), fm), axis_label_width(vertical_axis(position_chart), fm));
    const int left_margin = std::max(96, label_width + 20);
    price_chart->setMargins(QMargins(left_margin, 4, 4, 4));
    position_chart->setMargins(QMargins(left_margin, 4, 4, 4));

    price_canvas_->install_chart(price_chart);
    position_canvas_->install_chart(position_chart);
    // One shared snap set for both panes: the union of the report dates carried
    // by the two series. Hovering either pane can therefore reach a report date
    // whose price is a deliberate gap, while the gap itself stays a gap.
    const QVector<QDate> snap_dates = cftc_sync_snap_dates(data_);
    price_canvas_->set_snap_dates(snap_dates);
    position_canvas_->set_snap_dates(snap_dates);

    QTimer::singleShot(0, this, [this]() { align_plot_areas(); });
}

void CftcPricePositioningChart::align_plot_areas() {
    if (!price_canvas_ || !position_canvas_)
        return;
    QChart* price_chart = price_canvas_->chart();
    QChart* position_chart = position_canvas_->chart();
    if (!price_chart || !position_chart)
        return;
    const qreal price_left = price_chart->plotArea().left();
    const qreal position_left = position_chart->plotArea().left();
    if (qFuzzyCompare(price_left + 1.0, position_left + 1.0))
        return;
    if (price_left < position_left) {
        const QMargins margins = price_chart->margins();
        price_chart->setMargins(QMargins(qRound(margins.left() + (position_left - price_left)), margins.top(),
                                         margins.right(), margins.bottom()));
    } else {
        const QMargins margins = position_chart->margins();
        position_chart->setMargins(QMargins(qRound(margins.left() + (price_left - position_left)), margins.top(),
                                            margins.right(), margins.bottom()));
    }
}

void CftcPricePositioningChart::handle_hover(const QDate& date, const QPoint& global_pos) {
    if (!date.isValid())
        return;
    hover_date_ = date;
    price_canvas_->set_crosshair(date, price_canvas_->isVisible());
    position_canvas_->set_crosshair(date, position_canvas_->isVisible());

    QStringList lines;
    lines << date.toString(Qt::ISODate);
    const CftcSyncHoverValue hover = cftc_sync_hover_values(data_, date);
    if (hover.has_price)
        lines << tr("%1  %2").arg(price_header_->text(), pane_value_text(hover.price));
    if (hover.has_positioning)
        lines << tr("%1  %2").arg(data_.positioning_label, pane_value_text(hover.positioning));
    tooltip_->setText(lines.join(QLatin1Char('\n')));
    tooltip_->adjustSize();

    QPoint shown = global_pos + QPoint(12, -tooltip_->height() - 4);
    if (QScreen* screen = QGuiApplication::screenAt(global_pos)) {
        const QRect bounds = screen->availableGeometry();
        if (shown.x() + tooltip_->width() > bounds.right())
            shown.rx() -= tooltip_->width() + 24;
        if (shown.y() < bounds.top())
            shown.ry() = global_pos.y() + 12;
    }
    tooltip_->move(shown);
    tooltip_->show();
    tooltip_->raise();
}

void CftcPricePositioningChart::hide_hover() {
    hover_date_ = {};
    if (price_canvas_)
        price_canvas_->set_crosshair(QDate(), false);
    if (position_canvas_)
        position_canvas_->set_crosshair(QDate(), false);
    if (tooltip_)
        tooltip_->hide();
}

} // namespace fincept::screens
