//
// WorkflowRunner.hpp - Legacy compatibility wrapper
// Updated to delegate to WorkflowEngine
//

#pragma once

#include "WorkflowEngine.hpp"
#include "IWorkflowEngine.hpp"
#include "../core/ICommandRegistry.hpp"
#include "../core/CommandRegistry.hpp"

namespace Orcha::Workflow {

    /**
     * @class RegistryAdapter
     * @brief Adapts reference to shared_ptr for ICommandRegistry.
     */
    class RegistryAdapter : public Core::ICommandRegistry {
    public:
        explicit RegistryAdapter(Core::ICommandRegistry& registry) : registry_(registry) {}

        [[nodiscard]] Core::ICommand* get_command(const std::string& name) const override {
            return registry_.get_command(name);
        }

        [[nodiscard]] std::vector<std::string> list_commands() const override {
            return registry_.list_commands();
        }

        [[nodiscard]] bool has_command(const std::string& name) const override {
            return registry_.has_command(name);
        }

        [[nodiscard]] size_t command_count() const override {
            return registry_.command_count();
        }

    private:
        Core::ICommandRegistry& registry_;
    };

    /**
     * @class WorkflowRunner
     * @brief Legacy compatibility class - delegates to WorkflowEngine.
     *
     * This class maintains backward compatibility with existing code
     * while the new WorkflowEngine provides the actual implementation.
     *
     * @deprecated Use WorkflowEngine directly for new code.
     */
    class WorkflowRunner {
    public:
        explicit WorkflowRunner(Core::ICommandRegistry& registry)
            : adapter_(std::make_shared<RegistryAdapter>(registry))
            , engine_(adapter_, std::make_shared<SyncStepExecutor>(), nullptr) {}

        explicit WorkflowRunner(std::shared_ptr<Core::ICommandRegistry> registry)
            : engine_(registry, std::make_shared<SyncStepExecutor>(), nullptr) {}

        /**
         * @brief Synchronous CLI usage (returns true on success, fills step_results).
         * @deprecated Use WorkflowEngine::execute() instead.
         */
        bool run(const std::string& yaml_path, std::vector<WorkflowStepResult>& step_results) const {
            auto result = engine_.execute_yaml(yaml_path);
            step_results = result.step_results;
            return result.success;
        }

        /**
         * @brief Asynchronous: returns a task with a JSON array of results.
         * @deprecated Use WorkflowEngine::execute_json() instead.
         */
        [[nodiscard]] pplx::task<web::json::value> run_and_report_async(
            const std::string& yaml_content) const {
            return pplx::create_task([this, yaml_content]() {
                auto result = engine_.execute_yaml_string(yaml_content);
                return result.to_json();
            });
        }

        /**
         * @brief Execute workflow from JSON.
         * @deprecated Use WorkflowEngine::execute_json() instead.
         */
        [[nodiscard]] pplx::task<web::json::value> run_and_report_json(
            const web::json::value& workflow_json) const {
            return engine_.execute_json(workflow_json);
        }

        /**
         * @brief Placeholder resolution utility.
         */
        static web::json::value resolve_placeholders(
            const web::json::value& input,
            const std::vector<WorkflowStepResult>& previous_results) {
            return PlaceholderResolver::resolve(input, previous_results);
        }

    private:
        std::shared_ptr<RegistryAdapter> adapter_;
        mutable WorkflowEngine engine_;
    };

} // namespace Orcha::Workflow
