//
// JobStoreTests.hpp - Tests for the jobs subsystem (Phase 2)
//
// Exercises SqliteJobStore against an in-memory database, JobRoute path
// matching, and job-to-job linking via the run_job command.
//
// Uses ORCHA_ASSERT (always-on) rather than <cassert>'s assert(): Release
// builds define NDEBUG, which turns assert() into a no-op that does NOT even
// evaluate its argument — so any side-effecting check (create_job, etc.) would
// silently not run.
//

#pragma once

#include "TestFixtures.hpp"   // ORCHA_ASSERT (throws on failure, NDEBUG-proof)
#include "../jobs/SqliteJobStore.hpp"
#include "../jobs/JobService.hpp"
#include "../jobs/RunJobCommand.hpp"
#include "../jobs/JobScheduler.hpp"
#include "../agent/routes/JobRoute.hpp"
#include "../core/CommandRegistry.hpp"
#include "../workflow/WorkflowEngine.hpp"
#include "mocks/MockCommandRegistry.hpp"

#include <ctime>
#include <iostream>

namespace Orcha::Tests {

    inline Jobs::JobDefinition make_job(const std::string& name) {
        Jobs::JobDefinition d;
        d.name = name;
        d.description = "test job";
        d.definition = web::json::value::parse(utility::conversions::to_string_t(
            R"({"steps":[{"command":"echo","params":{"message":"a"}},)"
            R"({"command":"echo","params":{"message":"{{step1.output.message}}"}}]})"));
        return d;
    }

    inline void test_job_store_crud() {
        Jobs::SqliteJobStore store(":memory:");

        auto job = make_job("alpha");
        ORCHA_ASSERT(store.create_job(job));
        ORCHA_ASSERT(!job.id.empty());
        ORCHA_ASSERT(!job.created_at.empty());

        auto got = store.get_job(job.id);
        ORCHA_ASSERT(got && got->name == "alpha");
        ORCHA_ASSERT(got->definition.at(U("steps")).as_array().size() == 2);

        auto byName = store.get_job_by_name("alpha");
        ORCHA_ASSERT(byName && byName->id == job.id);

        ORCHA_ASSERT(store.list_jobs().size() == 1);

        job.description = "updated";
        job.enabled = false;
        ORCHA_ASSERT(store.update_job(job));
        ORCHA_ASSERT(store.get_job(job.id)->description == "updated");
        ORCHA_ASSERT(store.get_job(job.id)->enabled == false);

        ORCHA_ASSERT(store.delete_job(job.id));
        ORCHA_ASSERT(!store.get_job(job.id));
        ORCHA_ASSERT(!store.delete_job(job.id));

        std::cout << "[PASS] test_job_store_crud\n";
    }

    inline void test_job_store_name_unique() {
        Jobs::SqliteJobStore store(":memory:");
        auto a = make_job("dup");
        auto b = make_job("dup");
        ORCHA_ASSERT(store.create_job(a));
        ORCHA_ASSERT(!store.create_job(b));   // duplicate name violates UNIQUE
        ORCHA_ASSERT(store.list_jobs().size() == 1);
        std::cout << "[PASS] test_job_store_name_unique\n";
    }

    inline void test_job_store_runs() {
        Jobs::SqliteJobStore store(":memory:");
        auto job = make_job("withruns");
        ORCHA_ASSERT(store.create_job(job));

        Jobs::RunRecord r1;
        r1.job_id = job.id; r1.trigger = "manual"; r1.status = "success";
        r1.result = web::json::value::array();
        ORCHA_ASSERT(store.insert_run(r1));
        ORCHA_ASSERT(!r1.id.empty());

        Jobs::RunRecord r2;  // ad-hoc (no job id)
        r2.trigger = "api"; r2.status = "failed"; r2.error = "boom";
        ORCHA_ASSERT(store.insert_run(r2));

        ORCHA_ASSERT(store.list_runs(job.id, 50).size() == 1);
        ORCHA_ASSERT(store.list_runs(std::nullopt, 50).size() == 2);

        auto got = store.get_run(r2.id);
        ORCHA_ASSERT(got && got->status == "failed" && got->error == "boom" && !got->job_id);

        std::cout << "[PASS] test_job_store_runs\n";
    }

    inline void test_job_route_can_handle() {
        Agent::Routes::JobRoute route(nullptr, nullptr); // can_handle does not deref service
        ORCHA_ASSERT(route.can_handle("GET", "/api/jobs"));
        ORCHA_ASSERT(route.can_handle("POST", "/api/jobs"));
        ORCHA_ASSERT(route.can_handle("GET", "/api/jobs/abc"));
        ORCHA_ASSERT(route.can_handle("POST", "/api/jobs/abc/run"));
        ORCHA_ASSERT(route.can_handle("GET", "/api/jobs/abc/runs"));
        ORCHA_ASSERT(route.can_handle("GET", "/api/runs"));
        ORCHA_ASSERT(route.can_handle("GET", "/api/runs/xyz"));
        ORCHA_ASSERT(!route.can_handle("GET", "/api/plugins"));
        ORCHA_ASSERT(!route.can_handle("GET", "/api/jobsX"));
        std::cout << "[PASS] test_job_route_can_handle\n";
    }

    inline void test_run_job_linking() {
        auto store = std::make_shared<Jobs::SqliteJobStore>(":memory:");
        auto registry = std::make_shared<Core::CommandRegistry>();

        // Mock "echo" command: returns { echoed: <message> }.
        ORCHA_ASSERT(registry->register_command(std::make_shared<Mocks::MockCommand>("echo",
            [](const web::json::value& p){
                web::json::value o = web::json::value::object();
                o[U("echoed")] = p.has_field(U("message"))
                    ? p.at(U("message")) : web::json::value::string(U(""));
                return o;
            })));

        auto factory = [registry]() -> std::shared_ptr<Workflow::IWorkflowEngine> {
            return std::make_shared<Workflow::WorkflowEngine>(
                registry, std::make_shared<Workflow::SyncStepExecutor>(), nullptr);
        };
        auto service = std::make_shared<Jobs::JobService>(store, factory, nullptr);
        ORCHA_ASSERT(registry->register_command(std::make_shared<Jobs::RunJobCommand>(service)));

        Jobs::JobDefinition leaf; leaf.name = "leaf";
        leaf.definition = web::json::value::parse(utility::conversions::to_string_t(
            R"({"steps":[{"command":"echo","params":{"message":"hi"}}]})"));
        ORCHA_ASSERT(store->create_job(leaf));

        // Parent runs leaf, then echoes a NESTED reference into the sub-job's
        // last step output -> also exercises the multi-level placeholder fix.
        Jobs::JobDefinition parent; parent.name = "parent";
        parent.definition = web::json::value::parse(utility::conversions::to_string_t(
            R"({"steps":[{"command":"run_job","params":{"job":"leaf"}},)"
            R"({"command":"echo","params":{"message":"leaf said {{step1.output.last.echoed}}"}}]})"));
        ORCHA_ASSERT(store->create_job(parent));

        auto run = service->run_job(parent.id, "manual");
        ORCHA_ASSERT(run && run->status == "success");
        ORCHA_ASSERT(run->result.is_array() && run->result.as_array().size() == 2);

        const auto& stepOut = run->result.as_array().at(0).at(U("output"));
        ORCHA_ASSERT(stepOut.at(U("status")).as_string() == U("success"));
        // Nested placeholder {{step1.output.last.echoed}} must resolve fully.
        const auto& step2 = run->result.as_array().at(1).at(U("output"));
        ORCHA_ASSERT(step2.at(U("echoed")).as_string() == U("leaf said hi"));

        // Self-referential job must fail via the cycle guard (not hang/crash).
        Jobs::JobDefinition loop; loop.name = "loop";
        loop.definition = web::json::value::parse(utility::conversions::to_string_t(
            R"({"steps":[{"command":"run_job","params":{"job":"loop"}}]})"));
        ORCHA_ASSERT(store->create_job(loop));
        auto looprun = service->run_job(loop.id, "manual");
        ORCHA_ASSERT(looprun && looprun->status == "failed");

        std::cout << "[PASS] test_run_job_linking\n";
    }

    inline void test_scheduler_fires() {
        auto store = std::make_shared<Jobs::SqliteJobStore>(":memory:");
        auto registry = std::make_shared<Core::CommandRegistry>();
        ORCHA_ASSERT(registry->register_command(std::make_shared<Mocks::MockCommand>("echo",
            [](const web::json::value&){ return web::json::value::object(); })));
        auto factory = [registry]() -> std::shared_ptr<Workflow::IWorkflowEngine> {
            return std::make_shared<Workflow::WorkflowEngine>(
                registry, std::make_shared<Workflow::SyncStepExecutor>(), nullptr);
        };
        auto service = std::make_shared<Jobs::JobService>(store, factory, nullptr);

        const auto def = web::json::value::parse(utility::conversions::to_string_t(
            R"({"steps":[{"command":"echo","params":{}}]})"));

        Jobs::JobDefinition on;   on.name = "tick"; on.enabled = true;
        on.schedule_cron = "* * * * *"; on.definition = def;
        ORCHA_ASSERT(store->create_job(on));

        Jobs::JobDefinition off;  off.name = "off"; off.enabled = false;  // disabled
        off.schedule_cron = "* * * * *"; off.definition = def;
        ORCHA_ASSERT(store->create_job(off));

        Jobs::JobDefinition noon; noon.name = "noon"; noon.enabled = true;
        noon.schedule_cron = "0 12 * * *"; noon.definition = def;
        ORCHA_ASSERT(store->create_job(noon));

        Jobs::JobScheduler sched(service, nullptr, std::chrono::seconds(30));

        auto utc = [](int y,int mo,int d,int h,int mi){
            std::tm t{}; t.tm_year=y-1900; t.tm_mon=mo-1; t.tm_mday=d; t.tm_hour=h; t.tm_min=mi;
            time_t e = timegm(&t); std::tm o{}; gmtime_r(&e,&o); return o; };

        // 00:00 -> only "tick" (enabled, matches); "off" disabled; "noon" not due.
        ORCHA_ASSERT(sched.evaluate(utc(2024,1,1,0,0)) == 1);
        ORCHA_ASSERT(sched.evaluate(utc(2024,1,1,0,0)) == 0);   // de-duped within the minute
        ORCHA_ASSERT(sched.evaluate(utc(2024,1,1,0,1)) == 1);   // new minute -> fires again
        ORCHA_ASSERT(sched.evaluate(utc(2024,1,1,12,0)) == 2);  // "tick" + "noon"

        auto runs = store->list_runs(on.id, 10);
        ORCHA_ASSERT(!runs.empty() && runs[0].trigger == "schedule");

        std::cout << "[PASS] test_scheduler_fires\n";
    }

    inline void run_job_store_tests() {
        std::cout << "\n-- Jobs (Phase 2) --\n";
        test_job_store_crud();
        test_job_store_name_unique();
        test_job_store_runs();
        test_job_route_can_handle();
        test_run_job_linking();
        test_scheduler_fires();
    }

} // namespace Orcha::Tests
