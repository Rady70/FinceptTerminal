// src/services/etf/SecEdgarParse.h
//
// The SEC EDGAR boundary of the ETF data foundation (ETF Capital Flows Batch
// B): request construction, the declared User-Agent rule, and the parsers for
// the three documents the enabled N-PORT route reads (A2 sections 3, 5.3, 6.1):
//
//   * the registrant's submissions JSON (data.sec.gov/submissions), which is
//     where each filing's exact acceptance time (`acceptanceDateTime`, UTC)
//     comes from, including its older pages;
//   * the EDGAR series filing index (the company-browse Atom output for a
//     series id, as Batch A used it), which lists one series' N-PORT filings
//     inside a registrant that files for hundreds of series;
//   * the NPORT-P / NPORT-P/A primary document (primary_doc.xml).
//
// Parsers never repair data. An absent element or attribute is Missing, a
// present value that is not a decimal is Unparseable with its raw text, and a
// document whose structure cannot be trusted is refused as a whole.
//
// Header-only over Qt Core (QJsonDocument, QXmlStreamReader).
#pragma once
#include "services/etf/EtfDataModel.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QXmlStreamReader>

#include <algorithm>

namespace fincept::services::etf {

// ── Identifiers and URLs ─────────────────────────────────────────────────────

/// A CIK as the SEC writes it in URLs and filings: 10 digits, zero padded.
/// Returns empty for anything that is not 1-10 digits with a non-zero value.
inline QString sec_normalized_cik(const QString& cik) {
    static const QRegularExpression kDigits(QStringLiteral("^\\d{1,10}$"));
    const QString t = cik.trimmed();
    if (!kDigits.match(t).hasMatch())
        return {};
    bool ok = false;
    const qulonglong v = t.toULongLong(&ok);
    if (!ok || v == 0)
        return {};
    return QStringLiteral("%1").arg(v, 10, 10, QLatin1Char('0'));
}

inline bool sec_valid_series_id(const QString& s) {
    static const QRegularExpression kSeries(QStringLiteral("^S\\d{9}$"));
    return kSeries.match(s).hasMatch();
}

inline bool sec_valid_class_id(const QString& s) {
    static const QRegularExpression kClass(QStringLiteral("^C\\d{9}$"));
    return kClass.match(s).hasMatch();
}

inline bool sec_valid_accession(const QString& a) {
    static const QRegularExpression kAccession(QStringLiteral("^\\d{10}-\\d{2}-\\d{6}$"));
    return kAccession.match(a).hasMatch();
}

inline QString sec_submissions_url(const QString& cik10) {
    return QStringLiteral("https://data.sec.gov/submissions/CIK%1.json").arg(cik10);
}

/// An older submissions page named by the main document's `filings.files`.
/// Empty when the name is not exactly the SEC's page-name pattern.
inline QString sec_submissions_page_url(const QString& page_name) {
    static const QRegularExpression kPage(QStringLiteral("^CIK\\d{10}-submissions-\\d{3}\\.json$"));
    if (!kPage.match(page_name).hasMatch())
        return {};
    return QStringLiteral("https://data.sec.gov/submissions/%1").arg(page_name);
}

/// The series filing index: the NPORT-P filings (and, by prefix, NPORT-P/A)
/// of one series, newest first, at most 40.
inline QString sec_series_index_url(const QString& series_id) {
    return QStringLiteral("https://www.sec.gov/cgi-bin/browse-edgar?action=getcompany&CIK=%1&type=NPORT-P"
                          "&dateb=&owner=include&count=40&output=atom")
        .arg(series_id);
}

/// The filing's XML primary document. The submissions `primaryDocument`
/// (xslFormNPORT-P_X01/primary_doc.xml) is a rendered view; the filed XML is
/// primary_doc.xml at the accession folder root (A2 sec_role_probe.py).
inline QString sec_nport_primary_doc_url(const QString& cik10, const QString& accession) {
    bool ok = false;
    const qulonglong cik = cik10.toULongLong(&ok);
    QString folder = accession;
    folder.remove(QLatin1Char('-'));
    return QStringLiteral("https://www.sec.gov/Archives/edgar/data/%1/%2/primary_doc.xml")
        .arg(ok ? cik : 0)
        .arg(folder);
}

// ── Declared User-Agent (A2 review finding 4) ────────────────────────────────

inline const QRegularExpression& sec_contact_pattern() {
    static const QRegularExpression kContact(QStringLiteral("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}"));
    return kContact;
}

/// The SEC asks automated clients to declare a name or organisation and an
/// administrative contact e-mail. Same rule as A2's sec_role_probe.py.
inline bool sec_user_agent_valid(const QString& ua) {
    for (const QChar c : ua) {
        if (c.unicode() < 32 || c.unicode() == 127)
            return false;
    }
    const QString trimmed = ua.trimmed();
    if (!sec_contact_pattern().match(trimmed).hasMatch())
        return false;
    QString name = trimmed;
    name.remove(sec_contact_pattern());
    static const QRegularExpression kEdge(QStringLiteral("^[\\s;,()<>]+|[\\s;,()<>]+$"));
    name.remove(kEdge);
    return name.size() >= 2;
}

/// The User-Agent as stored with a retrieval: the contact e-mail replaced.
inline QString sec_user_agent_redacted(const QString& ua) {
    QString out = ua;
    out.replace(sec_contact_pattern(), QStringLiteral("<contact>"));
    return out;
}

// ── Submissions JSON ─────────────────────────────────────────────────────────

struct SecFilingRef {
    QString accession;
    QString form;
    QDate filing_date;
    QString report_date; ///< as listed ("" when the SEC lists none)
    QString accepted_at_raw;
    QDateTime accepted_at; ///< UTC; invalid when absent or unparseable
    QString primary_document;
};

struct SecOlderPage {
    QString name;
    QDate filing_from;
    QDate filing_to;
};

struct SecSubmissions {
    bool ok = false;
    QString error;
    QString cik; ///< 10 digits (main document only)
    QString name;
    QVector<SecFilingRef> filings;
    QVector<SecOlderPage> older_pages;
};

/// "2026-08-28T12:25:47.000Z" → UTC instant. Only an explicit UTC designator
/// is accepted: a local or offset-less time would be a guess.
inline QDateTime sec_parse_acceptance(const QString& raw) {
    const QString t = raw.trimmed();
    if (!t.endsWith(QLatin1Char('Z')))
        return {};
    QDateTime dt = QDateTime::fromString(t, Qt::ISODateWithMs);
    if (!dt.isValid())
        dt = QDateTime::fromString(t, Qt::ISODate);
    if (!dt.isValid())
        return {};
    return dt.toUTC();
}

/// Parse a submissions document. `older_page` documents carry the filing
/// arrays at the top level; the main document nests them under
/// filings.recent and lists the older pages under filings.files.
inline SecSubmissions parse_sec_submissions(const QByteArray& body, bool older_page) {
    SecSubmissions out;
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        out.error = QStringLiteral("submissions_not_json: %1").arg(perr.errorString());
        return out;
    }
    const QJsonObject root = doc.object();
    QJsonObject arrays;
    if (older_page) {
        arrays = root;
    } else {
        out.cik = sec_normalized_cik(root.value(QLatin1String("cik")).toVariant().toString());
        out.name = root.value(QLatin1String("name")).toString();
        const QJsonObject filings = root.value(QLatin1String("filings")).toObject();
        if (!filings.value(QLatin1String("recent")).isObject()) {
            out.error = QStringLiteral("submissions_without_recent_filings");
            return out;
        }
        arrays = filings.value(QLatin1String("recent")).toObject();
        for (const QJsonValue& v : filings.value(QLatin1String("files")).toArray()) {
            const QJsonObject f = v.toObject();
            SecOlderPage page;
            page.name = f.value(QLatin1String("name")).toString();
            page.filing_from = QDate::fromString(f.value(QLatin1String("filingFrom")).toString(), Qt::ISODate);
            page.filing_to = QDate::fromString(f.value(QLatin1String("filingTo")).toString(), Qt::ISODate);
            if (!page.name.isEmpty())
                out.older_pages.append(page);
        }
    }
    static const char* kRequired[] = {"accessionNumber",    "form",           "filingDate", "reportDate",
                                      "acceptanceDateTime", "primaryDocument"};
    qsizetype n = -1;
    for (const char* key : kRequired) {
        const QJsonValue v = arrays.value(QLatin1String(key));
        if (!v.isArray()) {
            out.error = QStringLiteral("submissions_missing_array: %1").arg(QLatin1String(key));
            return out;
        }
        const qsizetype len = v.toArray().size();
        if (n >= 0 && len != n) {
            out.error = QStringLiteral("submissions_ragged_arrays: %1").arg(QLatin1String(key));
            return out;
        }
        n = len;
    }
    const QJsonArray acc = arrays.value(QLatin1String("accessionNumber")).toArray();
    const QJsonArray form = arrays.value(QLatin1String("form")).toArray();
    const QJsonArray fdate = arrays.value(QLatin1String("filingDate")).toArray();
    const QJsonArray rdate = arrays.value(QLatin1String("reportDate")).toArray();
    const QJsonArray accepted = arrays.value(QLatin1String("acceptanceDateTime")).toArray();
    const QJsonArray primary = arrays.value(QLatin1String("primaryDocument")).toArray();
    for (qsizetype i = 0; i < n; ++i) {
        SecFilingRef r;
        r.accession = acc.at(i).toString();
        r.form = form.at(i).toString();
        r.filing_date = QDate::fromString(fdate.at(i).toString(), Qt::ISODate);
        r.report_date = rdate.at(i).toString();
        r.accepted_at_raw = accepted.at(i).toString();
        r.accepted_at = sec_parse_acceptance(r.accepted_at_raw);
        r.primary_document = primary.at(i).toString();
        out.filings.append(r);
    }
    out.ok = true;
    return out;
}

// ── Series filing index (EDGAR company-browse Atom) ──────────────────────────

struct SecIndexEntry {
    QString accession;
    QString form;
    QDate filing_date;
};

struct SecSeriesIndex {
    bool ok = false;
    QString error;
    QString company_cik; ///< 10 digits: the registrant that files for the series
    QString company_name;
    QVector<SecIndexEntry> entries;
};

inline SecSeriesIndex parse_sec_series_index(const QByteArray& body) {
    SecSeriesIndex out;
    QXmlStreamReader xml(body);
    QStringList path;
    SecIndexEntry current;
    bool saw_feed = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            if (path.isEmpty()) {
                if (name != QLatin1String("feed")) {
                    out.error = QStringLiteral("index_not_atom_feed: root <%1>").arg(name);
                    return out;
                }
                saw_feed = true;
            }
            path.append(name);
            const QString joined = path.join(QLatin1Char('/'));
            if (joined == QLatin1String("feed/entry")) {
                current = SecIndexEntry();
            } else if (joined == QLatin1String("feed/company-info/cik") ||
                       joined == QLatin1String("feed/company-info/conformed-name") ||
                       joined == QLatin1String("feed/entry/content/accession-number") ||
                       joined == QLatin1String("feed/entry/content/filing-type") ||
                       joined == QLatin1String("feed/entry/content/filing-date")) {
                const QString text = xml.readElementText().trimmed();
                if (joined.endsWith(QLatin1String("/cik")))
                    out.company_cik = sec_normalized_cik(text);
                else if (joined.endsWith(QLatin1String("conformed-name")))
                    out.company_name = text;
                else if (joined.endsWith(QLatin1String("accession-number")))
                    current.accession = text;
                else if (joined.endsWith(QLatin1String("filing-type")))
                    current.form = text;
                else
                    current.filing_date = QDate::fromString(text, Qt::ISODate);
                path.removeLast(); // readElementText() consumed the end element
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (path.join(QLatin1Char('/')) == QLatin1String("feed/entry") && !current.accession.isEmpty())
                out.entries.append(current);
            if (!path.isEmpty())
                path.removeLast();
        }
    }
    if (xml.hasError()) {
        out.error = QStringLiteral("index_xml_malformed: %1 (line %2)").arg(xml.errorString()).arg(xml.lineNumber());
        return out;
    }
    if (!saw_feed) {
        out.error = QStringLiteral("index_not_atom_feed: empty document");
        return out;
    }
    if (out.company_cik.isEmpty()) {
        // EDGAR answers an unknown series with no company block: the identity
        // requested is not known to the SEC, which is not the same as a series
        // with no filings yet.
        out.error = QStringLiteral("index_without_company: the SEC returned no registrant for this series id");
        return out;
    }
    out.ok = true;
    return out;
}

// ── N-PORT primary document ──────────────────────────────────────────────────

struct NportFlowMonth {
    int month_index = 0;          ///< 1..3 (month 3 is the report-period month)
    bool element_present = false; ///< the <monNFlow> element itself was present
    FieldValue sales;
    FieldValue redemption;
    FieldValue reinvestment;
};

struct NportDocument {
    bool ok = false;
    QString error;
    QString submission_type;       ///< NPORT-P or NPORT-P/A as written in headerData
    QString amended_accession;     ///< headerData/accessionNumber: the filing an NPORT-P/A amends
    QStringList header_series_ids; ///< headerData/filerInfo/seriesClassInfo/seriesId
    QStringList header_class_ids;  ///< headerData/filerInfo/seriesClassInfo/classId
    QString reg_cik;               ///< genInfo/regCik, normalized; empty when absent
    QString reg_name;
    QString reg_file_number;
    QString reg_lei;
    QString series_name;
    QString series_id; ///< genInfo/seriesId; empty when the registrant reports without a series
    QString series_lei;
    QString rep_pd_end_raw;  ///< fiscal year end
    QString rep_pd_date_raw; ///< the report period date
    QDate rep_pd_date;
    QString is_final_filing; ///< as written ("Y" / "N"), empty when absent
    FieldValue net_assets;
    bool returns_block_present = false;
    QStringList return_class_ids; ///< classId of each monthlyTotReturn, sorted, unique
    NportFlowMonth months[3];
};

namespace nport_detail {

inline FieldValue attribute_value(const QXmlStreamAttributes& attrs, const char* name) {
    const bool present = attrs.hasAttribute(QLatin1String(name));
    return parse_decimal_field(present ? attrs.value(QLatin1String(name)).toString() : QString(), present);
}

/// The genInfo leaves read as text. Other genInfo children (for example
/// regStateConditional, which carries attributes) are skipped rather than read
/// as text, so an unexpected child element cannot fail the whole document.
inline bool is_gen_info_text_field(const QString& name) {
    static const QStringList kFields = {QStringLiteral("regCik"),        QStringLiteral("regName"),
                                        QStringLiteral("regFileNumber"), QStringLiteral("regLei"),
                                        QStringLiteral("seriesName"),    QStringLiteral("seriesId"),
                                        QStringLiteral("seriesLei"),     QStringLiteral("repPdEnd"),
                                        QStringLiteral("repPdDate"),     QStringLiteral("isFinalFiling")};
    return kFields.contains(name);
}

} // namespace nport_detail

inline NportDocument parse_nport_primary_doc(const QByteArray& body) {
    static const QString kNportNamespace = QStringLiteral("http://www.sec.gov/edgar/nport");
    NportDocument out;
    for (int i = 0; i < 3; ++i)
        out.months[i].month_index = i + 1;
    QXmlStreamReader xml(body);
    QStringList path;
    bool net_assets_seen = false;
    bool saw_root = false;
    QSet<QString> return_classes;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            if (path.isEmpty()) {
                if (name != QLatin1String("edgarSubmission") || xml.namespaceUri() != kNportNamespace) {
                    out.error = QStringLiteral("not_an_nport_document: root <%1> in namespace '%2'")
                                    .arg(name, xml.namespaceUri().toString());
                    return out;
                }
                saw_root = true;
            }
            path.append(name);
            const QString joined = path.join(QLatin1Char('/'));
            static const QString kGen = QStringLiteral("edgarSubmission/formData/genInfo/");
            static const QString kFund = QStringLiteral("edgarSubmission/formData/fundInfo/");
            auto take_text = [&xml, &path]() {
                const QString text = xml.readElementText();
                path.removeLast(); // readElementText() consumed the end element
                return text;
            };
            if (joined == QLatin1String("edgarSubmission/headerData/submissionType")) {
                out.submission_type = take_text().trimmed();
            } else if (joined == QLatin1String("edgarSubmission/headerData/accessionNumber")) {
                out.amended_accession = take_text().trimmed();
            } else if (joined == QLatin1String("edgarSubmission/headerData/filerInfo/seriesClassInfo/seriesId")) {
                out.header_series_ids.append(take_text().trimmed());
            } else if (joined == QLatin1String("edgarSubmission/headerData/filerInfo/seriesClassInfo/classId")) {
                out.header_class_ids.append(take_text().trimmed());
            } else if (joined.startsWith(kGen) && path.size() == 4 && nport_detail::is_gen_info_text_field(name)) {
                const QString text = take_text().trimmed();
                if (name == QLatin1String("regCik"))
                    out.reg_cik = sec_normalized_cik(text);
                else if (name == QLatin1String("regName"))
                    out.reg_name = text;
                else if (name == QLatin1String("regFileNumber"))
                    out.reg_file_number = text;
                else if (name == QLatin1String("regLei"))
                    out.reg_lei = text;
                else if (name == QLatin1String("seriesName"))
                    out.series_name = text;
                else if (name == QLatin1String("seriesId"))
                    out.series_id = text;
                else if (name == QLatin1String("seriesLei"))
                    out.series_lei = text;
                else if (name == QLatin1String("repPdEnd"))
                    out.rep_pd_end_raw = text;
                else if (name == QLatin1String("repPdDate"))
                    out.rep_pd_date_raw = text;
                else if (name == QLatin1String("isFinalFiling"))
                    out.is_final_filing = text;
            } else if (joined == kFund + QLatin1String("netAssets")) {
                out.net_assets = parse_decimal_field(take_text(), true);
                net_assets_seen = true;
            } else if (joined == kFund + QLatin1String("returnInfo/monthlyTotReturns")) {
                out.returns_block_present = true;
            } else if (joined == kFund + QLatin1String("returnInfo/monthlyTotReturns/monthlyTotReturn")) {
                const QString cls = xml.attributes().value(QLatin1String("classId")).toString().trimmed();
                if (!cls.isEmpty())
                    return_classes.insert(cls);
            } else if (path.size() == 4 && joined.startsWith(kFund) &&
                       (name == QLatin1String("mon1Flow") || name == QLatin1String("mon2Flow") ||
                        name == QLatin1String("mon3Flow"))) {
                const int index = name.at(3).digitValue() - 1;
                NportFlowMonth& m = out.months[index];
                m.element_present = true;
                const QXmlStreamAttributes attrs = xml.attributes();
                m.sales = nport_detail::attribute_value(attrs, "sales");
                m.redemption = nport_detail::attribute_value(attrs, "redemption");
                m.reinvestment = nport_detail::attribute_value(attrs, "reinvestment");
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (!path.isEmpty())
                path.removeLast();
        }
    }
    if (xml.hasError()) {
        out.error = QStringLiteral("nport_xml_malformed: %1 (line %2)").arg(xml.errorString()).arg(xml.lineNumber());
        return out;
    }
    if (!saw_root) {
        out.error = QStringLiteral("not_an_nport_document: empty document");
        return out;
    }
    if (!net_assets_seen)
        out.net_assets = FieldValue::missing();
    out.return_class_ids = QStringList(return_classes.begin(), return_classes.end());
    std::sort(out.return_class_ids.begin(), out.return_class_ids.end());
    out.rep_pd_date = QDate::fromString(out.rep_pd_date_raw, Qt::ISODate);
    out.ok = true;
    return out;
}

/// The calendar month a monthly flow element covers: month 3 is the month of
/// the report period date, month 2 the month before, month 1 the one before
/// that (the mapping Batch A reconciled in 198 of 225 fund-months). Returns an
/// invalid date pair when the report period date is invalid.
inline QPair<QDate, QDate> nport_flow_month_bounds(const QDate& rep_pd_date, int month_index) {
    if (!rep_pd_date.isValid() || month_index < 1 || month_index > 3)
        return {QDate(), QDate()};
    const QDate first_of_report_month(rep_pd_date.year(), rep_pd_date.month(), 1);
    const QDate start = first_of_report_month.addMonths(month_index - 3);
    const QDate end = start.addMonths(1).addDays(-1);
    return {start, end};
}

/// True when the report period date is the last day of its month. The monthly
/// mapping above is only defined for month-end report dates; any other date is
/// refused rather than mapped by guesswork.
inline bool nport_report_date_is_month_end(const QDate& rep_pd_date) {
    return rep_pd_date.isValid() && rep_pd_date.day() == rep_pd_date.daysInMonth();
}

} // namespace fincept::services::etf
