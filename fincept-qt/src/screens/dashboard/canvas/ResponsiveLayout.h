// src/screens/dashboard/canvas/ResponsiveLayout.h
//
// The dashboard's canonical/responsive layout state, split out of
// DashboardCanvas so the transitions can be tested without a widget (see the
// tests/CMakeLists.txt HARD RULE).
//
// The saved arrangement is canonical. When the canvas is too narrow for it, a
// view is derived from the canonical copy (never from a previous view), and
// widening restores the canonical copy exactly. Geometry edits always run
// against the canonical arrangement: while a view is on screen, begin_edit()
// hands the caller the canonical cells to edit and end_edit() re-derives the
// view, so an edit made in a narrow pane can neither persist the derived view
// nor reinterpret its coordinates at the design width.
#pragma once
#include "screens/dashboard/canvas/GridLayout.h"

namespace fincept::screens {

class ResponsiveLayoutState {
  public:
    /// A freshly loaded arrangement (saved layout or template) becomes canonical.
    void reset(const GridLayout& layout) {
        canonical_ = layout;
        canonical_cols_ = layout.cols > 0 ? layout.cols : 12;
        canonical_.cols = canonical_cols_;
        view_active_ = false;
    }

    bool view_active() const { return view_active_; }
    int canonical_cols() const { return canonical_cols_; }

    /// Display layout for `target_cols`: the canonical arrangement itself at
    /// the design width, a view derived from it otherwise. `current` carries
    /// the runtime state (row height, margins, per-item config) that must
    /// survive the transition.
    GridLayout apply_view(const GridLayout& current, int target_cols) {
        if (target_cols <= 0 || target_cols == current.cols)
            return current;

        if (target_cols == canonical_cols_) {
            view_active_ = false;
            return merge_runtime(canonical_, current);
        }

        if (!view_active_) {
            canonical_ = current;
            view_active_ = true;
        } else {
            // The live layout is a view; carry any per-instance config edited
            // in it (the UI config path only updates the view) into the
            // canonical copy before deriving the next view, or the edit would
            // vanish on a 6 -> 9 / 9 -> 6 transition.
            canonical_ = merge_runtime(canonical_, current);
        }
        GridLayout view = canonical_;
        view.cols = target_cols;
        view.row_h = current.row_h;
        view.margin = current.margin;
        view.items = responsive_items(canonical_.items, target_cols);
        return view;
    }

    /// Base arrangement for a geometry edit. While a view is active this is the
    /// canonical arrangement (with the runtime state from `current`); the view
    /// is re-derived by end_edit(). With no view active the current layout is
    /// returned unchanged.
    GridLayout begin_edit(const GridLayout& current) {
        if (!view_active_)
            return current;
        return merge_runtime(canonical_, current);
    }

    /// `edited` is the arrangement the edit produced, in canonical cells.
    /// Returns the layout to display now, decided from the width at *edit
    /// completion*: the arranged cells at the design width, or a view of them
    /// for `target_cols`. A viewport that changes band mid-gesture therefore
    /// still ends with the right display (the canvas resize timer skips while
    /// a gesture is active, so there may be no later transition to repair it).
    GridLayout end_edit(const GridLayout& edited, int target_cols) {
        canonical_ = edited;
        canonical_.cols = canonical_cols_;

        if (target_cols <= 0 || target_cols >= canonical_cols_) {
            view_active_ = false;
            return canonical_;
        }
        view_active_ = true;
        GridLayout view = canonical_;
        view.cols = target_cols;
        view.items = responsive_items(canonical_.items, target_cols);
        return view;
    }

    /// The arrangement to persist: the canonical cells with the runtime state
    /// (row height, margins, per-item config) currently on screen. Never the
    /// derived view.
    GridLayout for_save(const GridLayout& current) const {
        if (!view_active_)
            return current;
        return merge_runtime(canonical_, current);
    }

  private:
    /// Copy the runtime state (row height, margins, per-item config) from the
    /// live layout onto a canonical base. Cells stay canonical.
    static GridLayout merge_runtime(GridLayout base, const GridLayout& current) {
        base.cols = base.cols > 0 ? base.cols : current.cols;
        base.row_h = current.row_h;
        base.margin = current.margin;
        for (auto& item : base.items) {
            for (const auto& live : current.items) {
                if (live.instance_id == item.instance_id) {
                    item.config = live.config;
                    break;
                }
            }
        }
        return base;
    }

    GridLayout canonical_;
    int canonical_cols_ = 12;
    bool view_active_ = false;
};

} // namespace fincept::screens
