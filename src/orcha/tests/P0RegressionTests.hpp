//
// P0RegressionTests.hpp - Regression tests for the P0 hardening fixes
//
// Each test reproduces a defect found in the architecture / concurrency review:
// - in-band command failures ({"success": false}) were recorded as success
// - placeholder substitution re-scanned its own output (endless loop)
// - an out-of-range {{stepN}} index threw out of the engine
// - unregistering a plugin command while it executed unmapped its code
//

#pragma once

#include "../core/CommandRegistry.hpp"
#include "../workflow/WorkflowEngine.hpp"

#include "Assertions.hpp"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace Orcha::Tests {

    namespace p0 {

        /// Returns { "out": <params.value> } -- or a literal placeholder.
        class LiteralCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "literal"; }
            Json execute(const Json& params) override {
                if (params.contains("emit_placeholder")) {
                    // Built at runtime so it is not itself a placeholder in params.
                    return Json{{"out", std::string("{{st") + "ep1.output.out}}"}};
                }
                return Json{{"out", params.value("value", Json(nullptr))}};
            }
        };

        /// Reports failure in-band, like the bundled plugins do.
        class InBandFailureCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "inband_fail"; }
            Json execute(const Json&) override {
                return Json{{"success", false}, {"error", "exit code 2"}};
            }
        };

        /// Sleeps, then returns { "v": 42 }.
        class SlowValueCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "slow_value"; }
            Json execute(const Json& params) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(params.value("ms", 100)));
                return Json{{"v", 42}};
            }
        };

        inline std::shared_ptr<Core::CommandRegistry> make_registry() {
            auto registry = std::make_shared<Core::CommandRegistry>();
            (void)registry->register_command(std::make_shared<LiteralCommand>());
            (void)registry->register_command(std::make_shared<InBandFailureCommand>());
            (void)registry->register_command(std::make_shared<SlowValueCommand>());
            return registry;
        }

        /// Run @p fn on a worker, failing the whole test binary if it hangs.
        template <typename Fn>
        auto run_with_deadline(const char* test, Fn fn) {
            auto fut = std::async(std::launch::async, std::move(fn));
            if (fut.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
                std::cerr << "[FAIL] " << test << ": did not finish within 10s\n";
                std::_Exit(1);
            }
            return fut.get();
        }

    } // namespace p0

    // ========== Command failure contract ==========

    inline void test_inband_failure_fails_step() {
        Workflow::WorkflowEngine engine(p0::make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back({"inband_fail", Json::object()});
        def.steps.push_back({"literal", Json{{"value", "must not run"}}});

        auto result = engine.execute(def);
        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT_FALSE(result.step_results[0].success);
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("exit code 2"));
        ORCHA_ASSERT_EQ(result.error_message, std::string("exit code 2"));
        ORCHA_ASSERT(result.step_results[1].output.is_null()); // stopped after the failure
        std::cout << "[PASS] test_inband_failure_fails_step\n";
    }

    // ========== Placeholder resolution ==========

    inline void test_placeholder_output_is_not_reexpanded() {
        Workflow::WorkflowEngine engine(p0::make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back({"literal", Json{{"emit_placeholder", true}}});
        def.steps.push_back({"literal", Json{{"value", "{{step1.output.out}}"}}});

        auto result = p0::run_with_deadline("test_placeholder_output_is_not_reexpanded",
                                            [&] { return engine.execute(def); });
        ORCHA_ASSERT(result.success);
        // Substituted verbatim, never re-scanned.
        ORCHA_ASSERT_EQ(result.step_results[1].output["out"].get<std::string>(),
                        std::string("{{step1.output.out}}"));
        std::cout << "[PASS] test_placeholder_output_is_not_reexpanded\n";
    }

    inline void test_placeholder_out_of_range_index() {
        Workflow::WorkflowEngine engine(p0::make_registry());
        auto out = engine.execute_json(Json{{"steps", Json::array({
            {{"command", "literal"},
             {"params", {{"value", "a{{step99999999999999999999.output}}b"}}}}})}});
        ORCHA_ASSERT(out.is_array() && out.size() == 1);
        ORCHA_ASSERT(out[0]["success"].get<bool>());
        ORCHA_ASSERT_EQ(out[0]["output"]["out"].get<std::string>(), std::string("ab"));
        std::cout << "[PASS] test_placeholder_out_of_range_index\n";
    }

    inline void test_placeholder_mixed_and_nested_paths() {
        std::vector<Workflow::WorkflowStepResult> prev(2);
        prev[0].output = Json{{"a", {{"b", 7}}}};
        prev[1].output = Json{{"s", "x"}};
        prev[1].name = "second";
        auto resolved = Workflow::PlaceholderResolver::resolve(
            Json{{"p", "{{step1.output.a.b}}-{{steps.second.output.s}}-{{steps.missing.output}}"}},
            prev);
        ORCHA_ASSERT_EQ(resolved["p"].get<std::string>(), std::string("7-x-"));
        std::cout << "[PASS] test_placeholder_mixed_and_nested_paths\n";
    }

    // ========== Plugin lifetime ==========

    inline void test_unregister_while_plugin_command_executing() {
#ifdef ORCHA_TEST_SLOW_PLUGIN_PATH
        Core::CommandRegistry registry;
        ORCHA_ASSERT(registry.load_command_library(ORCHA_TEST_SLOW_PLUGIN_PATH));
        auto command = registry.get_command("slow_test_plugin");
        ORCHA_ASSERT(command != nullptr);

        auto running = std::async(std::launch::async, [command]() mutable {
            auto out = command->execute(Json{{"ms", 200}});
            command.reset(); // last reference: destroy, then close the library
            return out;
        });
        command.reset();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ORCHA_ASSERT(registry.unregister_command("slow_test_plugin")); // mid-execution
        ORCHA_ASSERT_FALSE(registry.has_command("slow_test_plugin"));

        // Before the fix the library was dlclose()d here and this crashed.
        const auto out = running.get();
        ORCHA_ASSERT(out.value("done", false));

        // The library can be loaded again afterwards.
        ORCHA_ASSERT(registry.load_command_library(ORCHA_TEST_SLOW_PLUGIN_PATH));
        std::cout << "[PASS] test_unregister_while_plugin_command_executing\n";
#else
        std::cout << "[SKIP] test_unregister_while_plugin_command_executing (no test plugin)\n";
#endif
    }

    inline void run_p0_regression_tests() {
        std::cout << "\n-- P0 Regressions --\n";
        test_inband_failure_fails_step();
        test_placeholder_output_is_not_reexpanded();
        test_placeholder_out_of_range_index();
        test_placeholder_mixed_and_nested_paths();
        test_unregister_while_plugin_command_executing();
    }

} // namespace Orcha::Tests
