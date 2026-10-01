#include "screens/etf/EtfResearchLoader.h"

#include "services/etf/EtfDerivedAnalytics.h"
#include "storage/repositories/EtfDataRepository.h"
#include "storage/sqlite/Database.h"

namespace fincept::screens::etf_ui {
namespace {
// A deferred read transaction pins a consistent SQLite snapshot for all reads.
// Neither this loader nor the C/D services writes or starts an ingestion route.
class ReadSnapshot {
  public:
    ReadSnapshot() : begun_(Database::instance().exec(QStringLiteral("BEGIN DEFERRED"))) {}
    ~ReadSnapshot() {
        if (begun_.is_ok())
            Database::instance().rollback();
    }
    const Result<void>& begun() const { return begun_; }

  private:
    Result<void> begun_;
};
} // namespace

Result<QJsonObject> load_groups(const services::etf::GroupRunRequest& request) {
    ReadSnapshot snapshot;
    if (snapshot.begun().is_err())
        return Result<QJsonObject>::err(snapshot.begun().error());
    return services::etf::run_group_research(request);
}

Result<QJsonObject> load_subject(const services::etf::GroupRunRequest& request, const QJsonObject& subject) {
    using R = Result<QJsonObject>;
    using namespace services::etf;
    if (const QString problem = group_request_problem(request); !problem.isEmpty())
        return R::err(problem.toStdString());
    ReadSnapshot snapshot;
    if (snapshot.begun().is_err())
        return R::err(snapshot.begun().error());
    DerivedRunRequest derived;
    derived.frame = request.frame;
    derived.output_from = request.output_from;
    derived.output_to = request.output_to;
    derived.regulatory = subject.value(QStringLiteral("subject_type")).toString() == QLatin1String("reporting_entity");
    derived.rotation = subject.value(QStringLiteral("subject_type")).toString() == QLatin1String("listed_instrument");
    const QString key = subject.value(QStringLiteral("stable_key")).toString();
    auto& repo = EtfDataRepository::instance();
    if (derived.regulatory) {
        const auto rows = repo.reporting_entities(request.frame.known_at);
        if (rows.is_err())
            return R::err(rows.error());
        for (const auto& row : rows.value())
            if (row.cik + QLatin1Char('/') + row.series_id == key)
                derived.entity_id = row.entity_id;
        if (!derived.entity_id)
            return R::err("SEC reporting identity was not recorded by the knowledge cutoff");
    } else if (derived.rotation) {
        const auto rows = repo.listed_instruments(request.frame.known_at);
        if (rows.is_err())
            return R::err(rows.error());
        for (const auto& row : rows.value())
            if (QString::number(row.con_id) == key)
                derived.instrument_id = row.instrument_id;
        if (!derived.instrument_id)
            return R::err("Listed instrument was not recorded by the knowledge cutoff");
    } else {
        return R::err("Unsupported research subject");
    }
    // SEC/listed attribution stays in Batch D. The individual history is for
    // this exact subject only, never an inferred ticker or a convenient proxy.
    return run_derived_calculations(derived);
}
} // namespace fincept::screens::etf_ui
