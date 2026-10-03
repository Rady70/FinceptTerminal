// src/screens/etf_research/EtfResearchFormat.h
//
// Presentation helpers for the ETF Flow & Sector Rotation workspace. Every
// value is rendered with its sign and unit, so state never depends on colour
// alone; colours come from the active theme tokens.
#pragma once
#include "services/etf/research/EtfResearchModel.h"
#include "ui/theme/ThemeManager.h"

#include <QColor>
#include <QCoreApplication>
#include <QString>

#include <cmath>

namespace fincept::screens::etfr {

using services::etf::research::Credibility;
using services::etf::research::EvidenceClass;
using services::etf::research::ResearchValue;

inline QString tr_(const char* s) {
    return QCoreApplication::translate("fincept::screens::EtfResearchScreen", s);
}

inline QColor token(const char* ui::ThemeTokens::* field) {
    return QColor(QLatin1String(ui::ThemeManager::instance().tokens().*field));
}

inline QString na() {
    return QStringLiteral("—");
}

inline QString fmt_num(double v, int digits) {
    return QString::number(v, 'f', digits);
}

inline QString fmt_signed(double v, int digits, const QString& suffix = QString()) {
    const QString s = QString::number(std::abs(v), 'f', digits);
    if (v > 0 && QString::number(v, 'f', digits) != QString::number(0.0, 'f', digits))
        return QLatin1Char('+') + s + suffix;
    if (v < 0 && QString::number(-v, 'f', digits) != QString::number(0.0, 'f', digits))
        return QChar(0x2212) + s + suffix; // true minus sign
    return QString::number(0.0, 'f', digits) + suffix;
}

/// Compact USD: +1.23B, −456M, +12.3K, 0.
inline QString fmt_usd(double v, bool sign = true) {
    const double a = std::abs(v);
    QString body;
    if (a >= 1e12)
        body = QString::number(a / 1e12, 'f', 2) + QLatin1Char('T');
    else if (a >= 1e9)
        body = QString::number(a / 1e9, 'f', 2) + QLatin1Char('B');
    else if (a >= 1e6)
        body = QString::number(a / 1e6, 'f', 1) + QLatin1Char('M');
    else if (a >= 1e3)
        body = QString::number(a / 1e3, 'f', 1) + QLatin1Char('K');
    else
        body = QString::number(a, 'f', 0);
    if (!sign)
        return QLatin1Char('$') + body;
    if (v > 0)
        return QStringLiteral("+$") + body;
    if (v < 0)
        return QString(QChar(0x2212)) + QLatin1Char('$') + body;
    return QStringLiteral("$0");
}

/// A research value as table text, by its units.
inline QString fmt_value(const ResearchValue& v, int digits = 2) {
    if (!v.label.isEmpty() && !v.value)
        return v.label;
    if (!v.value)
        return na();
    const double x = *v.value;
    const QString u = v.units;
    if (u == QLatin1String("pct"))
        return fmt_signed(x, digits, QStringLiteral("%"));
    if (u == QLatin1String("pp"))
        return fmt_signed(x, digits, QStringLiteral("pp"));
    if (u == QLatin1String("bp"))
        return fmt_signed(x, 0, QStringLiteral("bp"));
    if (u == QLatin1String("z"))
        return fmt_signed(x, 2);
    if (u == QLatin1String("usd"))
        return fmt_usd(x);
    if (u == QLatin1String("probability") || u == QLatin1String("fraction"))
        return QString::number(x * 100.0, 'f', 0) + QLatin1Char('%');
    if (u == QLatin1String("shares"))
        return QString::number(x / 1e6, 'f', 2) + QStringLiteral("M");
    return fmt_num(x, digits);
}

inline QColor evidence_color(EvidenceClass e) {
    switch (e) {
        case EvidenceClass::Measured:
            return token(&ui::ThemeTokens::info);
        case EvidenceClass::Estimated:
            return token(&ui::ThemeTokens::warning);
        case EvidenceClass::Proxy:
            return token(&ui::ThemeTokens::cyan);
        case EvidenceClass::Model:
            return QColor(0xb0, 0x8c, 0xe8);
        case EvidenceClass::Unavailable:
            return token(&ui::ThemeTokens::text_tertiary);
    }
    return token(&ui::ThemeTokens::text_tertiary);
}

inline QColor credibility_color(Credibility c) {
    switch (c) {
        case Credibility::High:
            return token(&ui::ThemeTokens::positive);
        case Credibility::Medium:
            return token(&ui::ThemeTokens::text_primary);
        case Credibility::Low:
            return token(&ui::ThemeTokens::warning);
        case Credibility::Experimental:
            return token(&ui::ThemeTokens::negative);
        case Credibility::NotGraded:
            return token(&ui::ThemeTokens::text_secondary);
    }
    return token(&ui::ThemeTokens::text_secondary);
}

/// Diverging heat colour for a signed value with a symmetric range; neutral
/// near zero, never fully saturated so overlaid text stays readable.
inline QColor heat_color(double v, double range) {
    const QColor pos = token(&ui::ThemeTokens::positive);
    const QColor neg = token(&ui::ThemeTokens::negative);
    const QColor base = token(&ui::ThemeTokens::bg_raised);
    const double t = range > 0 ? std::clamp(std::abs(v) / range, 0.0, 1.0) : 0.0;
    const QColor tgt = v >= 0 ? pos : neg;
    const double k = 0.15 + 0.65 * t;
    return QColor::fromRgbF(static_cast<float>(base.redF() + (tgt.redF() - base.redF()) * k),
                            static_cast<float>(base.greenF() + (tgt.greenF() - base.greenF()) * k),
                            static_cast<float>(base.blueF() + (tgt.blueF() - base.blueF()) * k));
}

inline QColor value_color(const ResearchValue& v) {
    if (!v.value)
        return token(&ui::ThemeTokens::text_tertiary);
    if (*v.value > 0)
        return token(&ui::ThemeTokens::positive);
    if (*v.value < 0)
        return token(&ui::ThemeTokens::negative);
    return token(&ui::ThemeTokens::text_primary);
}

inline QColor quadrant_color(const QString& q) {
    if (q == QLatin1String("Leading"))
        return token(&ui::ThemeTokens::positive);
    if (q == QLatin1String("Improving"))
        return token(&ui::ThemeTokens::info);
    if (q == QLatin1String("Weakening"))
        return token(&ui::ThemeTokens::warning);
    if (q == QLatin1String("Lagging"))
        return token(&ui::ThemeTokens::negative);
    return token(&ui::ThemeTokens::text_tertiary);
}

inline QString quadrant_tag(const QString& q) {
    if (q == QLatin1String("Leading"))
        return QStringLiteral("LEAD");
    if (q == QLatin1String("Improving"))
        return QStringLiteral("IMPR");
    if (q == QLatin1String("Weakening"))
        return QStringLiteral("WEAK");
    if (q == QLatin1String("Lagging"))
        return QStringLiteral("LAG");
    return na();
}

/// Human tooltip for a value: evidence, credibility and its reasons, flags,
/// method, source, effective date and, if unavailable, why.
inline QString value_tooltip(const ResearchValue& v, const QString& title = QString()) {
    QStringList lines;
    if (!title.isEmpty())
        lines << QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped());
    lines << tr_("Evidence: %1").arg(QLatin1String(services::etf::research::evidence_id(v.evidence)));
    if (v.credibility != Credibility::NotGraded)
        lines << tr_("Credibility: %1").arg(QLatin1String(services::etf::research::credibility_id(v.credibility)));
    if (!v.usable())
        lines << tr_("Unavailable: %1").arg(v.reason.isEmpty() ? QStringLiteral("no value") : v.reason);
    if (!v.flags.isEmpty())
        lines << tr_("Flags: %1").arg(v.flags.join(QStringLiteral(", ")));
    if (!v.method.isEmpty())
        lines << tr_("Method: %1").arg(v.method);
    if (!v.source.isEmpty())
        lines << tr_("Source: %1").arg(v.source);
    if (v.effective.isValid())
        lines << tr_("Effective: %1").arg(v.effective.toString(Qt::ISODate));
    if (!v.credibility_reasons.isEmpty())
        lines << tr_("Grade: %1").arg(v.credibility_reasons.join(QStringLiteral(" · ")).toHtmlEscaped());
    return lines.join(QStringLiteral("<br/>"));
}

} // namespace fincept::screens::etfr
