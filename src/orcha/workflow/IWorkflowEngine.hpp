//
// IWorkflowEngine.hpp - Workflow engine interfaces
// Created as part of architectural improvements
//

#pragma once

#include <cpprest/json.h>
#include <vector>
#include <string>
#include <memory>
#include <pplx/pplxtasks.h>
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
        web::json::value output;
        std::string command_name;
        int step_index = -1;

        [[nodiscard]] web::json::value to_json() const {
            web::json::value obj;
            obj[U("success")] = web::json::value::boolean(success);
            obj[U("error_message")] = web::json::value::string(error_message);
            obj[U("output")] = output;
            if (!command_name.empty()) {
                obj[U("command")] = web::json::value::string(command_name);
            }
            if (step_index >= 0) {
                obj[U("step")] = web::json::value::number(step_index);
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
        web::json::value params;
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

        static WorkflowDefinition from_json(const web::json::value& json) {
            WorkflowDefinition def;

            if (json.has_field(U("name"))) {
                def.name = json.at(U("name")).as_string();
            }
            if (json.has_field(U("description"))) {
                def.description = json.at(U("description")).as_string();
            }

            if (json.has_field(U("steps")) && json.at(U("steps")).is_array()) {
                for (const auto& step_json : json.at(U("steps")).as_array()) {
                    WorkflowStep step;

                    if (step_json.has_field(U("command"))) {
                        step.command_name = step_json.at(U("command")).as_string();
                    }
                    if (step_json.has_field(U("params"))) {
                        step.params = step_json.at(U("params"));
                    } else {
                        step.params = web::json::value::object();
                    }
                    if (step_json.has_field(U("parallel"))) {
                        step.parallel = step_json.at(U("parallel")).as_bool();
                    }
                    if (step_json.has_field(U("name"))) {
                        step.name = step_json.at(U("name")).as_string();
                    }
                    if (step_json.has_field(U("timeout_ms"))) {
                        step.timeout_ms = step_json.at(U("timeout_ms")).as_integer();
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

        [[nodiscard]] web::json::value to_json() const {
            web::json::value arr = web::json::value::array(step_results.size());
            for (size_t i = 0; i < step_results.size(); ++i) {
                arr[i] = step_results[i].to_json();
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
            Core::ICommand* cmd,
            const web::json::value& params) = 0;
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
         * @brief Execute a workflow asynchronously.
         * @param definition The workflow to execute.
         * @return Task that resolves to workflow result.
         */
        [[nodiscard]] virtual pplx::task<WorkflowResult> execute_async(
            const WorkflowDefinition& definition) = 0;

        /**
         * @brief Execute workflow from JSON.
         * @param workflow_json JSON workflow definition.
         * @return Task that resolves to JSON result.
         */
        [[nodiscard]] virtual pplx::task<web::json::value> execute_json(
            const web::json::value& workflow_json) = 0;
    };

} // namespace Orcha::Workflow
