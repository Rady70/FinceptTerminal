#pragma once

// Painted FedWatch visualizations. None of them draws a connecting line
// between observations: probabilities are cells, bubbles, bars and stacked
// daily columns, and a missing value or missing day is left empty.

#include "screens/economics/panels/FedWatchPalette.h"
#include "screens/economics/panels/FedWatchViewModel.h"

#include <QWidget>

namespace fincept::screens {

class FedWatchChart : public QWidget {
    Q_OBJECT
  public:
    explicit FedWatchChart(QWidget* parent = nullptr);
    /// Tooltip text currently associated with a widget-local position (tests).
    QString hit_text(const QPointF& position) const;

  protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void add_hit(const QRectF& area, const QString& text) { hits_.push_back({area, text}); }
    void clear_hits() { hits_.clear(); }
    QFont chart_font(int pixel_size, bool bold = false) const;

  private:
    QVector<QPair<QRectF, QString>> hits_;
};

/// Meetings x target ranges probability matrix (the FedWatch table).
class FedWatchMatrix : public FedWatchChart {
    Q_OBJECT
  public:
    explicit FedWatchMatrix(QWidget* parent = nullptr);
    void set_workspace(const fedwatch::Workspace& workspace, const QString& selected);
    QVector<QPair<double, double>> columns() const { return columns_; }
    QStringList row_ids() const { return rows_; }
    QRectF row_rect(const QString& meeting) const;
    QSize sizeHint() const override;
  signals:
    void meeting_selected(const QString& meeting);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

  private:
    fedwatch::Workspace workspace_;
    QString selected_;
    QVector<QPair<double, double>> columns_;
    QStringList rows_;
    QVector<QRectF> row_rects_;
    int header_height() const;
    int row_height() const;
};

/// Market-implied path: probability bubbles per target range, expected-rate
/// markers now and as displayed one week earlier, and the current target band.
class FedWatchPathChart : public FedWatchChart {
    Q_OBJECT
  public:
    explicit FedWatchPathChart(QWidget* parent = nullptr);
    void set_workspace(const fedwatch::Workspace& workspace, const QString& selected);
  signals:
    void meeting_selected(const QString& meeting);

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;

  private:
    fedwatch::Workspace workspace_;
    QString selected_;
    QVector<QPair<QRectF, QString>> columns_;
};

/// Selected meeting: target-range probabilities with Investing's displayed
/// previous-day and previous-week values as markers.
class FedWatchBandBars : public FedWatchChart {
    Q_OBJECT
  public:
    explicit FedWatchBandBars(QWidget* parent = nullptr);
    void set_meeting(const fedwatch::Meeting& meeting, fedwatch::Value target_lower);
    int bar_count() const { return meeting_.fed.bands.size(); }

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    fedwatch::Meeting meeting_;
    fedwatch::Value target_lower_;
};

/// Selected meeting: decision outcome probabilities, Fed-side vs Polymarket.
class FedWatchOutcomeBars : public FedWatchChart {
    Q_OBJECT
  public:
    struct Row {
        int bp;
        bool open;
        fedwatch::Value fed, poly, diff;
    };
    explicit FedWatchOutcomeBars(QWidget* parent = nullptr);
    void set_meeting(const fedwatch::Meeting& meeting);
    const QVector<Row>& rows() const { return rows_; }

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QVector<Row> rows_;
    QString fed_state_, poly_state_;
};

/// How one meeting's expectations evolved: one labelled row per outcome (or
/// target range), one cell per day, brightness = probability. A day without
/// an accepted observation is an empty cell; nothing is interpolated.
class FedWatchHeatStrip : public FedWatchChart {
    Q_OBJECT
  public:
    struct Segment {
        QString key, label;
        double value;
    };
    struct Day {
        QDate date;
        QVector<Segment> segments;
        QString note;
    };
    explicit FedWatchHeatStrip(int source, QWidget* parent = nullptr);
    /// `order` lists every row key bottom-to-top (lowest rate or largest cut
    /// first) with its label; rows are drawn highest at the top.
    void set_data(const QVector<Day>& days, const QVector<QPair<QString, QString>>& order, const QDate& first,
                  const QDate& last, const QString& empty_text);
    int day_count() const { return days_.size(); }
    int row_count() const { return order_.size(); }
    QPair<QDate, QDate> domain() const { return {first_, last_}; }
    /// Widget-local centre of one day's cell for a row key (tests, hit checks).
    QPointF cell_center(const QDate& date, const QString& key) const;

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    int source_;
    QVector<Day> days_;
    QVector<QPair<QString, QString>> order_;
    QDate first_, last_;
    QString empty_text_;
    QRectF plot_rect() const;
    static constexpr double kRow = 26;
    static constexpr double kLabel = 130;
    static constexpr double kLatest = 84;
};

} // namespace fincept::screens
