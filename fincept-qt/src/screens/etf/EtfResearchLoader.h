#pragma once
#include "services/etf/EtfGroupAnalytics.h"

namespace fincept::screens::etf_ui {

// Existing JSON contracts are retained; this is orchestration, not a data model.
Result<QJsonObject> load_groups(const services::etf::GroupRunRequest& request);
Result<QJsonObject> load_subject(const services::etf::GroupRunRequest& request, const QJsonObject& subject);

} // namespace fincept::screens::etf_ui
