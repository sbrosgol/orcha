//
// ResilienceTests.hpp - Per-workflow rollback and circuit breaking
//
// Rollback: reverse completion order, resolved parameters, reverse vs
// best_effort, not-supported commands, rollback on timeout, no rollback when
// cancelled, and validation of the setting at the HTTP boundary.
// Circuit breaking: shared state across runs/engines, which failures count,
// the single half-open probe, recovery, and the admin API.
//

#pragma once

#include "../agent/routes/CircuitBreakerRoute.hpp"
#include "../agent/routes/JobRoute.hpp"
#include "../agent/routes/WorkflowRoute.hpp"
#include "../config/ConfigValidator.hpp"
#include "../config/YamlConfiguration.hpp"
#include "../core/CircuitBreaker.hpp"
#include "../core/CommandRegistry.hpp"
#include "../jobs/JobService.hpp"
#include "../jobs/RunQueue.hpp"
#include "../jobs/SqliteJobStore.hpp"
#include "../workflow/WorkflowEngine.hpp"

#include "Assertions.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace Orcha::Tests {

    namespace resilience {

        using namespace std::chrono_literals;

        /// Records the ids of rolled-back steps, in order.
        struct RollbackLog {
            std::mutex mutex;
            std::vector<std::string> ids;
            void add(std::string id) { std::lock_guard l(mutex); ids.push_back(std::move(id)); }
            std::vector<std::string> get() { std::lock_guard l(mutex); return ids; }
        };

        /// params: id, ms (sleep), fail (throw), rollback_fails (rollback throws).
        class UndoableCommand : public Core::ICommand {
        public:
            explicit UndoableCommand(std::shared_ptr<RollbackLog> log) : log_(std::move(log)) {}
            [[nodiscard]] std::string name() const override { return "undoable"; }
            Json execute(const Json& params) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(params.value("ms", 0)));
                if (params.value("fail", false)) throw std::runtime_error("step failed");
                return Json{{"id", params.value("id", "")}};
            }
            void rollback(const Json& params) override {
                if (params.value("rollback_fails", false)) throw std::runtime_error("undo failed");
                log_->add(params.value("id", ""));
            }
            [[nodiscard]] Core::CommandMetadata metadata() const override {
                Core::CommandMetadata m;
                m.name = name();
                m.supports_rollback = true;
                return m;
            }
        private:
            std::shared_ptr<RollbackLog> log_;
        };

        /// Succeeds; does not support rollback.
        class PlainCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "plain"; }
            Json execute(const Json&) override { return Json::object(); }
        };

        /// Counts executions; fails when params.fail; requires params.n.
        class FlakyCommand : public Core::ICommand {
        public:
            std::atomic<int> calls{0};
            [[nodiscard]] std::string name() const override { return "flaky"; }
            Json execute(const Json& params) override {
                ++calls;
                if (params.value("fail", false)) return Json{{"success", false}, {"error", "downstream down"}};
                return Json{{"ok", true}};
            }
            [[nodiscard]] Core::CommandMetadata metadata() const override {
                Core::CommandMetadata m;
                m.name = name();
                Core::CommandParameter n;
                n.name = "n";
                n.type = "int";
                n.required = true;
                m.parameters.push_back(n);
                return m;
            }
        };

        /// Sleeps in slices for params.ms, stopping when asked.
        class WaitCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "wait"; }
            Json execute(const Json& params) override { return execute_with_control(params, {}); }
            Json execute_with_control(const Json& params, const Core::ExecutionControl& control) override {
                const auto until = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(params.value("ms", 0));
                while (std::chrono::steady_clock::now() < until) {
                    if (control.should_stop()) return Json{{"success", false}, {"error", "stopped"}};
                    std::this_thread::sleep_for(5ms);
                }
                return Json::object();
            }
        };

        struct Fixture {
            std::shared_ptr<RollbackLog> log = std::make_shared<RollbackLog>();
            std::shared_ptr<FlakyCommand> flaky = std::make_shared<FlakyCommand>();
            std::shared_ptr<Core::CommandRegistry> registry = std::make_shared<Core::CommandRegistry>();
            Fixture() {
                (void)registry->register_command(std::make_shared<UndoableCommand>(log));
                (void)registry->register_command(std::make_shared<PlainCommand>());
                (void)registry->register_command(flaky);
                (void)registry->register_command(std::make_shared<WaitCommand>());
            }
            Workflow::WorkflowEngine engine(Workflow::EngineOptions options = {}) const {
                return Workflow::WorkflowEngine(registry, std::make_shared<Workflow::SyncStepExecutor>(),
                                                nullptr, std::move(options));
            }
        };

        inline Json step(const std::string& command, Json params = Json::object()) {
            return Json{{"command", command}, {"params", std::move(params)}};
        }

        /// Run @p wf on @p engine (an lvalue or a temporary).
        template <typename Engine>
        Workflow::WorkflowResult run(Engine&& engine, const Json& wf) {
            return engine.execute(Workflow::WorkflowDefinition::from_json(wf));
        }

        inline std::string rollback_status(const Workflow::WorkflowStepResult& r) {
            return r.rollback ? r.rollback->to_json()["status"].get<std::string>() : "<none>";
        }

        inline Agent::HttpRequest json_request(const std::string& method, const std::string& path,
                                               const Json& body) {
            Agent::HttpRequest req;
            req.method = method;
            req.path = path;
            req.headers.emplace_back("Content-Type", "application/json");
            req.body = body.dump();
            return req;
        }

    } // namespace resilience

    // ========== Rollback ==========

    inline void test_rollback_reverse_order_with_resolved_params() {
        using namespace resilience;
        Fixture fx;
        auto engine = fx.engine();
        auto result = run(engine, Json{
            {"rollback", "reverse"},
            {"steps", Json::array({
                step("undoable", {{"id", "A"}}),
                step("undoable", {{"id", "B-{{step1.output.id}}"}}),
                step("plain"),
                step("undoable", {{"id", "C"}, {"fail", true}})})}});

        ORCHA_ASSERT_FALSE(result.success);
        // Reverse completion order, with the parameters each step ran with.
        ORCHA_ASSERT(fx.log->get() == (std::vector<std::string>{"B-A", "A"}));
        ORCHA_ASSERT_EQ(rollback_status(result.step_results[0]), std::string("rolled_back"));
        ORCHA_ASSERT_EQ(rollback_status(result.step_results[1]), std::string("rolled_back"));
        ORCHA_ASSERT_EQ(rollback_status(result.step_results[2]), std::string("not_supported"));
        ORCHA_ASSERT_EQ(rollback_status(result.step_results[3]), std::string("<none>")); // the failed step

        // Reported in the API result array.
        const auto json = result.to_json();
        ORCHA_ASSERT_EQ(json[0]["rollback"]["status"].get<std::string>(), std::string("rolled_back"));
        ORCHA_ASSERT_FALSE(json[3].contains("rollback"));
        std::cout << "[PASS] test_rollback_reverse_order_with_resolved_params\n";
    }

    inline void test_no_rollback_unless_configured() {
        using namespace resilience;
        Fixture fx;
        auto engine = fx.engine();
        for (const char* setting : {"", "none"}) {
            Json wf{{"steps", Json::array({step("undoable", {{"id", "A"}}),
                                           step("undoable", {{"fail", true}})})}};
            if (*setting) wf["rollback"] = setting;
            auto result = run(engine, wf);
            ORCHA_ASSERT_FALSE(result.success);
            ORCHA_ASSERT_FALSE(result.step_results[0].rollback.has_value());
        }
        // A successful run is never rolled back.
        auto ok = run(engine, Json{{"rollback", "reverse"},
                                   {"steps", Json::array({step("undoable", {{"id", "A"}})})}});
        ORCHA_ASSERT(ok.success);
        ORCHA_ASSERT(fx.log->get().empty());
        std::cout << "[PASS] test_no_rollback_unless_configured\n";
    }

    inline void test_rollback_reverse_halts_best_effort_continues() {
        using namespace resilience;
        auto steps = Json::array({step("undoable", {{"id", "A"}}),
                                  step("undoable", {{"id", "B"}, {"rollback_fails", true}}),
                                  step("undoable", {{"fail", true}})});
        {
            Fixture fx;
            auto result = run(fx.engine(), Json{{"rollback", "reverse"}, {"steps", steps}});
            ORCHA_ASSERT_EQ(rollback_status(result.step_results[1]), std::string("failed"));
            ORCHA_ASSERT_EQ(result.step_results[1].rollback->message, std::string("undo failed"));
            ORCHA_ASSERT_EQ(rollback_status(result.step_results[0]), std::string("skipped"));
            ORCHA_ASSERT(fx.log->get().empty());
        }
        {
            Fixture fx;
            auto result = run(fx.engine(), Json{{"rollback", "best_effort"}, {"steps", steps}});
            ORCHA_ASSERT_EQ(rollback_status(result.step_results[1]), std::string("failed"));
            ORCHA_ASSERT_EQ(rollback_status(result.step_results[0]), std::string("rolled_back"));
            ORCHA_ASSERT(fx.log->get() == (std::vector<std::string>{"A"}));
        }
        std::cout << "[PASS] test_rollback_reverse_halts_best_effort_continues\n";
    }

    inline void test_rollback_on_timeout() {
        using namespace resilience;
        Fixture fx;
        Json slow = step("wait", {{"ms", 5000}});
        slow["timeout_ms"] = 50;
        auto result = run(fx.engine(), Json{{"rollback", "reverse"},
                                            {"steps", Json::array({step("undoable", {{"id", "A"}}), slow})}});
        ORCHA_ASSERT_EQ(result.step_results[1].error_message, std::string("Step timed out after 50 ms"));
        ORCHA_ASSERT(fx.log->get() == (std::vector<std::string>{"A"}));
        std::cout << "[PASS] test_rollback_on_timeout\n";
    }

    // Server shutdown (EngineOptions::cancel) skips rollback: the process is
    // going away and compensations could not finish anyway.
    inline void test_rollback_skipped_on_shutdown() {
        using namespace resilience;
        Fixture fx;
        std::stop_source shutdown;
        Workflow::EngineOptions options;
        options.cancel = shutdown.get_token();
        auto engine = fx.engine(options);

        auto running = std::async(std::launch::async, [&] {
            return run(engine, Json{{"rollback", "reverse"},
                                    {"steps", Json::array({step("undoable", {{"id", "A"}}),
                                                           step("wait", {{"ms", 10000}})})}});
        });
        std::this_thread::sleep_for(100ms);
        shutdown.request_stop();
        auto result = running.get();

        ORCHA_ASSERT_EQ(rollback_status(result.step_results[0]), std::string("skipped"));
        ORCHA_ASSERT_EQ(result.step_results[0].rollback->message, std::string("server shutting down"));
        ORCHA_ASSERT(fx.log->get().empty());
        std::cout << "[PASS] test_rollback_skipped_on_shutdown\n";
    }

    // A user cancel (the run's own stop token, as RunQueue installs it) rolls
    // back the completed steps like any other failure.
    inline void test_rollback_on_user_cancel() {
        using namespace resilience;
        Fixture fx;
        std::stop_source user_cancel;
        auto engine = fx.engine();

        auto running = std::async(std::launch::async, [&] {
            Workflow::ExecutionContext::Scope scope({{}, user_cancel.get_token()});
            return run(engine, Json{{"rollback", "reverse"},
                                    {"steps", Json::array({step("undoable", {{"id", "A"}}),
                                                           step("wait", {{"ms", 10000}})})}});
        });
        std::this_thread::sleep_for(100ms);
        user_cancel.request_stop();
        auto result = running.get();

        ORCHA_ASSERT(!result.success);
        ORCHA_ASSERT_EQ(rollback_status(result.step_results[0]), std::string("rolled_back"));
        ORCHA_ASSERT(fx.log->get() == (std::vector<std::string>{"A"}));
        std::cout << "[PASS] test_rollback_on_user_cancel\n";
    }

    inline void test_rollback_follows_completion_order() {
        using namespace resilience;
        Fixture fx;
        Json slow = step("undoable", {{"id", "slow"}, {"ms", 150}});
        slow["parallel"] = true;
        Json fast = step("undoable", {{"id", "fast"}, {"ms", 10}});
        fast["parallel"] = true;
        // References both, so it runs after they complete (fast first).
        Json failing = step("undoable", {{"id", "{{step1.output.id}}{{step2.output.id}}"}, {"fail", true}});

        auto result = run(fx.engine(), Json{{"rollback", "reverse"},
                                            {"steps", Json::array({slow, fast, failing})}});
        ORCHA_ASSERT_FALSE(result.success);
        // Completed fast, then slow -> undone slow, then fast.
        ORCHA_ASSERT(fx.log->get() == (std::vector<std::string>{"slow", "fast"}));
        std::cout << "[PASS] test_rollback_follows_completion_order\n";
    }

    inline void test_invalid_resilience_settings_rejected() {
        using namespace resilience;
        auto expect_invalid = [](const Json& wf) {
            bool threw = false;
            try { (void)Workflow::WorkflowDefinition::from_json(wf); }
            catch (const std::invalid_argument&) { threw = true; }
            ORCHA_ASSERT(threw);
        };
        expect_invalid(Json{{"rollback", "sideways"}, {"steps", Json::array()}});
        expect_invalid(Json{{"rollback", true}, {"steps", Json::array()}});
        expect_invalid(Json{{"circuit_breaker", "yes"}, {"steps", Json::array()}});

        Fixture fx;
        auto engine = std::make_shared<Workflow::WorkflowEngine>(fx.registry);
        auto store = std::make_shared<Jobs::SqliteJobStore>(":memory:");
        auto service = std::make_shared<Jobs::JobService>(store, [engine] { return engine; });
        auto runs = std::make_shared<Jobs::RunQueue>(service, Jobs::RunQueue::Options{1, 8});

        // POST /workflow -> 400, nothing queued or executed.
        Agent::Routes::WorkflowRoute route(runs);
        auto resp = route.handle(json_request("POST", "/workflow",
            Json{{"rollback", "sideways"}, {"steps", Json::array({step("flaky", {{"n", 1}})})}}));
        ORCHA_ASSERT_EQ(resp.status, Agent::status::BadRequest);
        ORCHA_ASSERT_EQ(fx.flaky->calls.load(), 0);
        ORCHA_ASSERT(store->list_runs(std::nullopt, 10).empty());

        // Saving a job validates the definition too.
        Agent::Routes::JobRoute jobs(service, runs);
        auto created = jobs.handle(json_request("POST", "/api/jobs",
            Json{{"name", "bad"}, {"definition", {{"circuit_breaker", 1}, {"steps", Json::array()}}}}));
        ORCHA_ASSERT_EQ(created.status, Agent::status::BadRequest);
        std::cout << "[PASS] test_invalid_resilience_settings_rejected\n";
    }

    // ========== Circuit breaking ==========

    inline void test_circuit_breaker_opens_across_runs() {
        using namespace resilience;
        Fixture fx;
        Workflow::EngineOptions options;
        options.circuit_breakers = std::make_shared<Core::CircuitBreakerRegistry>();
        options.circuit_breaker_config = {2, 1, std::chrono::seconds(60)};
        // Two engines sharing one registry (as the server's engine factory does).
        auto engine_a = fx.engine(options);
        auto engine_b = fx.engine(options);
        const Json failing{{"circuit_breaker", true},
                           {"steps", Json::array({step("flaky", {{"n", 1}, {"fail", true}})})}};

        ORCHA_ASSERT_FALSE(run(engine_a, failing).success);
        ORCHA_ASSERT_FALSE(run(engine_b, failing).success);
        ORCHA_ASSERT_EQ(fx.flaky->calls.load(), 2);

        // Open: rejected without running the command.
        auto rejected = run(engine_a, failing);
        ORCHA_ASSERT(rejected.step_results[0].error_message.rfind("Circuit breaker open for 'flaky'", 0) == 0);
        ORCHA_ASSERT_EQ(fx.flaky->calls.load(), 2);

        // Workflows that do not opt in are unaffected.
        auto opted_out = run(engine_a, Json{{"steps", Json::array({step("flaky", {{"n", 1}})})}});
        ORCHA_ASSERT(opted_out.success);
        ORCHA_ASSERT_EQ(fx.flaky->calls.load(), 3);
        std::cout << "[PASS] test_circuit_breaker_opens_across_runs\n";
    }

    inline void test_circuit_breaker_ignores_non_health_failures() {
        using namespace resilience;
        Fixture fx;
        Workflow::EngineOptions options;
        options.circuit_breakers = std::make_shared<Core::CircuitBreakerRegistry>();
        options.circuit_breaker_config = {1, 1, std::chrono::seconds(60)};   // any counted failure opens
        auto engine = fx.engine(options);

        // Validation failures (missing required "n") are the caller's fault.
        for (int i = 0; i < 3; ++i) {
            auto r = run(engine, Json{{"circuit_breaker", true},
                                      {"steps", Json::array({step("flaky")})}});
            ORCHA_ASSERT(r.step_results[0].error_message.find("Validation failed") != std::string::npos);
        }
        // Cancelled runs say nothing about the command either.
        std::stop_source stop;
        stop.request_stop();
        Workflow::EngineOptions cancelled = options;
        cancelled.cancel = stop.get_token();
        auto c = fx.engine(cancelled);
        (void)run(c, Json{{"circuit_breaker", true}, {"steps", Json::array({step("flaky", {{"n", 1}})})}});

        auto& breaker = options.circuit_breakers->get_or_create("flaky", options.circuit_breaker_config);
        ORCHA_ASSERT(breaker.state() == Core::CircuitState::Closed);
        ORCHA_ASSERT(run(engine, Json{{"circuit_breaker", true},
                                      {"steps", Json::array({step("flaky", {{"n", 1}})})}}).success);
        std::cout << "[PASS] test_circuit_breaker_ignores_non_health_failures\n";
    }

    inline void test_circuit_breaker_half_open_single_probe() {
        Core::CircuitBreaker breaker("x", {1, 1, std::chrono::milliseconds(0)});
        breaker.record_failure();                        // -> Open (probe allowed at once)

        std::atomic<int> admitted{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 16; ++i) {
            threads.emplace_back([&] { if (breaker.allow_request()) ++admitted; });
        }
        for (auto& t : threads) t.join();
        ORCHA_ASSERT_EQ(admitted.load(), 1);             // previously all 16
        ORCHA_ASSERT(breaker.state() == Core::CircuitState::HalfOpen);

        breaker.abandon();                               // probe concluded without a verdict
        ORCHA_ASSERT(breaker.allow_request());           // a new probe may go
        breaker.record_success();
        ORCHA_ASSERT(breaker.state() == Core::CircuitState::Closed);
        std::cout << "[PASS] test_circuit_breaker_half_open_single_probe\n";
    }

    inline void test_circuit_breaker_recovers() {
        using namespace resilience;
        Fixture fx;
        Workflow::EngineOptions options;
        options.circuit_breakers = std::make_shared<Core::CircuitBreakerRegistry>();
        options.circuit_breaker_config = {1, 1, std::chrono::milliseconds(100)};
        auto engine = fx.engine(options);
        auto wf = [](bool fail) {
            return Json{{"circuit_breaker", true},
                        {"steps", Json::array({step("flaky", {{"n", 1}, {"fail", fail}})})}};
        };

        ORCHA_ASSERT_FALSE(run(engine, wf(true)).success);           // opens
        auto rejected = run(engine, wf(false));
        ORCHA_ASSERT(rejected.step_results[0].error_message.find("next attempt allowed in") != std::string::npos);
        std::this_thread::sleep_for(150ms);
        ORCHA_ASSERT(run(engine, wf(false)).success);                // probe succeeds -> closed
        ORCHA_ASSERT(run(engine, wf(false)).success);
        std::cout << "[PASS] test_circuit_breaker_recovers\n";
    }

    inline void test_circuit_breaker_admin_route() {
        using namespace resilience;
        auto registry = std::make_shared<Core::CircuitBreakerRegistry>();
        auto& breaker = registry->get_or_create("flaky", {1, 1, std::chrono::seconds(60)});
        breaker.record_failure();

        Agent::Routes::CircuitBreakerRoute route(registry);
        auto list = route.handle(json_request("GET", "/api/circuit-breakers", Json::object()));
        ORCHA_ASSERT_EQ(list.status, Agent::status::OK);
        auto body = Json::parse(list.body);
        ORCHA_ASSERT_EQ(body["count"].get<int>(), 1);
        ORCHA_ASSERT_EQ(body["circuit_breakers"][0]["command"].get<std::string>(), std::string("flaky"));
        ORCHA_ASSERT_EQ(body["circuit_breakers"][0]["state"].get<std::string>(), std::string("open"));
        ORCHA_ASSERT(body["circuit_breakers"][0]["last_failure"].is_string());

        auto reset = route.handle(json_request("POST", "/api/circuit-breakers/flaky/reset", Json::object()));
        ORCHA_ASSERT_EQ(reset.status, Agent::status::OK);
        ORCHA_ASSERT(breaker.state() == Core::CircuitState::Closed);

        ORCHA_ASSERT_EQ(route.handle(json_request("POST", "/api/circuit-breakers/nope/reset", Json::object())).status,
                        Agent::status::NotFound);
        ORCHA_ASSERT_EQ(route.handle(json_request("POST", "/api/circuit-breakers", Json::object())).status,
                        Agent::status::MethodNotAllowed);
        std::cout << "[PASS] test_circuit_breaker_admin_route\n";
    }

    inline void test_circuit_breaker_config_validation() {
        Config::YamlConfiguration config;
        config.load_from_string(R"(
workflow:
  circuit_breaker:
    failure_threshold: 0
    reset_timeout_ms: -1
)");
        auto result = Config::ConfigValidator().validate(config);
        ORCHA_ASSERT_FALSE(result.valid);
        ORCHA_ASSERT_EQ(result.get_error_messages().size(), static_cast<size_t>(2));
        std::cout << "[PASS] test_circuit_breaker_config_validation\n";
    }

    inline void run_resilience_tests() {
        std::cout << "\n-- Rollback & Circuit Breaking --\n";
        test_rollback_reverse_order_with_resolved_params();
        test_no_rollback_unless_configured();
        test_rollback_reverse_halts_best_effort_continues();
        test_rollback_on_timeout();
        test_rollback_skipped_on_shutdown();
        test_rollback_on_user_cancel();
        test_rollback_follows_completion_order();
        test_invalid_resilience_settings_rejected();
        test_circuit_breaker_opens_across_runs();
        test_circuit_breaker_ignores_non_health_failures();
        test_circuit_breaker_half_open_single_probe();
        test_circuit_breaker_recovers();
        test_circuit_breaker_admin_route();
        test_circuit_breaker_config_validation();
    }

} // namespace Orcha::Tests
