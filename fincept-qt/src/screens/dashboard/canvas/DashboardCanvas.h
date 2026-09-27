#pragma once
#include "screens/dashboard/canvas/GridLayout.h"
#include "screens/dashboard/canvas/PlaceholderOverlay.h"
#include "screens/dashboard/canvas/WidgetTile.h"
#include "ui/theme/ThemeTokens.h"

#include <QTimer>
#include <QWidget>

namespace fincept::screens {

/// Absolute-positioned canvas — react-grid-layout port.
/// 12 columns, drag from title bar, resize from bottom-right, vertical compaction.
class DashboardCanvas : public QWidget {
    Q_OBJECT
  public:
    explicit DashboardCanvas(QWidget* parent = nullptr);
    ~DashboardCanvas() override;

    void load_layout(const GridLayout& layout);
    GridLayout current_layout() const { return layout_; }
    /// The arrangement to persist. While a narrow responsive view is active
    /// this is the saved canonical arrangement (current row height, margins
    /// and per-instance config), never the compacted view on screen.
    GridLayout canonical_layout() const;
    void apply_template(const QString& template_id);
    void add_widget(const QString& widget_type_id);
    void remove_widget(const QString& instance_id);
    void set_row_height(int px);

    int tile_count() const { return tiles_.size(); }

  signals:
    void layout_changed(GridLayout layout);
    void widget_count_changed(int count);

  protected:
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void showEvent(QShowEvent* e) override;

  private slots:
    void on_drag_started(WidgetTile* tile, QPoint canvas_pos);
    void on_drag_moved(WidgetTile* tile, QPoint canvas_pos);
    void on_drag_released(WidgetTile* tile, QPoint canvas_pos);
    void on_resize_started(WidgetTile* tile, QPoint canvas_pos);
    void on_resize_moved(WidgetTile* tile, QPoint canvas_pos);
    void on_resize_released(WidgetTile* tile);
    void on_tile_close(WidgetTile* tile);
    void on_scroll_tick();

  private:
    void apply_bg();
    void reflow_tiles(bool animate = false);
    void update_canvas_height();
    void update_placeholder(const GridCell& cell);
    void hide_placeholder();
    void rebuild_grid_cache();
    void connect_tile(WidgetTile* tile);
    WidgetTile* tile_for_id(const QString& instance_id) const;
    GridItem* item_for_id(const QString& instance_id);

    /// Switch the rendered grid to `target_cols`. Narrowing keeps a copy of
    /// the canonical arrangement and renders a derived view; widening back to
    /// canonical_cols_ restores that copy exactly.
    void apply_responsive_cols(int target_cols);
    /// Drop the remembered canonical arrangement — a user edit supersedes it.
    void forget_canonical_layout();

    GridLayout layout_;
    QVector<WidgetTile*> tiles_;
    PlaceholderOverlay* placeholder_ = nullptr;

    // Drag state
    WidgetTile* dragging_tile_ = nullptr;
    QPoint drag_offset_;
    GridCell ghost_cell_;
    GridLayout pre_drag_layout_;

    // Resize state
    WidgetTile* resizing_tile_ = nullptr;
    GridCell resize_origin_cell_;
    GridLayout pre_resize_layout_;

    // Auto-scroll during drag
    QTimer* scroll_timer_ = nullptr;
    QPoint last_drag_canvas_pos_;

    // Debounce rapid resize events (e.g. ADS splitter drag).
    // Column recalculation and compaction only happen after the timer fires,
    // preventing layout destruction during transient width changes.
    QTimer* resize_timer_ = nullptr;
    int pending_resize_w_ = 0;

    // The column count the user last explicitly set (via load/apply/drag).
    // Responsive shrink is allowed, but we restore this when width permits.
    int canonical_cols_ = 12;

    // Snapshot of the saved arrangement taken before the first responsive
    // shrink. It is the source of every narrow view, so widening the pane
    // restores the user's layout instead of persisting a compacted one.
    GridLayout canonical_layout_;
    bool canonical_saved_ = false;

    fincept::ui::ThemeTokens tokens_{};
};

} // namespace fincept::screens
