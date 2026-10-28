//
// RunQueue.hpp - Bounded admission queue and runner pool for workflow runs.
//
// Every top-level run (POST /workflow, dashboard "Run now", the cron scheduler)
// is admitted here: it is persisted as `queued`, waits in a bounded FIFO, and
// is executed by one of a fixed number of runner threads that are separate
// from the HTTP workers. Nested `run_job` steps do NOT go through the queue
// (that could deadlock: parents would hold every slot waiting for children);
// they run inline via JobService::run_job.
//
// Admission is backpressure, not buffering: when the queue is full, submit()
// fails immediately with QueueFull and a Retry-After estimate.
//

#pragma once

#include "JobService.hpp"
#include "../core/Json.hpp"
#include "../core/Result.hpp"
#include "../utils/ILogger.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Orcha::Jobs {

    class RunQueue {
    public:
        struct Options {
            size_t max_concurrent = 4;  ///< Runner threads (runs executing at once).
            size_t max_queued = 64;     ///< Runs that may wait; more are rejected.
        };

        enum class SubmitError {
            QueueFull,     ///< Backpressure: retry after retry_after().
            ShuttingDown,  ///< stop() has been called.
            StoreError     ///< The run could not be persisted.
        };

        enum class CancelOutcome {
            NotFound,
            AlreadyFinished,
            Cancelled,     ///< Was queued; it will never run.
            Cancelling,    ///< Was running; cancellation requested (cooperative).
            NotCancellable ///< Live but not a top-level run (a nested run_job):
                           ///< cancel the run that started it.
        };

        struct Stats {
            size_t queued = 0;
            size_t running = 0;
            size_t max_concurrent = 0;
            size_t max_queued = 0;

            [[nodiscard]] Orcha::Json to_json() const {
                return Orcha::Json{{"queued", queued}, {"running", running},
                                   {"max_concurrent", max_concurrent}, {"max_queued", max_queued}};
            }
        };

        RunQueue(std::shared_ptr<JobService> service, Options options,
                 std::shared_ptr<Utils::ILogger> logger = nullptr);
        ~RunQueue();

        RunQueue(const RunQueue&) = delete;
        RunQueue& operator=(const RunQueue&) = delete;

        /**
         * @brief Persist a run as queued and enqueue it.
         * @param job_id     Saved job id, or nullopt for an ad-hoc workflow.
         * @param trigger    "api" | "manual" | "schedule".
         * @param definition Workflow JSON; callers validate it first (an
         *                   invalid definition makes the run fail, not submit()).
         * @param name       Default workflow name (e.g. the job's name).
         * @return The queued RunRecord, or why it was not accepted.
         */
        [[nodiscard]] Core::Result<RunRecord, SubmitError> submit(
            std::optional<std::string> job_id, std::string trigger,
            Orcha::Json definition, std::string name = "");

        /// Current record of a run (from the store), if it exists.
        [[nodiscard]] std::optional<RunRecord> get(const std::string& run_id) const;

        /**
         * @brief Block until the run finishes or @p max_wait passes.
         * @return The final record, or nullopt if it is still queued/running
         *         (or does not exist).
         */
        [[nodiscard]] std::optional<RunRecord> wait(const std::string& run_id,
                                                    std::chrono::milliseconds max_wait);

        /// Cancel a queued run (it never starts) or request a running one to stop.
        CancelOutcome cancel(const std::string& run_id);

        /// Suggested client back-off when the queue is full (1-60 s).
        [[nodiscard]] std::chrono::seconds retry_after() const;

        [[nodiscard]] Stats stats() const;

        /**
         * @brief Stop accepting runs, cancel queued ones, request running ones
         *        to stop, and join the runners. Idempotent; called by the
         *        destructor.
         */
        void stop();

    private:
        /// Per-run synchronisation: cancellation and completion.
        struct Tracker {
            std::stop_source stop;
            std::mutex mutex;
            std::condition_variable done_cv;
            bool done = false;
            RunRecord final;
        };

        struct Entry {
            RunRecord run;
            Orcha::Json definition;
            std::string name;
            std::shared_ptr<Tracker> tracker;
        };

        void runner(const std::stop_token &stop);
        void complete(Tracker& tracker, RunRecord final);
        /// Persists a queued run as cancelled and completes its tracker.
        /// Called without mutex_ held (store I/O never runs under it).
        void finish_cancelled(Entry& entry, const std::string& reason);

        std::shared_ptr<JobService> service_;
        Options options_;
        std::shared_ptr<Utils::ILogger> logger_;

        mutable std::mutex mutex_;
        std::condition_variable_any work_cv_;
        std::deque<Entry> queue_;
        std::unordered_map<std::string, std::shared_ptr<Tracker>> active_;  ///< queued + running
        size_t running_ = 0;
        size_t reserved_ = 0;   ///< Admitted runs being persisted (not yet in queue_).
        bool stopping_ = false;
        double avg_run_seconds_ = 5.0;  ///< EWMA of run durations, for retry_after().

        std::vector<std::jthread> runners_;  // last: joined before the state they use
    };

} // namespace Orcha::Jobs
