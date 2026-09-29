// Batch D: read-only, versioned ETF group research over the Batch B/C store.
// Regulatory creation/redemption flow is monthly and is never mixed with
// secondary-market rotation. A group result carries every expected member,
// including those without a usable value or a proven identity link.
#pragma once

#include "core/result/Result.h"
#include "services/etf/EtfRegulatoryFlowAnalytics.h"
#include "services/etf/EtfRotationMeasures.h"

#include <QDate>
#include <QJsonObject>
#include <QString>

namespace fincept::services::etf {

inline constexpr const char* kGroupAnalyticsVersion = "etf_group_analytics_v1";

struct GroupRunRequest {
    DerivedTimeFrame frame;
    QString group_level; ///< complex | category | asset_class
    QString group_id;
    QString expected_taxonomy_version; ///< optional assertion; mismatch is an error
    QDate output_from;                 ///< first calendar month; required
    QDate output_to;                   ///< last calendar month; required
    bool include_leveraged = false;    ///< default long-only; explicit inclusion is labelled
};

/// The closed request is deliberately bounded. No network, write, composite
/// score, assumed ETF identity link, or unspecified date range is implied.
QString group_request_problem(const GroupRunRequest& request);

/// Reads the existing ETF repository and computes results on demand. Nothing
/// derived is persisted. A group's taxonomy membership is selected separately
/// for each effective month/session; the named taxonomy version is recorded.
/// A missing or ambiguous SEC/listed identity link is retained as such.
Result<QJsonObject> run_group_research(const GroupRunRequest& request);

} // namespace fincept::services::etf
