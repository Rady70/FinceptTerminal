// src/services/news/NewsTypes.h
//
// The news domain's plain data types, split out of NewsService.h so the
// pure-logic news helpers (and their unit tests) can use them without pulling
// in the service's Qt Network / application dependencies.
#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

namespace fincept::services {

enum class Priority { FLASH, URGENT, BREAKING, ROUTINE };
enum class Sentiment { BULLISH, BEARISH, NEUTRAL };
enum class Impact { HIGH, MEDIUM, LOW };
enum class ThreatLevel { CRITICAL, HIGH, MEDIUM, LOW, INFO };

/// Source credibility flags.
enum class SourceFlag {
    NONE = 0,
    STATE_MEDIA = 1, // Government-controlled outlet
    CAUTION = 2,     // Known for sensationalism or low editorial standards
};

struct ThreatClassification {
    ThreatLevel level = ThreatLevel::INFO;
    QString category;      // "conflict", "cyber", "natural", "market", "regulatory", "general"
    double confidence = 0; // 0.0 - 1.0
};

struct NewsArticle {
    QString id;
    QString time;
    Priority priority = Priority::ROUTINE;
    QString category;
    QString headline;
    QString summary;
    QString source;
    QString region;
    Sentiment sentiment = Sentiment::NEUTRAL;
    Impact impact = Impact::LOW;
    QStringList tickers;
    QString link;
    int64_t sort_ts = 0; // unix seconds
    int tier = 4;        // 1=wire, 2=major, 3=specialty, 4=blog
    int feed_order = 0;  // position in the effective feed list at fetch time; 0 = first listed
    ThreatClassification threat;
    SourceFlag source_flag = SourceFlag::NONE;
    QString lang; // ISO language code (e.g., "en", "fr", "ar")
};

} // namespace fincept::services
