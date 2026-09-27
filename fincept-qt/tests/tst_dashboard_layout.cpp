// tests/tst_dashboard_layout.cpp
//
// MarketLab: the dashboard's canonical/responsive layout state
// (screens/dashboard/canvas/ResponsiveLayout.h over GridLayout.h).
//
// A narrow pane derives its tile view from the saved arrangement (clamped into
// the narrower grid and packed without overlaps) instead of compacting the
// saved cells in place; widening restores the saved arrangement exactly; and a
// geometry edit made while narrow edits the canonical arrangement, so no save
// can persist a compacted or promoted view. The six-widget arrangement pinned
// here is the one a startup shrink previously scrambled — and whose scramble
// the dashboard's auto-save then persisted.
//
// Header-only over Qt Core; no app sources.

#include "screens/dashboard/canvas/GridLayout.h"
#include "screens/dashboard/canvas/ResponsiveLayout.h"

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

GridLayout make_layout(const QVector<GridItem>& items, int cols) {
    GridLayout layout;
    layout.cols = cols;
    layout.items = items;
    return layout;
}

GridCell cell_of(const QVector<GridItem>& items, const QString& id) {
    for (const auto& item : items) {
        if (item.id == id)
            return item.cell;
    }
    return {};
}

QJsonObject config_of(const QVector<GridItem>& items, const QString& id) {
    for (const auto& item : items) {
        if (item.id == id)
            return item.config;
    }
    return {};
}

bool has_item(const QVector<GridItem>& items, const QString& id) {
    for (const auto& item : items) {
        if (item.id == id)
            return true;
    }
    return false;
}

/// Assert the canonical cells of `persisted` match the saved arrangement
/// (same tile ids, same canonical geometry, design column count).
void assert_persisted_is_saved(const GridLayout& persisted, const QVector<GridItem>& saved) {
    QCOMPARE(persisted.cols, 12);
    QCOMPARE(persisted.items.size(), saved.size());
    for (const auto& item : saved) {
        QVERIFY2(has_item(persisted.items, item.id), qPrintable(item.id + QStringLiteral(" missing")));
        QCOMPARE(cell_of(persisted.items, item.id), item.cell);
    }
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
    // ── responsive_items (pure packing) ──────────────────────────────────────

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

    // ── canonical/responsive state machine ───────────────────────────────────

    void narrowSaveKeepsTheCanonicalArrangement() {
        // 12 -> 6 -> save: the persisted arrangement is the saved one, not the
        // derived view. This is the exact path the old code corrupted.
        const auto saved = saved_arrangement();
        ResponsiveLayoutState state;
        state.reset(make_layout(saved, 12));
        QVERIFY(!state.view_active());

        const auto view = state.apply_view(make_layout(saved, 12), 6);
        QCOMPARE(view.cols, 6);
        QVERIFY(state.view_active());

        const auto persisted = state.for_save(view);
        assert_persisted_is_saved(persisted, saved);
    }

    void wideningRestoresTheSavedArrangementExactly() {
        // 12 -> 6 -> 9 -> 12: every view derives from the canonical copy, and
        // the design width restores it byte for byte.
        const auto saved = saved_arrangement();
        ResponsiveLayoutState state;
        state.reset(make_layout(saved, 12));

        const auto view6 = state.apply_view(make_layout(saved, 12), 6);
        assert_within_grid(view6.items, 6);
        assert_no_overlap(view6.items);

        const auto view9 = state.apply_view(view6, 9);
        QCOMPARE(view9.cols, 9);
        assert_within_grid(view9.items, 9);
        assert_no_overlap(view9.items);

        const auto restored = state.apply_view(view9, 12);
        QCOMPARE(restored.cols, 12);
        QVERIFY(!state.view_active());
        for (const auto& item : saved)
            QCOMPARE(cell_of(restored.items, item.id), item.cell);
    }

    void narrowRemoveEditsTheCanonicalArrangement() {
        const auto saved = saved_arrangement();
        ResponsiveLayoutState state;
        state.reset(make_layout(saved, 12));
        const auto view = state.apply_view(make_layout(saved, 12), 6);

        // The edit base is the canonical arrangement, at the design width.
        auto edited = state.begin_edit(view);
        QCOMPARE(edited.cols, 12);
        QCOMPARE(cell_of(edited.items, QStringLiteral("news")), cell_of(saved, QStringLiteral("news")));
        for (int i = 0; i < edited.items.size(); ++i) {
            if (edited.items[i].id == QStringLiteral("indices")) {
                edited.items.removeAt(i);
                break;
            }
        }
        edited.items = compact_vertical(edited.items);

        const auto redisplayed = state.end_edit(edited, 6);
        QCOMPARE(redisplayed.cols, 6);
        QVERIFY(!has_item(redisplayed.items, QStringLiteral("indices")));

        const auto persisted = state.for_save(redisplayed);
        QCOMPARE(persisted.cols, 12);
        QVERIFY(!has_item(persisted.items, QStringLiteral("indices")));
        for (const auto& item : saved) {
            if (item.id == QStringLiteral("indices"))
                continue;
            QCOMPARE(cell_of(persisted.items, item.id), item.cell);
        }
    }

    void narrowAddPlacesTheNewTileInCanonicalSpace() {
        const auto saved = saved_arrangement();
        ResponsiveLayoutState state;
        state.reset(make_layout(saved, 12));
        const auto view = state.apply_view(make_layout(saved, 12), 6);

        auto edited = state.begin_edit(view);
        QCOMPARE(edited.cols, 12);
        const GridCell slot = find_first_fit(edited.items, 4, 4, edited.cols);
        edited.items.append(make_item(QStringLiteral("new_tile"), slot.x, slot.y, 4, 4));
        edited.items = compact_vertical(edited.items);

        const auto redisplayed = state.end_edit(edited, 6);
        QCOMPARE(redisplayed.cols, 6);
        QVERIFY(has_item(redisplayed.items, QStringLiteral("new_tile")));

        const auto persisted = state.for_save(redisplayed);
        QCOMPARE(persisted.cols, 12);
        QCOMPARE(cell_of(persisted.items, QStringLiteral("new_tile")).x, slot.x);
        QCOMPARE(cell_of(persisted.items, QStringLiteral("new_tile")).y, slot.y);
        for (const auto& item : saved)
            QCOMPARE(cell_of(persisted.items, item.id), item.cell);
    }

    void narrowDragEditsTheCanonicalArrangement() {
        const auto saved = saved_arrangement();
        ResponsiveLayoutState state;
        state.reset(make_layout(saved, 12));
        const auto view = state.apply_view(make_layout(saved, 12), 6);
        const GridCell view_news = cell_of(view.items, QStringLiteral("news"));

        auto edited = state.begin_edit(view);
        for (auto& item : edited.items) {
            if (item.id == QStringLiteral("news")) {
                item.cell.x = 4;
                item.cell.y = 10;
                break;
            }
        }

        const auto redisplayed = state.end_edit(edited, 6);
        QCOMPARE(redisplayed.cols, 6);

        const auto persisted = state.for_save(redisplayed);
        QCOMPARE(persisted.cols, 12);
        // The canonical edit, not the derived view's cell.
        QCOMPARE(cell_of(persisted.items, QStringLiteral("news")).x, 4);
        QCOMPARE(cell_of(persisted.items, QStringLiteral("news")).y, 10);
        QVERIFY(view_news.y != 10);

        // And widening restores the edited canonical arrangement.
        const auto restored = state.apply_view(redisplayed, 12);
        QCOMPARE(cell_of(restored.items, QStringLiteral("news")).x, 4);
        QCOMPARE(cell_of(restored.items, QStringLiteral("news")).y, 10);
    }

    void narrowResizeEditsTheCanonicalArrangement() {
        const auto saved = saved_arrangement();
        ResponsiveLayoutState state;
        state.reset(make_layout(saved, 12));
        const auto view = state.apply_view(make_layout(saved, 12), 9);

        auto edited = state.begin_edit(view);
        for (auto& item : edited.items) {
            if (item.id == QStringLiteral("risk_metrics")) {
                item.cell.w = 6;
                item.cell.h = 6;
                item.cell.x = std::min(item.cell.x, edited.cols - item.cell.w);
                break;
            }
        }

        const auto redisplayed = state.end_edit(edited, 9);
        const auto persisted = state.for_save(redisplayed);
        QCOMPARE(persisted.cols, 12);
        QCOMPARE(cell_of(persisted.items, QStringLiteral("risk_metrics")).w, 6);
        QCOMPARE(cell_of(persisted.items, QStringLiteral("risk_metrics")).h, 6);

        const auto restored = state.apply_view(redisplayed, 12);
        QCOMPARE(cell_of(restored.items, QStringLiteral("risk_metrics")).w, 6);
        QCOMPARE(cell_of(restored.items, QStringLiteral("risk_metrics")).h, 6);
    }

    void narrowConfigEditKeepsTheCanonicalCells() {
        const auto saved = saved_arrangement();
        ResponsiveLayoutState state;
        state.reset(make_layout(saved, 12));
        auto view = state.apply_view(make_layout(saved, 12), 6);

        // A config-only change on the view item must not touch the geometry.
        for (auto& item : view.items) {
            if (item.id == QStringLiteral("news"))
                item.config.insert(QStringLiteral("category"), QStringLiteral("markets"));
        }

        const auto persisted = state.for_save(view);
        assert_persisted_is_saved(persisted, saved);
        QCOMPARE(config_of(persisted.items, QStringLiteral("news")).value(QStringLiteral("category")).toString(),
                 QStringLiteral("markets"));

        // Widening keeps the canonical cells and the edited config.
        const auto restored = state.apply_view(view, 12);
        assert_persisted_is_saved(make_layout(restored.items, 12), saved);
        QCOMPARE(config_of(restored.items, QStringLiteral("news")).value(QStringLiteral("category")).toString(),
                 QStringLiteral("markets"));
    }
};

QTEST_MAIN(TstDashboardLayout)
#include "tst_dashboard_layout.moc"
