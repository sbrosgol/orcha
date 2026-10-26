//
// ExecutionControl.hpp - Cancellation and deadline handed to a running command.
//
// Commands run synchronously on the caller's thread and cannot be preempted
// safely, so timeouts and cancellation are cooperative: a command that blocks
// (a child process, a network call) should poll should_stop() or use
// remaining() to bound its waits, and return promptly once it is true. The
// engine still fails a step that returns after its deadline.
//

#pragma once

#include <algorithm>
#include <chrono>
#include <optional>
#include <stop_token>

namespace Orcha::Core {

    class ExecutionControl {
    public:
        using Clock = std::chrono::steady_clock;

        /// Never stops and has no deadline.
        ExecutionControl() = default;

        ExecutionControl(std::stop_token stop, std::optional<Clock::time_point> deadline)
            : stop_(std::move(stop)), deadline_(deadline) {}

        /// Requested when the run is cancelled (e.g. server shutdown).
        [[nodiscard]] const std::stop_token& stop_token() const noexcept { return stop_; }

        /// Point in time by which the command must finish, if any.
        [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept { return deadline_; }

        [[nodiscard]] bool stop_requested() const noexcept { return stop_.stop_requested(); }

        [[nodiscard]] bool expired() const {
            return deadline_ && Clock::now() >= *deadline_;
        }

        /// True once the command should give up: cancelled or past its deadline.
        [[nodiscard]] bool should_stop() const { return stop_requested() || expired(); }

        /// Time left before the deadline (never negative); nullopt if unbounded.
        [[nodiscard]] std::optional<std::chrono::milliseconds> remaining() const {
            if (!deadline_) {
                return std::nullopt;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                *deadline_ - Clock::now());
            return std::max(left, std::chrono::milliseconds::zero());
        }

    private:
        std::stop_token stop_;
        std::optional<Clock::time_point> deadline_;
    };

} // namespace Orcha::Core
