// tst_economics_envelope.cpp — economics response-envelope classification.
//
// Pins the contract EconomicsService applies to both freshly fetched and
// cached payloads: a CFTC-shaped {"error": {...}} object is a failure (never a
// success, never cached as one), a plain string error is a failure, an
// explicit success:false is a failure, an explicit partial signal is a failure
// naming the failed constituents (so composite output cannot be cached or
// rendered as complete success), and a success payload whose error is
// null/absent/empty is OK. The real CFTC error envelope is reproduced here
// exactly as scripts/cftc_data.py emits it.
//
// The unit under test is src/services/economics/EconomicsEnvelopeParse.h, a
// header-only leaf over Qt Core, so this target needs no app sources (see the
// HARD RULE in tests/CMakeLists.txt).

#include "services/economics/EconomicsEnvelopeParse.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using fincept::services::economics_detail::classify;
using fincept::services::economics_detail::EnvelopeDecision;

namespace {

QJsonObject obj_from(const char* json) {
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json), &err);
    Q_ASSERT(err.error == QJsonParseError::NoError);
    return doc.object();
}

} // namespace

class TstEconomicsEnvelope : public QObject {
    Q_OBJECT

  private slots:
    void cftc_object_error_is_a_failure();
    void string_error_is_a_failure();
    void array_error_is_a_failure();
    void null_and_empty_error_values_are_not_failures();
    void success_false_is_a_failure_even_without_an_error();
    void error_code_prefix_is_preserved();
    void success_payload_with_null_error_is_ok();
    void partial_flag_is_a_failure_naming_components();
    void failed_sessions_without_flag_is_a_failure();
    void failed_years_without_flag_is_a_failure();
    void partial_flag_without_details_is_a_failure();
    void benign_errors_key_is_not_a_partial_signal();
};

// The exact shape cftc_data.py returns for every CFTCError, including the
// nested message. This is the deterministic coverage for the real envelope.
void TstEconomicsEnvelope::cftc_object_error_is_a_failure() {
    const QJsonObject o = obj_from(R"({
        "error": {"endpoint": "cot_data", "error": "No COT data found for identifier: gold",
                  "status_code": null, "timestamp": 1789075590, "type": "CFTCError"}
    })");
    const EnvelopeDecision d = classify(o);
    QVERIFY2(!d.ok, "an object-shaped provider error must not classify as success");
    QCOMPARE(d.error, QStringLiteral("No COT data found for identifier: gold"));
}

void TstEconomicsEnvelope::string_error_is_a_failure() {
    const QJsonObject o = obj_from(R"({"error": "HTTP request failed: proxy refused"})");
    const EnvelopeDecision d = classify(o);
    QVERIFY(!d.ok);
    QCOMPARE(d.error, QStringLiteral("HTTP request failed: proxy refused"));
}

void TstEconomicsEnvelope::array_error_is_a_failure() {
    const QJsonObject o = obj_from(R"({"error": ["rate limited", "retry later"]})");
    const EnvelopeDecision d = classify(o);
    QVERIFY(!d.ok);
    QVERIFY(d.error.contains(QStringLiteral("rate limited")));
}

void TstEconomicsEnvelope::null_and_empty_error_values_are_not_failures() {
    for (const char* json : {R"({"success": true, "data": {"x": 1}, "error": null})",
                             R"({"success": true, "data": {"x": 1}, "error": ""})",
                             R"({"success": true, "data": {"x": 1}, "error": {}})",
                             R"({"success": true, "data": {"x": 1}, "error": []})"}) {
        const EnvelopeDecision d = classify(obj_from(json));
        QVERIFY2(d.ok, json);
        QVERIFY(d.error.isEmpty());
    }
}

void TstEconomicsEnvelope::success_false_is_a_failure_even_without_an_error() {
    const EnvelopeDecision d = classify(obj_from(R"({"success": false, "data": []})"));
    QVERIFY(!d.ok);
    QCOMPARE(d.error, QStringLiteral("provider reported failure"));
}

void TstEconomicsEnvelope::error_code_prefix_is_preserved() {
    // FRED returns {"error": "...", "error_code": "MISSING_API_KEY"} and
    // FredPanel branches on the "[MISSING_API_KEY]" prefix.
    const EnvelopeDecision d =
        classify(obj_from(R"({"error": "FRED API key not configured.", "error_code": "MISSING_API_KEY"})"));
    QVERIFY(!d.ok);
    QCOMPARE(d.error, QStringLiteral("[MISSING_API_KEY] FRED API key not configured."));
}

void TstEconomicsEnvelope::success_payload_with_null_error_is_ok() {
    const QJsonObject o = obj_from(R"({"success": true, "data": {"records": [1, 2]}, "error": null,
                                       "parameters": {"source": "example"}})");
    const EnvelopeDecision d = classify(o);
    QVERIFY(d.ok);
    QVERIFY(!d.partial);
    QVERIFY(d.error.isEmpty());
}

// A composite action that served only some constituents must fail closed and
// name them, so the partial payload can never be cached or displayed as a
// complete success.
void TstEconomicsEnvelope::partial_flag_is_a_failure_naming_components() {
    const QJsonObject o = obj_from(R"({
        "success": true, "partial": true,
        "failed_components": ["omo: HTTP 500", "forward_rates: timeout"],
        "data": [{"component": "pribor", "date": "2026-09-15", "THREE_MONTH": 2.75}]
    })");
    const EnvelopeDecision d = classify(o);
    QVERIFY(!d.ok);
    QVERIFY(d.partial);
    QVERIFY2(d.error.startsWith(QStringLiteral("Partial provider result:")), qPrintable(d.error));
    QVERIFY(d.error.contains(QStringLiteral("omo: HTTP 500")));
    QVERIFY(d.error.contains(QStringLiteral("forward_rates: timeout")));
}

// The failed-constituent list alone is enough: a script that forgets the
// boolean flag must still fail closed.
void TstEconomicsEnvelope::failed_sessions_without_flag_is_a_failure() {
    const QJsonObject o = obj_from(R"({
        "success": true, "failed_sessions": ["1130: provider returned no rate"],
        "data": [{"session": "0900", "middle": 4.07}]
    })");
    const EnvelopeDecision d = classify(o);
    QVERIFY(!d.ok);
    QVERIFY(d.partial);
    QVERIFY(d.error.contains(QStringLiteral("1130")));
}

void TstEconomicsEnvelope::failed_years_without_flag_is_a_failure() {
    const QJsonObject o = obj_from(R"({
        "success": true, "failed_years": ["2025: HTTP 500"], "data": []
    })");
    const EnvelopeDecision d = classify(o);
    QVERIFY(!d.ok);
    QVERIFY(d.partial);
    QVERIFY(d.error.contains(QStringLiteral("2025")));
}

void TstEconomicsEnvelope::partial_flag_without_details_is_a_failure() {
    const EnvelopeDecision d =
        classify(obj_from(R"({"success": true, "partial": true, "data": [{"x": 1}]})"));
    QVERIFY(!d.ok);
    QVERIFY(d.partial);
    QVERIFY(d.error.contains(QStringLiteral("some components failed")));
}

// `errors` is a generic key some scripts use for benign notices; it must not
// make an otherwise complete payload fail.
void TstEconomicsEnvelope::benign_errors_key_is_not_a_partial_signal() {
    const EnvelopeDecision d = classify(
        obj_from(R"({"success": true, "errors": ["cache warm"], "data": [{"x": 1}]})"));
    QVERIFY(d.ok);
    QVERIFY(!d.partial);
}

QTEST_GUILESS_MAIN(TstEconomicsEnvelope)
#include "tst_economics_envelope.moc"
