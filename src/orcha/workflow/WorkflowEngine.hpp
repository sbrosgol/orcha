//
// WorkflowEngine.hpp - Refactored workflow engine
// Created as part of architectural improvements
//

#pragma once

#include "IWorkflowEngine.hpp"
#include "../core/ICommandRegistry.hpp"
#include "../utils/ILogger.hpp"
#include <mutex>
#include <future>

namespace Orcha::Workflow {

    /**
     * @class SyncStepExecutor
     * @brief Synchronous step executor.
     */
    class SyncStepExecutor : public IStepExecutor {
    public:
        [[nodiscard]] WorkflowStepResult execute_step(
            Core::ICommand* cmd,
            const web::json::value& params) override;
    };

    /**
     * @class AsyncStepExecutor
     * @brief Asynchronous step executor with thread pool support.
     */
    class AsyncStepExecutor : public IStepExecutor {
    public:
        explicit AsyncStepExecutor(size_t thread_count = 4);

        [[nodiscard]] WorkflowStepResult execute_step(
            Core::ICommand* cmd,
            const web::json::value& params) override;

    private:
        size_t thread_count_;
    };

    /**
     * @class PlaceholderResolver
     * @brief Resolves placeholders in workflow step parameters.
     *
     * Supports syntax like {{step1.output.field}} for referencing
     * previous step outputs.
     */
    class PlaceholderResolver {
    public:
        /**
         * @brief Resolve placeholders in a JSON value.
         * @param input Input JSON with placeholders.
         * @param previous_results Results from previous steps.
         * @return JSON with placeholders resolved.
         */
        [[nodiscard]] static web::json::value resolve(
            const web::json::value& input,
            const std::vector<WorkflowStepResult>& previous_results);

    private:
        [[nodiscard]] static std::string resolve_string(
            const std::string& input,
            const std::vector<WorkflowStepResult>& previous_results);

        [[nodiscard]] static web::json::value navigate_output(
            const web::json::value& output,
            const std::string& field_path);

        [[nodiscard]] static std::string json_value_to_string(
            const web::json::value& value);
    };

    /**
     * @class WorkflowEngine
     * @brief Refactored workflow execution engine.
     *
     * Eliminates code duplication from the original WorkflowRunner
     * by using strategy pattern for step execution.
     */
    class WorkflowEngine : public IWorkflowEngine {
    public:
        /**
         * @brief Construct with dependencies.
         */
        WorkflowEngine(std::shared_ptr<Core::ICommandRegistry> registry,
                       std::shared_ptr<IStepExecutor> executor,
                       std::shared_ptr<Utils::ILogger> logger = nullptr);

        /**
         * @brief Construct with registry only (uses default executor).
         */
        explicit WorkflowEngine(std::shared_ptr<Core::ICommandRegistry> registry);

        // IWorkflowEngine interface
        [[nodiscard]] WorkflowResult execute(const WorkflowDefinition& definition) override;

        [[nodiscard]] pplx::task<WorkflowResult> execute_async(
            const WorkflowDefinition& definition) override;

        [[nodiscard]] pplx::task<web::json::value> execute_json(
            const web::json::value& workflow_json) override;

        /**
         * @brief Execute workflow from YAML file path.
         */
        [[nodiscard]] WorkflowResult execute_yaml(const std::string& yaml_path);

        /**
         * @brief Execute workflow from YAML string content.
         */
        [[nodiscard]] WorkflowResult execute_yaml_string(const std::string& yaml_content);

    private:
        [[nodiscard]] WorkflowStepResult execute_single_step(
            const WorkflowStep& step,
            const std::vector<WorkflowStepResult>& previous_results,
            int step_index);

        void log_step_start(const WorkflowStep& step, int index);
        void log_step_complete(const WorkflowStepResult& result, int index);

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<IStepExecutor> executor_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Workflow
