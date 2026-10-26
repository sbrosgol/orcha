//
// ResilienceTests.hpp - Circuit breaker behaviour
//
// The half-open state admits exactly one probe at a time.
//

#pragma once

#include "../core/CircuitBreaker.hpp"

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

    // ========== Circuit breaking ==========

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

    inline void run_resilience_tests() {
        std::cout << "\n-- Rollback & Circuit Breaking --\n";
        test_circuit_breaker_half_open_single_probe();
    }

} // namespace Orcha::Tests
