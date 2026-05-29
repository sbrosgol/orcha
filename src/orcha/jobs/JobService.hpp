//
// JobService.hpp - Orchestrates job execution + run recording (Phase 2)
//
// Single place that the API routes (and, later, the scheduler) call to run a
// saved job or to record an ad-hoc workflow execution. Runs synchronously via
// an injected workflow-engine factory and persists a RunRecord through IJobStore.
//

#pragma once

#include "IJobStore.hpp"
#include "../workflow/IWorkflowEngine.hpp"
#include "../utils/ILogger.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace Orcha::Jobs {

    class JobService {
    public:
        /// Creates a fresh workflow engine per run (engines are not shared across threads).
        using EngineFactory = std::function<std::shared_ptr<Workflow::IWorkflowEngine>()>;

        JobService(std::shared_ptr<IJobStore> store,
                   EngineFactory engine_factory,
                   std::shared_ptr<Utils::ILogger> logger = nullptr)
            : store_(std::move(store))
            , engine_factory_(std::move(engine_factory))
            , logger_(std::move(logger)) {}

        [[nodiscard]] std::shared_ptr<IJobStore> store() const { return store_; }

        /**
         * @brief Run a saved job by id and persist the resulting run.
         * @return The persisted RunRecord, or nullopt if the job does not exist.
         */
        [[nodiscard]] std::optional<RunRecord> run_job(
            const std::string& job_id, const std::string& trigger) {

            auto job = store_->get_job(job_id);
            if (!job) {
                return std::nullopt;
            }

            RunRecord run;
            run.job_id = job_id;
            run.trigger = trigger;
            run.started_at = now_iso_utc();

            if (logger_) logger_->info("Running job '" + job->name + "' (" + trigger + ")");

            try {
                auto def = Workflow::WorkflowDefinition::from_json(job->definition);
                if (def.name.empty()) def.name = job->name;
                auto engine = engine_factory_();
                auto result = engine->execute(def);

                run.status = result.success ? "success" : "failed";
                run.error = result.error_message;
                run.result = result.to_json();
            } catch (const std::exception& ex) {
                run.status = "failed";
                run.error = ex.what();
                run.result = web::json::value::null();
                if (logger_) logger_->error("Job '" + job->name + "' threw: " + ex.what());
            }

            run.finished_at = now_iso_utc();
            (void)store_->insert_run(run);
            return run;
        }

        /**
         * @brief Persist a run that was executed elsewhere (e.g. ad-hoc POST /workflow).
         */
        RunRecord record_run(const std::optional<std::string>& job_id,
                             const std::string& trigger,
                             bool success,
                             const web::json::value& result,
                             const std::string& error,
                             const std::string& started_at = "") {
            RunRecord run;
            run.job_id = job_id;
            run.trigger = trigger;
            run.started_at = started_at.empty() ? now_iso_utc() : started_at;
            run.finished_at = now_iso_utc();
            run.status = success ? "success" : "failed";
            run.result = result;
            run.error = error;
            (void)store_->insert_run(run);
            return run;
        }

    private:
        std::shared_ptr<IJobStore> store_;
        EngineFactory engine_factory_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Jobs
