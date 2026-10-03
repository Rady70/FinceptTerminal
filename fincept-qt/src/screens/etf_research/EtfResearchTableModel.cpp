#include "screens/etf_research/EtfResearchTableModel.h"

#include <QFont>

namespace fincept::screens::etfr {

void ResearchTableModel::set_content(const QVector<Column>& columns, const QVector<QVector<Cell>>& rows,
                                     const QStringList& keys, const QVector<QStringList>& tags) {
    beginResetModel();
    columns_ = columns;
    rows_ = rows;
    keys_ = keys;
    tags_ = tags;
    endResetModel();
}

int ResearchTableModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

int ResearchTableModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(columns_.size());
}

QVariant ResearchTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size() || index.column() >= rows_[index.row()].size())
        return {};
    const Cell& c = rows_[index.row()][index.column()];
    switch (role) {
        case Qt::DisplayRole:
            return c.text;
        case Qt::ToolTipRole:
            return c.tooltip.isEmpty() ? QVariant() : QVariant(c.tooltip);
        case Qt::ForegroundRole:
            return c.fg.isValid() ? QVariant(c.fg) : QVariant();
        case Qt::BackgroundRole:
            return c.bg.isValid() ? QVariant(c.bg) : QVariant();
        case Qt::TextAlignmentRole:
            return QVariant::fromValue(c.align);
        case Qt::FontRole:
            if (c.bold) {
                QFont f;
                f.setBold(true);
                return f;
            }
            return {};
        case kSortRole:
            if (c.sort)
                return *c.sort;
            return c.sort_text.isEmpty() ? QVariant() : QVariant(c.sort_text);
        case kMissingRole:
            return !c.sort && c.sort_text.isEmpty();
        case kKeyRole:
            return keys_.value(index.row());
        case kTagsRole:
            return tags_.value(index.row());
        default:
            return {};
    }
}

QVariant ResearchTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || section < 0 || section >= columns_.size())
        return {};
    if (role == Qt::DisplayRole)
        return columns_[section].header;
    if (role == Qt::ToolTipRole)
        return columns_[section].tooltip.isEmpty() ? QVariant() : QVariant(columns_[section].tooltip);
    return {};
}

void ResearchSortProxy::set_required_tag(const QString& tag) {
    tag_ = tag;
    invalidateFilter();
}

void ResearchSortProxy::set_search(const QString& text) {
    search_ = text.trimmed();
    invalidateFilter();
}

bool ResearchSortProxy::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    const bool lm = sourceModel()->data(left, ResearchTableModel::kMissingRole).toBool();
    const bool rm = sourceModel()->data(right, ResearchTableModel::kMissingRole).toBool();
    if (lm != rm) {
        // Missing sorts last whatever the direction: invert for descending order.
        const bool left_first = !lm;
        return sortOrder() == Qt::AscendingOrder ? left_first : !left_first;
    }
    if (lm && rm)
        return sourceModel()->data(left, ResearchTableModel::kKeyRole).toString() <
               sourceModel()->data(right, ResearchTableModel::kKeyRole).toString();
    const QVariant l = sourceModel()->data(left, ResearchTableModel::kSortRole);
    const QVariant r = sourceModel()->data(right, ResearchTableModel::kSortRole);
    if (l.typeId() == QMetaType::Double && r.typeId() == QMetaType::Double)
        return l.toDouble() < r.toDouble();
    return QString::localeAwareCompare(l.toString(), r.toString()) < 0;
}

bool ResearchSortProxy::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
    const QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
    if (!tag_.isEmpty()) {
        const QStringList tags = sourceModel()->data(idx, ResearchTableModel::kTagsRole).toStringList();
        if (!tags.contains(tag_))
            return false;
    }
    if (!search_.isEmpty()) {
        const int cols = sourceModel()->columnCount(source_parent);
        for (int c = 0; c < std::min(cols, 3); ++c)
            if (sourceModel()
                    ->data(sourceModel()->index(source_row, c, source_parent), Qt::DisplayRole)
                    .toString()
                    .contains(search_, Qt::CaseInsensitive))
                return true;
        const QStringList tags = sourceModel()->data(idx, ResearchTableModel::kTagsRole).toStringList();
        for (const QString& t : tags)
            if (t.contains(search_, Qt::CaseInsensitive))
                return true;
        return false;
    }
    return true;
}

} // namespace fincept::screens::etfr
