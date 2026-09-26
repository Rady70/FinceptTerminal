// etf_test_fixtures.h — deterministic fixtures for the ETF Capital Flows
// Batch B tests (tst_etf_sec_parse, tst_etf_ibkr_daily, tst_etf_store,
// tst_etf_ingest). No test touches the network or TWS.
//
// SEC fixtures are TRIMMED real public filings: the header, genInfo and the
// fundInfo elements Batch B reads are copied from the filed documents, the
// holdings are dropped. Values are public U.S. regulatory data:
//   * SPY  (SPDR S&P 500 ETF Trust, CIK 884394, a unit investment trust with no
//          series): NPORT-P 0001410368-26-089410, report period 2026-06-30,
//          accepted 2026-08-28T12:25:47Z;
//   * QQQ  (Invesco QQQ Trust, Series 1, CIK 1067839, series S000101292, class
//          C000271435): NPORT-P 0001067839-26-000030, accepted 2026-08-28T13:26:52Z;
//   * IVV  (iShares Trust, CIK 1100663, series S000004310, class C000012040):
//          NPORT-P 0002071691-25-007634 (accepted 2025-11-26T17:01:17Z) and its
//          NPORT-P/A 0002071691-26-015790 (accepted 2026-07-13T14:48:14Z), whose
//          header names the amended accession and whose values are unchanged.
// Variants (a missing element, an unparseable value, a second share class)
// are built from the same builders and say so where they are used.
//
// IBKR fixtures reproduce the wrapper's envelope shape
// (scripts/ibkr_tws_data.py command_history) with synthetic prices: no IBKR
// market data is copied into the repository.
#pragma once
#include "services/etf/EtfSessionCalendar.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QVector>

namespace etf_fixtures {

// ── SEC ──────────────────────────────────────────────────────────────────────

struct FlowTokens {
    bool present = true; ///< false: the <monNFlow> element is absent
    QString sales;
    QString redemption;
    QString reinvestment;
    bool omit_redemption = false; ///< true: the attribute is absent
};

struct NportSpec {
    QString submission_type = QStringLiteral("NPORT-P");
    QString amended_accession; ///< headerData/accessionNumber of an NPORT-P/A
    QString cik = QStringLiteral("0000884394");
    QString reg_name = QStringLiteral("State Street(R) SPDR(R) S&amp;P 500(R) ETF Trust");
    QString reg_file_number = QStringLiteral("811-06125");
    QString reg_lei = QStringLiteral("549300NZAMSJ8FXPQQ63");
    QString series_id; ///< empty: no <seriesId> element (a UIT)
    QString series_name = QStringLiteral("N/A");
    QString series_lei = QStringLiteral("549300NZAMSJ8FXPQQ63");
    QString rep_pd_end = QStringLiteral("2026-09-30");
    QString rep_pd_date = QStringLiteral("2026-06-30");
    bool net_assets_present = true;
    QString net_assets = QStringLiteral("781188872106.76");
    bool returns_present = true;
    QStringList class_ids; ///< monthlyTotReturn classId attributes (none for a UIT)
    FlowTokens months[3] = {{true, QStringLiteral("121393713302.60000000"), QStringLiteral("106822374029.05000000"),
                             QStringLiteral("0.00000000"), false},
                            {true, QStringLiteral("113494270849.20000000"), QStringLiteral("103589923878.65000000"),
                             QStringLiteral("0.00000000"), false},
                            {true, QStringLiteral("168709868513.65000000"), QStringLiteral("161982960949.05000000"),
                             QStringLiteral("0.00000000"), false}};
};

inline NportSpec spy_2026_06() {
    return NportSpec{};
}

inline NportSpec qqq_2026_06() {
    NportSpec s;
    s.cik = QStringLiteral("0001067839");
    s.reg_name = QStringLiteral("Invesco QQQ Trust, Series 1");
    s.reg_file_number = QStringLiteral("811-08947");
    s.reg_lei = QStringLiteral("549300VY6FEJBCIMET58");
    s.series_id = QStringLiteral("S000101292");
    s.series_name = QStringLiteral("Invesco QQQ Trust, Series 1");
    s.series_lei = QStringLiteral("549300VY6FEJBCIMET58");
    s.net_assets = QStringLiteral("490103179941.66");
    s.class_ids = {QStringLiteral("C000271435")};
    s.months[0] = {true, QStringLiteral("56960297589.31000000"), QStringLiteral("46510905488.86000000"),
                   QStringLiteral("0.00000000"), false};
    s.months[1] = {true, QStringLiteral("66720445189.63000000"), QStringLiteral("59529304484.22000000"),
                   QStringLiteral("0.00000000"), false};
    s.months[2] = {true, QStringLiteral("92884216232.13000000"), QStringLiteral("99679929497.66000000"),
                   QStringLiteral("0.00000000"), false};
    return s;
}

inline NportSpec ivv_2025_09(bool amendment) {
    NportSpec s;
    s.submission_type = amendment ? QStringLiteral("NPORT-P/A") : QStringLiteral("NPORT-P");
    s.amended_accession = amendment ? QStringLiteral("0002071691-25-007634") : QString();
    s.cik = QStringLiteral("0001100663");
    s.reg_name = QStringLiteral("iShares Trust");
    s.reg_file_number = QStringLiteral("811-09729");
    s.reg_lei = QStringLiteral("5493000860OXIC4B5K91");
    s.series_id = QStringLiteral("S000004310");
    s.series_name = QStringLiteral("iShares Core S&amp;P 500 ETF");
    s.series_lei = QStringLiteral("5493007M4YMN8XL48C14");
    s.rep_pd_end = QStringLiteral("2026-03-31");
    s.rep_pd_date = QStringLiteral("2025-09-30");
    s.net_assets = QStringLiteral("701369339021.20");
    s.class_ids = {QStringLiteral("C000012040")};
    s.months[0] = {true, QStringLiteral("12386061447.70000100"), QStringLiteral("9604552107.60000040"),
                   QStringLiteral("0.00000000"), false};
    s.months[1] = {true, QStringLiteral("12652498062.85000000"), QStringLiteral("4773861525.94999980"),
                   QStringLiteral("0.00000000"), false};
    s.months[2] = {true, QStringLiteral("72270823042.89999400"), QStringLiteral("53356396772.80000300"),
                   QStringLiteral("0.00000000"), false};
    return s;
}

inline QByteArray nport_xml(const NportSpec& s) {
    QString x;
    x += QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><edgarSubmission xmlns=\"http://www.sec.gov/edgar/nport\" "
        "xmlns:com=\"http://www.sec.gov/edgar/common\" xmlns:ncom=\"http://www.sec.gov/edgar/nportcommon\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        "xsi:schemaLocation=\"http://www.sec.gov/edgar/nport eis_NPORT_Filer.xsd\">\n");
    x += QStringLiteral("  <headerData>\n    <submissionType>%1</submissionType>\n    <isConfidential>false"
                        "</isConfidential>\n")
             .arg(s.submission_type);
    if (!s.amended_accession.isEmpty())
        x += QStringLiteral("    <accessionNumber>%1</accessionNumber>\n").arg(s.amended_accession);
    x += QStringLiteral("    <filerInfo>\n      <filer>\n        <issuerCredentials>\n          <cik>%1</cik>\n"
                        "        </issuerCredentials>\n      </filer>\n")
             .arg(s.cik);
    if (!s.series_id.isEmpty()) {
        x += QStringLiteral("      <seriesClassInfo>\n        <seriesId>%1</seriesId>\n").arg(s.series_id);
        for (const QString& c : s.class_ids)
            x += QStringLiteral("        <classId>%1</classId>\n").arg(c);
        x += QStringLiteral("      </seriesClassInfo>\n");
    }
    x += QStringLiteral("    </filerInfo>\n  </headerData>\n  <formData>\n    <genInfo>\n");
    x += QStringLiteral("      <regName>%1</regName>\n      <regFileNumber>%2</regFileNumber>\n"
                        "      <regCik>%3</regCik>\n      <regLei>%4</regLei>\n"
                        "      <regStreet1>1 Example Street</regStreet1>\n"
                        "      <regStateConditional regCountry=\"US\" regState=\"US-NY\"/>\n"
                        "      <seriesName>%5</seriesName>\n")
             .arg(s.reg_name, s.reg_file_number, s.cik, s.reg_lei, s.series_name);
    if (!s.series_id.isEmpty())
        x += QStringLiteral("      <seriesId>%1</seriesId>\n").arg(s.series_id);
    x += QStringLiteral("      <seriesLei>%1</seriesLei>\n      <repPdEnd>%2</repPdEnd>\n"
                        "      <repPdDate>%3</repPdDate>\n      <isFinalFiling>N</isFinalFiling>\n    </genInfo>\n")
             .arg(s.series_lei, s.rep_pd_end, s.rep_pd_date);
    x += QStringLiteral("    <fundInfo>\n      <totAssets>1.00</totAssets>\n      <totLiabs>0.00</totLiabs>\n");
    if (s.net_assets_present)
        x += QStringLiteral("      <netAssets>%1</netAssets>\n").arg(s.net_assets);
    if (s.returns_present) {
        x += QStringLiteral("      <returnInfo>\n        <monthlyTotReturns>\n");
        if (s.class_ids.isEmpty())
            x += QStringLiteral("          <monthlyTotReturn rtn1=\"1.0\" rtn2=\"1.0\" rtn3=\"1.0\"/>\n");
        for (const QString& c : s.class_ids)
            x += QStringLiteral("          <monthlyTotReturn classId=\"%1\" rtn1=\"1.0\" rtn2=\"1.0\" rtn3=\"1.0\"/>\n")
                     .arg(c);
        x += QStringLiteral("        </monthlyTotReturns>\n        <othMon1 netRealizedGain=\"0\" "
                            "netUnrealizedAppr=\"0\"/>\n      </returnInfo>\n");
    }
    for (int i = 0; i < 3; ++i) {
        const FlowTokens& m = s.months[i];
        if (!m.present)
            continue;
        x += QStringLiteral("      <mon%1Flow").arg(i + 1);
        if (!m.omit_redemption)
            x += QStringLiteral(" redemption=\"%1\"").arg(m.redemption);
        x += QStringLiteral(" reinvestment=\"%1\" sales=\"%2\"/>\n").arg(m.reinvestment, m.sales);
    }
    x += QStringLiteral("    </fundInfo>\n    <invstOrSecs>\n      <invstOrSec><name>trimmed holding</name>"
                        "<balance>1</balance></invstOrSec>\n    </invstOrSecs>\n  </formData>\n"
                        "</edgarSubmission>\n");
    return x.toUtf8();
}

struct FilingRow {
    QString accession;
    QString form;
    QString filing_date;
    QString report_date;
    QString acceptance; ///< as the SEC writes it: "2026-08-28T12:25:47.000Z"
    QString primary_document = QStringLiteral("xslFormNPORT-P_X01/primary_doc.xml");
};

struct OlderPageRow {
    QString name;
    QString from;
    QString to;
};

inline QJsonObject filing_arrays(const QVector<FilingRow>& rows) {
    QJsonArray acc, form, fdate, rdate, accepted, primary;
    for (const FilingRow& r : rows) {
        acc.append(r.accession);
        form.append(r.form);
        fdate.append(r.filing_date);
        rdate.append(r.report_date);
        accepted.append(r.acceptance);
        primary.append(r.primary_document);
    }
    return QJsonObject{{"accessionNumber", acc},
                       {"form", form},
                       {"filingDate", fdate},
                       {"reportDate", rdate},
                       {"acceptanceDateTime", accepted},
                       {"primaryDocument", primary}};
}

inline QByteArray submissions_json(const QString& cik10, const QString& name, const QVector<FilingRow>& recent,
                                   const QVector<OlderPageRow>& pages = {}) {
    QJsonArray files;
    for (const OlderPageRow& p : pages)
        files.append(QJsonObject{{"name", p.name}, {"filingCount", 1}, {"filingFrom", p.from}, {"filingTo", p.to}});
    const QJsonObject root{{"cik", cik10},
                           {"name", name},
                           {"entityType", "investment"},
                           {"filings", QJsonObject{{"recent", filing_arrays(recent)}, {"files", files}}}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

inline QByteArray submissions_page_json(const QVector<FilingRow>& rows) {
    return QJsonDocument(filing_arrays(rows)).toJson(QJsonDocument::Compact);
}

struct IndexRow {
    QString accession;
    QString form;
    QString filing_date;
};

inline QByteArray series_index_atom(const QString& cik10, const QString& name, const QVector<IndexRow>& rows) {
    QString x = QStringLiteral("<?xml version=\"1.0\" encoding=\"ISO-8859-1\" ?>\n"
                               "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n"
                               "  <author><email>webmaster@sec.gov</email><name>Webmaster</name></author>\n");
    if (!cik10.isEmpty())
        x += QStringLiteral("  <company-info>\n    <cik>%1</cik>\n    <conformed-name>%2</conformed-name>\n"
                            "  </company-info>\n")
                 .arg(cik10, name);
    for (const IndexRow& r : rows) {
        x += QStringLiteral(
                 "  <entry>\n    <category label=\"form type\" scheme=\"https://www.sec.gov/\" term=\"%1\" />\n"
                 "    <content type=\"text/xml\">\n      <accession-number>%2</accession-number>\n"
                 "      <act>40</act>\n      <filing-date>%3</filing-date>\n"
                 "      <filing-type>%1</filing-type>\n    </content>\n"
                 "    <updated>%3T10:00:00-04:00</updated>\n  </entry>\n")
                 .arg(r.form, r.accession, r.filing_date);
    }
    x += QStringLiteral("</feed>\n");
    return x.toLatin1();
}

// ── IBKR ─────────────────────────────────────────────────────────────────────

struct BarSpec {
    QString date; ///< yyyyMMdd as IBKR reports a daily bar
    double open = 100.0;
    double high = 101.0;
    double low = 99.0;
    double close = 100.5;
    double volume = 1000000.0;
};

/// A usable wrapper history envelope with synthetic bars.
inline QJsonObject ibkr_history_envelope(const QString& symbol, qint64 con_id, const QVector<BarSpec>& bars,
                                         const QString& end_date_time, const QString& duration = QStringLiteral("2 Y"),
                                         const QString& status = QStringLiteral("OK"), bool usable = true) {
    QJsonArray rows;
    for (const BarSpec& b : bars) {
        const QDate d = QDate::fromString(b.date, QStringLiteral("yyyyMMdd"));
        rows.append(QJsonObject{{"date", b.date},
                                {"timestamp", static_cast<double>(d.startOfDay(QTimeZone::UTC).toSecsSinceEpoch())},
                                {"open", b.open},
                                {"high", b.high},
                                {"low", b.low},
                                {"close", b.close},
                                {"volume", b.volume},
                                {"wap", (b.high + b.low) / 2}});
    }
    return QJsonObject{{"source", "ibkr_tws"},
                       {"command", "history"},
                       {"ok", true},
                       {"retrieved_at", "2026-09-26T09:00:00Z"},
                       {"adapter", QJsonObject{{"commit", "4a3c606e"},
                                               {"ibapi_version", "10.45.01"},
                                               {"tws_version", "10.50.1e"},
                                               {"client_id", 71},
                                               {"uses_official_runtime", "true"},
                                               {"ibapi_runtime_path_verified", "true"}}},
                       {"symbol", symbol},
                       {"contract", QJsonObject{{"con_id", static_cast<double>(con_id)},
                                                {"symbol", symbol},
                                                {"security_type", "STK"},
                                                {"exchange", "SMART"},
                                                {"primary_exchange", "ARCA"},
                                                {"currency", "USD"}}},
                       {"parameters", QJsonObject{{"end_date_time", end_date_time},
                                                  {"duration", duration},
                                                  {"bar_size", "1 day"},
                                                  {"what_to_show", "TRADES"},
                                                  {"use_rth", true}}},
                       {"bars", usable ? rows : QJsonArray()},
                       {"classification",
                        QJsonObject{{"usable", usable},
                                    {"feed", usable ? QJsonValue("HISTORICAL") : QJsonValue()},
                                    {"status", status},
                                    {"entitlement", status == QLatin1String("NOT_ENTITLED") ? "BLOCKED" : "AVAILABLE"},
                                    {"validation_reason", usable ? QStringLiteral("OK") : status},
                                    {"error_message", usable ? QJsonValue() : QJsonValue(status)}}}};
}

/// A typed wrapper failure (TWS not running, a killed process, ...).
inline QJsonObject ibkr_failure_envelope(const QString& type, const QString& stage, const QString& message) {
    return QJsonObject{{"source", "ibkr_tws"},
                       {"command", "history"},
                       {"ok", false},
                       {"retrieved_at", "2026-09-26T09:00:00Z"},
                       {"failure", QJsonObject{{"type", type}, {"stage", stage}, {"message", message}}}};
}

/// One synthetic bar for every calendar session from `first` to `last`.
inline QVector<BarSpec> bars_for_sessions(const QDate& first, const QDate& last) {
    QVector<BarSpec> out;
    for (const auto& d : fincept::services::etf::UsEquityCalendar::weekdays_in(first, last)) {
        if (!d.is_session())
            continue;
        BarSpec b;
        b.date = d.date.toString(QStringLiteral("yyyyMMdd"));
        out.append(b);
    }
    return out;
}

} // namespace etf_fixtures
