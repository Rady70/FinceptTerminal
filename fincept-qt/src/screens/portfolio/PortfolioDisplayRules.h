// src/screens/portfolio/PortfolioDisplayRules.h
//
// Small pure decisions shared by the read-only portfolio views so that
// missing, genuine zero and directional states stay distinct. Header-only over
// plain types so the rules can be unit-tested without linking the widget tree
// (the same reason QuoteDisplayFormat.h exists; see the HARD RULE at the top of
// tests/CMakeLists.txt).
#pragma once

namespace fincept::screens {

/// Trend/colour direction for a portfolio row.
///   1 = up, -1 = down, 0 = flat (a genuine observation), 2 = unavailable.
/// A two-point price history decides first; otherwise an observed day change
/// decides; with neither, the direction is unavailable — never "up".
inline int portfolio_trend_direction(int point_count, double first, double last, bool has_day_change,
                                     double day_change) {
    if (point_count >= 2)
        return last > first ? 1 : (last < first ? -1 : 0);
    if (has_day_change)
        return day_change > 0 ? 1 : (day_change < 0 ? -1 : 0);
    return 2;
}

/// Contribution (share of total P&L) is undefined when its denominator is
/// zero: a portfolio whose gains and losses cancel has no meaningful share.
/// The correct presentation is unavailable, not 0%.
inline bool portfolio_contribution_available(double total_pnl) {
    return total_pnl != 0.0;
}

/// Presence-aware numeric ordering for portfolio columns whose fallback values
/// are not observations: a missing reading sorts after every present one in
/// both ascending and descending order, and never compares by its hidden
/// fallback value.
inline bool portfolio_sort_before(bool has_a, double a, bool has_b, double b, bool ascending) {
    if (has_a != has_b)
        return has_a;
    if (!has_a || a == b)
        return false;
    return ascending ? a < b : a > b;
}

} // namespace fincept::screens
