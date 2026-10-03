#pragma once

#include <QString>

namespace fincept::services::economics_detail {
// The Python backend owns durable acquisition reuse and observation freshness.
// Generic result caches and DataHub refresh replay must not acquire FedWatch.
inline bool fedwatch_requires_manual_dispatch(const QString& script) {
    return script == QStringLiteral("fedwatch_data.py");
}
} // namespace fincept::services::economics_detail
