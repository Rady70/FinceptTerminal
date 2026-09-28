// tests/tst_news_dedupe.cpp
//
// MarketLab: cross-feed news de-duplication. The RSS catalog deliberately
// carries overlapping publisher views (three CNBC feeds, for example), and the
// aggregate would otherwise list the same story once per feed. The rule:
// identity is the link (normalized) or, absent a link, the normalized
// headline; the first occurrence survives; a different link stays a different
// story even with an identical headline.
//
// Header-only over Qt Core ("services/news/NewsDedupe.h"), no app sources.

#include "services/news/NewsDedupe.h"

#include <QTest>
#include <QVector>

using fincept::services::dedupe_news_articles;
using fincept::services::NewsArticle;

namespace {
NewsArticle article(const QString& headline, const QString& link, const QString& source = "CNBC",
                    qint64 sort_ts = 1000) {
    NewsArticle a;
    a.headline = headline;
    a.link = link;
    a.source = source;
    a.sort_ts = sort_ts;
    return a;
}
} // namespace

class TstNewsDedupe : public QObject {
    Q_OBJECT

  private slots:
    // The demonstrated defect: one CNBC story carried by the finance, world
    // and tech feeds was listed three times in MARKET NEWS.
    void sameStoryAcrossOverlappingFeedsCollapses() {
        const QString headline = "OpenAI abandons plan to release upcoming model as safety concerns escalate";
        const QString link = "https://www.cnbc.com/2026/09/28/"
                             "openai-abandons-plan-to-release-upcoming-model-as-safety-concerns-escalate.html";
        QVector<NewsArticle> articles = {
            article(headline, link),
            article(headline, link),
            article(headline, link),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].headline, headline);
    }

    void firstOccurrenceSurvives() {
        const QString link = "https://example.com/story";
        QVector<NewsArticle> articles = {
            article("Story", link, "CNBC"),
            article("Story", link, "OTHER"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].source, QString("CNBC"));
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
    // still identifies one story.
    void sameLinkUpdatedHeadlineCollapses() {
        QVector<NewsArticle> articles = {
            article("Stock futures are little changed: Live updates", "https://example.com/live"),
            article("Stock futures are little changed after higher yields: Live updates", "https://example.com/live"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].headline, QString("Stock futures are little changed: Live updates"));
    }

    void linkNormalizationIgnoresCaseFragmentAndTrailingSlash() {
        QVector<NewsArticle> articles = {
            article("Story", "HTTPS://WWW.Example.COM/Story/#live"),
            article("Story", "https://www.example.com/story"),
            article("Story", "https://www.example.com/story///"),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
    }

    void missingLinkFallsBackToNormalizedHeadline() {
        QVector<NewsArticle> articles = {
            article("  OpenAI   abandons PLAN ", ""),
            article("openai abandons plan", ""),
        };

        dedupe_news_articles(articles);

        QCOMPARE(articles.size(), 1);
        QCOMPARE(articles[0].headline, QString("  OpenAI   abandons PLAN "));
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
