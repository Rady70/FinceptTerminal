// src/services/etf/EtfIbkrDailyIngestor.h
//
// IBKR completed-session daily-bar ingestion for the ETF data foundation
// (ETF Capital Flows Batch B). One run reads one instrument's daily TRADES
// bars (regular trading hours) through the existing read-only wrapper, judges
// them against the session calendar (EtfIbkrDaily.h) and stores what was
// accepted as vintages, with the calendar days, the instrument identity and
// every issue found.
//
// The request never includes the session still trading: its end is the close
// of the last session on an exchange date before the request's own
// (kIbkrRequestEndRule), so a partial bar can only arrive if IBKR returns
// one anyway, and then it is rejected as IN_PROGRESS_SESSION.
//
// The wrapper fetch and the clock are injected: production passes the
// existing IbkrTwsService route and the wall clock; tests pass fixtures.
// Nothing here connects to TWS by itself, and nothing runs unless asked.
#pragma once
#include "services/etf/EtfDataModel.h"
#include "services/etf/EtfIbkrDaily.h"

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

namespace fincept::services::etf {

struct IbkrDailyRequest {
    QString symbol;
    QString duration;      ///< IBKR duration, e.g. "2 Y" (A2 verified 2 Y for 62 ETFs)
    QString end_date_time; ///< "yyyyMMdd 23:59:59 US/Eastern" of the last completed session
};

using IbkrDailyFetch = std::function<void(const IbkrDailyRequest&, std::function<void(const QJsonObject& payload)>)>;

struct IbkrDailyRunSummary {
    QString run_id;
    RetrievalStatus status = RetrievalStatus::SourceError;
    QString detail_code;
    QString detail;
    QString symbol;
    qint64 con_id = 0;
    qint64 instrument_id = 0;
    QString requested_last_session;
    int bars_returned = 0;
    int bars_accepted = 0;
    int observations_inserted = 0;
    int observations_revised = 0;
    int observations_confirmed = 0;
    int observations_already_recorded = 0;
    int missing_sessions = 0;
    int in_progress_rejected = 0;
    int other_issues = 0;

    QJsonObject to_json() const;
};

class EtfIbkrDailyIngestor : public QObject {
    Q_OBJECT
  public:
    using Clock = std::function<QDateTime()>;
    using Done = std::function<void(const IbkrDailyRunSummary&)>;

    /// `configured`: whether a local IBKR configuration exists. When false the
    /// run is refused as NOT_CONFIGURED before anything is requested.
    EtfIbkrDailyIngestor(IbkrDailyFetch fetch, bool configured, Clock clock, QObject* parent = nullptr);

    void run(const QString& symbol, const QString& duration, Done done);

    /// A symbol the wrapper can be asked for: 1-12 characters of A-Z, 0-9, '.', '-'.
    static bool valid_symbol(const QString& symbol);
    /// "<n> D|W|M|Y" with n >= 1 and at most seven years in any unit (2555 D,
    /// 364 W, 84 M, 7 Y), so the window stays inside the verified session
    /// calendar (which starts 2019-01-01).
    static bool valid_duration(const QString& duration);

  private:
    qint64 record_retrieval(RetrievalStatus status, const QString& code, const QString& detail,
                            const QString& request_ref, const QDateTime& requested_at, const QDateTime& retrieved_at,
                            const QJsonObject* payload);
    /// Store one usable response in one transaction. False when anything could
    /// not be stored: nothing of the response is then left behind, and `error`
    /// says why; the run must not report success.
    bool persist(const IbkrDailyEnvelope& envelope, const IbkrDailyAssessment& assessment, qint64 retrieval_id,
                 const QDateTime& requested_at, const QDateTime& seen_at, QString* error);
    void finish(RetrievalStatus status, const QString& code, const QString& detail);

    IbkrDailyFetch fetch_;
    bool configured_ = false;
    Clock clock_;
    Done done_;
    IbkrDailyRunSummary summary_;
    bool finished_ = false;
};

} // namespace fincept::services::etf
