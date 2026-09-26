// tst_etf_sec_parse.cpp — ETF Capital Flows Batch B: the SEC EDGAR boundary.
//
// services/etf/SecEdgarParse.h over trimmed real filings (etf_test_fixtures.h)
// and degraded variants of them. Pins: exact values and identity from the
// N-PORT genInfo / fundInfo; the monthly-flow month mapping; the amendment's
// pointer to the filing it amends; the share-class list that tells a
// multi-class series apart; missing and unparseable values staying so; exact
// acceptance times; refusal of malformed or foreign documents; and the
// declared User-Agent rule.

#include "etf_test_fixtures.h"
#include "services/etf/SecEdgarParse.h"

#include <QTest>

using namespace fincept::services::etf;
using namespace etf_fixtures;

class TstEtfSecParse : public QObject {
    Q_OBJECT

  private slots:
    void uit_document_parses_exactly();
    void series_document_carries_series_and_class();
    void amendment_names_the_filing_it_amends();
    void multi_class_series_is_visible();
    void missing_and_unparseable_values_stay_so();
    void malformed_or_foreign_documents_are_refused();
    void month_mapping_follows_the_report_period();
    void submissions_parse_with_exact_acceptance();
    void submissions_refuse_ragged_or_foreign_shapes();
    void series_index_lists_filings_and_registrant();
    void unknown_series_index_is_an_error();
    void identifiers_and_urls_are_strict();
    void declared_user_agent_rule();
};

void TstEtfSecParse::uit_document_parses_exactly() {
    const NportDocument d = parse_nport_primary_doc(nport_xml(spy_2026_06()));
    QVERIFY2(d.ok, qPrintable(d.error));
    QCOMPARE(d.submission_type, QStringLiteral("NPORT-P"));
    QCOMPARE(d.reg_cik, QStringLiteral("0000884394"));
    QCOMPARE(d.reg_name, QStringLiteral("State Street(R) SPDR(R) S&P 500(R) ETF Trust"));
    QVERIFY(d.series_id.isEmpty()); // a UIT reports without a series
    QCOMPARE(d.rep_pd_date, QDate(2026, 6, 30));
    QCOMPARE(d.rep_pd_end_raw, QStringLiteral("2026-09-30"));
    QVERIFY(d.net_assets.reported());
    QCOMPARE(d.net_assets.value, 781188872106.76);
    QCOMPARE(d.months[0].sales.value, 121393713302.60);
    QCOMPARE(d.months[0].redemption.raw, QStringLiteral("106822374029.05000000"));
    QCOMPARE(d.months[2].redemption.value, 161982960949.05);
    QVERIFY(d.months[1].reinvestment.reported());
    QCOMPARE(d.months[1].reinvestment.value, 0.0); // a reported zero, not a missing one
    QVERIFY(d.returns_block_present);
    QVERIFY(d.return_class_ids.isEmpty());
    QVERIFY(d.amended_accession.isEmpty());
}

void TstEtfSecParse::series_document_carries_series_and_class() {
    const NportDocument d = parse_nport_primary_doc(nport_xml(qqq_2026_06()));
    QVERIFY(d.ok);
    QCOMPARE(d.series_id, QStringLiteral("S000101292"));
    QCOMPARE(d.header_series_ids, QStringList{QStringLiteral("S000101292")});
    QCOMPARE(d.return_class_ids, QStringList{QStringLiteral("C000271435")});
    QCOMPARE(d.net_assets.value, 490103179941.66);
    QCOMPARE(d.months[2].sales.value, 92884216232.13);
}

void TstEtfSecParse::amendment_names_the_filing_it_amends() {
    const NportDocument original = parse_nport_primary_doc(nport_xml(ivv_2025_09(false)));
    const NportDocument amendment = parse_nport_primary_doc(nport_xml(ivv_2025_09(true)));
    QVERIFY(original.ok && amendment.ok);
    QCOMPARE(amendment.submission_type, QStringLiteral("NPORT-P/A"));
    QCOMPARE(amendment.amended_accession, QStringLiteral("0002071691-25-007634"));
    QVERIFY(original.amended_accession.isEmpty());
    // The real amendment repeats its original's values.
    for (int i = 0; i < 3; ++i) {
        QVERIFY(original.months[i].sales.same_value(amendment.months[i].sales));
        QVERIFY(original.months[i].redemption.same_value(amendment.months[i].redemption));
    }
    QCOMPARE(amendment.months[0].redemption.value, 9604552107.60000040);
}

void TstEtfSecParse::multi_class_series_is_visible() {
    NportSpec s = qqq_2026_06();
    s.class_ids = {QStringLiteral("C000000002"), QStringLiteral("C000000001")}; // synthetic second class
    const NportDocument d = parse_nport_primary_doc(nport_xml(s));
    QVERIFY(d.ok);
    QCOMPARE(d.return_class_ids, (QStringList{QStringLiteral("C000000001"), QStringLiteral("C000000002")}));
}

void TstEtfSecParse::missing_and_unparseable_values_stay_so() {
    NportSpec s = spy_2026_06();
    s.net_assets_present = false;
    s.months[1].present = false;                    // no <mon2Flow> at all
    s.months[0].redemption = QStringLiteral("N/A"); // present, not a number
    s.months[2].omit_redemption = true;             // attribute absent
    s.months[2].sales = QString();                  // present but empty
    const NportDocument d = parse_nport_primary_doc(nport_xml(s));
    QVERIFY(d.ok);
    QCOMPARE(d.net_assets.state, ValueState::Missing);
    QVERIFY(!d.months[1].element_present);
    QCOMPARE(d.months[1].sales.state, ValueState::Missing);
    QCOMPARE(d.months[0].redemption.state, ValueState::Unparseable);
    QCOMPARE(d.months[0].redemption.raw, QStringLiteral("N/A"));
    QCOMPARE(d.months[2].redemption.state, ValueState::Missing);
    QCOMPARE(d.months[2].sales.state, ValueState::Unparseable);
    QVERIFY(d.months[2].reinvestment.reported());
    NportSpec no_returns = spy_2026_06();
    no_returns.returns_present = false;
    QVERIFY(!parse_nport_primary_doc(nport_xml(no_returns)).returns_block_present);
}

void TstEtfSecParse::malformed_or_foreign_documents_are_refused() {
    QByteArray truncated = nport_xml(spy_2026_06());
    truncated.truncate(truncated.size() / 2);
    const NportDocument t = parse_nport_primary_doc(truncated);
    QVERIFY(!t.ok);
    QVERIFY(t.error.startsWith(QLatin1String("nport_xml_malformed")));
    const NportDocument html = parse_nport_primary_doc("<html><body>Not Found</body></html>");
    QVERIFY(!html.ok);
    QVERIFY(html.error.startsWith(QLatin1String("not_an_nport_document")));
    QByteArray other_ns = nport_xml(spy_2026_06());
    other_ns.replace("http://www.sec.gov/edgar/nport\"", "http://example.com/other\"");
    QVERIFY(!parse_nport_primary_doc(other_ns).ok);
    QVERIFY(!parse_nport_primary_doc(QByteArray()).ok);
}

void TstEtfSecParse::month_mapping_follows_the_report_period() {
    const auto m1 = nport_flow_month_bounds(QDate(2026, 6, 30), 1);
    const auto m3 = nport_flow_month_bounds(QDate(2026, 6, 30), 3);
    QCOMPARE(m1.first, QDate(2026, 4, 1));
    QCOMPARE(m1.second, QDate(2026, 4, 30));
    QCOMPARE(m3.first, QDate(2026, 6, 1));
    QCOMPARE(m3.second, QDate(2026, 6, 30));
    const auto jan = nport_flow_month_bounds(QDate(2026, 2, 28), 1);
    QCOMPARE(jan.first, QDate(2025, 12, 1)); // across a year end
    QCOMPARE(jan.second, QDate(2025, 12, 31));
    QVERIFY(!nport_flow_month_bounds(QDate(), 1).first.isValid());
    QVERIFY(!nport_flow_month_bounds(QDate(2026, 6, 30), 4).first.isValid());
    QVERIFY(nport_report_date_is_month_end(QDate(2024, 2, 29)));
    QVERIFY(!nport_report_date_is_month_end(QDate(2026, 6, 15)));
}

void TstEtfSecParse::submissions_parse_with_exact_acceptance() {
    const QByteArray body =
        submissions_json(QStringLiteral("0000884394"), QStringLiteral("SPDR S&P 500 ETF TRUST"),
                         {{"0001410368-26-089410", "NPORT-P", "2026-08-28", "2026-06-30", "2026-08-28T12:25:47.000Z"},
                          {"0001410368-26-055357", "NPORT-P", "2026-05-28", "2026-03-31", "2026-05-28T19:11:03.000Z"},
                          {"0000000000-26-000001", "N-CSR", "2026-05-01", "", "2026-05-01T20:00:00.000Z"},
                          {"0000000000-26-000002", "NPORT-P", "2026-05-02", "2026-03-31", "not a time"}},
                         {{"CIK0000884394-submissions-001.json", "2018-01-01", "2020-12-31"}});
    const SecSubmissions s = parse_sec_submissions(body, false);
    QVERIFY2(s.ok, qPrintable(s.error));
    QCOMPARE(s.cik, QStringLiteral("0000884394"));
    QCOMPARE(s.filings.size(), 4);
    QCOMPARE(s.filings[0].accepted_at, QDateTime(QDate(2026, 8, 28), QTime(12, 25, 47), QTimeZone::UTC));
    QVERIFY(!s.filings[3].accepted_at.isValid()); // unparseable stays unknown
    QCOMPARE(s.older_pages.size(), 1);
    QCOMPARE(s.older_pages[0].filing_to, QDate(2020, 12, 31));
    // An offset-less time is refused rather than guessed.
    QVERIFY(!sec_parse_acceptance(QStringLiteral("2026-08-28T12:25:47.000")).isValid());
    QVERIFY(sec_parse_acceptance(QStringLiteral("2026-08-28T12:25:47Z")).isValid());
    // Older pages carry the arrays at the top level.
    const SecSubmissions page =
        parse_sec_submissions(submissions_page_json({{"0002071691-25-007634", "NPORT-P", "2025-11-26", "2025-09-30",
                                                      "2025-11-26T17:01:17.000Z"}}),
                              true);
    QVERIFY(page.ok);
    QCOMPARE(page.filings[0].form, QStringLiteral("NPORT-P"));
}

void TstEtfSecParse::submissions_refuse_ragged_or_foreign_shapes() {
    QVERIFY(!parse_sec_submissions("<html>blocked</html>", false).ok);
    QVERIFY(!parse_sec_submissions("{\"cik\":\"884394\"}", false).ok);
    QJsonObject ragged =
        QJsonDocument::fromJson(submissions_page_json({{"0001410368-26-089410", "NPORT-P", "2026-08-28", "2026-06-30",
                                                        "2026-08-28T12:25:47.000Z"}}))
            .object();
    ragged.insert("form", QJsonArray{"NPORT-P", "N-CSR"});
    const SecSubmissions r = parse_sec_submissions(QJsonDocument(ragged).toJson(), true);
    QVERIFY(!r.ok);
    QVERIFY(r.error.startsWith(QLatin1String("submissions_ragged_arrays")));
}

void TstEtfSecParse::series_index_lists_filings_and_registrant() {
    const SecSeriesIndex idx =
        parse_sec_series_index(series_index_atom(QStringLiteral("0001100663"), QStringLiteral("iSHARES TRUST"),
                                                 {{"0002071691-26-019760", "NPORT-P", "2026-08-25"},
                                                  {"0002071691-26-015790", "NPORT-P/A", "2026-07-13"},
                                                  {"0002071691-25-007634", "NPORT-P", "2025-11-26"}}));
    QVERIFY2(idx.ok, qPrintable(idx.error));
    QCOMPARE(idx.company_cik, QStringLiteral("0001100663"));
    QCOMPARE(idx.entries.size(), 3);
    QCOMPARE(idx.entries[1].form, QStringLiteral("NPORT-P/A"));
    QCOMPARE(idx.entries[2].filing_date, QDate(2025, 11, 26));
}

void TstEtfSecParse::unknown_series_index_is_an_error() {
    const SecSeriesIndex none = parse_sec_series_index(series_index_atom(QString(), QString(), {}));
    QVERIFY(!none.ok);
    QVERIFY(none.error.startsWith(QLatin1String("index_without_company")));
    QVERIFY(!parse_sec_series_index("<html><body>No matching CIK.</body></html>").ok);
    QVERIFY(!parse_sec_series_index("{\"not\":\"xml\"}").ok);
}

void TstEtfSecParse::identifiers_and_urls_are_strict() {
    QCOMPARE(sec_normalized_cik(QStringLiteral("884394")), QStringLiteral("0000884394"));
    QCOMPARE(sec_normalized_cik(QStringLiteral("0000884394")), QStringLiteral("0000884394"));
    for (const char* bad : {"", "0", "12345678901", "88439A", "-884394"})
        QVERIFY(sec_normalized_cik(QString::fromLatin1(bad)).isEmpty());
    QVERIFY(sec_valid_series_id(QStringLiteral("S000004310")));
    QVERIFY(!sec_valid_series_id(QStringLiteral("S00004310")));
    QVERIFY(!sec_valid_series_id(QStringLiteral("C000012040")));
    QVERIFY(sec_valid_accession(QStringLiteral("0002071691-25-007634")));
    QVERIFY(!sec_valid_accession(QStringLiteral("000207169125007634")));
    QCOMPARE(sec_nport_primary_doc_url(QStringLiteral("0001100663"), QStringLiteral("0002071691-26-015790")),
             QStringLiteral("https://www.sec.gov/Archives/edgar/data/1100663/000207169126015790/primary_doc.xml"));
    QCOMPARE(sec_submissions_url(QStringLiteral("0000884394")),
             QStringLiteral("https://data.sec.gov/submissions/CIK0000884394.json"));
    QVERIFY(sec_submissions_page_url(QStringLiteral("../../etc/passwd")).isEmpty());
    QVERIFY(sec_series_index_url(QStringLiteral("S000004310")).contains(QLatin1String("CIK=S000004310")));
}

void TstEtfSecParse::declared_user_agent_rule() {
    QVERIFY(sec_user_agent_valid(QStringLiteral("MarketLab research admin@example.com")));
    QVERIFY(sec_user_agent_valid(QStringLiteral("Example Org (ops@example.org)")));
    QVERIFY(!sec_user_agent_valid(QStringLiteral("MarketLab-ETF-qualification/0.2 (personal research)")));
    QVERIFY(!sec_user_agent_valid(QStringLiteral("admin@example.com")));   // no name
    QVERIFY(!sec_user_agent_valid(QStringLiteral("A admin@example.com"))); // name too short
    QVERIFY(!sec_user_agent_valid(QStringLiteral("MarketLab\nadmin@example.com")));
    QVERIFY(!sec_user_agent_valid(QString()));
    QCOMPARE(sec_user_agent_redacted(QStringLiteral("MarketLab research admin@example.com")),
             QStringLiteral("MarketLab research <contact>"));
}

QTEST_GUILESS_MAIN(TstEtfSecParse)
#include "tst_etf_sec_parse.moc"
