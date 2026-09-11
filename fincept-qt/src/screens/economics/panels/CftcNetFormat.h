// src/screens/economics/panels/CftcNetFormat.h
//
// CFTC net-position text. A positive net gets a "+" prefix, a negative net
// keeps its own "-", and an exact zero is neutral: it renders as "0" with no
// sign. Header-only over Qt Core so the exact-zero rule is unit-testable
// without linking the panel or the widget tree (the same reason QuoteDisplayFormat.h
// exists; see the HARD RULE at the top of tests/CMakeLists.txt).
#pragma once

#include <QString>

namespace fincept::screens {

inline QString cftc_signed_net(double net) {
    return QStringLiteral("%1%2")
        .arg(net > 0 ? QStringLiteral("+") : QString())
        .arg(QString::number(static_cast<qint64>(net)));
}

} // namespace fincept::screens
