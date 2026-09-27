// tests/tst_dashboard_layout.cpp
//
// MarketLab: the dashboard's responsive-column view. A narrow pane derives its
// tile view from the saved arrangement (clamped into the narrower grid and
// packed without overlaps) instead of compacting the saved cells in place, so
// widening the pane can restore the saved arrangement exactly. The six-widget
// arrangement pinned here is the one a startup shrink previously scrambled —
// and whose scramble the dashboard's auto-save then persisted.
//
// Header-only over Qt Core ("screens/dashboard/canvas/GridLayout.h"), no app
// sources.

#include "screens/dashboard/canvas/GridLayout.h"

#include <QJsonObject>
#include <QTest>

using namespace fincept::screens;

namespace {

GridItem make_item(const QString& id, int x, int y, int w, int h) {
    GridItem item;
    item.id = id;
    item.instance_id = id + QStringLiteral("-instance");
    item.cell = {x, y, w, h, 2, 3};
    return item;
}

// The saved six-widget arrangement (recovered from the pre-migration settings
// backup): indices | performance | risk_metrics on the top row, news beneath
// them, watchlist and sentiment below.
QVector<GridItem> saved_arrangement() {
    return {
        make_item(QStringLiteral("indices"), 0, 0, 4, 5),      make_item(QStringLiteral("performance"), 4, 0, 4, 5),
        make_item(QStringLiteral("risk_metrics"), 8, 0, 4, 5), make_item(QStringLiteral("news"), 0, 5, 8, 4),
        make_item(QStringLiteral("watchlist"), 0, 9, 6, 4),    make_item(QStringLiteral("sentiment"), 8, 5, 4, 4),
    };
}

GridCell cell_of(const QVector<GridItem>& items, const QString& id) {
    for (const auto& item : items) {
        if (item.id == id)
            return item.cell;
    }
    return {};
}

void assert_within_grid(const QVector<GridItem>& items, int cols) {
    for (const auto& item : items) {
        QVERIFY2(item.cell.x >= 0, qPrintable(item.id + QStringLiteral(" x < 0")));
        QVERIFY2(item.cell.y >= 0, qPrintable(item.id + QStringLiteral(" y < 0")));
        QVERIFY2(item.cell.w >= 1, qPrintable(item.id + QStringLiteral(" w < 1")));
        QVERIFY2(item.cell.w <= cols, qPrintable(item.id + QStringLiteral(" wider than grid")));
        QVERIFY2(item.cell.x + item.cell.w <= cols, qPrintable(item.id + QStringLiteral(" past right edge")));
    }
}

void assert_no_overlap(const QVector<GridItem>& items) {
    for (int a = 0; a < items.size(); ++a) {
        for (int b = a + 1; b < items.size(); ++b) {
            QVERIFY2(!cells_overlap(items[a].cell, items[b].cell),
                     qPrintable(items[a].id + QStringLiteral(" overlaps ") + items[b].id));
        }
    }
}

} // namespace

class TstDashboardLayout : public QObject {
    Q_OBJECT

  private slots:
    void designWidthViewIsTheSavedArrangement() {
        const auto saved = saved_arrangement();
        const auto view = responsive_items(saved, 12);
        QCOMPARE(view.size(), saved.size());
        assert_within_grid(view, 12);
        assert_no_overlap(view);
        for (const auto& item : saved)
            QCOMPARE(cell_of(view, item.id), item.cell);
    }

    void narrowViewKeepsEveryTileInsideTheGrid() {
        const auto saved = saved_arrangement();
        for (int cols : {9, 6}) {
            const auto view = responsive_items(saved, cols);
            QCOMPARE(view.size(), saved.size());
            assert_within_grid(view, cols);
            assert_no_overlap(view);
        }
    }

    void narrowViewPreservesIdentityAndConfig() {
        auto saved = saved_arrangement();
        saved[3].config.insert(QStringLiteral("symbols"), 42);
        const auto view = responsive_items(saved, 9);
        for (const auto& item : saved) {
            bool found = false;
            for (const auto& v : view) {
                if (v.instance_id == item.instance_id) {
                    found = true;
                    QCOMPARE(v.id, item.id);
                    QCOMPARE(v.config, item.config);
                }
            }
            QVERIFY(found);
        }
    }

    void collidingClampIsPackedApart() {
        // Clamping a right-aligned tile into a narrower grid must never leave
        // it sitting on top of the neighbour it landed on.
        QVector<GridItem> items;
        items.append(make_item(QStringLiteral("a"), 4, 0, 4, 5));
        items.append(make_item(QStringLiteral("b"), 8, 0, 4, 5));
        const auto view = responsive_items(items, 9);
        assert_within_grid(view, 9);
        assert_no_overlap(view);
    }
};

QTEST_MAIN(TstDashboardLayout)
#include "tst_dashboard_layout.moc"
