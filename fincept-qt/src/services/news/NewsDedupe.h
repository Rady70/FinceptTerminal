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
//     the article's source AND its publication time: a title is only unique
//     within one source at one instant, so the same headline on another day
//     stays a separate article. An undated link-less article has no
//     established identity and is always kept. A link-less copy and a linked
//     copy of the same story also stay separate — they share no identity to
//     match on.
//   * Keys are namespaced ("L:" link, "H:" headline) so the two identity
//     kinds cannot collide.
//   * The canonical envelope: when one story is carried by several feeds, the
//     copy from the earliest feed on the effective feed list (the order the
//     RSS manager shows) represents it. Its source, category and region are
//     what every consumer sees and what the category slices follow. This is
//     the operator-visible feed priority, not an accident of field ordering.
//   * The first occurrence in news_newer_first() order survives, so callers
//     surface a list through canonicalize_news_articles().
//   * An entry with neither link nor headline is always kept.
#pragma once

#include "services/news/NewsTypes.h"

#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

#include <algorithm>

namespace fincept::services {

/// Story identity used by dedupe_news_articles(). Empty when no identity can
/// be established (no link and no usable headline/date); such entries are
/// always kept.
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
            // toEncoded() is the stable internal representation. toString()
            // is PrettyDecoded: it may decode reserved delimiters such as
            // %2F, which would merge genuinely distinct resources.
            return QLatin1String("L:") + QString::fromUtf8(url.toEncoded(QUrl::RemoveFragment));
        }
        // Not a hierarchical URL (e.g. a guid-shaped link): keep the exact
        // bytes the feed supplied.
        return QLatin1String("L:") + link;
    }

    // Link-less identity: source + publication time + normalized headline.
    const QString headline = article.headline.simplified().toLower();
    if (headline.isEmpty() || article.sort_ts <= 0)
        return {};
    return QLatin1String("H:") + article.source.trimmed().toLower() + QLatin1Char('\n') +
           QString::number(article.sort_ts) + QLatin1Char('\n') + headline;
}

/// Total order applied before de-duplication, newest first. Equal publication
/// instants (the same story carried by several feeds) are resolved by the
/// canonical-envelope rule — earliest feed on the effective list wins — and
/// then by stable content fields, so the survivor is the same copy no matter
/// which feed answered first. The generated article id is deliberately not
/// used: it is regenerated on every fetch.
inline bool news_newer_first(const NewsArticle& a, const NewsArticle& b) {
    if (a.sort_ts != b.sort_ts)
        return a.sort_ts > b.sort_ts;
    if (a.feed_order != b.feed_order)
        return a.feed_order < b.feed_order;
    // Articles from a legacy cache carry no feed order; these fields keep the
    // comparison total and deterministic for them.
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

/// The single entry point every surfacing path uses: sorts with
/// news_newer_first() and then collapses repeats. Fresh aggregation,
/// progressive snapshots and cache reads all go through this helper, so a
/// cache written before the rule existed is canonicalized exactly like a
/// fresh fetch.
inline void canonicalize_news_articles(QVector<NewsArticle>& articles) {
    std::sort(articles.begin(), articles.end(), news_newer_first);
    dedupe_news_articles(articles);
}

} // namespace fincept::services
