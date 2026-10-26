//
// JobScheduler.cpp - Cron-driven background scheduler (Phase 3)
//

#include "JobScheduler.hpp"
#include "CronExpr.hpp"

namespace Orcha::Jobs {

    namespace {
        std::string minute_key(const std::tm& t) {
            char buf[20];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", &t);
            return buf;
        }
    }

    JobScheduler::JobScheduler(std::shared_ptr<JobService> service,
                               std::shared_ptr<Utils::ILogger> logger,
                               std::chrono::seconds tick)
        : service_(std::move(service))
        , logger_(std::move(logger))
        , tick_(tick.count() > 0 ? tick : std::chrono::seconds(30)) {}

    JobScheduler::~JobScheduler() {
        stop();
    }

    void JobScheduler::start() {
        if (running_.exchange(true)) {
            return; // already running
        }
        thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
        if (logger_) {
            logger_->info("Job scheduler started (tick " +
                          std::to_string(tick_.count()) + "s)");
        }
    }

    void JobScheduler::stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.request_stop();   // wakes the tick wait
            thread_.join();
        }
        if (logger_) logger_->info("Job scheduler stopped");
    }

    void JobScheduler::loop(std::stop_token stop) {
        while (!stop.stop_requested()) {
            {
                // Interruptible sleep: wakes as soon as stop is requested.
                std::unique_lock wait_lock(wait_mutex_);
                (void)wait_cv_.wait_for(wait_lock, stop, tick_, [] { return false; });
            }
            if (stop.stop_requested()) break;

            std::time_t now = std::time(nullptr);
            std::tm utc{};
#if defined(_WIN32)
            gmtime_s(&utc, &now);
#else
            gmtime_r(&now, &utc);
#endif
            try {
                evaluate(utc);
            } catch (const std::exception& ex) {
                if (logger_) logger_->error(std::string("Scheduler tick error: ") + ex.what());
            } catch (...) {
                // An exception escaping a thread function calls std::terminate.
                if (logger_) logger_->error("Scheduler tick error: unknown exception");
            }
        }
    }

    size_t JobScheduler::evaluate(const std::tm& now_utc) {
        const std::string key = minute_key(now_utc);
        size_t fired = 0;

        for (const auto& job : service_->store()->list_jobs()) {
            if (!job.enabled || !job.schedule_cron || job.schedule_cron->empty()) {
                continue;
            }

            auto cron = CronExpr::parse(*job.schedule_cron);
            if (!cron) {
                std::lock_guard lock(mutex_);
                if (warned_bad_cron_.insert(job.id).second && logger_) {
                    logger_->warn("Job '" + job.name + "' has an invalid cron expression: " +
                                  *job.schedule_cron);
                }
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                warned_bad_cron_.erase(job.id); // expression may have been fixed
            }

            if (!cron->matches(now_utc)) {
                continue;
            }

            {
                std::lock_guard lock(mutex_);
                auto it = last_fired_.find(job.id);
                if (it != last_fired_.end() && it->second == key) {
                    continue; // already fired this minute
                }
                last_fired_[job.id] = key;
            }

            if (logger_) logger_->info("Scheduler firing job '" + job.name + "' (" + key + ")");
            (void)service_->run_job(job.id, "schedule");
            ++fired;
        }
        return fired;
    }

} // namespace Orcha::Jobs
