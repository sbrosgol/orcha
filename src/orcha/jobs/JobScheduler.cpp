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
                               std::shared_ptr<RunQueue> runs,
                               std::shared_ptr<Utils::ILogger> logger,
                               std::chrono::seconds tick)
        : service_(std::move(service))
        , runs_(std::move(runs))
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

            auto submitted = runs_->submit(job.id, "schedule", job.definition, job.name);
            if (submitted.is_ok()) {
                if (logger_) logger_->info("Scheduler queued job '" + job.name + "' (" + key + ")");
                ++fired;
                continue;
            }

            // Rejected: record it so the missed run is visible in the history
            // instead of silently lost.
            RunRecord skipped;
            skipped.job_id = job.id;
            skipped.trigger = "schedule";
            skipped.status = run_status::Skipped;
            skipped.finished_at = now_iso_utc();
            switch (submitted.error()) {
                case RunQueue::SubmitError::QueueFull:
                    skipped.error = "Skipped: the run queue was full";
                    break;
                case RunQueue::SubmitError::ShuttingDown:
                    skipped.error = "Skipped: the server was shutting down";
                    break;
                case RunQueue::SubmitError::StoreError:
                    // Recording the skip will most likely fail too; log loudly.
                    skipped.error = "Skipped: the run could not be stored";
                    if (logger_) logger_->error("Scheduler could not store a run of job '" +
                                                job.name + "'");
                    break;
            }
            (void)service_->store()->insert_run(skipped);
            if (logger_) logger_->warn("Scheduler skipped job '" + job.name + "': " + skipped.error);
        }
        return fired;
    }

} // namespace Orcha::Jobs
