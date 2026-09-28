// src/services/news/NewsDedupe.h
//
// Cross-feed news de-duplication. The RSS catalog deliberately carries
// overlapping publisher views (three CNBC feeds, for example) and several
// feeds republish the same story; the fetcher appends every feed's items into
// one aggregate, so the same story would otherwise appear once per feed.
// This helper collapses those repeats.
//
// Header-only over Qt Core ("services/news/NewsTypes.h"): no service, network
// or storage types, so the suite can pin the rule without linking the
// application.
//
// Rules (MarketLab semantics):
//   * The first occurrence in the given order survives, so a caller that
//     orders newest-first keeps the copy it ordered first.
//   * Identity is the article link (trimmed, lowercased, fragment and
//     trailing slash removed). A feed that updates a live-blog headline keeps
//     one entry.
//   * When a link is missing, the normalized headline is the key, so a
//     link-less feed still collapses its own exact repeats.
//   * A different link is a different story even with an identical headline:
//     two publishers covering the same event stay separate.
//   * An entry with neither link nor headline is always kept.
#pragma once

#include "services/news/NewsTypes.h"

#include <QSet>
#include <QString>
#include <QVector>

#include <algorithm>

namespace fincept::services {

/// Story identity used by dedupe_news_articles(). Empty when the article
/// carries neither a link nor a headline.
inline QString news_dedupe_key(const NewsArticle& article) {
    QString link = article.link.trimmed().toLower();
    if (!link.isEmpty()) {
        const int fragment = link.indexOf(QLatin1Char('#'));
        if (fragment >= 0)
            link.truncate(fragment);
        while (link.endsWith(QLatin1Char('/')))
            link.chop(1);
        if (!link.isEmpty())
            return link;
    }
    return article.headline.simplified().toLower();
}

/// Removes cross-feed repeats in place, keeping the first occurrence of every
/// story. The relative order of the surviving articles is unchanged.
inline void dedupe_news_articles(QVector<NewsArticle>& articles) {
    QSet<QString> seen;
    seen.reserve(articles.size());
    articles.erase(std::remove_if(articles.begin(), articles.end(),
                                  [&seen](const NewsArticle& article) {
                                      const QString key = news_dedupe_key(article);
                                      if (key.isEmpty())
                                          return false;
                                      if (seen.contains(key))
                                          return true;
                                      seen.insert(key);
                                      return false;
                                  }),
                   articles.end());
}

} // namespace fincept::services
