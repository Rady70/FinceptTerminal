// tests/tst_news_dedupe.cpp
//
// MarketLab: cross-feed news de-duplication. The RSS catalog deliberately
// carries overlapping publisher views (three CNBC feeds, for example), and the
// aggregate would otherwise list the same story once per feed. The rule:
// identity is the link, normalized component-aware (scheme/host case
// insensitive, path/query case preserved, fragment dropped, encoded form
// preserved); absent a link, source + publication time + normalized headline.
// The canonical envelope comes from the earliest feed on the effective feed
// list — regardless of timestamps or list position — and a copy with a
// recorded priority always beats one without (a legacy cache).
//
// Header-only over Qt Core ("services/news/NewsDedupe.h"), no app sources.

#include "services/news/NewsDedupe.h"

#include <QTest>
#include <QVector>

#include <algorithm>

using fincept::services::canonicalize_news_articles;
using fincept::services::dedupe_news_articles;
using fincept::services::news_envelope_precedes;
using fincept::services::news_newer_first;
using fincept::services::NewsArticle;

namespace {
NewsArticle article(const QString& headline, const QString& link, const QString& source = "CNBC", qint64 sort_ts = 1000,
                    const QString& category = "MARKETS", const QString& region = "GLOBAL", int feed_order = -1) {
    NewsArticle a;
    a.headline = headline;
    a.link = link;
    a.source = source;
    a.sort_ts = sort_ts;
    a.category = category;
    a.region = region;
    a.feed_order = feed_order;
    return a;
}

// The three overlapping CNBC views of one story, with their real envelope
// metadata and representative relative feed positions: cnbc-finance is listed
// before cnbc-world and cnbc-tech in the built-in catalog (only the order
// matters, not the exact indices).
QVector<NewsArticle> cnbc_copies() {
    const QString headline = "OpenAI abandons plan to release upcoming model as safety concerns escalate";
    const QString link = "https://www.cnbc.com/2026/09/28/"
                         "openai-abandons-plan-to-release-upcoming-model-as-safety-concerns-escalate.html";
    return {
        article(headline, link, "CNBC", 1000, "MARKETS", "US", 8),      // cnbc-finance
        article(headline, link, "CNBC", 1000, "MARKETS", "GLOBAL", 25), // cnbc-world
        article(headline, link, "CNBC", 1000, "TECH", "US", 26),        // cnbc-tech
    };
}

QString survivor_signature(const NewsArticle& a) {
    return a.source + QLatin1Char('|') + a.category + QLatin1Char('|') + a.region + QLatin1Char('|') + a.headline +
           QLatin1Char('|') + a.link;
}
} // namespace

class TstNewsDedupe : public QObject {
    Q_OBJECT

  private slots:
    // The demonstrated defect: one CNBC story carried by the finance, world
    // and tech feeds was listed three times in MARKET NEWS.
    void sameStoryAcrossOverlappingFeedsCollapses() {
        auto copies = cnbc_copies();

        canonicalize_news_articles(copies);

        QCOMPARE(copies.size(), 1);
    }

    // The survivor must not depend on feed completion order or on the
    // regenerated per-fetch article id, and it must be the canonical envelope:
    // cnbc-finance is the earliest CNBC feed on the effective list, so its
    // MARKETS/US envelope represents the story.
    void survivorIsStableAcrossFeedOrder() {
        QVector<int> order = {0, 1, 2};
        QString first_signature;
        do {
            auto copies = cnbc_copies();
            QVector<NewsArticle> permuted;
            permuted.reserve(copies.size());
            for (int i : order)
                permuted.append(copies[i]);
            for (int i = 0; i < permuted.size(); ++i)
                permuted[i].id = QString("generated-%1-%2").arg(i).arg(order[i]);

            canonicalize_news_articles(permuted);

            QCOMPARE(permuted.size(), 1);
            const QString signature = survivor_signature(permuted[0]);
            if (first_signature.isEmpty())
                first_signature = signature;
            QCOMPARE(signature, first_signature);
        } while (std::next_permutation(order.begin(), order.end()));

        const auto expected = cnbc_copies().first();
        QCOMPARE(first_signature, QString("CNBC|MARKETS|US|%1|%2").arg(expected.headline, expected.link));
    }

    // Feed priority decides the envelope, not the publication time: the
    // earlier feed's older copy must survive, while the surviving list is
    // still ordered chronologically.
    void envelopeFollowsFeedPriorityRegardlessOfTimestamps() {
        QVector<NewsArticle> articles = {
            article("Shared story", "https://example.com/shared", "EARLIER", 900, "TECH", "US", 5),
            article("Shared story", "https://example.com/shared", "LATER", 1000, "MARKETS", "US", 6),
            article("Other story", "https://example.com/other", "OTHER", 1000, "MARKETS", "US", 7),
        };

        canonicalize_news_articles(articles);

        QCOMPARE(articles.size(), 2);
        QCOMPARE(articles[0].headline, QString("Other story"));
        QCOMPARE(articles[1].headline, QString("Shared story"));
        QCOMPARE(articles[1].source, QString("EARLIER"));
        QCOMPARE(articles[1].category, QString("TECH"));
    }

    // The canonical envelope follows the feed list, not the category name:
    // a TECH copy listed before a MARKETS copy wins.
    void canonicalEnvelopeFollowsTheFeedListNotTheCategoryName() {
        QVector<NewsArticle> articles = {
            article("Shared story", "https://example.com/shared", "WIRE", 1000, "TECH", "US", 5),
            article("Shared story", "https://example.com/shared", "LATER", 1000, "MARKETS", "US", 6),
        };

        canonicalize_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].source, QString("WIRE"));
        QCOMPARE(articles[0].category, QString("TECH"));
    }

    // Selection is independent of list position: the later-listed copy does
    // not win by appearing first.
    void envelopeSelectionDoesNotDependOnListPosition() {
        QVector<NewsArticle> articles = {
            article("Shared story", "https://example.com/shared", "LATER", 1000, "MARKETS", "US", 6),
            article("Shared story", "https://example.com/shared", "EARLIER", 900, "TECH", "US", 5),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].source, QString("EARLIER"));
        QVERIFY(news_envelope_precedes(
            articles[0], article("Shared story", "https://example.com/shared", "LATER", 1000, "MARKETS", "US", 6)));
    }

    // A cache written before the canonical rule existed stored equal-time
    // copies in feed-completion order, but its entries still carry feed_order,
    // so the cache-read paths pick the same copy a fresh fetch would.
    void staleCacheOrderIsCanonicalizedLikeFreshData() {
        auto copies = cnbc_copies();
        QVector<NewsArticle> stale_order = {copies[1], copies[2], copies[0]};

        canonicalize_news_articles(stale_order);

        QCOMPARE(stale_order.size(), 1);
        QCOMPARE(survivor_signature(stale_order[0]), survivor_signature(copies[0]));
    }

    // A legacy cache (written before feed_order existed) has no recorded
    // priority. Such a list is resolved best-effort and deterministically —
    // here MARKETS/GLOBAL by the stable-field fallback — and is NOT claimed
    // to match the fresh-fetch envelope (CNBC/MARKETS/US); it is replaced by
    // the first completed fetch.
    void legacyCacheWithoutFeedOrderIsBestEffort() {
        auto legacy = cnbc_copies();
        for (auto& copy : legacy)
            copy.feed_order = -1;

        canonicalize_news_articles(legacy);

        QCOMPARE(legacy.size(), 1);
        QCOMPARE(legacy[0].category, QString("MARKETS"));
        QCOMPARE(legacy[0].region, QString("GLOBAL"));
    }

    // A copy with a recorded priority always beats an unknown-priority copy,
    // whatever the list order or the timestamps.
    void legacyCacheEntryNeverOutranksRecordedPriority() {
        QVector<NewsArticle> articles = {
            article("Shared story", "https://example.com/shared", "CNBC", 1000, "MARKETS", "GLOBAL", -1),
            article("Shared story", "https://example.com/shared", "CNBC", 900, "MARKETS", "US", 8),
        };

        canonicalize_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].region, QString("US"));
    }

    void distinctStoriesKeepTheirOrder() {
        QVector<NewsArticle> articles = {
            article("First", "https://example.com/1"),
            article("Second", "https://example.com/2"),
            article("Third", "https://example.com/3"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 3);
        QCOMPARE(articles[0].headline, QString("First"));
        QCOMPARE(articles[1].headline, QString("Second"));
        QCOMPARE(articles[2].headline, QString("Third"));
    }

    void repeatLaterInTheListCollapses() {
        QVector<NewsArticle> articles = {
            article("A", "https://example.com/a"),
            article("B", "https://example.com/b"),
            article("A again", "https://example.com/a"),
            article("C", "https://example.com/c"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 3);
        QCOMPARE(articles[0].headline, QString("A"));
        QCOMPARE(articles[1].headline, QString("B"));
        QCOMPARE(articles[2].headline, QString("C"));
    }

    // Two publishers covering the same event are different stories: the link,
    // not the headline wording, decides.
    void sameHeadlineDifferentLinksStaysSeparate() {
        QVector<NewsArticle> articles = {
            article("Fed holds rates steady", "https://example.com/fed-hold", "AP"),
            article("Fed holds rates steady", "https://other.example.com/fed-decision", "BBC"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    // A live-blog entry can be retitled between two feeds; the unchanged link
    // still identifies one story and the newest copy survives when both
    // priorities are unknown.
    void sameLinkUpdatedHeadlineCollapses() {
        QVector<NewsArticle> articles = {
            article("Stock futures are little changed: Live updates", "https://example.com/live", "CNBC", 1000),
            article("Stock futures are little changed after higher yields: Live updates", "https://example.com/live",
                    "CNBC", 900),
        };

        canonicalize_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].headline, QString("Stock futures are little changed: Live updates"));
    }

    void urlSchemeAndHostCaseCollapses() {
        QVector<NewsArticle> articles = {
            article("Story", "HTTPS://WWW.Example.COM/story"),
            article("Story", "https://www.example.com/story"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
    }

    void urlFragmentCollapses() {
        QVector<NewsArticle> articles = {
            article("Story", "https://www.example.com/story#live"),
            article("Story", "https://www.example.com/story"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
    }

    // Path case is significant (RFC 3986): these can be distinct resources.
    void pathCaseStaysDistinct() {
        QVector<NewsArticle> articles = {
            article("Story", "https://www.example.com/Story"),
            article("Story", "https://www.example.com/story"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    // A trailing slash is not stripped: the two paths are not established as
    // equivalent.
    void trailingSlashStaysDistinct() {
        QVector<NewsArticle> articles = {
            article("Story", "https://www.example.com/story/"),
            article("Story", "https://www.example.com/story"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    // Query text is preserved exactly, including its case.
    void queryStaysDistinct() {
        QVector<NewsArticle> articles = {
            article("Story", "https://www.example.com/story?id=AbC"),
            article("Story", "https://www.example.com/story?id=abc"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    // The identity key uses the encoded form: an encoded reserved delimiter
    // (%2F) stays a different resource from a literal '/', while two spellings
    // of the same encoded byte collapse.
    void encodedReservedDelimiterStaysDistinct() {
        QVector<NewsArticle> articles = {
            article("Story", "https://www.example.com/a%2Fb"),
            article("Story", "https://www.example.com/a/b"),
            article("Story", "https://www.example.com/a%2fb"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
        QCOMPARE(articles[0].link, QString("https://www.example.com/a%2Fb"));
        QCOMPARE(articles[1].link, QString("https://www.example.com/a/b"));
    }

    void missingLinkFallsBackToNormalizedHeadlineWithinSource() {
        QVector<NewsArticle> articles = {
            article("  OpenAI   abandons PLAN ", "", "CNBC", 1000),
            article("openai abandons plan", "", "CNBC", 1000),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].headline, QString("  OpenAI   abandons PLAN "));
    }

    // A title is only unique within one source at one instant: the same
    // headline on another publication day is a different article.
    void linklessSameHeadlineDifferentPublicationTimesStaySeparate() {
        QVector<NewsArticle> articles = {
            article("Markets today", "", "CNBC", 1000),
            article("Markets today", "", "CNBC", 86400),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    // The headline fallback is scoped to the source: a coincident title from
    // another publisher is never merged.
    void linklessSameHeadlineDifferentSourcesStaySeparate() {
        QVector<NewsArticle> articles = {
            article("Fed holds rates steady", "", "AP", 1000),
            article("Fed holds rates steady", "", "BBC", 1000),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    // An undated link-less article has no established identity and is always
    // kept.
    void undatedLinklessCopiesAreAlwaysKept() {
        QVector<NewsArticle> articles = {
            article("Markets today", "", "CNBC", 0),
            article("Markets today", "", "CNBC", 0),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    // Explicit limitation: a linked copy and a link-less copy of the same
    // story share no identity to match on, so both are kept.
    void linkedAndLinklessCopiesOfOneStoryStaySeparate() {
        QVector<NewsArticle> articles = {
            article("Fed holds rates steady", "https://example.com/fed-hold", "AP"),
            article("Fed holds rates steady", "", "AP"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }

    void entryWithNeitherLinkNorHeadlineIsKept() {
        QVector<NewsArticle> articles = {
            article("", ""),
            article("", ""),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 2);
    }
};

QTEST_MAIN(TstNewsDedupe)
#include "tst_news_dedupe.moc"
