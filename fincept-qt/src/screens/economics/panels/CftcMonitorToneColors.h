// src/screens/economics/panels/CftcMonitorToneColors.h
//
// The single mapping from the descriptive attention classes to presentation
// colors for the cross-market monitor. Every color is a theme token, and every
// color-coded element also carries its class label as text, so meaning is never
// conveyed by color alone. This is a category palette: it does not encode
// market direction, value sign or severity.
#pragma once

#include "screens/economics/panels/CftcMonitorVisualModel.h"
#include "ui/theme/Theme.h"

#include <QColor>

namespace fincept::screens {

inline QColor cftc_monitor_tone_color(CftcMonitorTone tone) {
    switch (tone) {
        case CftcMonitorTone::Ordinary:
            return QColor(ui::colors::TEXT_SECONDARY());
        case CftcMonitorTone::DataQuality:
            return QColor(ui::colors::NEGATIVE());
        case CftcMonitorTone::Extreme:
            return QColor(ui::colors::AMBER());
        case CftcMonitorTone::ExtremeTransition:
            return QColor(ui::colors::WARNING());
        case CftcMonitorTone::Repositioning:
            return QColor(ui::colors::CYAN());
        case CftcMonitorTone::OpenInterest:
            return QColor(ui::colors::INFO());
        case CftcMonitorTone::Concentration:
            return QColor(ui::colors::TEXT_SECONDARY());
    }
    return QColor(ui::colors::TEXT_SECONDARY());
}

inline QColor cftc_monitor_status_color(CftcMonitorStatusTone tone) {
    switch (tone) {
        case CftcMonitorStatusTone::Ordinary:
            return QColor(ui::colors::TEXT_SECONDARY());
        case CftcMonitorStatusTone::Warning:
            return QColor(ui::colors::WARNING());
        case CftcMonitorStatusTone::Problem:
            return QColor(ui::colors::NEGATIVE());
    }
    return QColor(ui::colors::TEXT_SECONDARY());
}

/// A compact bordered chip style for class labels, derived from the class
/// color. The alpha variants keep the surface dark while remaining readable.
inline QString cftc_monitor_chip_style(CftcMonitorTone tone) {
    QColor color = cftc_monitor_tone_color(tone);
    QColor background = color;
    background.setAlpha(36);
    QColor border = color;
    border.setAlpha(110);
    return QStringLiteral("background:%1; color:%2; border:1px solid %3; border-radius:2px;"
                          " font-size:9px; font-weight:700; padding:1px 6px;")
        .arg(background.name(QColor::HexArgb), color.name(), border.name(QColor::HexArgb));
}

} // namespace fincept::screens
