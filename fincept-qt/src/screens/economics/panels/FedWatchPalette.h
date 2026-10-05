#pragma once

// FedWatch chart colours. Every value comes from the active theme tokens:
// source identity uses chart slots 0 (Fed-side) and 1 (Polymarket), and
// probability heat is a sequential scale in the source's own hue.

#include "ui/theme/Theme.h"

#include <QColor>

#include <algorithm>
#include <cmath>

namespace fincept::screens::fedwatch {

// ── Colour (all from theme tokens) ───────────────────────────────────────────
inline QColor mix(const QColor& from, const QColor& to, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(float(from.redF() + (to.redF() - from.redF()) * t),
                            float(from.greenF() + (to.greenF() - from.greenF()) * t),
                            float(from.blueF() + (to.blueF() - from.blueF()) * t));
}
// Source identity: Fed-side = chart slot 0 (accent), Polymarket = chart slot 1.
inline QColor source_color(int source) {
    const auto& colors = ui::ThemeManager::instance().tokens().chart_colors;
    return QColor(colors[source == 0 ? 0 : 1]);
}
// Sequential probability heat on the surface, in the source's hue.
inline QColor heat(double pct, int source = 0) {
    const QColor surface(ui::colors::BG_RAISED());
    const QColor accent = source_color(source);
    if (pct <= 0)
        return surface;
    // Dark -> source hue -> light tint, so a dominant outcome clearly outshines
    // a minority one (the source hue alone is too dim on the dark surface).
    const double t = std::sqrt(std::clamp(pct, 0.0, 100.0) / 100.0);
    if (t <= 0.6)
        return mix(surface, accent, 0.12 + 0.88 * t / 0.6);
    return mix(accent, QColor(ui::colors::TEXT_PRIMARY()), 0.65 * (t - 0.6) / 0.4);
}
inline QColor ink_on(const QColor& background) {
    const double luminance = 0.2126 * background.redF() + 0.7152 * background.greenF() + 0.0722 * background.blueF();
    return luminance > 0.45 ? QColor(ui::colors::BG_BASE()) : QColor(ui::colors::TEXT_PRIMARY());
}

} // namespace fincept::screens::fedwatch
