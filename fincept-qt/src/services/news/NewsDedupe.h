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
//     what consumers of the completed fetch see and what the category slices
//     follow. The choice is by feed priority, never by timestamp or by
//     whichever feed answered first. This is the operator-visible feed
//     priority, not an accident of field ordering.
//   * Unknown priority: an article whose feed_order is -1 (a cache written
//     before feed_order existed, or a list never parsed from a feed) has no
//     recorded priority. A copy with a recorded priority always beats one
//     without; a list made only of unknown-priority copies is resolved
//     best-effort by stable fields and is NOT claimed to match a fresh
//     fetch. Such a cache is replaced by the first completed fetch.
//   * Provisional snapshots: the progressive fetch republishes the list as
//     feeds complete, so a snapshot is canonical only over the feeds that
//     have already answered; an envelope can still change when an earlier
//     feed lands. The completed fetch is the canonical list.
//   * An entry with neither link nor headline is always kept.
#pragma once

#include "services/news/NewsTypes.h"

#include <QHash>
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

/// True when `a` is a better canonical representative of the same story than
/// `b`, independent of where either sits in a list. Feed priority decides
/// first (a recorded priority always beats an unknown one); timestamp does
/// not participate: the canonical envelope must not change because one feed
/// published or updated a copy later. Stable content fields break ties and
/// keep the choice deterministic. The generated article id is never used: it
/// is regenerated on every fetch.
inline bool news_envelope_precedes(const NewsArticle& a, const NewsArticle& b) {
    const bool a_known = a.feed_order >= 0;
    const bool b_known = b.feed_order >= 0;
    if (a_known != b_known)
        return a_known;
    if (a_known && a.feed_order != b.feed_order)
        return a.feed_order < b.feed_order;

    // Same feed, or both priorities unknown (legacy cache): resolve on stable
    // fields. Newest is preferred only here, where feed priority cannot
    // distinguish the copies.
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

/// Display order of the surviving articles: newest first, then stable fields
/// so equal publication instants are ordered deterministically.
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

/// Removes cross-feed repeats in place. The surviving copy is selected by
/// news_envelope_precedes() regardless of input order; the slot of the first
/// occurrence keeps the relative order of distinct stories unchanged.
inline void dedupe_news_articles(QVector<NewsArticle>& articles) {
    QHash<QString, int> canonical_index;
    canonical_index.reserve(articles.size());
    QVector<NewsArticle> out;
    out.reserve(articles.size());
    for (auto& article : articles) {
        const QString key = news_dedupe_key(article);
        if (key.isEmpty()) {
            out.append(std::move(article));
            continue;
        }
        auto it = canonical_index.find(key);
        if (it == canonical_index.end()) {
            canonical_index.insert(key, out.size());
            out.append(std::move(article));
            continue;
        }
        if (news_envelope_precedes(article, out[*it]))
            out[*it] = std::move(article);
    }
    articles = std::move(out);
}

/// The single entry point every surfacing path uses: collapse repeats, then
/// order the survivors chronologically. Fresh aggregation, progressive
/// snapshots and cache reads all go through this helper, so a cache is
/// canonicalized by the same rule as a fresh fetch when its entries carry a
/// recorded feed priority.
inline void canonicalize_news_articles(QVector<NewsArticle>& articles) {
    dedupe_news_articles(articles);
    std::sort(articles.begin(), articles.end(), news_newer_first);
}

} // namespace fincept::services
