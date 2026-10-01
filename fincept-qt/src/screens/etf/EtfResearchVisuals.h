#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

namespace fincept::screens {
// Graphical presentation of emitted backend records. No research aggregation.
class EtfGroupBoard : public QWidget {
    Q_OBJECT
  public:
    explicit EtfGroupBoard(QWidget* parent = nullptr);
    void set_groups(const QJsonArray& groups, const QString& month);
    void set_selected(const QString& group);
    QJsonArray groups() const { return groups_; }
    QJsonObject month_for(const QJsonObject& group) const;
    QRect card_rect(int index) const;
  signals:
    void group_activated(const QString& group);

  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

  private:
    void fit_height();
    QJsonArray groups_;
    QString month_, selected_;
    int focus_index_ = 0;
};

class EtfSessionChart : public QWidget {
    Q_OBJECT
  public:
    explicit EtfSessionChart(QWidget* parent = nullptr);
    void set_sessions(const QJsonArray& sessions);
    QJsonArray sessions() const { return sessions_; }

  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;

  private:
    QJsonArray sessions_;
};
} // namespace fincept::screens
