//
// RunQueue.cpp - Bounded admission queue and runner pool for workflow runs.
//

#include "RunQueue.hpp"
#include "../workflow/ExecutionContext.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace Orcha::Jobs {

    RunQueue::RunQueue(std::shared_ptr<JobService> service, Options options,
                       std::shared_ptr<Utils::ILogger> logger)
        : service_(std::move(service))
        , options_(options)
        , logger_(std::move(logger)) {
        options_.max_concurrent = std::max<size_t>(1, options_.max_concurrent);
        runners_.reserve(options_.max_concurrent);
        for (size_t i = 0; i < options_.max_concurrent; ++i) {
            runners_.emplace_back([this](const std::stop_token &stop) { runner(stop); });
        }
    }

    RunQueue::~RunQueue() {
        stop();
    }

    Core::Result<RunRecord, RunQueue::SubmitError> RunQueue::submit(
        std::optional<std::string> job_id, std::string trigger,
        Json definition, std::string name) {
        using R = Core::Result<RunRecord, SubmitError>;

        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return R::Err(SubmitError::ShuttingDown);
            }
            // queue_ also holds runs an idle runner is about to pick up, so the
            // capacity is the waiting slots plus the currently idle runners.
            const size_t idle_runners = options_.max_concurrent - running_;
            if (queue_.size() + reserved_ >= options_.max_queued + idle_runners) {
                return R::Err(SubmitError::QueueFull);
            }
            ++reserved_;  // hold the slot while the run is persisted, unlocked
        }

        RunRecord run;
        run.job_id = std::move(job_id);
        run.trigger = std::move(trigger);
        run.status = run_status::Queued;
        const bool stored = service_->store()->insert_run(run);

        Entry entry{run, std::move(definition), std::move(name), std::make_shared<Tracker>()};
        {
            std::lock_guard lock(mutex_);
            --reserved_;
            if (stored && !stopping_) {
                active_.emplace(run.id, entry.tracker);
                queue_.push_back(std::move(entry));
                work_cv_.notify_one();
                return R::Ok(std::move(run));
            }
        }
        if (!stored) {
            return R::Err(SubmitError::StoreError);
        }
        // stop() ran while the run was being persisted: it never starts.
        finish_cancelled(entry, "Cancelled: server shutting down");
        return R::Err(SubmitError::ShuttingDown);
    }

    void RunQueue::runner(const std::stop_token &stop) {
        for (;;) {
            Entry entry;
            {
                std::unique_lock lock(mutex_);
                // Wakes for work or stop; stop() empties the queue first.
                work_cv_.wait(lock, stop, [this] { return !queue_.empty(); });
                if (queue_.empty()) {
                    return;
                }
                entry = std::move(queue_.front());
                queue_.pop_front();
                ++running_;
            }

            const auto started = std::chrono::steady_clock::now();
            const std::string run_id = entry.run.id;
            RunRecord final;
            {
                // The run's own stop token: cancel() and stop() request it;
                // nested run_job steps inherit it through the engine.
                Workflow::ExecutionContext::Scope scope({{}, entry.tracker->stop.get_token()});
                try {
                    final = service_->execute(entry.run, entry.definition, entry.name);
                } catch (...) {
                    // execute() does not throw; never let anything escape a
                    // runner, and never leave the run looking live.
                    final = entry.run;
                    final.status = run_status::Failed;
                    final.error = "Run failed unexpectedly";
                    final.finished_at = now_iso_utc();
                    try { (void)service_->store()->update_run(final); } catch (...) {}
                }
            }
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

            // Waiters see the final record first, then the run leaves active_.
            complete(*entry.tracker, std::move(final));
            {
                std::lock_guard lock(mutex_);
                --running_;
                active_.erase(run_id);
                avg_run_seconds_ = 0.8 * avg_run_seconds_ + 0.2 * seconds;
            }
        }
    }

    void RunQueue::complete(Tracker& tracker, RunRecord final) {
        {
            std::lock_guard lock(tracker.mutex);
            tracker.final = std::move(final);
            tracker.done = true;
        }
        tracker.done_cv.notify_all();
    }

    void RunQueue::finish_cancelled(Entry& entry, const std::string& reason) const {
        entry.run.status = run_status::Cancelled;
        entry.run.error = reason;
        entry.run.finished_at = now_iso_utc();
        try {
            (void)service_->store()->update_run(entry.run);
        } catch (...) {
            if (logger_) logger_->error("Could not record run " + entry.run.id + " as cancelled");
        }
        complete(*entry.tracker, entry.run);
    }

    std::optional<RunRecord> RunQueue::get(const std::string& run_id) const {
        return service_->store()->get_run(run_id);
    }

    std::optional<RunRecord> RunQueue::wait(const std::string& run_id,
                                            std::chrono::milliseconds max_wait) {
        std::shared_ptr<Tracker> tracker;
        {
            std::lock_guard lock(mutex_);
            if (auto it = active_.find(run_id); it != active_.end()) {
                tracker = it->second;
            }
        }
        if (!tracker) {
            // Not active: either finished already or unknown.
            auto run = get(run_id);
            if (run && run_status::is_terminal(run->status)) return run;
            return std::nullopt;
        }
        std::unique_lock lock(tracker->mutex);
        if (!tracker->done_cv.wait_for(lock, max_wait, [&] { return tracker->done; })) {
            return std::nullopt;
        }
        return tracker->final;
    }

    RunQueue::CancelOutcome RunQueue::cancel(const std::string& run_id) {
        std::optional<Entry> cancelled;
        {
            std::lock_guard lock(mutex_);
            auto queued = std::find_if(queue_.begin(), queue_.end(),
                [&](const Entry& e) { return e.run.id == run_id; });
            if (queued != queue_.end()) {
                cancelled = std::move(*queued);
                queue_.erase(queued);
                active_.erase(run_id);
            } else if (auto it = active_.find(run_id); it != active_.end()) {
                it->second->stop.request_stop();
                return CancelOutcome::Cancelling;
            }
        }
        if (cancelled) {
            finish_cancelled(*cancelled, "Cancelled before it started");
            if (logger_) logger_->info("Run " + run_id + " cancelled while queued");
            return CancelOutcome::Cancelled;
        }
        const auto run = get(run_id);
        if (!run) return CancelOutcome::NotFound;
        // Live but not ours: a nested run_job executing inside its parent's slot.
        return run_status::is_terminal(run->status) ? CancelOutcome::AlreadyFinished
                                                    : CancelOutcome::NotCancellable;
    }

    std::chrono::seconds RunQueue::retry_after() const {
        std::lock_guard lock(mutex_);
        // Rough time for the backlog ahead to drain by one slot.
        const double waves = static_cast<double>(queue_.size()) /
                             static_cast<double>(options_.max_concurrent);
        const double estimate = avg_run_seconds_ * std::max(1.0, waves);
        return std::chrono::seconds(std::clamp<long>(std::lround(estimate), 1, 60));
    }

    RunQueue::Stats RunQueue::stats() const {
        std::lock_guard lock(mutex_);
        return Stats{queue_.size(), running_, options_.max_concurrent, options_.max_queued};
    }

    void RunQueue::stop() {
        std::deque<Entry> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) {
                return;
            }
            stopping_ = true;
            cancelled.swap(queue_);
            for (const auto& entry : cancelled) {
                active_.erase(entry.run.id);
            }
            for (const auto &tracker: active_ | std::views::values) {
                tracker->stop.request_stop();   // running runs stop cooperatively
            }
        }
        for (auto& entry : cancelled) {
            finish_cancelled(entry, "Cancelled: server shutting down");
        }
        for (auto& runner : runners_) {
            runner.request_stop();
        }
        for (auto& runner : runners_) {
            if (runner.joinable()) runner.join();
        }
        if (logger_) {
            logger_->info("Run queue stopped (" + std::to_string(cancelled.size()) +
                          " queued run(s) cancelled)");
        }
    }

} // namespace Orcha::Jobs
