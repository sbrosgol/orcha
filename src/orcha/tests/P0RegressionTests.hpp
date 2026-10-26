//
// P0RegressionTests.hpp - Regression tests for the P0 hardening fixes
//
// Each test reproduces a defect found in the architecture / concurrency review:
// - in-band command failures ({"success": false}) were recorded as success
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
        test_unregister_while_plugin_command_executing();
    }

} // namespace Orcha::Tests
