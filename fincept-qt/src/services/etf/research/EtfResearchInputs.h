// src/services/etf/research/EtfResearchInputs.h
//
// What the research engine computes FROM: stored source observations selected
// for one time frame (as_of, known_at), already reduced to the latest vintage
// of each key that MarketLab had recorded by known_at. Built by
// EtfResearchStore (production) or by tests directly (deterministic fixtures).
//
// Nothing here is derived. A bar without a close does not exist; a volume the
// source did not send is nullopt; a distribution of 0 means the source reported
// no distribution event that session.
#pragma once
#include "services/etf/research/EtfResearchUniverse.h"

#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

namespace fincept::services::etf::research {

struct DailyBar {
    QDate date;
    double close = 0.0;           ///< split-adjusted close, not dividend-adjusted
    std::optional<double> volume; ///< shares; nullopt = not supplied
    double dividend = 0.0;        ///< cash distribution per share with ex-date this session
    double capital_gain = 0.0;    ///< capital-gain distribution per share, ex-date this session
    double split = 0.0;           ///< split ratio effective this session (0 = none)
    bool revised = false;         ///< the selected vintage replaced an earlier different one
};

struct BarSeries {
    QString symbol;
    QVector<DailyBar> bars; ///< ascending by date, unique dates
    QString currency;       ///< from the latest quote-summary capture when known
    bool any_revised = false;
};

/// One manual capture of a fund's quote-summary snapshot.
struct FundCapture {
    QDateTime captured_at;
    QDate effective_session; ///< assigned by the store's named rule (Yahoo does not date AUM/NAV)
    QString effective_rule;
    std::optional<double> total_assets;
    std::optional<double> nav;
    std::optional<double> previous_close;
    std::optional<double> shares_outstanding;
    QDateTime market_time;
    QJsonObject fields; ///< raw Yahoo field names -> values (see EtfResearchFundFields.h)
    qint64 retrieval_id = 0;
};

struct Holding {
    int rank = 0;
    QString symbol;
    QString name;
    std::optional<double> weight; ///< fraction of fund
};

struct HoldingsCapture {
    QDateTime captured_at;
    QVector<Holding> holdings;
    QHash<QString, double> sector_weights; ///< Yahoo sector key -> fraction
};

struct Fundamentals {
    QDateTime captured_at;
    QJsonObject fields;
};

struct MacroPoint {
    QDate date;
    double value = 0.0;
    bool revised = false;
};

struct MacroSeries {
    QString source; ///< fred | world_bank
    QString id;
    QString area;               ///< World Bank ISO2, '' for FRED
    QVector<MacroPoint> points; ///< ascending; World Bank years dated Dec 31
    QString source_updated;
};

/// One month of SEC N-PORT regulatory flow for a universe row (Batch C).
struct MeasuredMonth {
    QDate month;                    ///< first day of the calendar month
    std::optional<double> flow_usd; ///< sales - redemptions + reinvestment
    QString quality;                ///< CONFIRMED | REVISED | MISSING
    QString reason;
    QString reporting_key; ///< cik/series
    QString accession;
    QDateTime available_from;
};

/// Status of one source stage in the most recent manual refresh.
struct SourceStageStatus {
    QString stage;
    QString status; ///< UPDATED | UNCHANGED | PARTIAL | STALE | UNAVAILABLE | FAILED | NOT_CONFIGURED
    QString detail;
    QDateTime requested_at;
    QDateTime retrieved_at;
    QString latest_effective;
    int items_requested = 0;
    int items_ok = 0;
    int rows_inserted = 0;
    int rows_revised = 0;
    int rows_confirmed = 0;
    int stale_items = 0;
    QStringList failed_subjects;
    QStringList dependent_calculations;
};

/// One Batch D group-month of measured SEC regulatory flow (read from the
/// finalized etf_group_analytics_v2 service, unchanged).
struct GroupFlowRow {
    QString level;
    QString group_id;
    QDate month;
    std::optional<double> observed_net_flow_usd;
    std::optional<double> complete_net_flow_usd;
    QString quality;
    int unique_reporting_identities = 0;
    int observed_reporting_identities = 0;
    int unresolved_subjects = 0;
    int excluded_subjects = 0;
    QString taxonomy_version;
};

struct ResearchInputs {
    ResearchUniverse universe;
    QDateTime as_of;           ///< decision time (UTC)
    QDateTime known_at;        ///< knowledge cutoff (UTC)
    QDate expected_us_session; ///< latest NYSE session completed by as_of
    QHash<QString, BarSeries> bars;
    QHash<QString, QVector<FundCapture>> funds;                ///< ascending by captured_at
    QHash<QString, HoldingsCapture> holdings;                  ///< latest capture per parent
    QHash<QString, QVector<HoldingsCapture>> holdings_history; ///< every capture, ascending
    QHash<QString, Fundamentals> fundamentals;                 ///< latest capture per constituent
    QHash<QString, MacroSeries> fred;                          ///< series id ->
    QHash<QString, QHash<QString, MacroSeries>> world_bank;    ///< indicator -> iso2 ->
    QHash<QString, QVector<MeasuredMonth>> measured;           ///< universe symbol -> months
    QHash<QString, QVector<QPair<QDate, double>>> ibkr_close;  ///< universe symbol -> stored IBKR closes
    QVector<GroupFlowRow> group_flows;
    QVector<SourceStageStatus> last_refresh;
    QString last_refresh_run_id;
    QDateTime last_refresh_finished;
    QStringList load_warnings;
};

} // namespace fincept::services::etf::research
