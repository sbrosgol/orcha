//
// CircuitBreaker.hpp - Circuit breaker pattern for resilient command execution
//

#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <optional>

namespace Orcha::Core {

    /**
     * @enum CircuitState
     * @brief Represents the state of a circuit breaker.
     */
    enum class CircuitState {
        Closed,     // Normal operation, requests flow through
        Open,       // Circuit tripped, requests are rejected
        HalfOpen    // Testing if service recovered
    };

    /**
     * @struct CircuitBreakerConfig
     * @brief Configuration for circuit breaker behavior.
     */
    struct CircuitBreakerConfig {
        size_t failure_threshold = 5;           // Failures before opening
        size_t success_threshold = 2;           // Successes in half-open to close
        std::chrono::seconds reset_timeout{30}; // Time before trying half-open
        std::chrono::seconds window_size{60};   // Sliding window for failure count

        // Factory for common configurations
        static CircuitBreakerConfig strict() {
            return {3, 1, std::chrono::seconds{60}, std::chrono::seconds{30}};
        }

        static CircuitBreakerConfig lenient() {
            return {10, 3, std::chrono::seconds{15}, std::chrono::seconds{120}};
        }
    };

    /**
     * @struct CircuitStats
     * @brief Statistics for a circuit breaker.
     */
    struct CircuitStats {
        std::string name;
        CircuitState state = CircuitState::Closed;
        size_t total_calls = 0;
        size_t successful_calls = 0;
        size_t failed_calls = 0;
        size_t rejected_calls = 0;
        std::optional<std::chrono::system_clock::time_point> last_failure;
        std::optional<std::chrono::system_clock::time_point> last_state_change;
    };

    /**
     * @class CircuitBreaker
     * @brief Implements the circuit breaker pattern for a single command/service.
     *
     * The circuit breaker monitors failures and prevents repeated calls to
     * a failing service, giving it time to recover.
     *
     * States:
     * - CLOSED: Normal operation, all requests pass through
     * - OPEN: Too many failures, requests are rejected immediately
     * - HALF-OPEN: Testing recovery, limited requests allowed
     */
    class CircuitBreaker {
    public:
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        explicit CircuitBreaker(std::string name, CircuitBreakerConfig config = {})
            : name_(std::move(name))
            , config_(config)
            , state_(CircuitState::Closed)
            , failure_count_(0)
            , success_count_(0)
            , last_failure_time_()
            , state_changed_time_(Clock::now()) {}

        /**
         * @brief Check if a request should be allowed.
         * @return true if request can proceed, false if circuit is open.
         */
        [[nodiscard]] bool allow_request() {
            std::lock_guard lock(mutex_);

            switch (state_) {
                case CircuitState::Closed:
                    return true;

                case CircuitState::Open: {
                    // Check if reset timeout has elapsed
                    auto now = Clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        now - state_changed_time_);

                    if (elapsed >= config_.reset_timeout) {
                        transition_to(CircuitState::HalfOpen);
                        return true;  // Allow one request to test
                    }
                    rejected_count_++;
                    return false;
                }

                case CircuitState::HalfOpen:
                    // Allow limited requests in half-open state
                    return true;
            }

            return false;
        }

        /**
         * @brief Record a successful call.
         */
        void record_success() {
            std::lock_guard lock(mutex_);
            total_count_++;
            success_count_++;

            switch (state_) {
                case CircuitState::Closed:
                    // Reset failure count on success in closed state
                    failure_count_ = 0;
                    break;

                case CircuitState::HalfOpen:
                    // Count successes toward closing the circuit
                    if (++half_open_successes_ >= config_.success_threshold) {
                        transition_to(CircuitState::Closed);
                    }
                    break;

                case CircuitState::Open:
                    // Should not happen
                    break;
            }
        }

        /**
         * @brief Record a failed call.
         */
        void record_failure() {
            std::lock_guard lock(mutex_);
            total_count_++;
            failure_count_++;
            last_failure_time_ = Clock::now();

            switch (state_) {
                case CircuitState::Closed:
                    // Check if we should open the circuit
                    if (failure_count_ >= config_.failure_threshold) {
                        transition_to(CircuitState::Open);
                    }
                    break;

                case CircuitState::HalfOpen:
                    // Any failure in half-open reopens the circuit
                    transition_to(CircuitState::Open);
                    break;

                case CircuitState::Open:
                    // Already open, nothing to do
                    break;
            }
        }

        /**
         * @brief Execute a function with circuit breaker protection.
         * @tparam Func Callable type.
         * @tparam Fallback Fallback callable type.
         * @param func Function to execute.
         * @param fallback Function to call if circuit is open.
         * @return Result from func or fallback.
         */
        template<typename Func, typename Fallback>
        auto execute(Func&& func, Fallback&& fallback)
            -> decltype(std::forward<Func>(func)()) {

            if (!allow_request()) {
                return std::forward<Fallback>(fallback)();
            }

            try {
                auto result = std::forward<Func>(func)();
                record_success();
                return result;
            } catch (...) {
                record_failure();
                throw;
            }
        }

        /**
         * @brief Execute without fallback (throws on open circuit).
         */
        template<typename Func>
        auto execute(Func&& func) -> decltype(std::forward<Func>(func)()) {
            if (!allow_request()) {
                throw std::runtime_error("Circuit breaker '" + name_ + "' is open");
            }

            try {
                auto result = std::forward<Func>(func)();
                record_success();
                return result;
            } catch (...) {
                record_failure();
                throw;
            }
        }

        /**
         * @brief Get current circuit state.
         */
        [[nodiscard]] CircuitState state() const {
            std::lock_guard lock(mutex_);
            return state_;
        }

        /**
         * @brief Get circuit statistics.
         */
        [[nodiscard]] CircuitStats stats() const {
            std::lock_guard lock(mutex_);
            CircuitStats s;
            s.name = name_;
            s.state = state_;
            s.total_calls = total_count_;
            s.successful_calls = success_count_;
            s.failed_calls = failure_count_;
            s.rejected_calls = rejected_count_;
            if (last_failure_time_ != TimePoint{}) {
                s.last_failure = std::chrono::system_clock::now() -
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        Clock::now() - last_failure_time_);
            }
            return s;
        }

        /**
         * @brief Manually reset the circuit to closed state.
         */
        void reset() {
            std::lock_guard lock(mutex_);
            transition_to(CircuitState::Closed);
            failure_count_ = 0;
            half_open_successes_ = 0;
        }

        /**
         * @brief Get the circuit name.
         */
        [[nodiscard]] const std::string& name() const { return name_; }

    private:
        void transition_to(CircuitState new_state) {
            if (state_ != new_state) {
                state_ = new_state;
                state_changed_time_ = Clock::now();
                half_open_successes_ = 0;

                if (on_state_change_) {
                    on_state_change_(name_, state_);
                }
            }
        }

        std::string name_;
        CircuitBreakerConfig config_;
        mutable std::mutex mutex_;

        CircuitState state_;
        size_t failure_count_;
        size_t success_count_;
        size_t total_count_ = 0;
        size_t rejected_count_ = 0;
        size_t half_open_successes_ = 0;
        TimePoint last_failure_time_;
        TimePoint state_changed_time_;

        std::function<void(const std::string&, CircuitState)> on_state_change_;
    };

    /**
     * @class CircuitBreakerRegistry
     * @brief Manages circuit breakers for multiple commands/services.
     */
    class CircuitBreakerRegistry {
    public:
        /**
         * @brief Get or create a circuit breaker for a command.
         */
        CircuitBreaker& get_or_create(const std::string& name,
                                       CircuitBreakerConfig config = {}) {
            std::lock_guard lock(mutex_);

            auto it = breakers_.find(name);
            if (it == breakers_.end()) {
                it = breakers_.emplace(name,
                    std::make_unique<CircuitBreaker>(name, config)).first;
            }
            return *it->second;
        }

        /**
         * @brief Check if a circuit breaker exists.
         */
        [[nodiscard]] bool exists(const std::string& name) const {
            std::lock_guard lock(mutex_);
            return breakers_.contains(name);
        }

        /**
         * @brief Get statistics for all circuit breakers.
         */
        [[nodiscard]] std::vector<CircuitStats> all_stats() const {
            std::lock_guard lock(mutex_);
            std::vector<CircuitStats> result;
            result.reserve(breakers_.size());
            for (const auto& [name, breaker] : breakers_) {
                result.push_back(breaker->stats());
            }
            return result;
        }

        /**
         * @brief Reset all circuit breakers.
         */
        void reset_all() {
            std::lock_guard lock(mutex_);
            for (auto& [name, breaker] : breakers_) {
                breaker->reset();
            }
        }

        /**
         * @brief Get count of open circuits.
         */
        [[nodiscard]] size_t open_circuit_count() const {
            std::lock_guard lock(mutex_);
            size_t count = 0;
            for (const auto& [name, breaker] : breakers_) {
                if (breaker->state() == CircuitState::Open) {
                    count++;
                }
            }
            return count;
        }

    private:
        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::unique_ptr<CircuitBreaker>> breakers_;
    };

} // namespace Orcha::Core
