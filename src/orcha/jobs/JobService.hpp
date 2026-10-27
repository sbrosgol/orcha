//
// JobService.hpp - Executes workflow runs and records them (Phase 2)
//
// execute() is the single place a persisted run is executed: it marks the run
// running, runs the workflow through an injected engine factory and persists
// the final status. RunQueue workers call it for every top-level run (API,
// dashboard, scheduler); run_job() creates and executes a run inline, which is
// what nested `run_job` steps use so a child runs in its parent's slot.
//

#pragma once

#include "IJobStore.hpp"
#include "../workflow/ExecutionContext.hpp"
#include "../workflow/IWorkflowEngine.hpp"
#include "../utils/ILogger.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace Orcha::Jobs {

    class JobService {
    public:
        /// Creates a fresh workflow engine per run (engines are not shared across threads).
        using EngineFactory = std::function<std::shared_ptr<Workflow::IWorkflowEngine>()>;

        /**
         * @param shutdown Requested at server shutdown (the engines' cancel
         *                 token); runs it interrupts are recorded as cancelled.
         */
        JobService(std::shared_ptr<IJobStore> store,
                   EngineFactory engine_factory,
                   std::shared_ptr<Utils::ILogger> logger = nullptr,
                   std::stop_token shutdown = {})
            : store_(std::move(store))
            , engine_factory_(std::move(engine_factory))
            , logger_(std::move(logger))
            , shutdown_(std::move(shutdown)) {}

        [[nodiscard]] std::shared_ptr<IJobStore> store() const { return store_; }

        /**
         * @brief Execute a run that already exists in the store and persist
         *        its progress (running) and outcome.
         *
         * The run is cancelled through the ExecutionContext stop token of the
         * calling thread (RunQueue installs one per run) or by server
         * shutdown; a run that fails after cancellation was requested is
         * recorded as cancelled. The final status is always persisted (if the
         * result cannot be stored, it is dropped and the error says so).
         * Never throws.
         *
         * @param run        The persisted run (status queued, or new).
         * @param definition Workflow JSON (`{"steps": [...]}` plus settings).
         * @param name       Default workflow name (e.g. the job's name).
         * @return The final record.
         */
        RunRecord execute(RunRecord run, const Orcha::Json& definition, const std::string& name = "") {
            run.status = run_status::Running;
            run.started_at = now_iso_utc();
            (void)store_->update_run(run);

            try {
                auto def = Workflow::WorkflowDefinition::from_json(definition);
                if (def.name.empty()) def.name = name;
                auto engine = engine_factory_();
                auto result = engine->execute(def);

                run.status = result.success ? run_status::Success : run_status::Failed;
                run.error = result.error_message;
                run.result = result.to_json();
            } catch (const std::exception& ex) {
                run.status = run_status::Failed;
                run.error = ex.what();
                run.result = Orcha::Json(nullptr);
                if (logger_) logger_->error("Run " + run.id + " threw: " + ex.what());
            } catch (...) {
                run.status = run_status::Failed;
                run.error = "Unknown error while executing the run";
                run.result = Orcha::Json(nullptr);
            }

            if (run.status != run_status::Success &&
                (Workflow::ExecutionContext::stop_token().stop_requested() ||
                 shutdown_.stop_requested())) {
                run.status = run_status::Cancelled;
                if (run.error.empty()) run.error = "Cancelled";
            }

            run.finished_at = now_iso_utc();
            if (!store_->update_run(run)) {
                // Never leave the run non-terminal: retry without the result.
                if (logger_) logger_->error("Could not store the result of run " + run.id +
                                            "; storing its status without it");
                RunRecord fallback = run;
                fallback.result = Orcha::Json(nullptr);
                fallback.error = (run.error.empty() ? "" : run.error + "; ") +
                                 "the run's result could not be stored";
                if (store_->update_run(fallback)) run = std::move(fallback);
            }
            return run;
        }

        /**
         * @brief Run a saved job inline on the calling thread and persist it.
         *
         * Used by nested `run_job` steps: the child runs in its parent's
         * runner slot, under the parent's deadline and cancellation. Top-level
         * runs go through RunQueue instead.
         *
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
            run.status = run_status::Running;
            (void)store_->insert_run(run);

            if (logger_) logger_->info("Running job '" + job->name + "' (" + trigger + ")");
            return execute(std::move(run), job->definition, job->name);
        }

    private:
        std::shared_ptr<IJobStore> store_;
        EngineFactory engine_factory_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::stop_token shutdown_;
    };

} // namespace Orcha::Jobs
