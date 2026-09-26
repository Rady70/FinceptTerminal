#include "services/etf/EtfIbkrDailyIngestor.h"

#include "core/logging/Logger.h"
#include "services/etf/EtfRoutePolicy.h"
#include "services/etf/EtfTiming.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace fincept::services::etf {

namespace {

constexpr const char* kIbkrIngestTag = "EtfIbkrIngest";

/// The runtime identity stored with a retrieval: the adapter pin and the
/// observed ibapi / TWS versions and client id. Never an account identifier:
/// the wrapper discards the account list TWS sends and never reports one.
QString ibkr_runtime_identity(const QJsonObject& adapter) {
    QJsonObject o;
    for (const char* key : {"commit", "adapter_sha256", "ibapi_version", "tws_version", "client_id", "host", "port"}) {
        const QJsonValue v = adapter.value(QLatin1String(key));
        if (!v.isUndefined() && !v.isNull())
            o.insert(QLatin1String(key), v);
    }
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

} // namespace

QJsonObject IbkrDailyRunSummary::to_json() const {
    QJsonObject o;
    o["run_id"] = run_id;
    o["status"] = QLatin1String(retrieval_status_id(status));
    o["detail_code"] = detail_code;
    o["detail"] = detail;
    o["symbol"] = symbol;
    o["con_id"] = con_id;
    o["instrument_id"] = instrument_id;
    o["requested_last_session"] = requested_last_session;
    o["bars_returned"] = bars_returned;
    o["bars_accepted"] = bars_accepted;
    o["observations_inserted"] = observations_inserted;
    o["observations_revised"] = observations_revised;
    o["observations_confirmed"] = observations_confirmed;
    o["observations_already_recorded"] = observations_already_recorded;
    o["missing_sessions"] = missing_sessions;
    o["in_progress_rejected"] = in_progress_rejected;
    o["other_issues"] = other_issues;
    return o;
}

EtfIbkrDailyIngestor::EtfIbkrDailyIngestor(IbkrDailyFetch fetch, bool configured, Clock clock, QObject* parent)
    : QObject(parent), fetch_(std::move(fetch)), configured_(configured), clock_(std::move(clock)) {}

bool EtfIbkrDailyIngestor::valid_symbol(const QString& symbol) {
    static const QRegularExpression kSymbol(QStringLiteral("^[A-Z0-9][A-Z0-9.\\-]{0,11}$"));
    return kSymbol.match(symbol).hasMatch();
}

bool EtfIbkrDailyIngestor::valid_duration(const QString& duration) {
    static const QRegularExpression kDuration(QStringLiteral("^([1-9]\\d{0,3}) ([DWMY])$"));
    const auto m = kDuration.match(duration);
    if (!m.hasMatch())
        return false;
    return m.captured(2) != QLatin1String("Y") || m.captured(1).toInt() <= 7;
}

qint64 EtfIbkrDailyIngestor::record_retrieval(RetrievalStatus status, const QString& code, const QString& detail,
                                              const QString& request_ref, const QDateTime& requested_at,
                                              const QDateTime& retrieved_at, const QJsonObject* payload) {
    etf_store::RetrievalRecord r;
    r.run_id = summary_.run_id;
    r.source_type = SourceType::IbkrTwsReadonly;
    r.acquisition_mode = AcquisitionMode::IbkrReadonlyWrapper;
    r.endpoint = payload ? QStringLiteral("ibkr_history_daily") : QStringLiteral("refused_before_request");
    r.request_ref = request_ref;
    r.subject_ref = summary_.symbol;
    r.requested_at = requested_at;
    r.status = status;
    r.detail_code = code;
    r.detail = detail;
    if (payload) {
        const QByteArray canonical = QJsonDocument(*payload).toJson(QJsonDocument::Compact);
        r.retrieved_at = retrieved_at;
        r.response_sha256 =
            QString::fromLatin1(QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
        r.response_bytes = canonical.size();
        r.runtime_identity = ibkr_runtime_identity(payload->value(QLatin1String("adapter")).toObject());
    }
    r.interpretation = QLatin1String(kIbkrDailyInterpretation) + QLatin1Char('+') + QLatin1String(kIbkrRequestEndRule);
    r.route_policy = QLatin1String(kEtfRoutePolicyVersion);
    r.calendar_version = QLatin1String(kUsEquityCalendarVersion);
    auto id = EtfDataRepository::instance().record_retrieval(r);
    if (id.is_err()) {
        LOG_ERROR(kIbkrIngestTag, QString("could not record retrieval: %1").arg(QString::fromStdString(id.error())));
        return 0;
    }
    return id.value();
}

void EtfIbkrDailyIngestor::run(const QString& symbol, const QString& duration, Done done) {
    done_ = std::move(done);
    summary_ = IbkrDailyRunSummary();
    summary_.run_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    summary_.symbol = symbol.trimmed().toUpper();

    if (!valid_symbol(summary_.symbol) || !valid_duration(duration)) {
        finish(RetrievalStatus::SourceError, QStringLiteral("invalid_request"),
               QStringLiteral("a symbol (A-Z, 0-9, '.', '-') and a duration '<n> D|W|M|Y' (at most 7 Y) are "
                              "required"));
        return;
    }
    const RouteDecision route = acquisition_route(SourceType::IbkrTwsReadonly, AcquisitionMode::IbkrReadonlyWrapper);
    const QDateTime requested_at = clock_();
    if (!route.enabled) {
        record_retrieval(RetrievalStatus::RouteDisabled, QStringLiteral("route_disabled"), route.reason,
                         QStringLiteral("none"), requested_at, QDateTime(), nullptr);
        finish(RetrievalStatus::RouteDisabled, QStringLiteral("route_disabled"), route.reason);
        return;
    }
    if (!configured_) {
        const QString detail = QStringLiteral("no local ibkr_tws.json: the read-only IBKR path is not configured");
        record_retrieval(RetrievalStatus::NotConfigured, QStringLiteral("ibkr_not_configured"), detail,
                         QStringLiteral("none"), requested_at, QDateTime(), nullptr);
        finish(RetrievalStatus::NotConfigured, QStringLiteral("ibkr_not_configured"), detail);
        return;
    }
    const auto last = ibkr_last_requestable_session(requested_at);
    if (!last) {
        const QString detail = QStringLiteral("the session calendar cannot name the last completed session");
        record_retrieval(RetrievalStatus::SourceError, QStringLiteral("calendar_unavailable"), detail,
                         QStringLiteral("none"), requested_at, QDateTime(), nullptr);
        finish(RetrievalStatus::SourceError, QStringLiteral("calendar_unavailable"), detail);
        return;
    }
    IbkrDailyRequest req;
    req.symbol = summary_.symbol;
    req.duration = duration;
    req.end_date_time = ibkr_end_date_time_for(last->date);
    summary_.requested_last_session = last->date.toString(Qt::ISODate);
    const QString request_ref = QStringLiteral("history %1 duration='%2' bar_size='%3' what_to_show=%4 use_rth=true "
                                               "end='%5'")
                                    .arg(req.symbol, req.duration, QLatin1String(kIbkrBarSize),
                                         QLatin1String(kIbkrWhatToShow), req.end_date_time);
    const QDate last_session = last->date;
    QPointer<EtfIbkrDailyIngestor> self(this);
    fetch_(req, [self, req, request_ref, requested_at, last_session](const QJsonObject& payload) {
        if (!self || self->finished_)
            return;
        const QDateTime retrieved_at = self->clock_();
        const IbkrDailyEnvelope envelope = parse_ibkr_daily_envelope(payload);
        const IbkrDailyAssessment assessment =
            assess_ibkr_daily(envelope, req.symbol, req.duration, req.end_date_time, last_session, requested_at);
        self->summary_.bars_returned = envelope.bars.size() + envelope.malformed_bars;
        self->summary_.con_id = envelope.con_id;
        const qint64 retrieval_id = self->record_retrieval(assessment.status, assessment.detail_code, assessment.detail,
                                                           request_ref, requested_at, retrieved_at, &payload);
        if (retrieval_id <= 0) {
            self->finish(RetrievalStatus::SourceError, QStringLiteral("storage_error"),
                         QStringLiteral("the retrieval could not be recorded"));
            return;
        }
        // Accepted bars, and the issues found in a usable response (an
        // in-progress bar, a missing session), are stored with the instrument.
        // A refused or failed response is recorded by its retrieval row alone.
        if (!assessment.accepted.isEmpty() || !assessment.issues.isEmpty())
            self->persist(envelope, assessment, retrieval_id, requested_at, retrieved_at);
        self->finish(assessment.status, assessment.detail_code, assessment.detail);
    });
}

void EtfIbkrDailyIngestor::persist(const IbkrDailyEnvelope& envelope, const IbkrDailyAssessment& assessment,
                                   qint64 retrieval_id, const QDateTime& requested_at, const QDateTime& seen_at) {
    auto& repo = EtfDataRepository::instance();
    auto& db = Database::instance();
    auto begin = db.begin_transaction();
    if (begin.is_err()) {
        summary_.other_issues++;
        return;
    }
    auto fail = [&](const std::string& what) {
        db.rollback();
        LOG_ERROR(kIbkrIngestTag,
                  QString("could not store %1 bars: %2").arg(summary_.symbol, QString::fromStdString(what)));
        etf_store::IssueRecord issue;
        issue.state = QualityState::SourceError;
        issue.code = QStringLiteral("storage_error");
        issue.detail = QString::fromStdString(what);
        repo.record_issue(retrieval_id, issue);
        summary_.other_issues++;
        summary_.observations_inserted = summary_.observations_revised = summary_.observations_confirmed = 0;
        summary_.bars_accepted = 0;
    };

    etf_store::ListedInstrumentFacts facts;
    facts.con_id = envelope.con_id;
    facts.symbol = envelope.contract_symbol.toUpper();
    facts.security_type = envelope.security_type;
    facts.exchange = envelope.exchange;
    facts.primary_exchange = envelope.primary_exchange;
    facts.currency = envelope.currency;
    auto instrument = repo.upsert_listed_instrument(facts, seen_at);
    if (instrument.is_err())
        return fail(instrument.error());
    summary_.instrument_id = instrument.value();

    QDateTime observation_start;
    if (!assessment.accepted.isEmpty()) {
        auto start = repo.observation_start(SourceType::IbkrTwsReadonly, SubjectType::ListedInstrument,
                                            instrument.value(), requested_at, retrieval_id);
        if (start.is_err())
            return fail(start.error());
        observation_start = start.value();
        auto sessions = repo.upsert_sessions(assessment.window_days);
        if (sessions.is_err())
            return fail(sessions.error());
    }

    for (const IbkrDailyIssue& i : assessment.issues) {
        etf_store::IssueRecord issue;
        issue.subject_type = SubjectType::ListedInstrument;
        issue.subject_id = instrument.value();
        issue.effective_date = i.session_date;
        issue.state = i.state;
        issue.code = i.code;
        issue.detail = i.detail;
        auto r = repo.record_issue(retrieval_id, issue);
        if (r.is_err())
            return fail(r.error());
        if (i.state == QualityState::Missing)
            summary_.missing_sessions++;
        else if (i.state == QualityState::InProgressSession)
            summary_.in_progress_rejected++;
        else
            summary_.other_issues++;
    }

    const QString price_units = envelope.currency + QStringLiteral("_per_share");
    for (const IbkrDailyBarRow& bar : assessment.accepted) {
        const std::pair<const char*, const FieldValue*> fields[] = {{"bar_open", &bar.open},
                                                                    {"bar_high", &bar.high},
                                                                    {"bar_low", &bar.low},
                                                                    {"bar_close", &bar.close},
                                                                    {"bar_volume", &bar.volume}};
        for (const auto& [measure, value] : fields) {
            const bool is_volume = value == &bar.volume;
            etf_store::ObservationInput in;
            in.subject_type = SubjectType::ListedInstrument;
            in.subject_id = instrument.value();
            in.kind = MeasurementKind::MarketBar;
            in.measure = QLatin1String(measure);
            // Volume is IBKR's filtered regular-hours count: not consolidated
            // volume, not dollars, never flow (A2 section 7.5).
            in.units = is_volume ? QStringLiteral("shares_ibkr_filtered") : price_units;
            in.basis = QStringLiteral("ibkr_trades_rth_daily_split_adjusted");
            in.source_type = SourceType::IbkrTwsReadonly;
            in.acquisition_mode = AcquisitionMode::IbkrReadonlyWrapper;
            in.effective_date = bar.session_date;
            in.value = *value;
            in.retrieval_id = retrieval_id;
            in.seen_at = seen_at;
            in.observation_start = observation_start;
            auto outcome = repo.record_observation(in);
            if (outcome.is_err())
                return fail(outcome.error());
            switch (outcome.value()) {
                case etf_store::ObservationOutcome::InsertedOriginal:
                case etf_store::ObservationOutcome::InsertedAmendment:
                    summary_.observations_inserted++;
                    break;
                case etf_store::ObservationOutcome::InsertedRevision:
                    summary_.observations_revised++;
                    break;
                case etf_store::ObservationOutcome::Confirmed:
                    summary_.observations_confirmed++;
                    break;
                case etf_store::ObservationOutcome::AlreadyRecorded:
                case etf_store::ObservationOutcome::RefusedDocumentChanged:
                    summary_.observations_already_recorded++;
                    break;
            }
        }
    }
    auto commit = db.commit();
    if (commit.is_err())
        return fail(commit.error());
    summary_.bars_accepted = assessment.accepted.size();
}

void EtfIbkrDailyIngestor::finish(RetrievalStatus status, const QString& code, const QString& detail) {
    if (finished_)
        return;
    finished_ = true;
    summary_.status = status;
    summary_.detail_code = code;
    summary_.detail = detail;
    LOG_INFO(kIbkrIngestTag,
             QString("run %1 %2 finished: %3 %4 (%5 bars accepted)")
                 .arg(summary_.run_id, summary_.symbol, QLatin1String(retrieval_status_id(status)), code)
                 .arg(summary_.bars_accepted));
    if (done_) {
        QPointer<EtfIbkrDailyIngestor> self(this);
        QTimer::singleShot(0, this, [self]() {
            if (self && self->done_)
                self->done_(self->summary_);
        });
    }
}

} // namespace fincept::services::etf
