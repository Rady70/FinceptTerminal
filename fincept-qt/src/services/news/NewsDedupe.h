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
//   * Identity is the article link, component-aware (RFC 3986): the scheme
//     and host compare case-insensitively, while the path and query keep the
//     provider's exact case. A fragment is dropped — a MarketLab anchor is
//     the same story. Nothing else is rewritten: `/story` and `/story/` are
//     different resources, and path case is significant.
//   * When a link is missing, identity is the normalized headline scoped to
//     the article's source, so only that source's exact repeats collapse and
//     two publishers never merge on a coincident title. A link-less copy and
//     a linked copy of the same story stay separate — the two carry no shared
//     identity to match on.
//   * Keys are namespaced ("L:" link, "H:" headline) so the two identity
//     kinds cannot collide.
//   * The first occurrence in the given order survives, so callers sort with
//     news_newer_first() first.
//   * An entry with neither link nor headline is always kept.
#pragma once

#include "services/news/NewsTypes.h"

#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

#include <algorithm>

namespace fincept::services {

/// Story identity used by dedupe_news_articles(). Empty when the article
/// carries neither a link nor a headline.
inline QString news_dedupe_key(const NewsArticle& article) {
    const QString link = article.link.trimmed();
    if (!link.isEmpty()) {
        QUrl url(link);
        if (url.isValid() && !url.scheme().isEmpty() && !url.host().isEmpty()) {
            // Scheme and host are case-insensitive; path and query are not.
            // Only the fragment is removed (see the file header).
            url.setScheme(url.scheme().toLower());
            url.setHost(url.host().toLower());
            url.setFragment(QString());
            return QLatin1String("L:") + url.toString(QUrl::RemoveFragment);
        }
        // Not a hierarchical URL (e.g. a guid-shaped link): keep the exact
        // bytes the feed supplied.
        return QLatin1String("L:") + link;
    }

    const QString headline = article.headline.simplified().toLower();
    if (headline.isEmpty())
        return {};
    return QLatin1String("H:") + article.source.trimmed().toLower() + QLatin1Char('\n') + headline;
}

/// Total order applied before de-duplication, newest first. Equal publication
/// instants (the same story carried by several feeds) break on stable envelope
/// fields — tier, source, category, region, then the content itself — so the
/// survivor is the same copy no matter which feed answered first. The generated
/// article id is deliberately not used: it is regenerated on every fetch.
inline bool news_newer_first(const NewsArticle& a, const NewsArticle& b) {
    if (a.sort_ts != b.sort_ts)
        return a.sort_ts > b.sort_ts;
    if (a.tier != b.tier)
        return a.tier < b.tier;
    if (a.source != b.source)
        return a.source < b.source;
    if (a.category != b.category)
        return a.category < b.category;
    if (a.region != b.region)
        return a.region < b.region;
    if (a.headline != b.headline)
        return a.headline < b.headline;
    if (a.summary != b.summary)
        return a.summary < b.summary;
    return a.link < b.link;
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
