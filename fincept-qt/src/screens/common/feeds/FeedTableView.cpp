#include "screens/common/feeds/FeedTableView.h"

#include "network/http/ExternalUrlGuard.h"
#include "ui/tables/DataTable.h"

#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace fincept::feeds {

FeedTableView::FeedTableView(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    table_ = new ui::DataTable(this);
    lay->addWidget(table_);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
        // MarketLab containment (FINCEPT_FORK_PLAN.md §11.2). The row's link
        // comes from a fetched feed item, so the destination is whatever the
        // subscribed feed put there.
        if (r >= 0 && r < items_.size() && !items_[r].link.isEmpty())
            network::ExternalUrlGuard::open_external(QUrl(items_[r].link), this);
    });
}

void FeedTableView::set_items(const QVector<FeedItem>& items) {
    items_ = items;
    QStringList extraCols;
    for (const auto& it : items)
        for (auto k = it.fields.begin(); k != it.fields.end(); ++k)
            if (!extraCols.contains(k.key()))
                extraCols.append(k.key());

    QStringList headers{tr("Time"), tr("Title")};
    headers += extraCols;
    table_->set_headers(headers);

    QVector<QStringList> rows;
    rows.reserve(items.size());
    for (const auto& it : items) {
        QStringList row{it.time, it.title};
        for (const auto& c : extraCols)
            row << it.fields.value(c);
        rows.append(row);
    }
    table_->set_data(rows);
}

} // namespace fincept::feeds
