//
// TimeoutTests.hpp - Step/workflow timeouts, cancellation and the plugin ABI check
//
// The plugin ABI handshake is tested with a versioned and a legacy test plugin.
//

#pragma once

#include "../core/CommandRegistry.hpp"
#include "../core/ExecutionControl.hpp"

#include "Assertions.hpp"
#include <chrono>
#include <iostream>
#include <stop_token>
#include <string>


namespace Orcha::Tests {

    namespace timeouts {

        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        inline long long ms_since(Clock::time_point start) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
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
        test_plugin_abi_version_checked();   // last: see the ASan note inside
    }

} // namespace Orcha::Tests
