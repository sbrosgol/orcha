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
#include <stdexcept>
#include "../core/ICommandRegistry.hpp"
#include "../core/ICommand.hpp"
#include "../core/Result.hpp"

namespace Orcha::Workflow {

    /**
     * @enum StepFailure
     * @brief Why a step failed. Decides whether the failure counts against the
     *        command's circuit breaker (only Command and Timeout do).
     */
    enum class StepFailure {
        None,
        Command,      ///< The command threw or reported {"success": false}
        Validation,   ///< Parameters failed the command's validation
        NotFound,     ///< No such command registered
        Timeout,      ///< Step or workflow deadline passed
        Cancelled,    ///< The run was cancelled
        CircuitOpen   ///< Rejected by an open circuit breaker
    };

    /**
     * @struct StepRollback
     * @brief Outcome of rolling back one completed step.
     */
    struct StepRollback {
        enum class Status {
            RolledBack,     ///< rollback() returned normally
            Failed,         ///< rollback() threw (message holds the error)
            Skipped,        ///< not attempted (message holds the reason)
            NotSupported    ///< the command does not support rollback
        };

        Status status = Status::Skipped;
        std::string message;

        [[nodiscard]] Json to_json() const {
            Json obj = Json::object();
            switch (status) {
                case Status::RolledBack:   obj["status"] = "rolled_back"; break;
                case Status::Failed:       obj["status"] = "failed"; break;
                case Status::Skipped:      obj["status"] = "skipped"; break;
                case Status::NotSupported: obj["status"] = "not_supported"; break;
            }
            if (!message.empty()) {
                obj["message"] = message;
            }
            return obj;
        }
    };

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
        StepFailure failure = StepFailure::None;   // Not serialized
        std::optional<StepRollback> rollback;      // Set when the run was rolled back

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
            if (rollback) {
                obj["rollback"] = rollback->to_json();
            }
            return obj;
        }
    };

    /**
     * @enum RollbackStrategy
     * @brief What to do with completed steps when a run fails.
     */
    enum class RollbackStrategy {
        None,        ///< Leave completed steps as they are (default)
        Reverse,     ///< Undo in reverse completion order; stop at the first rollback failure
        BestEffort   ///< Undo in reverse completion order; continue past rollback failures
    };

    /**
     * @brief Parse "none" / "reverse" / "best_effort".
     * @throws std::invalid_argument for any other value.
     */
    [[nodiscard]] inline RollbackStrategy parse_rollback_strategy(const std::string& value) {
        if (value == "none") return RollbackStrategy::None;
        if (value == "reverse") return RollbackStrategy::Reverse;
        if (value == "best_effort") return RollbackStrategy::BestEffort;
        throw std::invalid_argument("Invalid 'rollback' value '" + value +
                                    "': expected none, reverse or best_effort");
    }

    /**
     * @struct WorkflowStep
     * @brief Definition of a single workflow step.
     */
    struct WorkflowStep {
        std::string command_name;
        Json params;
        bool parallel = false;
        std::optional<std::string> name;  // Optional step name for reference
        std::optional<int> timeout_ms;    // Per-step limit; <= 0 means none
    };

    /**
     * @struct WorkflowDefinition
     * @brief Complete workflow definition.
     */
    struct WorkflowDefinition {
        std::string name;
        std::string description;
        std::vector<WorkflowStep> steps;
        std::optional<int> timeout_ms;    // Whole-run limit; <= 0 means none
        RollbackStrategy rollback = RollbackStrategy::None;
        bool circuit_breaker = false;     // Consult per-command circuit breakers

        /**
         * @brief Build from JSON.
         * @throws std::invalid_argument for an invalid "rollback" or
         *         "circuit_breaker" setting; nlohmann exceptions for wrongly
         *         typed fields.
         */
        static WorkflowDefinition from_json(const Json& json) {
            WorkflowDefinition def;

            if (json.contains("name")) {
                def.name = json.at("name").get<std::string>();
            }
            if (json.contains("description")) {
                def.description = json.at("description").get<std::string>();
            }
            if (json.contains("timeout_ms")) {
                def.timeout_ms = json.at("timeout_ms").get<int>();
            }
            if (json.contains("rollback")) {
                const auto& rb = json.at("rollback");
                if (!rb.is_string()) {
                    throw std::invalid_argument(
                        "'rollback' must be a string: none, reverse or best_effort");
                }
                def.rollback = parse_rollback_strategy(rb.get<std::string>());
            }
            if (json.contains("circuit_breaker")) {
                const auto& cb = json.at("circuit_breaker");
                if (!cb.is_boolean()) {
                    throw std::invalid_argument("'circuit_breaker' must be true or false");
                }
                def.circuit_breaker = cb.get<bool>();
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
         * @param control Cancellation token and deadline for this step.
         * @return Step execution result.
         */
        [[nodiscard]] virtual WorkflowStepResult execute_step(
            const std::shared_ptr<Core::ICommand>& cmd,
            const Json& params,
            const Core::ExecutionControl& control) = 0;
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
