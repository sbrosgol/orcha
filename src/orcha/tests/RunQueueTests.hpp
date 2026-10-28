//
// RunQueueTests.hpp - Asynchronous runs: the bounded run queue and its API
//
// - RunQueue: run lifecycle and persistence, bounded concurrency, admission
//   (queue full -> rejected with a retry hint), cancel (queued / running /
//   finished / unknown), wait with timeout, stop() during shutdown, nested
//   run_job inline in the parent's slot (no deadlock with one runner).
// - Store: runs left queued/running are marked interrupted; the queued_at
//   migration of an existing database.
// - Scheduler: a run the queue rejects is recorded as skipped.
// - HTTP: POST /workflow 202 + Location, Prefer: wait, the waiter cap, run
//   status and cancel endpoints (ad-hoc only), 429 + Retry-After, the admin
//   run/cancel endpoints and queue stats, and custom headers on the wire.
//

#pragma once

#include "../agent/HttpServer.hpp"
#include "../agent/routes/JobRoute.hpp"
#include "../agent/routes/WorkflowRoute.hpp"
#include "../core/CommandRegistry.hpp"
#include "../jobs/JobScheduler.hpp"
#include "../jobs/JobService.hpp"
#include "../jobs/RunJobCommand.hpp"
#include "../jobs/RunQueue.hpp"
#include "../jobs/SqliteJobStore.hpp"
#include "../workflow/WorkflowEngine.hpp"
#include "ShutdownTests.hpp"   // shutdown::Client (raw HTTP client)

#include "Assertions.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <sqlite3.h>
#include <string>
#include <thread>

namespace Orcha::Tests {

    namespace runq {

        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;
        using Jobs::RunQueue;
        namespace rs = Jobs::run_status;

        inline long long ms_since(Clock::time_point t) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
        }

        /// Shared knobs for the test commands.
        struct Probe {
            std::atomic<int> executions{0};
            std::atomic<int> active{0};
            std::atomic<int> peak{0};
            std::atomic<bool> release{false};   ///< opens the "gate" command
        };

        /// Sleeps params.ms (cooperatively) and tracks concurrency.
        class WorkCommand : public Core::ICommand {
        public:
            explicit WorkCommand(std::shared_ptr<Probe> p) : p_(std::move(p)) {}
            [[nodiscard]] std::string name() const override { return "work"; }
            Json execute(const Json& params) override { return execute_with_control(params, {}); }
            Json execute_with_control(const Json& params, const Core::ExecutionControl& c) override {
                ++p_->executions;
                const int now = ++p_->active;
                int prev = p_->peak.load();
                while (now > prev && !p_->peak.compare_exchange_weak(prev, now)) {}
                const auto until = Clock::now() + std::chrono::milliseconds(params.value("ms", 0));
                bool stopped = false;
                while (Clock::now() < until) {
                    if (c.should_stop()) { stopped = true; break; }
                    std::this_thread::sleep_for(2ms);
                }
                --p_->active;
                if (stopped) return Json{{"success", false}, {"error", "stopped"}};
                return Json{{"ok", true}};
            }
        private:
            std::shared_ptr<Probe> p_;
        };

        /// Blocks until probe.release or cancellation.
        class GateCommand : public Core::ICommand {
        public:
            explicit GateCommand(std::shared_ptr<Probe> p) : p_(std::move(p)) {}
            [[nodiscard]] std::string name() const override { return "gate"; }
            Json execute(const Json& params) override { return execute_with_control(params, {}); }
            Json execute_with_control(const Json&, const Core::ExecutionControl& c) override {
                ++p_->executions;
                while (!p_->release.load()) {
                    if (c.should_stop()) return Json{{"success", false}, {"error", "stopped"}};
                    std::this_thread::sleep_for(2ms);
                }
                return Json{{"opened", true}};
            }
        private:
            std::shared_ptr<Probe> p_;
        };

        struct Env {
            std::shared_ptr<Probe> probe = std::make_shared<Probe>();
            std::shared_ptr<Core::CommandRegistry> registry = std::make_shared<Core::CommandRegistry>();
            std::shared_ptr<Jobs::SqliteJobStore> store;
            std::shared_ptr<Jobs::JobService> service;
            std::shared_ptr<RunQueue> runs;

            explicit Env(RunQueue::Options options, std::string db = ":memory:") {
                (void)registry->register_command(std::make_shared<WorkCommand>(probe));
                (void)registry->register_command(std::make_shared<GateCommand>(probe));
                store = std::make_shared<Jobs::SqliteJobStore>(db);
                auto reg = registry;
                service = std::make_shared<Jobs::JobService>(store, [reg] {
                    return std::make_shared<Workflow::WorkflowEngine>(reg);
                });
                (void)registry->register_command(std::make_shared<Jobs::RunJobCommand>(service));
                runs = std::make_shared<RunQueue>(service, options);
            }
            ~Env() { probe->release = true; runs->stop(); }
        };

        inline Json wf(const std::string& command, Json params = Json::object()) {
            return Json{{"steps", Json::array({{{"command", command}, {"params", std::move(params)}}})}};
        }

        inline Jobs::RunRecord submit_ok(RunQueue& q, Json def) {
            auto r = q.submit(std::nullopt, "api", std::move(def));
            ORCHA_ASSERT(r.is_ok());
            return r.value();
        }

        /// Spin until @p pred or 5 s (fails the test on timeout).
        template <typename Pred>
        void wait_until(Pred pred, const char* what) {
            const auto t = Clock::now();
            while (!pred()) {
                if (ms_since(t) > 5000) throw std::runtime_error(std::string("timed out waiting for ") + what);
                std::this_thread::sleep_for(2ms);
            }
        }

        inline Agent::HttpRequest req(const std::string& method, const std::string& path,
                                      const Json& body = Json::object(),
                                      std::optional<std::string> prefer = std::nullopt) {
            Agent::HttpRequest r;
            r.method = method;
            r.path = path;
            r.headers.emplace_back("Content-Type", "application/json");
            if (prefer) r.headers.emplace_back("Prefer", *prefer);
            r.body = body.dump();
            return r;
        }

        inline std::optional<std::string> header(const Agent::HttpResponse& r, const std::string& name) {
            for (const auto& [k, v] : r.headers) if (k == name) return v;
            return std::nullopt;
        }

    } // namespace runq

    // ========== RunQueue ==========

    inline void test_run_queue_lifecycle() {
        using namespace runq;
        Env env({2, 8});
        auto queued = submit_ok(*env.runs, wf("work", {{"ms", 20}}));
        ORCHA_ASSERT(queued.status == rs::Queued || queued.status == rs::Running);
        ORCHA_ASSERT(!queued.queued_at.empty());
        ORCHA_ASSERT(env.store->get_run(queued.id).has_value());

        auto done = env.runs->wait(queued.id, 5s);
        ORCHA_ASSERT(done.has_value());
        ORCHA_ASSERT_EQ(done->status, std::string(rs::Success));
        auto stored = env.store->get_run(queued.id);
        ORCHA_ASSERT(stored && stored->status == rs::Success && !stored->started_at.empty() &&
                     stored->finished_at && stored->result.is_array() && stored->result.size() == 1);
        ORCHA_ASSERT(stored->trigger == "api" && !stored->job_id);
        std::cout << "[PASS] test_run_queue_lifecycle\n";
    }

    inline void test_run_queue_bounds_concurrency() {
        using namespace runq;
        Env env({2, 16});
        std::vector<std::string> ids;
        for (int i = 0; i < 6; ++i) ids.push_back(submit_ok(*env.runs, wf("work", {{"ms", 60}})).id);
        for (const auto& id : ids) {
            auto r = env.runs->wait(id, 5s);
            ORCHA_ASSERT(r && r->status == rs::Success);
        }
        ORCHA_ASSERT_EQ(env.probe->executions.load(), 6);
        ORCHA_ASSERT(env.probe->peak.load() == 2);           // never more than max_concurrent
        std::cout << "[PASS] test_run_queue_bounds_concurrency\n";
    }

    inline void test_run_queue_rejects_when_full() {
        using namespace runq;
        Env env({1, 2});
        auto running = submit_ok(*env.runs, wf("gate"));
        wait_until([&] { return env.runs->stats().running == 1; }, "first run to start");
        (void)submit_ok(*env.runs, wf("work"));
        (void)submit_ok(*env.runs, wf("work"));
        ORCHA_ASSERT_EQ(env.runs->stats().queued, static_cast<size_t>(2));

        auto rejected = env.runs->submit(std::nullopt, "api", wf("work"));
        ORCHA_ASSERT(rejected.is_err() && rejected.error() == RunQueue::SubmitError::QueueFull);
        const auto retry = env.runs->retry_after().count();
        ORCHA_ASSERT(retry >= 1 && retry <= 60);
        ORCHA_ASSERT_EQ(env.store->list_runs(std::nullopt, 10).size(), static_cast<size_t>(3)); // rejected not recorded

        env.probe->release = true;
        ORCHA_ASSERT(env.runs->wait(running.id, 5s).has_value());
        std::cout << "[PASS] test_run_queue_rejects_when_full\n";
    }

    inline void test_run_queue_idle_runners_admit_without_queue_slots() {
        using namespace runq;
        Env env({2, 0});    // no waiting room, two runners
        auto a = env.runs->submit(std::nullopt, "api", wf("gate"));
        auto b = env.runs->submit(std::nullopt, "api", wf("gate"));
        ORCHA_ASSERT(a.is_ok() && b.is_ok());
        wait_until([&] { return env.runs->stats().running == 2; }, "both runs to start");
        auto c = env.runs->submit(std::nullopt, "api", wf("work"));
        ORCHA_ASSERT(c.is_err() && c.error() == RunQueue::SubmitError::QueueFull);
        std::cout << "[PASS] test_run_queue_idle_runners_admit_without_queue_slots\n";
    }

    inline void test_run_queue_cancel() {
        using namespace runq;
        Env env({1, 4});
        auto running = submit_ok(*env.runs, wf("gate"));
        wait_until([&] { return env.runs->stats().running == 1; }, "gate run to start");
        auto queued = submit_ok(*env.runs, wf("work"));

        // Queued: removed, never executes.
        ORCHA_ASSERT(env.runs->cancel(queued.id) == RunQueue::CancelOutcome::Cancelled);
        auto q = env.runs->wait(queued.id, 1s);
        ORCHA_ASSERT(q && q->status == rs::Cancelled && q->started_at.empty());

        // Running: cooperative stop, recorded as cancelled.
        ORCHA_ASSERT(env.runs->cancel(running.id) == RunQueue::CancelOutcome::Cancelling);
        auto r = env.runs->wait(running.id, 5s);
        ORCHA_ASSERT(r && r->status == rs::Cancelled);

        ORCHA_ASSERT(env.runs->cancel(running.id) == RunQueue::CancelOutcome::AlreadyFinished);
        ORCHA_ASSERT(env.runs->cancel("nope") == RunQueue::CancelOutcome::NotFound);
        ORCHA_ASSERT_EQ(env.probe->executions.load(), 1);   // the queued run never started
        std::cout << "[PASS] test_run_queue_cancel\n";
    }

    inline void test_run_queue_wait_timeout() {
        using namespace runq;
        Env env({1, 4});
        auto run = submit_ok(*env.runs, wf("work", {{"ms", 300}}));
        const auto t = Clock::now();
        ORCHA_ASSERT_FALSE(env.runs->wait(run.id, 50ms).has_value());
        ORCHA_ASSERT(ms_since(t) < 250);
        ORCHA_ASSERT(env.runs->wait(run.id, 5s).has_value());
        ORCHA_ASSERT_FALSE(env.runs->wait("nope", 10ms).has_value());
        std::cout << "[PASS] test_run_queue_wait_timeout\n";
    }

    inline void test_run_queue_stop() {
        using namespace runq;
        Env env({1, 4});
        auto running = submit_ok(*env.runs, wf("gate"));
        wait_until([&] { return env.runs->stats().running == 1; }, "gate run to start");
        auto queued = submit_ok(*env.runs, wf("work"));

        const auto t = Clock::now();
        env.runs->stop();
        ORCHA_ASSERT(ms_since(t) < 2000);
        ORCHA_ASSERT_EQ(env.store->get_run(running.id)->status, std::string(rs::Cancelled));
        auto q = env.store->get_run(queued.id);
        ORCHA_ASSERT(q && q->status == rs::Cancelled && q->error.find("shutting down") != std::string::npos);

        auto after = env.runs->submit(std::nullopt, "api", wf("work"));
        ORCHA_ASSERT(after.is_err() && after.error() == RunQueue::SubmitError::ShuttingDown);
        std::cout << "[PASS] test_run_queue_stop\n";
    }

    inline void test_run_queue_nested_run_job_inline() {
        using namespace runq;
        Env env({1, 4});     // a single runner: queuing the child would deadlock
        Jobs::JobDefinition child;
        child.name = "child";
        child.definition = wf("work", {{"ms", 10}});
        ORCHA_ASSERT(env.store->create_job(child));
        Jobs::JobDefinition parent;
        parent.name = "parent";
        parent.definition = wf("run_job", {{"job", "child"}});
        ORCHA_ASSERT(env.store->create_job(parent));

        auto run = env.runs->submit(parent.id, "manual", parent.definition, parent.name);
        ORCHA_ASSERT(run.is_ok());
        auto done = env.runs->wait(run.value().id, 5s);
        ORCHA_ASSERT(done && done->status == rs::Success);
        auto child_runs = env.store->list_runs(child.id, 5);
        ORCHA_ASSERT(child_runs.size() == 1 && child_runs[0].trigger == "nested" &&
                     child_runs[0].status == rs::Success);
        std::cout << "[PASS] test_run_queue_nested_run_job_inline\n";
    }

    // A nested run cannot be cancelled on its own (409 via the routes); the
    // parent can, and that stops the child too.
    inline void test_run_queue_nested_run_not_cancellable() {
        using namespace runq;
        Env env({1, 4});
        Jobs::JobDefinition child;
        child.name = "child";
        child.definition = wf("gate");
        ORCHA_ASSERT(env.store->create_job(child));
        auto parent = submit_ok(*env.runs, wf("run_job", {{"job", "child"}}));
        wait_until([&] { return env.probe->executions.load() == 1; }, "child to start");
        const auto child_run = env.store->list_runs(child.id, 1).at(0);

        ORCHA_ASSERT(env.runs->cancel(child_run.id) == RunQueue::CancelOutcome::NotCancellable);
        auto reply = Agent::Routes::reply_cancel(*env.runs, child_run.id, "/api/runs");
        ORCHA_ASSERT_EQ(reply.status, Agent::status::Conflict);
        ORCHA_ASSERT(Json::parse(reply.body)["error"] == "nested run: cancel the run that started it");

        ORCHA_ASSERT(env.runs->cancel(parent.id) == RunQueue::CancelOutcome::Cancelling);
        auto done = env.runs->wait(parent.id, 5s);
        ORCHA_ASSERT(done && done->status == rs::Cancelled);
        ORCHA_ASSERT_EQ(env.store->get_run(child_run.id)->status, std::string(rs::Cancelled));
        std::cout << "[PASS] test_run_queue_nested_run_not_cancellable\n";
    }

    // A nested run_job is bound by its parent's workflow deadline.
    inline void test_run_queue_nested_run_inherits_deadline() {
        using namespace runq;
        Env env({1, 4});
        Jobs::JobDefinition child;
        child.name = "slow-child";
        child.definition = wf("work", {{"ms", 5000}});
        ORCHA_ASSERT(env.store->create_job(child));
        Json parent = wf("run_job", {{"job", "slow-child"}});
        parent["timeout_ms"] = 200;

        const auto t = Clock::now();
        auto run = submit_ok(*env.runs, parent);
        auto done = env.runs->wait(run.id, 5s);
        ORCHA_ASSERT(done && done->status == rs::Failed);
        ORCHA_ASSERT(ms_since(t) < 2000);
        auto child_run = env.store->list_runs(child.id, 1).at(0);
        ORCHA_ASSERT(child_run.status == rs::Failed);
        ORCHA_ASSERT(child_run.error.find("calling step's deadline") != std::string::npos);
        std::cout << "[PASS] test_run_queue_nested_run_inherits_deadline\n";
    }

    /// Returns bytes that are not valid UTF-8 (e.g. binary process output).
    class BinaryCommand : public Core::ICommand {
    public:
        [[nodiscard]] std::string name() const override { return "binary"; }
        Json execute(const Json&) override { return Json{{"stdout", std::string("ok \xff\xfe end")}}; }
    };

    // Invalid UTF-8 in a step's output must not leave the run "running".
    inline void test_run_queue_invalid_utf8_output() {
        using namespace runq;
        Env env({1, 4});
        (void)env.registry->register_command(std::make_shared<BinaryCommand>());
        auto run = submit_ok(*env.runs, wf("binary"));
        auto done = env.runs->wait(run.id, 5s);
        ORCHA_ASSERT(done && done->status == rs::Success);
        auto stored = env.store->get_run(run.id);
        ORCHA_ASSERT(stored && stored->status == rs::Success);
        const std::string out = stored->result.dump();
        ORCHA_ASSERT(out.find("end") != std::string::npos);
        // And the HTTP reply serializes it instead of throwing.
        auto reply = Agent::Routes::reply_run(*stored, "/workflow/runs");
        ORCHA_ASSERT_EQ(reply.status, Agent::status::OK);
        std::cout << "[PASS] test_run_queue_invalid_utf8_output\n";
    }

    // ========== Store ==========

    inline std::string temp_db(const std::string& tag) {
        const auto path = (std::filesystem::temp_directory_path() /
                           ("orcha-" + tag + "-" + std::to_string(::getpid()) + ".db")).string();
        std::filesystem::remove(path);
        return path;
    }

    // Another process's live runs are marked interrupted; this process's own
    // (and finished) runs are not.
    inline void test_store_marks_interrupted_runs() {
        const auto path = temp_db("interrupted");
        {
            Jobs::SqliteJobStore previous(path);   // the process that died
            for (const char* st : {Jobs::run_status::Queued, Jobs::run_status::Running, Jobs::run_status::Success}) {
                Jobs::RunRecord r;
                r.trigger = "api";
                r.status = st;
                ORCHA_ASSERT(previous.insert_run(r));
            }
            ORCHA_ASSERT_EQ(previous.mark_interrupted_runs("own"), static_cast<size_t>(0));

            Jobs::SqliteJobStore current(path);
            Jobs::RunRecord mine;
            mine.trigger = "api";
            mine.status = Jobs::run_status::Running;
            ORCHA_ASSERT(current.insert_run(mine));
            ORCHA_ASSERT_EQ(current.mark_interrupted_runs("restarted"), static_cast<size_t>(2));
            size_t interrupted = 0, success = 0;
            for (const auto& r : current.list_runs(std::nullopt, 10)) {
                if (r.status == Jobs::run_status::Interrupted) { ++interrupted; ORCHA_ASSERT(r.error == "restarted" && r.finished_at); }
                if (r.status == Jobs::run_status::Success) ++success;
            }
            ORCHA_ASSERT(interrupted == 2 && success == 1);
            ORCHA_ASSERT(current.get_run(mine.id)->status == Jobs::run_status::Running);
        }
        std::filesystem::remove(path);
        std::cout << "[PASS] test_store_marks_interrupted_runs\n";
    }

    // A final status is never overwritten (e.g. interrupted by a newer
    // process, then the old runner finishing).
    inline void test_store_update_run_is_monotonic() {
        Jobs::SqliteJobStore store(":memory:");
        Jobs::RunRecord r;
        r.trigger = "api";
        r.status = Jobs::run_status::Queued;
        ORCHA_ASSERT(store.insert_run(r));
        r.status = Jobs::run_status::Running;
        ORCHA_ASSERT(store.update_run(r));
        r.status = Jobs::run_status::Cancelled;
        ORCHA_ASSERT(store.update_run(r));
        r.status = Jobs::run_status::Success;
        ORCHA_ASSERT_FALSE(store.update_run(r));
        ORCHA_ASSERT_EQ(store.get_run(r.id)->status, std::string(Jobs::run_status::Cancelled));
        std::cout << "[PASS] test_store_update_run_is_monotonic\n";
    }

    // Only the oldest finished runs are pruned beyond jobs.max_runs.
    inline void test_store_prunes_old_runs() {
        Jobs::SqliteJobStore store(":memory:");
        Jobs::RunRecord live;
        live.trigger = "api";
        live.status = Jobs::run_status::Running;
        ORCHA_ASSERT(store.insert_run(live));
        store.set_max_runs(5);
        std::vector<std::string> ids;
        for (int i = 0; i < 12; ++i) {
            Jobs::RunRecord r;
            r.trigger = "api";
            r.status = Jobs::run_status::Success;
            ORCHA_ASSERT(store.insert_run(r));
            ids.push_back(r.id);
        }
        auto all = store.list_runs(std::nullopt, 100);
        ORCHA_ASSERT_EQ(all.size(), static_cast<size_t>(6));      // 5 finished + the live one
        ORCHA_ASSERT(store.get_run(live.id).has_value());
        ORCHA_ASSERT(store.get_run(ids.back()).has_value());
        ORCHA_ASSERT_FALSE(store.get_run(ids.front()).has_value());
        ORCHA_ASSERT_EQ(store.prune_runs(2), static_cast<size_t>(3));
        std::cout << "[PASS] test_store_prunes_old_runs\n";
    }

    inline void test_store_persistent_flag() {
        ORCHA_ASSERT_FALSE(Jobs::SqliteJobStore(":memory:").persistent());
        const auto path = temp_db("persistent");
        ORCHA_ASSERT(Jobs::SqliteJobStore(path).persistent());
        std::filesystem::remove(path);
        std::cout << "[PASS] test_store_persistent_flag\n";
    }

    inline void test_store_migrates_queued_at() {
        const auto path = (std::filesystem::temp_directory_path() /
                           ("orcha-migrate-" + std::to_string(::getpid()) + ".db")).string();
        std::filesystem::remove(path);
        {
            // A database as created before runs had queued_at.
            sqlite3* db = nullptr;
            ORCHA_ASSERT(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
            const char* ddl =
                "CREATE TABLE runs(id TEXT PRIMARY KEY, job_id TEXT, trigger TEXT NOT NULL, status TEXT NOT NULL,"
                " started_at TEXT, finished_at TEXT, result TEXT, error TEXT);"
                "CREATE INDEX idx_runs_started ON runs(started_at);"
                "INSERT INTO runs VALUES('old1',NULL,'api','success','2026-01-01T10:00:00Z','2026-01-01T10:00:01Z','[]','');";
            ORCHA_ASSERT(sqlite3_exec(db, ddl, nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(db);
        }
        {
            Jobs::SqliteJobStore store(path);
            auto old = store.get_run("old1");
            ORCHA_ASSERT(old && old->queued_at == "2026-01-01T10:00:00Z");
            Jobs::RunRecord fresh;
            fresh.trigger = "api";
            fresh.status = Jobs::run_status::Queued;
            ORCHA_ASSERT(store.insert_run(fresh));
            auto list = store.list_runs(std::nullopt, 10);
            ORCHA_ASSERT(list.size() == 2 && list[0].id == fresh.id);   // newest first
        }
        {
            // Versioned: the old index is gone and reopening is a no-op.
            sqlite3* db = nullptr;
            ORCHA_ASSERT(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
            auto scalar = [db](const char* sql) {
                sqlite3_stmt* st = nullptr;
                (void)sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
                const int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
                sqlite3_finalize(st);
                return v;
            };
            ORCHA_ASSERT_EQ(scalar("PRAGMA user_version"), 1);
            ORCHA_ASSERT_EQ(scalar("SELECT count(*) FROM sqlite_master WHERE name='idx_runs_started'"), 0);
            sqlite3_close(db);
            Jobs::SqliteJobStore reopened(path);
            ORCHA_ASSERT_EQ(reopened.list_runs(std::nullopt, 10).size(), static_cast<size_t>(2));
        }
        std::filesystem::remove(path);
        std::cout << "[PASS] test_store_migrates_queued_at\n";
    }

    // ========== Scheduler ==========

    inline void test_scheduler_records_skipped_run() {
        using namespace runq;
        Env env({1, 0});
        auto blocker = submit_ok(*env.runs, wf("gate"));
        wait_until([&] { return env.runs->stats().running == 1; }, "blocker to start");

        Jobs::JobDefinition job;
        job.name = "every-minute";
        job.schedule_cron = "* * * * *";
        job.definition = wf("work");
        ORCHA_ASSERT(env.store->create_job(job));

        Jobs::JobScheduler sched(env.service, env.runs, nullptr, 30s);
        std::tm t{}; t.tm_year = 124; t.tm_mon = 0; t.tm_mday = 1; t.tm_hour = 0; t.tm_min = 0;
        ORCHA_ASSERT_EQ(sched.evaluate(t), static_cast<size_t>(0));
        auto runs = env.store->list_runs(job.id, 5);
        ORCHA_ASSERT(runs.size() == 1 && runs[0].status == rs::Skipped && runs[0].trigger == "schedule" &&
                     runs[0].error.find("queue was full") != std::string::npos);
        env.probe->release = true;
        (void)env.runs->wait(blocker.id, 5s);
        std::cout << "[PASS] test_scheduler_records_skipped_run\n";
    }

    // ========== HTTP ==========

    inline void test_prefer_wait_parsing() {
        using R = Agent::Routes::WorkflowRoute;
        ORCHA_ASSERT(R::preferred_wait("wait=10") == 10L);
        ORCHA_ASSERT(R::preferred_wait("respond-async, wait=5") == 5L);
        ORCHA_ASSERT(R::preferred_wait("WAIT=3 , return=minimal") == 3L);
        ORCHA_ASSERT_FALSE(R::preferred_wait("respond-async").has_value());
        ORCHA_ASSERT_FALSE(R::preferred_wait("wait=abc").has_value());
        ORCHA_ASSERT_FALSE(R::preferred_wait("wait=-1").has_value());
        ORCHA_ASSERT_FALSE(R::preferred_wait("").has_value());
        ORCHA_ASSERT(R::preferred_wait("wait=10;foo=bar") == 10L);
        ORCHA_ASSERT(R::preferred_wait("wait=99999999999999999999999") == std::numeric_limits<long>::max());
        std::cout << "[PASS] test_prefer_wait_parsing\n";
    }

    inline void test_workflow_route_async_and_wait() {
        using namespace runq;
        Env env({2, 8});
        Agent::Routes::WorkflowRoute route(env.runs, nullptr, {std::chrono::seconds(10), 1});

        // Default: 202 + Location + queued run resource.
        auto accepted = route.handle(req("POST", "/workflow", wf("work", {{"ms", 50}})));
        ORCHA_ASSERT_EQ(accepted.status, Agent::status::Accepted);
        auto body = Json::parse(accepted.body);
        const std::string id = body["id"];
        ORCHA_ASSERT(header(accepted, "Location") == "/workflow/runs/" + id);
        ORCHA_ASSERT(body["links"]["cancel"] == "/workflow/runs/" + id + "/cancel");

        // Poll until finished.
        wait_until([&] {
            auto r = route.handle(req("GET", "/workflow/runs/" + id));
            return r.status == Agent::status::OK && Json::parse(r.body)["status"] == "success";
        }, "run to finish");

        // Prefer: wait -> 200 with the finished run.
        auto waited = route.handle(req("POST", "/workflow", wf("work", {{"ms", 20}}), "wait=5"));
        ORCHA_ASSERT_EQ(waited.status, Agent::status::OK);
        auto wbody = Json::parse(waited.body);
        ORCHA_ASSERT(wbody["status"] == "success" && wbody["result"].is_array());

        // Wait shorter than the run -> 202 after about the wait.
        const auto t = Clock::now();
        auto partial = route.handle(req("POST", "/workflow", wf("work", {{"ms", 1500}}), "wait=1"));
        ORCHA_ASSERT_EQ(partial.status, Agent::status::Accepted);
        ORCHA_ASSERT(ms_since(t) >= 900 && ms_since(t) < 1400);
        std::cout << "[PASS] test_workflow_route_async_and_wait\n";
    }

    inline void test_workflow_route_waiter_cap() {
        using namespace runq;
        Env env({2, 8});
        Agent::Routes::WorkflowRoute route(env.runs, nullptr, {std::chrono::seconds(10), 1});
        auto first = std::async(std::launch::async, [&] {
            return route.handle(req("POST", "/workflow", wf("work", {{"ms", 400}}), "wait=5"));
        });
        wait_until([&] { return env.probe->executions.load() >= 1; }, "first run to start");
        const auto t = Clock::now();
        auto second = route.handle(req("POST", "/workflow", wf("work", {{"ms", 400}}), "wait=5"));
        ORCHA_ASSERT_EQ(second.status, Agent::status::Accepted);   // cap reached: not waited
        ORCHA_ASSERT(ms_since(t) < 200);
        ORCHA_ASSERT_EQ(first.get().status, Agent::status::OK);

        // max_waiters = 0 (a single HTTP worker): never wait.
        Agent::Routes::WorkflowRoute no_wait(env.runs, nullptr, {std::chrono::seconds(10), 0});
        const auto t0 = Clock::now();
        auto immediate = no_wait.handle(req("POST", "/workflow", wf("work", {{"ms", 400}}), "wait=5"));
        ORCHA_ASSERT_EQ(immediate.status, Agent::status::Accepted);
        ORCHA_ASSERT(ms_since(t0) < 200);

        // A huge wait is clamped to max_wait (no overflow into a negative wait).
        Agent::Routes::WorkflowRoute short_wait(env.runs, nullptr, {std::chrono::milliseconds(300), 1});
        const auto t1 = Clock::now();
        auto clamped = short_wait.handle(
            req("POST", "/workflow", wf("work", {{"ms", 2000}}), "wait=99999999999999999999"));
        ORCHA_ASSERT_EQ(clamped.status, Agent::status::Accepted);
        ORCHA_ASSERT(ms_since(t1) >= 250 && ms_since(t1) < 1000);
        std::cout << "[PASS] test_workflow_route_waiter_cap\n";
    }

    inline void test_workflow_route_runs_endpoints() {
        using namespace runq;
        Env env({1, 1});
        Agent::Routes::WorkflowRoute route(env.runs);

        auto gate = Json::parse(route.handle(req("POST", "/workflow", wf("gate"))).body);
        wait_until([&] { return env.runs->stats().running == 1; }, "gate run to start");
        auto queued = Json::parse(route.handle(req("POST", "/workflow", wf("work"))).body);

        // Queue full -> 429 + Retry-After.
        auto full = route.handle(req("POST", "/workflow", wf("work")));
        ORCHA_ASSERT_EQ(full.status, Agent::status::TooManyRequests);
        ORCHA_ASSERT(header(full, "Retry-After").has_value());

        // Cancel queued -> 200 cancelled; cancel again -> 409; unknown -> 404.
        auto c1 = route.handle(req("POST", "/workflow/runs/" + queued["id"].get<std::string>() + "/cancel"));
        ORCHA_ASSERT(c1.status == Agent::status::OK && Json::parse(c1.body)["status"] == "cancelled");
        ORCHA_ASSERT_EQ(route.handle(req("POST", "/workflow/runs/" + queued["id"].get<std::string>() + "/cancel")).status,
                        Agent::status::Conflict);
        ORCHA_ASSERT_EQ(route.handle(req("GET", "/workflow/runs/nope")).status, Agent::status::NotFound);

        // Cancel running -> 202 (cooperative), then finishes as cancelled.
        const std::string gid = gate["id"];
        ORCHA_ASSERT_EQ(route.handle(req("POST", "/workflow/runs/" + gid + "/cancel")).status, Agent::status::Accepted);
        ORCHA_ASSERT(env.runs->wait(gid, 5s)->status == rs::Cancelled);

        // Job runs are not visible through the API-client endpoints.
        Jobs::JobDefinition job;
        job.name = "j";
        job.definition = wf("work");
        ORCHA_ASSERT(env.store->create_job(job));
        auto jr = env.runs->submit(job.id, "manual", job.definition);
        ORCHA_ASSERT(jr.is_ok());
        ORCHA_ASSERT_EQ(route.handle(req("GET", "/workflow/runs/" + jr.value().id)).status, Agent::status::NotFound);
        ORCHA_ASSERT_EQ(route.handle(req("POST", "/workflow/runs/" + jr.value().id + "/cancel")).status,
                        Agent::status::NotFound);
        std::cout << "[PASS] test_workflow_route_runs_endpoints\n";
    }

    inline void test_job_route_queues_runs() {
        using namespace runq;
        Env env({1, 4});
        Agent::Routes::JobRoute route(env.service, env.runs);
        Jobs::JobDefinition job;
        job.name = "slow";
        job.definition = wf("gate");
        ORCHA_ASSERT(env.store->create_job(job));

        auto run = route.handle(req("POST", "/api/jobs/" + job.id + "/run"));
        ORCHA_ASSERT_EQ(run.status, Agent::status::Accepted);
        const std::string id = Json::parse(run.body)["id"];
        ORCHA_ASSERT(header(run, "Location") == "/api/runs/" + id);

        wait_until([&] { return env.runs->stats().running == 1; }, "job run to start");
        auto list = Json::parse(route.handle(req("GET", "/api/runs")).body);
        ORCHA_ASSERT(list["queue"]["running"] == 1 && list["queue"]["max_concurrent"] == 1);
        ORCHA_ASSERT(list["runs"][0]["status"] == "running");

        ORCHA_ASSERT_EQ(route.handle(req("POST", "/api/runs/" + id + "/cancel")).status, Agent::status::Accepted);
        ORCHA_ASSERT(env.runs->wait(id, 5s)->status == rs::Cancelled);
        ORCHA_ASSERT_EQ(route.handle(req("POST", "/api/jobs/missing/run")).status, Agent::status::NotFound);
        std::cout << "[PASS] test_job_route_queues_runs\n";
    }

    inline void test_http_server_sends_custom_headers() {
#if !defined(_WIN32)
        Agent::HttpServer server([](const Agent::HttpRequest&) {
            return Agent::HttpResponse::text(Agent::status::Accepted, "queued")
                .with_header("Location", "/workflow/runs/abc")
                .with_header("Retry-After", "7");
        });
        server.start("127.0.0.1", 0);
        shutdown::Client c(server.port());
        c.send(shutdown::get_request("/x"));
        const auto resp = c.read_all();
        ORCHA_ASSERT(resp.rfind("HTTP/1.1 202", 0) == 0);
        ORCHA_ASSERT(resp.find("Location: /workflow/runs/abc") != std::string::npos);
        ORCHA_ASSERT(resp.find("Retry-After: 7") != std::string::npos);
        server.stop();
        std::cout << "[PASS] test_http_server_sends_custom_headers\n";
#else
        std::cout << "[SKIP] test_http_server_sends_custom_headers\n";
#endif
    }

    inline void test_workflow_route_accepts_yaml() {
        using namespace runq;
        Env env({2, 8});
        Agent::Routes::WorkflowRoute route(env.runs, nullptr, {std::chrono::seconds(10), 1});
        const auto raw = [](const std::string& content_type, std::string body,
                            std::optional<std::string> prefer = std::nullopt) {
            Agent::HttpRequest r;
            r.method = "POST";
            r.path = "/workflow";
            r.body = std::move(body);
            if (!content_type.empty()) r.headers.emplace_back("Content-Type", content_type);
            if (prefer) r.headers.emplace_back("Prefer", *prefer);
            return r;
        };

        // YAML runs like JSON (ms is a YAML integer, so the step gets a number).
        auto yaml = route.handle(raw("application/yaml",
            "# deploy\nsteps:\n  - name: one\n    command: work\n    params:\n      ms: 10\n", "wait=5"));
        ORCHA_ASSERT_EQ(yaml.status, Agent::status::OK);
        ORCHA_ASSERT(Json::parse(yaml.body)["status"] == "success");
        ORCHA_ASSERT_EQ(route.handle(raw("text/yaml; charset=utf-8", "steps:\n  - command: work\n")).status,
                        Agent::status::Accepted);

        // Errors: position for YAML, settings validated as for JSON, size capped.
        auto bad = route.handle(raw("application/yaml", "steps:\n  - [unclosed\n"));
        ORCHA_ASSERT_EQ(bad.status, Agent::status::BadRequest);
        auto err = Json::parse(bad.body);
        ORCHA_ASSERT(err.contains("line") && err["error"].get<std::string>().rfind("Invalid YAML body: ", 0) == 0);
        auto settings = route.handle(raw("application/yaml", "rollback: sideways\nsteps: []\n"));
        ORCHA_ASSERT_EQ(settings.status, Agent::status::BadRequest);
        ORCHA_ASSERT(Json::parse(settings.body)["error"].get<std::string>().rfind("Invalid workflow: ", 0) == 0);
        ORCHA_ASSERT_EQ(route.handle(raw("application/yaml", std::string(Agent::kMaxYamlBodyBytes + 1, '#'))).status,
                        Agent::status::PayloadTooLarge);

        // JSON is unchanged; media types are matched case-insensitively.
        ORCHA_ASSERT_EQ(route.handle(raw("Application/JSON", R"({"steps":[{"command":"work"}]})")).status,
                        Agent::status::Accepted);
        auto bad_json = route.handle(raw("application/json", "{"));
        ORCHA_ASSERT(Json::parse(bad_json.body)["error"].get<std::string>().rfind("Invalid JSON body: ", 0) == 0);
        auto other = route.handle(raw("text/plain", "steps: []"));
        ORCHA_ASSERT_EQ(other.status, Agent::status::UnsupportedMediaType);
        ORCHA_ASSERT(Json::parse(other.body)["error"] == "Send the workflow as application/json or application/yaml");
        ORCHA_ASSERT_EQ(route.handle(raw("", R"({"steps":[]})")).status, Agent::status::UnsupportedMediaType);
        std::cout << "[PASS] test_workflow_route_accepts_yaml\n";
    }

    inline void run_run_queue_tests() {
        std::cout << "\n-- Async Runs (Run Queue) --\n";
        test_run_queue_lifecycle();
        test_run_queue_bounds_concurrency();
        test_run_queue_rejects_when_full();
        test_run_queue_idle_runners_admit_without_queue_slots();
        test_run_queue_cancel();
        test_run_queue_wait_timeout();
        test_run_queue_stop();
        test_run_queue_nested_run_job_inline();
        test_run_queue_nested_run_not_cancellable();
        test_run_queue_nested_run_inherits_deadline();
        test_run_queue_invalid_utf8_output();
        test_store_marks_interrupted_runs();
        test_store_update_run_is_monotonic();
        test_store_prunes_old_runs();
        test_store_persistent_flag();
        test_store_migrates_queued_at();
        test_scheduler_records_skipped_run();
        test_prefer_wait_parsing();
        test_workflow_route_async_and_wait();
        test_workflow_route_waiter_cap();
        test_workflow_route_accepts_yaml();
        test_workflow_route_runs_endpoints();
        test_job_route_queues_runs();
        test_http_server_sends_custom_headers();
    }

} // namespace Orcha::Tests
