//
// JobScheduler.hpp - Cron-driven background scheduler for jobs (Phase 3).
//
// Ticks on a background thread; on each tick it evaluates every enabled job
// that has a schedule_cron and fires those whose expression matches the current
// UTC minute (de-duplicated so a job fires at most once per minute).
//

#pragma once

#include "JobService.hpp"
#include "../utils/ILogger.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Orcha::Jobs {

    class JobScheduler {
    public:
        JobScheduler(std::shared_ptr<JobService> service,
                     std::shared_ptr<Utils::ILogger> logger,
                     std::chrono::seconds tick = std::chrono::seconds(30));
        ~JobScheduler();

        JobScheduler(const JobScheduler&) = delete;
        JobScheduler& operator=(const JobScheduler&) = delete;

        void start();
        void stop();
        [[nodiscard]] bool is_running() const { return running_.load(); }

        /**
         * @brief Evaluate all jobs against @p now_utc and fire those that are due.
         * @return Number of jobs fired this evaluation.
         *
         * Public for testability; also called by the background thread each tick.
         * A job fires at most once per (job, minute) via internal de-duplication.
         */
        size_t evaluate(const std::tm& now_utc);

    private:
        void loop();

        std::shared_ptr<JobService> service_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::chrono::seconds tick_;

        std::atomic<bool> running_{false};
        std::thread thread_;

        std::mutex mutex_;
        std::unordered_map<std::string, std::string> last_fired_; // job id -> "YYYY-MM-DDTHH:MM"
        std::unordered_set<std::string> warned_bad_cron_;          // jobs with an unparseable cron
    };

} // namespace Orcha::Jobs
