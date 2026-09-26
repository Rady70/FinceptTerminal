// src/screens/economics/panels/CftcSparkline.h
//
// Batch 6: a tiny historical context strip for the markets-requiring-attention
// cards. It draws the principal participant's retained Net %OI trajectory at
// its real report cadence: the line breaks across a missing report or a
// non-weekly gap and never interpolates across it. It owns no analysis and no
// axis chrome; the attention card provides the explanation text.
#pragma once

#include <QDate>
#include <QVector>
#include <QWidget>

namespace fincept::screens {

struct CftcSparkPoint {
    QDate date;
    bool has_value = false;
    double value = 0.0;
};

class CftcSparkline : public QWidget {
    Q_OBJECT
  public:
    explicit CftcSparkline(QWidget* parent = nullptr);

    void set_points(const QVector<CftcSparkPoint>& points);
    void refresh_theme();
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QVector<CftcSparkPoint> points_;
};

} // namespace fincept::screens
