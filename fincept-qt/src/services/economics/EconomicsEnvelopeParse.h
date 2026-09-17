// src/services/economics/EconomicsEnvelopeParse.h
//
// The economics Python scripts share one response-envelope contract:
//
//   {"success": true,  "data": ..., "error": null}      -> ok
//   {"error": {...}}                                    -> failure (CFTC shape)
//   {"error": "..."}                                    -> failure
//   {"success": false, ...}                             -> failure
//   {"success": true, "partial": true, "failed_*": ...} -> failure (partial)
//
// Composite commands (central-bank overviews, multi-series fetches) may serve
// only some of their constituents. Those envelopes carry the explicit partial
// signal (`partial: true` and/or a non-empty `failed_*` list); the application
// must never cache or present them as an ordinary complete success, so classify
// treats them as failures with the failed constituents named in the message.
// The generic `errors` key is deliberately NOT a partial signal: some scripts
// use it for benign notices.
//
// PythonRunner already turns a script-level `{"error": ...}` envelope into a
// failed PythonResult, but EconomicsService also has a cache path that never
// passed through PythonRunner: a cached payload was replayed as success and a
// stale string-only check missed object-shaped errors. The decision is
// extracted here so both paths classify identically and the real CFTC error
// shape is unit-testable without linking the Python runner, the cache or the
// DataHub (tests/ HARD RULE).
#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace fincept::services::economics_detail {

struct EnvelopeDecision {
    bool ok = true;
    bool partial = false;
    QString error;
};

/// True when `v` is an error value worth surfacing: a non-empty string, or a
/// non-empty object/array. `null`, `""`, `{}` and `[]` are not failures — the
/// same rule PythonRunner::extract_error_envelope applies to script stdout.
inline bool has_error_value(const QJsonValue& v) {
    if (v.isUndefined() || v.isNull())
        return false;
    if (v.isString())
        return !v.toString().trimmed().isEmpty();
    if (v.isObject())
        return !v.toObject().isEmpty();
    if (v.isArray())
        return !v.toArray().isEmpty();
    return false;
}

/// Human-readable message for an error value. CFTC-shaped objects carry their
/// message in a nested `error` string; anything else is rendered compactly.
inline QString error_message(const QJsonValue& v) {
    if (v.isString())
        return v.toString().trimmed();
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        const QJsonValue nested = o.value("error");
        if (nested.isString() && !nested.toString().trimmed().isEmpty())
            return nested.toString().trimmed();
        return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
    }
    if (v.isArray())
        return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    return {};
}

/// Names of the explicit partial-failure markers the composite scripts emit.
inline QString partial_failure_details(const QJsonObject& obj) {
    static const char* kKeys[] = {"failed_components", "failed_sessions", "failed_years", "failed_dates",
                                  "failed_series"};
    QStringList parts;
    for (const char* key : kKeys) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (v.isArray()) {
            for (const QJsonValue& item : v.toArray()) {
                const QString text = item.isString() ? item.toString().trimmed()
                                                     : error_message(item);
                if (!text.isEmpty())
                    parts << text;
            }
        } else if (v.isString() && !v.toString().trimmed().isEmpty()) {
            parts << v.toString().trimmed();
        }
    }
    return parts.join(QStringLiteral("; "));
}

/// Classify one economics response envelope. An error value fails even when
/// `data` is present; an explicit `success: false` fails even without one; an
/// explicit partial signal fails so partial constituent failures cannot be
/// cached or rendered as complete success.
inline EnvelopeDecision classify(const QJsonObject& obj) {
    EnvelopeDecision decision;
    const QJsonValue error = obj.value("error");
    if (has_error_value(error)) {
        decision.ok = false;
        decision.error = error_message(error);
        if (decision.error.isEmpty())
            decision.error = QStringLiteral("provider reported an error");
        // FRED (and other key-gated scripts) attach a structured error_code the
        // panel branches on; keep the existing "[CODE] message" contract.
        const QString code = obj.value("error_code").toString();
        if (!code.isEmpty())
            decision.error = QStringLiteral("[") + code + QStringLiteral("] ") + decision.error;
        return decision;
    }
    const QJsonValue success = obj.value("success");
    if (success.isBool() && !success.toBool()) {
        decision.ok = false;
        decision.error = QStringLiteral("provider reported failure");
        return decision;
    }
    const QJsonValue partial_flag = obj.value("partial");
    const bool partial = partial_flag.isBool() && partial_flag.toBool();
    const QString details = partial_failure_details(obj);
    if (partial || !details.isEmpty()) {
        decision.ok = false;
        decision.partial = true;
        decision.error = QStringLiteral("Partial provider result: ") +
                         (details.isEmpty() ? QStringLiteral("some components failed") : details);
        return decision;
    }
    return decision; // ok
}

} // namespace fincept::services::economics_detail
