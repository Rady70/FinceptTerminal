// tests/cftc_validation_harness.cpp
//
// Batch 3 CFTC historical-validation harness (research tool, not a CTest test).
//
// It reads the locally acquired validation data set produced by
// marketlab/cftc_validation_acquire.py (CFTC histories, Socrata publication
// metadata, retained free price histories), replays every market/family through
// the one Batch 2 research-state engine via CftcHistoricalReplay.h, and writes
// the validation evidence as CSV files.
//
// It implements no research rules and no outcome substitution: every state is
// the engine's, every outcome is measured from actual price observations after
// the predeclared effective date, and every exclusion carries its reason.
//
// Build:  cmake --preset win-dev -DFINCEPT_BUILD_TESTS=ON && cmake --build --preset win-dev
// Run:    build/win-dev/tests/cftc_validation_harness.exe --data <acquired-data> --out <evidence-dir>
//
// Usage:  --data <dir>   acquired data directory (required)
//         --out <dir>    output directory for evidence CSVs (required)
//         --phase <p>    development | holdout | all (default all)
//         --markets <k,...>  optional subset of market keys
//         --strip-participant-legs
//                        diagnostic only: drop non-speculative participant legs
//                        so the participant family is unavailable (used once in
//                        Batch 3 to test whether that family helps or hurts;
//                        it changes input availability, never a research rule)
//
// Out-of-Git by design: the acquired histories and these full evidence CSVs are
// runtime artifacts; only the compact validation record is committed.

#include "services/economics/CftcHistoricalReplay.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace fincept::services;

namespace {

struct AcquiredPrices {
    QString market_key;
    QString symbol;
    QString proxy_type;
    QString source;
    QVector<CftcPricePoint> points;
    QString file_name;
    bool ok = false;
    QString error;
};

struct StateRow {
    QString market_key;
    QString label;
    QString asset_class;
    QString family_code;
    QString proxy_type;
    QString proxy_scope;
    QString phase;
    QDate report_date;
    QDate effective_date;
    QString state;
    QString confidence;
    QString tactical;
    QString swing;
    QString regime;
    QString context;
    bool price_used = false;
    bool core_opposed = false;
    bool conflict_price = false;
    bool conflict_oi = false;
    bool conflict_participant = false;
    bool conflict_historical = false;
    QString status_4w;
    QString status_13w;
    double net_change_4w = 0.0;
    double net_change_13w = 0.0;
    double net_pct_oi_change_4w = 0.0;
    double price_change_4w = 0.0;
    double price_change_13w = 0.0;
    double open_interest_change_4w = 0.0;
    double return_4w = 0.0;
    bool valid_4w = false;
    double return_13w = 0.0;
    bool valid_13w = false;
    QSet<QString> available_families;
    QStringList supporting_families;
};

struct Dist {
    int n = 0;
    double mean = 0.0;
    double median = 0.0;
    double positive = 0.0;
    double negative = 0.0;
};

Dist distribution(QVector<double> values) {
    Dist dist;
    dist.n = values.size();
    if (values.isEmpty())
        return dist;
    std::stable_sort(values.begin(), values.end());
    double sum = 0.0;
    int positive = 0;
    int negative = 0;
    for (double value : values) {
        sum += value;
        if (value > 0.0)
            ++positive;
        if (value < 0.0)
            ++negative;
    }
    dist.mean = sum / static_cast<double>(values.size());
    const int count = values.size();
    dist.median = (count % 2 == 1) ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2.0;
    dist.positive = static_cast<double>(positive) / static_cast<double>(count);
    dist.negative = static_cast<double>(negative) / static_cast<double>(count);
    return dist;
}

double median_of(QVector<double> values) {
    if (values.isEmpty())
        return 0.0;
    std::stable_sort(values.begin(), values.end());
    const int count = values.size();
    return (count % 2 == 1) ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

QString csv_field(const QString& value) {
    if (value.contains(QLatin1Char(',')) || value.contains(QLatin1Char('"')) || value.contains(QLatin1Char('\n'))) {
        QString escaped = value;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"%1\"").arg(escaped);
    }
    return value;
}

bool write_csv(const QString& path, const QStringList& headers, const QVector<QStringList>& rows) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream stream(&file);
    QStringList header_line;
    for (const auto& header : headers)
        header_line << csv_field(header);
    stream << header_line.join(QLatin1Char(',')) << '\n';
    for (const auto& row : rows) {
        QStringList fields;
        for (const auto& field : row)
            fields << csv_field(field);
        stream << fields.join(QLatin1Char(',')) << '\n';
    }
    return true;
}

QString number(double value) {
    if (!std::isfinite(value))
        return QString();
    return QString::number(value, 'f', 6);
}

QString state_word(CftcResearchState state) {
    switch (state) {
        case CftcResearchState::Buy:
            return QStringLiteral("BUY");
        case CftcResearchState::Sell:
            return QStringLiteral("SELL");
        case CftcResearchState::Hold:
            break;
    }
    return QStringLiteral("HOLD");
}

QString confidence_word(CftcResearchConfidence confidence) {
    switch (confidence) {
        case CftcResearchConfidence::High:
            return QStringLiteral("High");
        case CftcResearchConfidence::Medium:
            return QStringLiteral("Medium");
        case CftcResearchConfidence::Low:
            break;
    }
    return QStringLiteral("Low");
}

QString tactical_word(CftcTacticalState state) {
    switch (state) {
        case CftcTacticalState::Bullish:
            return QStringLiteral("Bullish");
        case CftcTacticalState::Bearish:
            return QStringLiteral("Bearish");
        case CftcTacticalState::Neutral:
            return QStringLiteral("Neutral");
        case CftcTacticalState::Unavailable:
            break;
    }
    return QStringLiteral("Unavailable");
}

QString context_word(CftcHistoricalContext context) {
    switch (context) {
        case CftcHistoricalContext::CrowdedLong:
            return QStringLiteral("CrowdedLong");
        case CftcHistoricalContext::CrowdedShort:
            return QStringLiteral("CrowdedShort");
        case CftcHistoricalContext::Neutral:
            return QStringLiteral("Neutral");
        case CftcHistoricalContext::Mixed:
            return QStringLiteral("Mixed");
        case CftcHistoricalContext::Unavailable:
            break;
    }
    return QStringLiteral("Unavailable");
}

QDate parse_iso_date(const QString& value) {
    return QDate::fromString(value, Qt::ISODate);
}

bool parse_json_file(const QString& path, QJsonObject& root, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    QJsonParseError parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("unreadable JSON in %1: %2").arg(path, parse_error.errorString());
        return false;
    }
    root = document.object();
    return true;
}

CftcFamily family_from_code(const QString& code) {
    if (code == QStringLiteral("disaggregated"))
        return CftcFamily::Disaggregated;
    if (code == QStringLiteral("tff"))
        return CftcFamily::Tff;
    return CftcFamily::Legacy;
}

bool conflict_in_group(const CftcResearchResult& result, const QString& family_code) {
    for (const auto& group : result.groups) {
        if (cftc_evidence_family_code(group.family) != family_code)
            continue;
        for (const auto& item : group.items) {
            if (item.available && item.conflicted)
                return true;
        }
    }
    return false;
}

void collect_available_families(const CftcResearchResult& result, QSet<QString>& families) {
    for (const auto& group : result.groups) {
        if (group.counts_for_confidence && group.available)
            families.insert(cftc_evidence_family_code(group.family));
    }
}

/// Families that actually satisfied the v0 independence gate for this state:
/// available, independence-eligible, and pointing the same way as the emitted
/// state (BUY -> bullish, SELL -> bearish, HOLD -> the provisional candidate).
QStringList supporting_families(const CftcResearchResult& result) {
    QStringList families;
    const CftcEvidenceDirection wanted =
        result.state == CftcResearchState::Buy
            ? CftcEvidenceDirection::Bullish
            : (result.state == CftcResearchState::Sell ? CftcEvidenceDirection::Bearish
                                                       : CftcEvidenceDirection::Unavailable);
    if (wanted == CftcEvidenceDirection::Unavailable)
        return families;
    for (const auto& group : result.groups) {
        if (!group.counts_for_independence || !group.available || group.direction != wanted)
            continue;
        families << cftc_evidence_family_code(group.family);
    }
    families.sort();
    return families;
}

struct AggregateBucket {
    QVector<double> returns_4w;
    QVector<double> returns_13w;
};

QString horizon_status(const CftcForwardOutcome& outcome) {
    return outcome.valid ? QStringLiteral("valid") : QStringLiteral("invalid: %1").arg(outcome.invalid_reason);
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cftc_validation_harness"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Batch 3 CFTC historical-validation harness (research tool)"));
    parser.addHelpOption();
    QCommandLineOption data_option(QStringLiteral("data"), QStringLiteral("Acquired data directory"), QStringLiteral("dir"));
    QCommandLineOption out_option(QStringLiteral("out"), QStringLiteral("Evidence output directory"), QStringLiteral("dir"));
    QCommandLineOption phase_option(QStringLiteral("phase"), QStringLiteral("development | holdout | all"),
                                    QStringLiteral("phase"), QStringLiteral("all"));
    QCommandLineOption markets_option(QStringLiteral("markets"), QStringLiteral("Optional market keys (comma separated)"),
                                      QStringLiteral("keys"));
    QCommandLineOption strip_option(
        QStringLiteral("strip-participant-legs"),
        QStringLiteral("Diagnostic only: drop non-speculative participant legs so the participant family is unavailable"));
    parser.addOption(data_option);
    parser.addOption(out_option);
    parser.addOption(phase_option);
    parser.addOption(markets_option);
    parser.addOption(strip_option);
    parser.process(app);

    const QString data_dir = parser.value(data_option);
    const QString out_dir = parser.value(out_option);
    const QString phase_filter = parser.value(phase_option).toLower();
    if (data_dir.isEmpty() || out_dir.isEmpty()) {
        QTextStream err(stderr);
        err << "Both --data and --out are required.\n";
        return 2;
    }
    if (phase_filter != QStringLiteral("development") && phase_filter != QStringLiteral("holdout") &&
        phase_filter != QStringLiteral("all")) {
        QTextStream err(stderr);
        err << "--phase must be development, holdout or all.\n";
        return 2;
    }
    QSet<QString> market_filter;
    if (parser.isSet(markets_option)) {
        for (const auto& key : parser.value(markets_option).split(QLatin1Char(','), Qt::SkipEmptyParts))
            market_filter.insert(key.trimmed());
    }
    const bool strip_participant_legs = parser.isSet(strip_option);

    const QDir cftc_dir(QDir(data_dir).filePath(QStringLiteral("cftc")));
    const QDir prices_dir(QDir(data_dir).filePath(QStringLiteral("prices")));
    QDir().mkpath(out_dir);
    const CftcReplayOptions options;

    // ── Load price series ───────────────────────────────────────────────────
    QHash<QString, AcquiredPrices> prices_by_market;
    QStringList price_issues;
    for (const auto& info :
         prices_dir.entryInfoList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name)) {
        QJsonObject root;
        QString error;
        if (!parse_json_file(info.absoluteFilePath(), root, error)) {
            price_issues << error;
            continue;
        }
        AcquiredPrices prices;
        prices.market_key = root.value(QStringLiteral("market_key")).toString();
        prices.symbol = root.value(QStringLiteral("symbol")).toString();
        prices.proxy_type = root.value(QStringLiteral("proxy_type")).toString();
        prices.source = root.value(QStringLiteral("source")).toString();
        prices.file_name = info.fileName();
        prices.ok = root.value(QStringLiteral("success")).toBool(false);
        prices.error = root.value(QStringLiteral("error")).toString();
        const QJsonArray points = root.value(QStringLiteral("points")).toArray();
        for (const auto& value : points) {
            const QJsonObject point = value.toObject();
            const QDate date = parse_iso_date(point.value(QStringLiteral("date")).toString());
            const double close = point.value(QStringLiteral("close")).toDouble(std::nan(""));
            if (!date.isValid() || !std::isfinite(close) || close <= 0.0)
                continue;
            prices.points.append({date, close});
        }
        std::stable_sort(prices.points.begin(), prices.points.end(),
                         [](const CftcPricePoint& left, const CftcPricePoint& right) { return left.date < right.date; });
        if (prices.market_key.isEmpty())
            prices.market_key = info.completeBaseName();
        if (!prices.points.isEmpty())
            prices.ok = true;
        prices_by_market.insert(prices.market_key, prices);
    }

    // ── Load CFTC histories and replay ──────────────────────────────────────
    QVector<StateRow> eligible_rows;
    QVector<QStringList> market_rows;
    QVector<QStringList> quality_rows;
    QVector<QStringList> exclusion_rows;
    QHash<QString, int> exclusion_counts; // key: market|family|reason
    QHash<QString, AggregateBucket> confidence_buckets; // phase|confidence
    QHash<QString, AggregateBucket> context_buckets;    // phase|context|tactical|swing|family
    QHash<QString, AggregateBucket> conflict_buckets;   // phase|dimension|flag|family

    const auto add_exclusion = [&](const QString& market, const QString& family, const QString& reason) {
        const QString key = market + QLatin1Char('|') + family + QLatin1Char('|') + reason;
        exclusion_counts[key] = exclusion_counts.value(key) + 1;
    };

    int markets_replayed = 0;
    int eligible_total = 0;
    int candidate_total = 0;

    for (const auto& info : cftc_dir.entryInfoList(QStringList() << QStringLiteral("*.json"), QDir::Files, QDir::Name)) {
        QJsonObject root;
        QString error;
        if (!parse_json_file(info.absoluteFilePath(), root, error)) {
            add_exclusion(info.completeBaseName(), QStringLiteral("-"), error);
            continue;
        }
        const QJsonObject validation = root.value(QStringLiteral("validation")).toObject();
        const QString market_key = validation.value(QStringLiteral("market_key")).toString(info.completeBaseName());
        const QString family_code = validation.value(QStringLiteral("report_family")).toString(QStringLiteral("legacy"));
        const QString asset_class = validation.value(QStringLiteral("asset_class")).toString();
        const QString label = validation.value(QStringLiteral("label")).toString(market_key);
        if (!market_filter.isEmpty() && !market_filter.contains(market_key))
            continue;
        if (!root.value(QStringLiteral("success")).toBool(false)) {
            add_exclusion(market_key, family_code,
                          root.value(QStringLiteral("error")).toString(QStringLiteral("acquisition failed")));
            continue;
        }
        const auto price_it = prices_by_market.constFind(market_key);
        if (price_it == prices_by_market.constEnd()) {
            add_exclusion(market_key, family_code,
                          QStringLiteral("no retained price series was acquired for this market"));
            continue;
        }
        const AcquiredPrices& prices = price_it.value();
        if (!prices.ok || prices.points.isEmpty()) {
            add_exclusion(market_key, family_code,
                          QStringLiteral("the acquired price series has no usable observations"));
            continue;
        }

        const CftcHistory history =
            cftc_parse_history(root.value(QStringLiteral("data")).toArray(), family_from_code(family_code));
        if (!history.error.isEmpty()) {
            add_exclusion(market_key, family_code, history.error);
            continue;
        }

        CftcReplayMarket market;
        market.family = family_from_code(family_code);
        market.market_key = market_key;
        market.label = label;
        market.asset_class = asset_class;
        market.observations = history.observations;
        market.prices = prices.points;
        market.price_source = prices.source;
        market.price_continuous_proxy = prices.proxy_type == QStringLiteral("continuous_front_month");
        market.price_spot_index = prices.proxy_type == QStringLiteral("spot_index");

        if (strip_participant_legs) {
            // Diagnostic input variant only: removing non-speculative legs makes
            // the participant confirmation family unavailable without changing
            // any research rule. It answers "does that family help or hurt?"
            const QVector<CftcParticipant> participants = cftc_family_participants(market.family);
            const int speculative_index = cftc_speculative_index(participants);
            for (auto& observation : market.observations) {
                for (int i = 0; i < observation.longs.size(); ++i) {
                    if (i == speculative_index)
                        continue;
                    observation.longs[i] = std::nullopt;
                    observation.shorts[i] = std::nullopt;
                }
            }
        }

        const QJsonObject publication = root.value(QStringLiteral("publication")).toObject();
        const QJsonObject created = publication.value(QStringLiteral("created_at_by_report_date")).toObject();
        for (auto it = created.constBegin(); it != created.constEnd(); ++it) {
            const QDate report_date = parse_iso_date(it.key());
            if (!report_date.isValid() || !it.value().isString())
                continue;
            const QString stamp = it.value().toString();
            QDateTime published = QDateTime::fromString(stamp, Qt::ISODateWithMs);
            if (!published.isValid())
                published = QDateTime::fromString(stamp, Qt::ISODate);
            if (published.isValid())
                market.created_at_dates.insert(report_date, published.date());
        }

        const auto replayed = cftc_replay_market(market, options);
        ++markets_replayed;

        struct Bucket {
            QVector<double> all;
            QVector<double> buy;
            QVector<double> sell;
            QVector<double> hold;
            int buy_n = 0;
            int sell_n = 0;
            int hold_n = 0;
            int valid = 0;
        };
        Bucket bucket_4w;
        Bucket bucket_13w;
        int candidate = 0;
        int timing_excluded = 0;
        int unavailable = 0;
        int transitions = 0;
        int previous_state = -1;
        int eligible_market = 0;
        QMap<QString, int> ineligibility_reasons;
        QMap<QString, int> state_counts;
        QMap<QString, int> confidence_counts;
        QMap<QString, int> coverage_counts;

        for (const auto& observation : replayed) {
            const QString phase = observation.development ? QStringLiteral("development") : QStringLiteral("holdout");
            if (phase_filter != QStringLiteral("all") && phase != phase_filter)
                continue;
            if (observation.report_date >= options.evaluation_start)
                ++candidate;
            if (!observation.eligible) {
                if (observation.timing_excluded)
                    ++timing_excluded;
                ++unavailable;
                if (!observation.ineligibility_reason.isEmpty())
                    ineligibility_reasons[observation.ineligibility_reason] += 1;
                continue;
            }
            ++eligible_market;
            StateRow row;
            row.market_key = market_key;
            row.label = label;
            row.asset_class = asset_class;
            row.family_code = family_code;
            row.proxy_type = prices.proxy_type;
            row.proxy_scope = market.price_spot_index ? QStringLiteral("spot_index_reported_separately")
                                                      : QStringLiteral("primary_continuous");
            row.phase = phase;
            row.report_date = observation.report_date;
            row.effective_date = observation.effective_date;
            row.state = state_word(observation.result.state);
            row.confidence = confidence_word(observation.result.confidence);
            row.tactical = tactical_word(observation.result.tactical_4w);
            row.swing = tactical_word(observation.result.swing_13w);
            row.regime = tactical_word(observation.result.regime_26w);
            row.context = context_word(observation.result.historical_context);
            row.price_used = observation.result.price_used;
            row.core_opposed = (observation.result.tactical_4w == CftcTacticalState::Bullish &&
                                observation.result.swing_13w == CftcTacticalState::Bearish) ||
                               (observation.result.tactical_4w == CftcTacticalState::Bearish &&
                                observation.result.swing_13w == CftcTacticalState::Bullish);
            row.conflict_price = conflict_in_group(observation.result, QStringLiteral("price_cot"));
            row.conflict_oi = conflict_in_group(observation.result, QStringLiteral("open_interest"));
            row.conflict_participant = conflict_in_group(observation.result, QStringLiteral("participant"));
            row.conflict_historical = conflict_in_group(observation.result, QStringLiteral("historical_context"));
            collect_available_families(observation.result, row.available_families);
            row.supporting_families = supporting_families(observation.result);
            row.valid_4w = observation.outcome_4w.valid;
            row.return_4w = observation.outcome_4w.return_pct;
            row.valid_13w = observation.outcome_13w.valid;
            row.return_13w = observation.outcome_13w.return_pct;
            row.status_4w = horizon_status(observation.outcome_4w);
            row.status_13w = horizon_status(observation.outcome_13w);
            const CftcStateReadings& readings = observation.result.readings;
            row.net_change_4w = readings.changes_4w.net.has_value ? readings.changes_4w.net.value : 0.0;
            row.net_change_13w = readings.changes_13w.net.has_value ? readings.changes_13w.net.value : 0.0;
            row.net_pct_oi_change_4w =
                readings.changes_4w.net_pct_oi.has_value ? readings.changes_4w.net_pct_oi.value : 0.0;
            row.price_change_4w = readings.price_change_4w_available ? readings.price_change_4w : 0.0;
            row.price_change_13w = readings.price_change_13w_available ? readings.price_change_13w : 0.0;
            row.open_interest_change_4w =
                readings.open_interest_change_4w.has_value ? readings.open_interest_change_4w.value : 0.0;
            eligible_rows.append(row);

            state_counts[row.state] += 1;
            confidence_counts[row.confidence] += 1;
            if (previous_state >= 0 && previous_state != static_cast<int>(observation.result.state))
                ++transitions;
            previous_state = static_cast<int>(observation.result.state);
            for (const auto& family : row.available_families)
                coverage_counts[family] += 1;

            const auto accumulate = [&](Bucket& bucket, bool valid, double value) {
                bucket.buy_n += row.state == QStringLiteral("BUY") ? 1 : 0;
                bucket.hold_n += row.state == QStringLiteral("HOLD") ? 1 : 0;
                bucket.sell_n += row.state == QStringLiteral("SELL") ? 1 : 0;
                if (!valid)
                    return;
                ++bucket.valid;
                bucket.all.append(value);
                if (row.state == QStringLiteral("BUY"))
                    bucket.buy.append(value);
                else if (row.state == QStringLiteral("SELL"))
                    bucket.sell.append(value);
                else
                    bucket.hold.append(value);
            };
            accumulate(bucket_4w, row.valid_4w, row.return_4w);
            accumulate(bucket_13w, row.valid_13w, row.return_13w);

            if (row.proxy_scope == QStringLiteral("primary_continuous")) {
                AggregateBucket& confidence = confidence_buckets[phase + QLatin1Char('|') + row.confidence];
                if (row.valid_4w)
                    confidence.returns_4w.append(row.return_4w);
                if (row.valid_13w)
                    confidence.returns_13w.append(row.return_13w);
                AggregateBucket& context = context_buckets[phase + QLatin1Char('|') + row.context + QLatin1Char('|') +
                                                          row.tactical + QLatin1Char('|') + row.swing + QLatin1Char('|') +
                                                          row.family_code];
                if (row.valid_4w)
                    context.returns_4w.append(row.return_4w);
                if (row.valid_13w)
                    context.returns_13w.append(row.return_13w);
                const auto add_conflict = [&](const QString& dimension, bool flag) {
                    AggregateBucket& conflict = conflict_buckets[phase + QLatin1Char('|') + dimension +
                                                                 QLatin1Char('|') +
                                                                 (flag ? QStringLiteral("conflicted")
                                                                       : QStringLiteral("clear")) +
                                                                 QLatin1Char('|') + row.family_code];
                    if (row.valid_4w)
                        conflict.returns_4w.append(row.return_4w);
                    if (row.valid_13w)
                        conflict.returns_13w.append(row.return_13w);
                };
                add_conflict(QStringLiteral("core_opposed"), row.core_opposed);
                add_conflict(QStringLiteral("price_cot"), row.conflict_price);
                add_conflict(QStringLiteral("open_interest"), row.conflict_oi);
                add_conflict(QStringLiteral("participant"), row.conflict_participant);
                add_conflict(QStringLiteral("historical_context"), row.conflict_historical);
            }
        }

        const auto market_summary = [&](const QString& horizon, const Bucket& bucket) {
            const Dist all = distribution(bucket.all);
            const Dist buy = distribution(bucket.buy);
            const Dist sell = distribution(bucket.sell);
            market_rows.append({
                market_key, family_code, asset_class, prices.proxy_type,
                prices.proxy_type == QStringLiteral("spot_index") ? QStringLiteral("spot_index_reported_separately")
                                                                  : QStringLiteral("primary_continuous"),
                phase_filter, horizon, QString::number(candidate), QString::number(timing_excluded),
                QString::number(unavailable), QString::number(eligible_market), QString::number(bucket.valid),
                QString::number(bucket.buy_n), QString::number(bucket.hold_n), QString::number(bucket.sell_n),
                QString::number(bucket.buy.size()), QString::number(bucket.hold.size()), QString::number(bucket.sell.size()),
                number(all.mean), number(all.median), number(all.positive), number(buy.mean), number(buy.median),
                number(buy.positive), number(sell.mean), number(sell.median), number(sell.negative),
                number(buy.mean - sell.mean), QString::number(transitions),
                number(eligible_market > 1 ? static_cast<double>(transitions) / static_cast<double>(eligible_market - 1)
                                           : 0.0),
            });
        };
        market_summary(QStringLiteral("4W"), bucket_4w);
        market_summary(QStringLiteral("13W"), bucket_13w);

        QStringList quality;
        quality << market_key << family_code << asset_class << phase_filter << QString::number(candidate)
                << QString::number(eligible_market)
                << QString::number(candidate > 0 ? static_cast<double>(eligible_market) / candidate : 0.0, 'f', 6)
                << QString::number(state_counts.value(QStringLiteral("BUY")))
                << QString::number(state_counts.value(QStringLiteral("HOLD")))
                << QString::number(state_counts.value(QStringLiteral("SELL"))) << QString::number(transitions)
                << number(eligible_market > 1
                              ? static_cast<double>(transitions) / static_cast<double>(eligible_market - 1)
                              : 0.0)
                << QString::number(confidence_counts.value(QStringLiteral("High")))
                << QString::number(confidence_counts.value(QStringLiteral("Medium")))
                << QString::number(confidence_counts.value(QStringLiteral("Low")));
        for (const QString& family :
             {QStringLiteral("tactical_4w"), QStringLiteral("swing_13w"), QStringLiteral("price_cot"),
              QStringLiteral("open_interest"), QStringLiteral("participant"), QStringLiteral("historical_context")}) {
            quality << number(eligible_market > 0
                                  ? static_cast<double>(coverage_counts.value(family)) / static_cast<double>(eligible_market)
                                  : 0.0);
        }
        for (const auto& reason : ineligibility_reasons.keys())
            exclusion_rows.append({market_key, family_code, reason, QString::number(ineligibility_reasons.value(reason))});
        quality_rows.append(quality);
        eligible_total += eligible_market;
        candidate_total += candidate;
    }

    // ── Aggregate evidence ──────────────────────────────────────────────────
    QVector<QStringList> aggregate_rows;
    const auto add_aggregate = [&](const QString& scope, const QString& group, bool horizon_4w,
                                   const std::function<bool(const StateRow&)>& predicate) {
        QVector<double> returns;
        QVector<double> buys;
        QVector<double> sells;
        for (const auto& row : eligible_rows) {
            if (row.proxy_scope != QStringLiteral("primary_continuous") || !predicate(row))
                continue;
            const bool valid = horizon_4w ? row.valid_4w : row.valid_13w;
            const double value = horizon_4w ? row.return_4w : row.return_13w;
            if (!valid)
                continue;
            returns.append(value);
            if (row.state == QStringLiteral("BUY"))
                buys.append(value);
            if (row.state == QStringLiteral("SELL"))
                sells.append(value);
        }
        const Dist all = distribution(returns);
        const Dist buy = distribution(buys);
        const Dist sell = distribution(sells);
        aggregate_rows.append({phase_filter, scope, group, horizon_4w ? QStringLiteral("4W") : QStringLiteral("13W"),
                               QString::number(all.n), number(all.mean), number(all.median), number(all.positive),
                               QString::number(buy.n), number(buy.mean), number(buy.median), number(buy.positive),
                               QString::number(sell.n), number(sell.mean), number(sell.median), number(sell.negative),
                               number(buy.mean - sell.mean)});
    };

    const auto state_is = [](const QString& state) {
        return [state](const StateRow& row) { return row.state == state; };
    };
    const auto all_states = [](const StateRow&) { return true; };

    for (const bool horizon_4w : {true, false}) {
        add_aggregate(QStringLiteral("observation_weighted"), QStringLiteral("all"), horizon_4w, all_states);
        for (const QString& state : {QStringLiteral("BUY"), QStringLiteral("HOLD"), QStringLiteral("SELL")})
            add_aggregate(QStringLiteral("observation_weighted"), state, horizon_4w, state_is(state));
        for (const QString& family : {QStringLiteral("legacy"), QStringLiteral("disaggregated"), QStringLiteral("tff")})
            add_aggregate(QStringLiteral("report_family"), family, horizon_4w,
                          [&](const StateRow& row) { return row.family_code == family; });
        QStringList asset_classes;
        for (const auto& row : eligible_rows) {
            if (!asset_classes.contains(row.asset_class))
                asset_classes.append(row.asset_class);
        }
        asset_classes.sort();
        for (const auto& asset_class : asset_classes)
            add_aggregate(QStringLiteral("asset_class"), asset_class, horizon_4w,
                          [&](const StateRow& row) { return row.asset_class == asset_class; });
    }

    // Market-level medians (a long-history series must not define the result).
    QHash<QString, QVector<double>> market_means_4w;
    QHash<QString, QVector<double>> market_means_13w;
    for (const auto& row : eligible_rows) {
        if (row.proxy_scope != QStringLiteral("primary_continuous"))
            continue;
        const QString key = row.market_key + QLatin1Char('|') + row.family_code;
        if (row.valid_4w)
            market_means_4w[key].append(row.return_4w);
        if (row.valid_13w)
            market_means_13w[key].append(row.return_13w);
    }
    const auto add_market_median = [&](bool horizon_4w) {
        QVector<double> means;
        QVector<double> medians;
        const auto& source = horizon_4w ? market_means_4w : market_means_13w;
        for (const auto& key : source.keys()) {
            const QVector<double>& series = source.value(key);
            if (series.isEmpty())
                continue;
            double sum = 0.0;
            for (double value : series)
                sum += value;
            means.append(sum / series.size());
            medians.append(median_of(series));
        }
        aggregate_rows.append({phase_filter, QStringLiteral("market_median"), QStringLiteral("all"),
                               horizon_4w ? QStringLiteral("4W") : QStringLiteral("13W"), QString::number(means.size()),
                               number(median_of(means)), number(median_of(medians)), QString(), QString(), QString(),
                               QString(), QString(), QString(), QString(), QString(), QString()});
    };
    add_market_median(true);
    add_market_median(false);

    // Confidence, context and conflict aggregates.
    QVector<QStringList> confidence_rows;
    for (const auto& key : confidence_buckets.keys()) {
        const QStringList parts = key.split(QLatin1Char('|'));
        const AggregateBucket& bucket = confidence_buckets.value(key);
        for (int horizon = 0; horizon < 2; ++horizon) {
            const bool horizon_4w = horizon == 0;
            const QVector<double>& series = horizon_4w ? bucket.returns_4w : bucket.returns_13w;
            const Dist dist = distribution(series);
            confidence_rows.append({parts.value(0), parts.value(1), horizon_4w ? QStringLiteral("4W") : QStringLiteral("13W"),
                                    QString::number(dist.n), number(dist.mean), number(dist.median), number(dist.positive)});
        }
    }
    std::stable_sort(confidence_rows.begin(), confidence_rows.end(), [](const QStringList& left, const QStringList& right) {
        return left.join(QLatin1Char('|')) < right.join(QLatin1Char('|'));
    });

    QVector<QStringList> context_rows;
    for (const auto& key : context_buckets.keys()) {
        const QStringList parts = key.split(QLatin1Char('|'));
        const AggregateBucket& bucket = context_buckets.value(key);
        for (int horizon = 0; horizon < 2; ++horizon) {
            const bool horizon_4w = horizon == 0;
            const QVector<double>& series = horizon_4w ? bucket.returns_4w : bucket.returns_13w;
            const Dist dist = distribution(series);
            context_rows.append({parts.value(0), parts.value(1), parts.value(2), parts.value(3), parts.value(4),
                                 horizon_4w ? QStringLiteral("4W") : QStringLiteral("13W"), QString::number(dist.n),
                                 number(dist.mean), number(dist.median), number(dist.positive)});
        }
    }
    std::stable_sort(context_rows.begin(), context_rows.end(), [](const QStringList& left, const QStringList& right) {
        return left.join(QLatin1Char('|')) < right.join(QLatin1Char('|'));
    });

    QVector<QStringList> conflict_rows;
    for (const auto& key : conflict_buckets.keys()) {
        const QStringList parts = key.split(QLatin1Char('|'));
        const AggregateBucket& bucket = conflict_buckets.value(key);
        for (int horizon = 0; horizon < 2; ++horizon) {
            const bool horizon_4w = horizon == 0;
            const QVector<double>& series = horizon_4w ? bucket.returns_4w : bucket.returns_13w;
            const Dist dist = distribution(series);
            conflict_rows.append({parts.value(0), parts.value(1), parts.value(2), parts.value(3),
                                  horizon_4w ? QStringLiteral("4W") : QStringLiteral("13W"), QString::number(dist.n),
                                  number(dist.mean), number(dist.median), number(dist.positive)});
        }
    }
    std::stable_sort(conflict_rows.begin(), conflict_rows.end(), [](const QStringList& left, const QStringList& right) {
        return left.join(QLatin1Char('|')) < right.join(QLatin1Char('|'));
    });

    // Eligible-state detail (the primary evidence file).
    QVector<QStringList> state_rows;
    for (const auto& row : eligible_rows) {
        QStringList fields;
        fields << row.market_key << row.label << row.family_code << row.asset_class << row.proxy_type << row.proxy_scope
               << row.phase << row.report_date.toString(Qt::ISODate) << row.effective_date.toString(Qt::ISODate)
               << row.state << row.confidence << row.tactical << row.swing << row.regime << row.context
               << (row.price_used ? QStringLiteral("yes") : QStringLiteral("no"));
        fields << (row.valid_4w ? number(row.return_4w) : QString()) << row.status_4w;
        fields << (row.valid_13w ? number(row.return_13w) : QString()) << row.status_13w;
        fields << (row.core_opposed ? QStringLiteral("yes") : QStringLiteral("no"))
               << (row.conflict_price ? QStringLiteral("yes") : QStringLiteral("no"))
               << (row.conflict_oi ? QStringLiteral("yes") : QStringLiteral("no"))
               << (row.conflict_participant ? QStringLiteral("yes") : QStringLiteral("no"))
               << (row.conflict_historical ? QStringLiteral("yes") : QStringLiteral("no"));
        fields << number(row.net_change_4w) << number(row.net_change_13w) << number(row.net_pct_oi_change_4w)
               << number(row.price_change_4w) << number(row.price_change_13w) << number(row.open_interest_change_4w);
        QStringList families = QStringList(row.available_families.values());
        families.sort();
        fields << families.join(QLatin1Char('+'));
        fields << row.supporting_families.join(QLatin1Char('+'));
        state_rows.append(fields);
    }

    for (auto it = exclusion_counts.constBegin(); it != exclusion_counts.constEnd(); ++it) {
        const QStringList parts = it.key().split(QLatin1Char('|'));
        if (parts.size() >= 3)
            exclusion_rows.append({parts[0], parts[1], parts.mid(2).join(QLatin1Char('|')), QString::number(it.value())});
    }
    for (const auto& issue : price_issues)
        exclusion_rows.append({QString(), QString(), issue, QStringLiteral("1")});
    std::stable_sort(exclusion_rows.begin(), exclusion_rows.end(),
                     [](const QStringList& left, const QStringList& right) {
                         return left.join(QLatin1Char('|')) < right.join(QLatin1Char('|'));
                     });

    // ── Write evidence ──────────────────────────────────────────────────────
    const QString prefix = out_dir + QLatin1Char('/');
    write_csv(prefix + QStringLiteral("states.csv"),
              {QStringLiteral("market_key"), QStringLiteral("label"), QStringLiteral("family"), QStringLiteral("asset_class"),
               QStringLiteral("proxy_type"), QStringLiteral("proxy_scope"), QStringLiteral("phase"),
               QStringLiteral("report_date"), QStringLiteral("effective_date"), QStringLiteral("state"),
               QStringLiteral("confidence"), QStringLiteral("tactical_4w"), QStringLiteral("swing_13w"),
               QStringLiteral("regime_26w"), QStringLiteral("historical_context"), QStringLiteral("price_used"),
               QStringLiteral("return_4w_pct"), QStringLiteral("outcome_4w_status"), QStringLiteral("return_13w_pct"),
               QStringLiteral("outcome_13w_status"), QStringLiteral("core_opposed"), QStringLiteral("price_conflicted"),
               QStringLiteral("oi_conflicted"), QStringLiteral("participant_conflicted"),
               QStringLiteral("historical_conflicted"), QStringLiteral("net_change_4w"),
               QStringLiteral("net_change_13w"), QStringLiteral("net_pct_oi_change_4w"), QStringLiteral("price_change_4w"),
               QStringLiteral("price_change_13w"), QStringLiteral("open_interest_change_4w"),
               QStringLiteral("available_confidence_families"), QStringLiteral("supporting_families")},
              state_rows);
    write_csv(prefix + QStringLiteral("summary_market.csv"),
              {QStringLiteral("market_key"), QStringLiteral("family"), QStringLiteral("asset_class"), QStringLiteral("proxy_type"),
               QStringLiteral("proxy_scope"), QStringLiteral("phase"), QStringLiteral("horizon"),
               QStringLiteral("candidate_observations"), QStringLiteral("timing_excluded"), QStringLiteral("unavailable"),
               QStringLiteral("eligible"), QStringLiteral("valid_outcomes"), QStringLiteral("buy_n"), QStringLiteral("hold_n"),
               QStringLiteral("sell_n"), QStringLiteral("buy_valid"), QStringLiteral("hold_valid"), QStringLiteral("sell_valid"),
               QStringLiteral("mean_all"), QStringLiteral("median_all"), QStringLiteral("positive_all"),
               QStringLiteral("mean_buy"), QStringLiteral("median_buy"), QStringLiteral("positive_buy"),
               QStringLiteral("mean_sell"), QStringLiteral("median_sell"), QStringLiteral("negative_sell"),
               QStringLiteral("mean_buy_minus_sell"), QStringLiteral("state_transitions"), QStringLiteral("churn_rate")},
              market_rows);
    write_csv(prefix + QStringLiteral("summary_aggregate.csv"),
              {QStringLiteral("phase"), QStringLiteral("scope"), QStringLiteral("group"), QStringLiteral("horizon"),
               QStringLiteral("n"), QStringLiteral("mean"), QStringLiteral("median"), QStringLiteral("positive_rate"),
               QStringLiteral("buy_n"), QStringLiteral("buy_mean"), QStringLiteral("buy_median"),
               QStringLiteral("buy_positive_rate"), QStringLiteral("sell_n"), QStringLiteral("sell_mean"),
               QStringLiteral("sell_median"), QStringLiteral("sell_negative_rate"),
               QStringLiteral("buy_minus_sell_mean")},
              aggregate_rows);
    write_csv(prefix + QStringLiteral("summary_confidence.csv"),
              {QStringLiteral("phase"), QStringLiteral("confidence"), QStringLiteral("horizon"), QStringLiteral("n"),
               QStringLiteral("mean"), QStringLiteral("median"), QStringLiteral("positive_rate")},
              confidence_rows);
    write_csv(prefix + QStringLiteral("context_cases.csv"),
              {QStringLiteral("phase"), QStringLiteral("historical_context"), QStringLiteral("tactical_4w"),
               QStringLiteral("swing_13w"), QStringLiteral("family"), QStringLiteral("horizon"), QStringLiteral("n"),
               QStringLiteral("mean"), QStringLiteral("median"), QStringLiteral("positive_rate")},
              context_rows);
    write_csv(prefix + QStringLiteral("family_conflicts.csv"),
              {QStringLiteral("phase"), QStringLiteral("dimension"), QStringLiteral("flag"), QStringLiteral("family"),
               QStringLiteral("horizon"), QStringLiteral("n"), QStringLiteral("mean"), QStringLiteral("median"),
               QStringLiteral("positive_rate")},
              conflict_rows);
    write_csv(prefix + QStringLiteral("state_quality.csv"),
              {QStringLiteral("market_key"), QStringLiteral("family"), QStringLiteral("asset_class"), QStringLiteral("phase"),
               QStringLiteral("candidate_observations"), QStringLiteral("eligible"), QStringLiteral("coverage_rate"),
               QStringLiteral("buy_count"), QStringLiteral("hold_count"), QStringLiteral("sell_count"),
               QStringLiteral("state_transitions"), QStringLiteral("churn_rate"), QStringLiteral("confidence_high"),
               QStringLiteral("confidence_medium"), QStringLiteral("confidence_low"),
               QStringLiteral("coverage_tactical_4w"), QStringLiteral("coverage_swing_13w"),
               QStringLiteral("coverage_price_cot"), QStringLiteral("coverage_open_interest"),
               QStringLiteral("coverage_participant"), QStringLiteral("coverage_historical_context")},
              quality_rows);
    write_csv(prefix + QStringLiteral("exclusions.csv"),
              {QStringLiteral("market_key"), QStringLiteral("family"), QStringLiteral("reason"), QStringLiteral("count")},
              exclusion_rows);

    QTextStream(stdout) << "Replayed " << markets_replayed << " market/family series; eligible states " << eligible_total
                        << " of " << candidate_total << " candidate observations. Evidence -> " << out_dir << "\n";
    return 0;
}
