//
// WorkflowEngine.hpp - Refactored workflow engine
// Created as part of architectural improvements
//

#pragma once

#include "IWorkflowEngine.hpp"
#include "../core/ICommandRegistry.hpp"
#include "../core/Json.hpp"
#include "../utils/ILogger.hpp"
#include <mutex>
#include <future>

namespace Orcha::Workflow {

    /**
     * @class SyncStepExecutor
     * @brief Synchronous step executor.
     *
     * A step fails if the command throws, fails validation, or returns an
     * object whose "success" field is false (see ICommand::execute).
     */
    class SyncStepExecutor : public IStepExecutor {
    public:
        [[nodiscard]] WorkflowStepResult execute_step(
            const std::shared_ptr<Core::ICommand>& cmd,
            const Orcha::Json& params) override;
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
         * @struct References
         * @brief Step references found in a parameter tree.
         */
        struct References {
            std::vector<size_t> positions;   ///< 1-based, from {{stepN.output...}}
            std::vector<std::string> names;  ///< from {{steps.<name>.output...}}
        };

        /**
         * @brief Collect every step referenced by placeholders in @p input.
         */
        [[nodiscard]] static References references(const Orcha::Json& input);

        /**
         * @brief Resolve placeholders in a JSON value.
         * @param input Input JSON with placeholders.
         * @param previous_results Results from previous steps.
         * @return JSON with placeholders resolved.
         */
        [[nodiscard]] static Orcha::Json resolve(
            const Orcha::Json& input,
            const std::vector<WorkflowStepResult>& previous_results);

    private:
        [[nodiscard]] static std::string resolve_string(
            const std::string& input,
            const std::vector<WorkflowStepResult>& previous_results);

        static void collect_references(const Orcha::Json& input, References& out);

        [[nodiscard]] static Orcha::Json navigate_output(
            const Orcha::Json& output,
            const std::string& field_path);

        [[nodiscard]] static std::string json_value_to_string(
            const Orcha::Json& value);
    };

    /**
     * @class WorkflowEngine
     * @brief Refactored workflow execution engine.
     *
     * Uses strategy pattern for step execution.
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

        [[nodiscard]] Orcha::Json execute_json(
            const Orcha::Json& workflow_json) override;

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
            const Orcha::Json& resolved_params,
            int step_index);

        void log_step_start(const WorkflowStep& step, int index);
        void log_step_complete(const WorkflowStepResult& result, int index);

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<IStepExecutor> executor_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Workflow
