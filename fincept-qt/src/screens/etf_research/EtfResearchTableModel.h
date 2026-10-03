// src/screens/etf_research/EtfResearchTableModel.h
//
// A dense, sortable research table: cells carry their display text (sign and
// unit included), a numeric sort key (absent for a missing value), colours
// and a tooltip. Sorting keeps missing values LAST in both directions, so an
// unavailable value never sorts as a zero.
#pragma once
#include <QAbstractTableModel>
#include <QColor>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QVector>

#include <optional>

namespace fincept::screens::etfr {

struct Cell {
    QString text;
    std::optional<double> sort; ///< numeric sort key; nullopt = missing (sorted last)
    QString sort_text;          ///< used when there is no numeric key
    QColor fg;
    QColor bg;
    QString tooltip;
    Qt::Alignment align = Qt::AlignRight | Qt::AlignVCenter;
    bool bold = false;
};

struct Column {
    QString header;
    QString tooltip;
    int width = 70;
    bool text = false; ///< sorts by text
};

class ResearchTableModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    static constexpr int kSortRole = Qt::UserRole + 1;
    static constexpr int kKeyRole = Qt::UserRole + 2;
    static constexpr int kTagsRole = Qt::UserRole + 3;
    static constexpr int kMissingRole = Qt::UserRole + 4;

    explicit ResearchTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void set_content(const QVector<Column>& columns, const QVector<QVector<Cell>>& rows, const QStringList& keys,
                     const QVector<QStringList>& tags);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    const QVector<Column>& columns() const { return columns_; }
    QString key_at(int row) const { return keys_.value(row); }

  private:
    QVector<Column> columns_;
    QVector<QVector<Cell>> rows_;
    QStringList keys_;
    QVector<QStringList> tags_;
};

class ResearchSortProxy : public QSortFilterProxyModel {
    Q_OBJECT
  public:
    explicit ResearchSortProxy(QObject* parent = nullptr) : QSortFilterProxyModel(parent) {}
    void set_required_tag(const QString& tag);
    void set_search(const QString& text);

  protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
    QString tag_;
    QString search_;
};

} // namespace fincept::screens::etfr
