//
// RollbackOrchestrator.hpp - Workflow rollback orchestration
//

#pragma once

#include "IWorkflowEngine.hpp"
#include "../core/ICommandRegistry.hpp"
#include "../utils/ILogger.hpp"
#include <vector>
#include <string>
#include <memory>
#include <future>

namespace Orcha::Workflow {

    /**
     * @enum RollbackMode
     * @brief Defines when and how rollback should occur.
     */
    enum class RollbackMode {
        None,       // No automatic rollback
        All,        // Rollback all completed steps on any failure
        Completed   // Only rollback steps that successfully completed
    };

    /**
     * @enum RollbackStrategy
     * @brief Defines the order of rollback execution.
     */
    enum class RollbackStrategy {
        ReverseOrder,   // Rollback in reverse order of execution (LIFO)
        Parallel,       // Rollback all steps in parallel
        BestEffort      // Rollback in reverse order, continue on individual failures
    };

    /**
     * @struct RollbackStepResult
     * @brief Result of rolling back a single step.
     */
    struct RollbackStepResult {
        std::string command_name;
        int step_index = -1;
        bool success = false;
        std::string error_message;
    };

    /**
     * @struct RollbackResult
     * @brief Complete result of rollback operation.
     */
    struct RollbackResult {
        bool initiated = false;         // Was rollback initiated
        bool all_successful = true;     // Did all rollbacks succeed
        size_t steps_rolled_back = 0;
        size_t steps_failed = 0;
        std::vector<RollbackStepResult> step_results;
        std::string trigger_reason;     // Why rollback was triggered

        [[nodiscard]] web::json::value to_json() const {
            web::json::value obj;
            obj[U("initiated")] = web::json::value::boolean(initiated);
            obj[U("all_successful")] = web::json::value::boolean(all_successful);
            obj[U("steps_rolled_back")] = web::json::value::number(steps_rolled_back);
            obj[U("steps_failed")] = web::json::value::number(steps_failed);
            obj[U("trigger_reason")] = web::json::value::string(trigger_reason);

            web::json::value steps = web::json::value::array(step_results.size());
            for (size_t i = 0; i < step_results.size(); ++i) {
                web::json::value step;
                step[U("command")] = web::json::value::string(step_results[i].command_name);
                step[U("step_index")] = web::json::value::number(step_results[i].step_index);
                step[U("success")] = web::json::value::boolean(step_results[i].success);
                if (!step_results[i].error_message.empty()) {
                    step[U("error")] = web::json::value::string(step_results[i].error_message);
                }
                steps[i] = step;
            }
            obj[U("steps")] = steps;

            return obj;
        }
    };

    /**
     * @struct CompletedStep
     * @brief Tracks a completed step for potential rollback.
     */
    struct CompletedStep {
        int step_index;
        std::string command_name;
        web::json::value original_params;
        web::json::value output;
        bool supports_rollback;
    };

    /**
     * @class RollbackOrchestrator
     * @brief Orchestrates rollback of workflow steps on failure.
     *
     * This class tracks completed steps during workflow execution and
     * performs automatic rollback when a step fails, based on the
     * configured rollback mode and strategy.
     */
    class RollbackOrchestrator {
    public:
        RollbackOrchestrator(
            std::shared_ptr<Core::ICommandRegistry> registry,
            std::shared_ptr<Utils::ILogger> logger = nullptr,
            RollbackMode mode = RollbackMode::Completed,
            RollbackStrategy strategy = RollbackStrategy::ReverseOrder)
            : registry_(std::move(registry))
            , logger_(std::move(logger))
            , mode_(mode)
            , strategy_(strategy) {}

        /**
         * @brief Record a successfully completed step.
         */
        void record_completed_step(int step_index,
                                   const std::string& command_name,
                                   const web::json::value& params,
                                   const web::json::value& output) {
            auto* cmd = registry_->get_command(command_name);
            bool supports_rollback = cmd && cmd->metadata().supports_rollback;

            completed_steps_.push_back({
                step_index,
                command_name,
                params,
                output,
                supports_rollback
            });

            log("Recorded completed step " + std::to_string(step_index) +
                ": " + command_name +
                (supports_rollback ? " (rollback supported)" : " (no rollback)"));
        }

        /**
         * @brief Clear recorded steps (for reuse).
         */
        void clear() {
            completed_steps_.clear();
        }

        /**
         * @brief Execute rollback for all recorded steps.
         * @param trigger_reason Reason why rollback was triggered.
         * @return Rollback result.
         */
        [[nodiscard]] RollbackResult execute_rollback(const std::string& trigger_reason) {
            RollbackResult result;
            result.trigger_reason = trigger_reason;

            if (mode_ == RollbackMode::None) {
                log("Rollback mode is None, skipping rollback");
                return result;
            }

            if (completed_steps_.empty()) {
                log("No completed steps to roll back");
                return result;
            }

            result.initiated = true;
            log("Starting rollback due to: " + trigger_reason);
            log("Rolling back " + std::to_string(completed_steps_.size()) + " steps");

            switch (strategy_) {
                case RollbackStrategy::ReverseOrder:
                    execute_reverse_order_rollback(result);
                    break;

                case RollbackStrategy::Parallel:
                    execute_parallel_rollback(result);
                    break;

                case RollbackStrategy::BestEffort:
                    execute_best_effort_rollback(result);
                    break;
            }

            return result;
        }

        /**
         * @brief Get steps that support rollback.
         */
        [[nodiscard]] std::vector<CompletedStep> get_rollbackable_steps() const {
            std::vector<CompletedStep> result;
            for (const auto& step : completed_steps_) {
                if (step.supports_rollback) {
                    result.push_back(step);
                }
            }
            return result;
        }

        /**
         * @brief Check if any steps support rollback.
         */
        [[nodiscard]] bool has_rollbackable_steps() const {
            for (const auto& step : completed_steps_) {
                if (step.supports_rollback) {
                    return true;
                }
            }
            return false;
        }

        // Configuration accessors
        void set_mode(RollbackMode mode) { mode_ = mode; }
        void set_strategy(RollbackStrategy strategy) { strategy_ = strategy; }
        [[nodiscard]] RollbackMode mode() const { return mode_; }
        [[nodiscard]] RollbackStrategy strategy() const { return strategy_; }

    private:
        void execute_reverse_order_rollback(RollbackResult& result) {
            // Process in reverse order, stop on first failure
            for (auto it = completed_steps_.rbegin(); it != completed_steps_.rend(); ++it) {
                auto step_result = rollback_single_step(*it);
                result.step_results.push_back(step_result);

                if (step_result.success) {
                    result.steps_rolled_back++;
                } else {
                    result.steps_failed++;
                    result.all_successful = false;
                    log("Stopping rollback due to failure at step " +
                        std::to_string(it->step_index));
                    break;  // Stop on failure
                }
            }
        }

        void execute_parallel_rollback(RollbackResult& result) {
            // Execute all rollbacks in parallel
            std::vector<std::future<RollbackStepResult>> futures;

            for (auto it = completed_steps_.rbegin(); it != completed_steps_.rend(); ++it) {
                const auto& step = *it;
                futures.push_back(std::async(std::launch::async,
                    [this, step]() {
                        return rollback_single_step(step);
                    }));
            }

            for (auto& future : futures) {
                auto step_result = future.get();
                result.step_results.push_back(step_result);

                if (step_result.success) {
                    result.steps_rolled_back++;
                } else {
                    result.steps_failed++;
                    result.all_successful = false;
                }
            }
        }

        void execute_best_effort_rollback(RollbackResult& result) {
            // Process in reverse order, continue even on failure
            for (auto it = completed_steps_.rbegin(); it != completed_steps_.rend(); ++it) {
                auto step_result = rollback_single_step(*it);
                result.step_results.push_back(step_result);

                if (step_result.success) {
                    result.steps_rolled_back++;
                } else {
                    result.steps_failed++;
                    result.all_successful = false;
                    log("Rollback failed for step " + std::to_string(it->step_index) +
                        ", continuing with next...");
                    // Continue despite failure
                }
            }
        }

        RollbackStepResult rollback_single_step(const CompletedStep& step) {
            RollbackStepResult result;
            result.command_name = step.command_name;
            result.step_index = step.step_index;

            if (!step.supports_rollback) {
                log("Step " + std::to_string(step.step_index) + " (" +
                    step.command_name + ") does not support rollback, skipping");
                result.success = true;  // Not a failure, just not supported
                return result;
            }

            auto* cmd = registry_->get_command(step.command_name);
            if (!cmd) {
                result.success = false;
                result.error_message = "Command not found for rollback: " + step.command_name;
                log(result.error_message);
                return result;
            }

            try {
                log("Rolling back step " + std::to_string(step.step_index) +
                    ": " + step.command_name);
                cmd->rollback(step.original_params);
                result.success = true;
                log("Successfully rolled back step " + std::to_string(step.step_index));
            } catch (const std::exception& ex) {
                result.success = false;
                result.error_message = ex.what();
                log("Rollback failed for step " + std::to_string(step.step_index) +
                    ": " + ex.what());
            } catch (...) {
                result.success = false;
                result.error_message = "Unknown error during rollback";
                log("Rollback failed for step " + std::to_string(step.step_index) +
                    ": Unknown error");
            }

            return result;
        }

        void log(const std::string& message) {
            if (logger_) {
                logger_->debug("[RollbackOrchestrator] " + message);
            }
        }

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<Utils::ILogger> logger_;
        RollbackMode mode_;
        RollbackStrategy strategy_;
        std::vector<CompletedStep> completed_steps_;
    };

    /**
     * @brief Parse rollback mode from string.
     */
    inline RollbackMode parse_rollback_mode(const std::string& str) {
        if (str == "none") return RollbackMode::None;
        if (str == "all") return RollbackMode::All;
        if (str == "completed") return RollbackMode::Completed;
        return RollbackMode::None;  // Default
    }

    /**
     * @brief Parse rollback strategy from string.
     */
    inline RollbackStrategy parse_rollback_strategy(const std::string& str) {
        if (str == "reverse") return RollbackStrategy::ReverseOrder;
        if (str == "parallel") return RollbackStrategy::Parallel;
        if (str == "best_effort") return RollbackStrategy::BestEffort;
        return RollbackStrategy::ReverseOrder;  // Default
    }

} // namespace Orcha::Workflow
