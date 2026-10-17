//
// IWorkflowEngine.hpp - Workflow engine interfaces
// Created as part of architectural improvements
//

#pragma once

#include "../core/Json.hpp"
#include <vector>
#include <string>
#include <memory>
#include <optional>
#include "../core/ICommandRegistry.hpp"
#include "../core/ICommand.hpp"
#include "../core/Result.hpp"

namespace Orcha::Workflow {

    /**
     * @struct WorkflowStepResult
     * @brief Result of executing a single workflow step.
     */
    struct WorkflowStepResult {
        bool success = false;
        std::string error_message;
        Json output;
        std::string command_name;
        std::string name;          // Optional step name (for {{steps.<name>.output}} refs)
        int step_index = -1;

        [[nodiscard]] Json to_json() const {
            Json obj = Json::object();
            obj["success"] = success;
            obj["error_message"] = error_message;
            obj["output"] = output;
            if (!command_name.empty()) {
                obj["command"] = command_name;
            }
            if (!name.empty()) {
                obj["name"] = name;
            }
            if (step_index >= 0) {
                obj["step"] = step_index;
            }
            return obj;
        }
    };

    /**
     * @struct WorkflowStep
     * @brief Definition of a single workflow step.
     */
    struct WorkflowStep {
        std::string command_name;
        Json params;
        bool parallel = false;
        std::optional<std::string> name;  // Optional step name for reference
        std::optional<int> timeout_ms;
    };

    /**
     * @struct WorkflowDefinition
     * @brief Complete workflow definition.
     */
    struct WorkflowDefinition {
        std::string name;
        std::string description;
        std::vector<WorkflowStep> steps;

        static WorkflowDefinition from_json(const Json& json) {
            WorkflowDefinition def;

            if (json.contains("name")) {
                def.name = json.at("name").get<std::string>();
            }
            if (json.contains("description")) {
                def.description = json.at("description").get<std::string>();
            }

            if (json.contains("steps") && json.at("steps").is_array()) {
                for (const auto& step_json : json.at("steps")) {
                    WorkflowStep step;

                    if (step_json.contains("command")) {
                        step.command_name = step_json.at("command").get<std::string>();
                    }
                    if (step_json.contains("params")) {
                        step.params = step_json.at("params");
                    } else {
                        step.params = Json::object();
                    }
                    if (step_json.contains("parallel")) {
                        step.parallel = step_json.at("parallel").get<bool>();
                    }
                    if (step_json.contains("name")) {
                        step.name = step_json.at("name").get<std::string>();
                    }
                    if (step_json.contains("timeout_ms")) {
                        step.timeout_ms = step_json.at("timeout_ms").get<int>();
                    }

                    def.steps.push_back(step);
                }
            }

            return def;
        }
    };

    /**
     * @struct WorkflowResult
     * @brief Complete result of workflow execution.
     */
    struct WorkflowResult {
        bool success = false;
        std::vector<WorkflowStepResult> step_results;
        std::string error_message;

        [[nodiscard]] Json to_json() const {
            Json arr = Json::array();
            for (const auto& step_result : step_results) {
                arr.push_back(step_result.to_json());
            }
            return arr;
        }
    };

    /**
     * @interface IStepExecutor
     * @brief Strategy interface for step execution.
     */
    class IStepExecutor {
    public:
        virtual ~IStepExecutor() = default;

        /**
         * @brief Execute a single step.
         * @param cmd The command to execute.
         * @param params Resolved parameters.
         * @return Step execution result.
         */
        [[nodiscard]] virtual WorkflowStepResult execute_step(
            const std::shared_ptr<Core::ICommand>& cmd,
            const Json& params) = 0;
    };

    /**
     * @interface IWorkflowEngine
     * @brief Interface for workflow execution engines.
     */
    class IWorkflowEngine {
    public:
        virtual ~IWorkflowEngine() = default;

        /**
         * @brief Execute a workflow synchronously.
         * @param definition The workflow to execute.
         * @return Workflow execution result.
         */
        [[nodiscard]] virtual WorkflowResult execute(const WorkflowDefinition& definition) = 0;

        /**
         * @brief Execute a workflow from JSON.
         * @param workflow_json JSON workflow definition.
         * @return JSON result (array of step results).
         */
        [[nodiscard]] virtual Json execute_json(const Json& workflow_json) = 0;
    };

} // namespace Orcha::Workflow
