//
// ParallelWorkflowExecutor.hpp - DAG-based parallel workflow execution
//

#pragma once

#include "IWorkflowEngine.hpp"
#include "WorkflowEngine.hpp"
#include "RollbackOrchestrator.hpp"
#include "../core/ICommandRegistry.hpp"
#include "../core/CircuitBreaker.hpp"
#include "../utils/ILogger.hpp"
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <future>
#include <mutex>
#include <condition_variable>

namespace Orcha::Workflow {

    /**
     * @struct StepDependency
     * @brief Represents dependency information for a workflow step.
     */
    struct StepDependency {
        int step_index;
        std::unordered_set<int> depends_on;  // Indices of steps this depends on
        std::unordered_set<int> dependents;  // Indices of steps that depend on this
        int in_degree = 0;                   // Number of unresolved dependencies
    };

    /**
     * @struct ExecutionLevel
     * @brief Groups of steps that can execute in parallel.
     */
    struct ExecutionLevel {
        int level;
        std::vector<int> step_indices;
    };

    /**
     * @class WorkflowDependencyAnalyzer
     * @brief Analyzes workflow steps to build dependency graph.
     *
     * Parses placeholder references ({{step1.output}}) to determine
     * which steps depend on outputs from previous steps.
     */
    class WorkflowDependencyAnalyzer {
    public:
        /**
         * @brief Analyze workflow and build dependency graph.
         * @param definition Workflow to analyze.
         * @return Map of step index to dependency info.
         */
        [[nodiscard]] static std::unordered_map<int, StepDependency> analyze(
            const WorkflowDefinition& definition) {

            std::unordered_map<int, StepDependency> deps;

            // Initialize all steps
            for (size_t i = 0; i < definition.steps.size(); ++i) {
                deps[static_cast<int>(i)] = {static_cast<int>(i), {}, {}, 0};
            }

            // Find dependencies based on placeholder references
            static const std::regex placeholder_regex(R"(\{\{step(\d+)\.output)");

            for (size_t i = 0; i < definition.steps.size(); ++i) {
                const auto& step = definition.steps[i];
                auto referenced_steps = find_referenced_steps(step.params, placeholder_regex);

                for (int ref : referenced_steps) {
                    if (ref >= 0 && ref < static_cast<int>(i)) {
                        // Step i depends on step ref
                        deps[static_cast<int>(i)].depends_on.insert(ref);
                        deps[ref].dependents.insert(static_cast<int>(i));
                        deps[static_cast<int>(i)].in_degree++;
                    }
                }

                // Also respect explicit 'depends_on' field if present
                // (future enhancement - could add to WorkflowStep)
            }

            return deps;
        }

        /**
         * @brief Compute execution levels for parallel execution.
         *
         * Returns steps grouped by execution level where:
         * - Level 0: Steps with no dependencies (can run immediately)
         * - Level N: Steps that depend only on steps from levels 0 to N-1
         *
         * @param definition Workflow definition.
         * @return Vector of execution levels.
         */
        [[nodiscard]] static std::vector<ExecutionLevel> compute_execution_levels(
            const WorkflowDefinition& definition) {

            auto deps = analyze(definition);
            std::vector<ExecutionLevel> levels;
            std::unordered_set<int> processed;

            while (processed.size() < definition.steps.size()) {
                ExecutionLevel level;
                level.level = static_cast<int>(levels.size());

                // Find all steps with no unprocessed dependencies
                for (const auto& [idx, dep] : deps) {
                    if (processed.contains(idx)) continue;

                    bool all_deps_processed = true;
                    for (int dep_idx : dep.depends_on) {
                        if (!processed.contains(dep_idx)) {
                            all_deps_processed = false;
                            break;
                        }
                    }

                    if (all_deps_processed) {
                        level.step_indices.push_back(idx);
                    }
                }

                if (level.step_indices.empty()) {
                    // Circular dependency detected
                    break;
                }

                // Mark these steps as processed
                for (int idx : level.step_indices) {
                    processed.insert(idx);
                }

                levels.push_back(std::move(level));
            }

            return levels;
        }

        /**
         * @brief Check if workflow has any parallelizable steps.
         */
        [[nodiscard]] static bool has_parallelism(const WorkflowDefinition& definition) {
            auto levels = compute_execution_levels(definition);
            for (const auto& level : levels) {
                if (level.step_indices.size() > 1) {
                    return true;
                }
            }
            return false;
        }

    private:
        [[nodiscard]] static std::unordered_set<int> find_referenced_steps(
            const web::json::value& params,
            const std::regex& pattern) {

            std::unordered_set<int> refs;
            find_refs_recursive(params, pattern, refs);
            return refs;
        }

        static void find_refs_recursive(
            const web::json::value& value,
            const std::regex& pattern,
            std::unordered_set<int>& refs) {

            if (value.is_string()) {
                std::string str = value.as_string();
                std::sregex_iterator it(str.begin(), str.end(), pattern);
                std::sregex_iterator end;

                while (it != end) {
                    int step_num = std::stoi((*it)[1].str());
                    refs.insert(step_num - 1);  // Convert to 0-indexed
                    ++it;
                }
            } else if (value.is_object()) {
                for (const auto& field : value.as_object()) {
                    find_refs_recursive(field.second, pattern, refs);
                }
            } else if (value.is_array()) {
                for (const auto& elem : value.as_array()) {
                    find_refs_recursive(elem, pattern, refs);
                }
            }
        }
    };

    /**
     * @struct ParallelExecutionConfig
     * @brief Configuration for parallel workflow execution.
     */
    struct ParallelExecutionConfig {
        size_t max_parallel_steps = 8;
        bool enable_circuit_breaker = false;
        Core::CircuitBreakerConfig circuit_breaker_config;
        RollbackMode rollback_mode = RollbackMode::None;
        RollbackStrategy rollback_strategy = RollbackStrategy::ReverseOrder;
        bool fail_fast = true;  // Stop all execution on first failure
    };

    /**
     * @class ParallelWorkflowExecutor
     * @brief Executes workflow steps in parallel based on dependency analysis.
     *
     * This executor analyzes step dependencies and maximizes parallelism
     * by running independent steps concurrently while respecting data
     * dependencies between steps.
     */
    class ParallelWorkflowExecutor {
    public:
        ParallelWorkflowExecutor(
            std::shared_ptr<Core::ICommandRegistry> registry,
            std::shared_ptr<IStepExecutor> step_executor,
            std::shared_ptr<Utils::ILogger> logger = nullptr,
            ParallelExecutionConfig config = {})
            : registry_(std::move(registry))
            , step_executor_(std::move(step_executor))
            , logger_(std::move(logger))
            , config_(config) {

            if (!step_executor_) {
                step_executor_ = std::make_shared<SyncStepExecutor>();
            }

            if (config_.rollback_mode != RollbackMode::None) {
                rollback_orchestrator_ = std::make_unique<RollbackOrchestrator>(
                    registry_, logger_, config_.rollback_mode, config_.rollback_strategy);
            }

            if (config_.enable_circuit_breaker) {
                circuit_breakers_ = std::make_unique<Core::CircuitBreakerRegistry>();
            }
        }

        /**
         * @brief Execute workflow with parallel optimization.
         */
        [[nodiscard]] WorkflowResult execute(const WorkflowDefinition& definition) {
            WorkflowResult result;

            // Analyze dependencies and compute execution levels
            auto levels = WorkflowDependencyAnalyzer::compute_execution_levels(definition);

            log("Workflow analysis complete:");
            log("  Total steps: " + std::to_string(definition.steps.size()));
            log("  Execution levels: " + std::to_string(levels.size()));
            for (const auto& level : levels) {
                log("  Level " + std::to_string(level.level) + ": " +
                    std::to_string(level.step_indices.size()) + " steps");
            }

            // Storage for step results (thread-safe access)
            std::vector<WorkflowStepResult> step_results(definition.steps.size());
            std::mutex results_mutex;
            std::atomic<bool> failed{false};

            // Execute levels sequentially, steps within level in parallel
            for (const auto& level : levels) {
                if (failed.load() && config_.fail_fast) {
                    log("Skipping level " + std::to_string(level.level) + " due to previous failure");
                    break;
                }

                log("Executing level " + std::to_string(level.level) +
                    " with " + std::to_string(level.step_indices.size()) + " parallel steps");

                std::vector<std::future<WorkflowStepResult>> futures;

                // Limit parallelism to config max
                size_t batch_size = std::min(level.step_indices.size(), config_.max_parallel_steps);

                for (size_t batch_start = 0; batch_start < level.step_indices.size();
                     batch_start += batch_size) {

                    size_t batch_end = std::min(batch_start + batch_size, level.step_indices.size());
                    futures.clear();

                    // Launch batch of parallel tasks
                    for (size_t i = batch_start; i < batch_end; ++i) {
                        int step_idx = level.step_indices[i];
                        const auto& step = definition.steps[step_idx];

                        futures.push_back(std::async(std::launch::async,
                            [this, &step, &step_results, step_idx, &failed]() {
                                if (failed.load() && config_.fail_fast) {
                                    WorkflowStepResult skipped;
                                    skipped.step_index = step_idx;
                                    skipped.command_name = step.command_name;
                                    skipped.success = false;
                                    skipped.error_message = "Skipped due to previous failure";
                                    return skipped;
                                }

                                return execute_single_step(step, step_results, step_idx);
                            }));
                    }

                    // Wait for batch to complete
                    for (size_t i = batch_start; i < batch_end; ++i) {
                        int step_idx = level.step_indices[i];
                        auto step_result = futures[i - batch_start].get();

                        {
                            std::lock_guard lock(results_mutex);
                            step_results[step_idx] = step_result;
                        }

                        if (!step_result.success) {
                            failed.store(true);
                            log("Step " + std::to_string(step_idx) + " failed: " +
                                step_result.error_message);
                        } else {
                            // Record for potential rollback
                            if (rollback_orchestrator_) {
                                rollback_orchestrator_->record_completed_step(
                                    step_idx,
                                    step_result.command_name,
                                    definition.steps[step_idx].params,
                                    step_result.output);
                            }
                        }
                    }
                }
            }

            // Collect results
            result.step_results = std::move(step_results);
            result.success = !failed.load();

            // Handle rollback if failed
            if (!result.success && rollback_orchestrator_) {
                log("Workflow failed, initiating rollback...");
                auto rollback_result = rollback_orchestrator_->execute_rollback(
                    "Workflow step failure");

                // Could attach rollback result to workflow result
                // (enhancement: add rollback_result to WorkflowResult)
            }

            if (!result.success) {
                // Find first error
                for (const auto& sr : result.step_results) {
                    if (!sr.success && !sr.error_message.empty()) {
                        result.error_message = sr.error_message;
                        break;
                    }
                }
            }

            return result;
        }

        /**
         * @brief Execute workflow asynchronously.
         */
        [[nodiscard]] pplx::task<WorkflowResult> execute_async(
            const WorkflowDefinition& definition) {

            return pplx::create_task([this, definition]() {
                return execute(definition);
            });
        }

        /**
         * @brief Get circuit breaker statistics (if enabled).
         */
        [[nodiscard]] std::vector<Core::CircuitStats> get_circuit_stats() const {
            if (circuit_breakers_) {
                return circuit_breakers_->all_stats();
            }
            return {};
        }

    private:
        WorkflowStepResult execute_single_step(
            const WorkflowStep& step,
            const std::vector<WorkflowStepResult>& all_results,
            int step_index) {

            WorkflowStepResult result;
            result.step_index = step_index;
            result.command_name = step.command_name;

            // Get command
            auto* cmd = registry_->get_command(step.command_name);
            if (!cmd) {
                result.success = false;
                result.error_message = "Command not found: " + step.command_name;
                return result;
            }

            // Resolve placeholders
            auto resolved_params = PlaceholderResolver::resolve(step.params, all_results);

            // Execute (with optional circuit breaker)
            auto execute_fn = [&]() {
                return step_executor_->execute_step(cmd, resolved_params);
            };

            if (circuit_breakers_) {
                auto& breaker = circuit_breakers_->get_or_create(
                    step.command_name, config_.circuit_breaker_config);

                if (!breaker.allow_request()) {
                    result.success = false;
                    result.error_message = "Circuit breaker open for: " + step.command_name;
                    return result;
                }

                result = execute_fn();

                if (result.success) {
                    breaker.record_success();
                } else {
                    breaker.record_failure();
                }
            } else {
                result = execute_fn();
            }

            result.step_index = step_index;
            return result;
        }

        void log(const std::string& message) {
            if (logger_) {
                logger_->debug("[ParallelExecutor] " + message);
            }
        }

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<IStepExecutor> step_executor_;
        std::shared_ptr<Utils::ILogger> logger_;
        ParallelExecutionConfig config_;

        std::unique_ptr<RollbackOrchestrator> rollback_orchestrator_;
        std::unique_ptr<Core::CircuitBreakerRegistry> circuit_breakers_;
    };

} // namespace Orcha::Workflow
