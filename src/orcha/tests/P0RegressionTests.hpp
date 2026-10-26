//
// P0RegressionTests.hpp - Regression tests for the P0 hardening fixes
//
// Each test reproduces a defect found in the architecture / concurrency review:
// - unregistering a plugin command while it executed unmapped its code
//

#pragma once

#include "../core/CommandRegistry.hpp"

#include "Assertions.hpp"
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace Orcha::Tests {

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
        test_unregister_while_plugin_command_executing();
    }

} // namespace Orcha::Tests
