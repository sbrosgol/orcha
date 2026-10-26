//
// TimeoutTests.hpp - Step/workflow timeouts, cancellation and the plugin ABI check
//
// Engine behaviour is tested with in-process commands.
//

#pragma once

#include "../core/CommandRegistry.hpp"
#include "../core/ExecutionControl.hpp"
#include "../jobs/JobService.hpp"
#include "../jobs/RunJobCommand.hpp"
#include "../jobs/SqliteJobStore.hpp"
#include "../workflow/WorkflowEngine.hpp"

#include "Assertions.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>


namespace Orcha::Tests {

    namespace timeouts {

        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        inline long long ms_since(Clock::time_point start) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        }

        /// Works in 10 ms slices for params.ms, stopping early when asked.
        class CooperativeCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "cooperative"; }
            Json execute(const Json& params) override { return execute_with_control(params, {}); }
            Json execute_with_control(const Json& params, const Core::ExecutionControl& control) override {
                const auto until = Clock::now() + std::chrono::milliseconds(params.value("ms", 0));
                while (Clock::now() < until) {
                    if (control.should_stop()) {
                        return Json{{"success", false}, {"error", "stopped"}};
                    }
                    std::this_thread::sleep_for(10ms);
                }
                return Json{{"done", true}};
            }
        };

        /// Sleeps for params.ms and ignores its ExecutionControl.
        class StubbornCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "stubborn"; }
            Json execute(const Json& params) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(params.value("ms", 0)));
                return Json{{"done", true}};
            }
        };

        inline std::shared_ptr<Core::CommandRegistry> make_registry() {
            auto registry = std::make_shared<Core::CommandRegistry>();
            (void)registry->register_command(std::make_shared<CooperativeCommand>());
            (void)registry->register_command(std::make_shared<StubbornCommand>());
            return registry;
        }

        inline Workflow::WorkflowStep step(const std::string& command, int ms,
                                           std::optional<int> timeout_ms = std::nullopt) {
            Workflow::WorkflowStep s{command, Json{{"ms", ms}}};
            s.timeout_ms = timeout_ms;
            return s;
        }

        inline Workflow::WorkflowEngine make_engine(std::shared_ptr<Core::CommandRegistry> registry,
                                                    Workflow::EngineOptions options = {}) {
            return Workflow::WorkflowEngine(std::move(registry),
                                            std::make_shared<Workflow::SyncStepExecutor>(),
                                            nullptr, std::move(options));
        }

    } // namespace timeouts

    // ========== ExecutionControl ==========

    inline void test_execution_control_basics() {
        using namespace timeouts;
        Core::ExecutionControl unbounded;
        ORCHA_ASSERT_FALSE(unbounded.should_stop());
        ORCHA_ASSERT_FALSE(unbounded.remaining().has_value());

        std::stop_source source;
        Core::ExecutionControl past(source.get_token(), Clock::now() - 1ms);
        ORCHA_ASSERT(past.expired() && past.should_stop());
        ORCHA_ASSERT(past.remaining() == std::chrono::milliseconds::zero());

        Core::ExecutionControl future(source.get_token(), Clock::now() + 10s);
        ORCHA_ASSERT_FALSE(future.should_stop());
        source.request_stop();
        ORCHA_ASSERT(future.stop_requested() && future.should_stop());
        std::cout << "[PASS] test_execution_control_basics\n";
    }

    // ========== Step and workflow timeouts ==========

    inline void test_step_timeout_cooperative() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 5000, 100));
        def.steps.push_back(step("cooperative", 0));

        const auto start = Clock::now();
        auto result = engine.execute(def);
        const auto elapsed = ms_since(start);

        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Step timed out after 100 ms"));
        ORCHA_ASSERT(result.step_results[1].command_name.empty()); // never started
        ORCHA_ASSERT(elapsed >= 100 && elapsed < 1000);
        std::cout << "[PASS] test_step_timeout_cooperative\n";
    }

    inline void test_step_timeout_non_cooperative_fails_late() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("stubborn", 250, 50));

        auto result = engine.execute(def);
        ORCHA_ASSERT_FALSE(result.success);
        // Its {"done": true} output is not trusted: it returned past the deadline.
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Step timed out after 50 ms"));
        std::cout << "[PASS] test_step_timeout_non_cooperative_fails_late\n";
    }

    inline void test_step_within_timeout_succeeds() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 20, 2000));
        auto result = engine.execute(def);
        ORCHA_ASSERT(result.success);
        ORCHA_ASSERT(result.step_results[0].output.value("done", false));
        std::cout << "[PASS] test_step_within_timeout_succeeds\n";
    }

    inline void test_default_step_timeout_applies() {
        using namespace timeouts;
        Workflow::EngineOptions options;
        options.default_step_timeout = 80ms;
        auto engine = make_engine(make_registry(), options);

        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 5000));           // uses the default
        auto result = engine.execute(def);
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Step timed out after 80 ms"));

        Workflow::WorkflowDefinition overridden;
        overridden.steps.push_back(step("cooperative", 150, 1000)); // own limit wins
        ORCHA_ASSERT(engine.execute(overridden).success);
        std::cout << "[PASS] test_default_step_timeout_applies\n";
    }

    inline void test_workflow_timeout() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        auto def = Workflow::WorkflowDefinition::from_json(Json{
            {"timeout_ms", 150},
            {"steps", Json::array({
                {{"command", "cooperative"}, {"params", {{"ms", 100}}}},
                {{"command", "cooperative"}, {"params", {{"ms", 5000}}}},
                {{"command", "cooperative"}, {"params", {{"ms", 0}}}}})}});

        const auto start = Clock::now();
        auto result = engine.execute(def);
        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT(result.step_results[0].success);
        ORCHA_ASSERT_EQ(result.step_results[1].error_message, std::string("Workflow timed out after 150 ms"));
        ORCHA_ASSERT(result.step_results[2].command_name.empty());
        ORCHA_ASSERT(ms_since(start) < 1000);
        std::cout << "[PASS] test_workflow_timeout\n";
    }

    // ========== Cancellation ==========

    inline void test_cancel_running_workflow() {
        using namespace timeouts;
        std::stop_source shutdown;
        Workflow::EngineOptions options;
        options.cancel = shutdown.get_token();
        auto engine = make_engine(make_registry(), options);

        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 10000));
        def.steps.push_back(step("cooperative", 0));

        const auto start = Clock::now();
        auto running = std::async(std::launch::async, [&] { return engine.execute(def); });
        std::this_thread::sleep_for(100ms);
        shutdown.request_stop();
        auto result = running.get();

        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Cancelled: stopped"));
        ORCHA_ASSERT(result.step_results[1].command_name.empty());
        ORCHA_ASSERT(ms_since(start) < 1000);

        // Already cancelled: nothing runs at all.
        auto again = engine.execute(def);
        ORCHA_ASSERT_EQ(again.step_results[0].error_message, std::string("Cancelled"));
        std::cout << "[PASS] test_cancel_running_workflow\n";
    }

    inline void test_cancel_reaches_parallel_steps() {
        using namespace timeouts;
        std::stop_source shutdown;
        Workflow::EngineOptions options;
        options.cancel = shutdown.get_token();
        auto engine = make_engine(make_registry(), options);

        Workflow::WorkflowDefinition def;
        for (int i = 0; i < 3; ++i) {
            auto s = step("cooperative", 10000);
            s.parallel = true;
            def.steps.push_back(s);
        }

        const auto start = Clock::now();
        auto running = std::async(std::launch::async, [&] { return engine.execute(def); });
        std::this_thread::sleep_for(100ms);
        shutdown.request_stop();
        auto result = running.get();

        ORCHA_ASSERT_FALSE(result.success);
        for (const auto& r : result.step_results) {
            ORCHA_ASSERT_EQ(r.error_message, std::string("Cancelled: stopped"));
        }
        ORCHA_ASSERT(ms_since(start) < 1000);
        std::cout << "[PASS] test_cancel_reaches_parallel_steps\n";
    }

    inline void test_cancel_reaches_nested_run_job() {
        using namespace timeouts;
        auto registry = make_registry();
        auto store = std::make_shared<Jobs::SqliteJobStore>(":memory:");
        // Nested engines get NO cancel token of their own: cancellation must
        // reach them through the enclosing run's ExecutionContext.
        auto service = std::make_shared<Jobs::JobService>(store, [registry] {
            return std::make_shared<Workflow::WorkflowEngine>(registry);
        });
        ORCHA_ASSERT(registry->register_command(std::make_shared<Jobs::RunJobCommand>(service)));

        Jobs::JobDefinition inner;
        inner.name = "inner";
        inner.definition = Json{{"steps", Json::array({
            {{"command", "cooperative"}, {"params", {{"ms", 10000}}}}})}};
        ORCHA_ASSERT(store->create_job(inner));

        std::stop_source shutdown;
        Workflow::EngineOptions options;
        options.cancel = shutdown.get_token();
        auto outer = make_engine(registry, options);
        Workflow::WorkflowDefinition def;
        def.steps.push_back({"run_job", Json{{"job", "inner"}}});

        const auto start = Clock::now();
        auto running = std::async(std::launch::async, [&] { return outer.execute(def); });
        std::this_thread::sleep_for(100ms);
        shutdown.request_stop();
        auto result = running.get();

        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT(result.step_results[0].error_message.find("Cancelled") != std::string::npos);
        ORCHA_ASSERT(ms_since(start) < 1000);
        std::cout << "[PASS] test_cancel_reaches_nested_run_job\n";
    }

    // ========== Plugin ABI ==========

    inline void test_plugin_abi_version_checked() {
#if defined(ORCHA_TEST_LEGACY_PLUGIN_PATH) && defined(ORCHA_TEST_SLOW_PLUGIN_PATH)
        // Order matters under ASan on macOS: it does not unregister an unloaded
        // image's globals, so mapping a DIFFERENT instrumented library at the
        // refused plugin's old address yields a false global-buffer-overflow.
        // Load the versioned plugin first and refuse the legacy one last.
        Core::CommandRegistry registry;
        ORCHA_ASSERT(registry.load_command_library(ORCHA_TEST_SLOW_PLUGIN_PATH));
        ORCHA_ASSERT(registry.unregister_command("slow_test_plugin"));
        ORCHA_ASSERT_FALSE(registry.load_command_library(ORCHA_TEST_LEGACY_PLUGIN_PATH));
        ORCHA_ASSERT_FALSE(registry.has_command("slow_test_plugin"));
        std::cout << "[PASS] test_plugin_abi_version_checked\n";
#else
        std::cout << "[SKIP] test_plugin_abi_version_checked (no test plugins)\n";
#endif
    }

    inline void run_timeout_tests() {
        std::cout << "\n-- Timeouts & Cancellation --\n";
        test_execution_control_basics();
        test_step_timeout_cooperative();
        test_step_timeout_non_cooperative_fails_late();
        test_step_within_timeout_succeeds();
        test_default_step_timeout_applies();
        test_workflow_timeout();
        test_cancel_running_workflow();
        test_cancel_reaches_parallel_steps();
        test_cancel_reaches_nested_run_job();
        test_plugin_abi_version_checked();   // last: see the ASan note inside
    }

} // namespace Orcha::Tests
