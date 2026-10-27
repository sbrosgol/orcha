//
// JobScheduler.hpp - Cron-driven background scheduler for jobs (Phase 3).
//
// Ticks on a background thread; on each tick it evaluates every enabled job
// that has a schedule_cron and fires those whose expression matches the current
// UTC minute (de-duplicated so a job fires at most once per minute). Firing
// submits the run to the RunQueue, so a long job never delays the scheduler or
// other jobs; a run the queue rejects is recorded as "skipped".
//

#pragma once

#include "JobService.hpp"
#include "RunQueue.hpp"
#include "../utils/ILogger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Orcha::Jobs {

    class JobScheduler {
    public:
        JobScheduler(std::shared_ptr<JobService> service,
                     std::shared_ptr<RunQueue> runs,
                     std::shared_ptr<Utils::ILogger> logger,
                     std::chrono::seconds tick = std::chrono::seconds(30));
        ~JobScheduler();

        JobScheduler(const JobScheduler&) = delete;
        JobScheduler& operator=(const JobScheduler&) = delete;

        void start();
        /// Stop the background thread. Returns promptly (the tick wait is
        /// interrupted) unless a job is executing; cancel runs first.
        void stop();
        [[nodiscard]] bool is_running() const { return running_.load(); }

        /**
         * @brief Evaluate all jobs against @p now_utc and fire those that are due.
         * @return Number of runs submitted (not counting skipped ones).
         *
         * Public for testability; also called by the background thread each tick.
         * A job fires at most once per (job, minute) via internal de-duplication.
         */
        size_t evaluate(const std::tm& now_utc);

    private:
        void loop(std::stop_token stop);

        std::shared_ptr<JobService> service_;
        std::shared_ptr<RunQueue> runs_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::chrono::seconds tick_;

        std::atomic<bool> running_{false};
        std::mutex wait_mutex_;
        std::condition_variable_any wait_cv_;

        std::mutex mutex_;
        std::unordered_map<std::string, std::string> last_fired_; // job id -> "YYYY-MM-DDTHH:MM"
        std::unordered_set<std::string> warned_bad_cron_;          // jobs with an unparseable cron

        std::jthread thread_;   // last: stopped/joined before the members it uses
    };

} // namespace Orcha::Jobs
