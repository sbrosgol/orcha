//
// WorkflowEngine.cpp - Refactored workflow engine implementation
// Created as part of architectural improvements
//

#include "WorkflowEngine.hpp"
#include "../utils/YamlToJson.hpp"
#include <yaml-cpp/yaml.h>
#include <regex>
#include <algorithm>

namespace Orcha::Workflow {

    // ============================================================================
    // SyncStepExecutor Implementation
    // ============================================================================

    WorkflowStepResult SyncStepExecutor::execute_step(
        const std::shared_ptr<Core::ICommand>& cmd,
        const web::json::value& params) {

        WorkflowStepResult result;
        result.command_name = cmd->name();

        try {
            // Validate parameters first
            auto validation = cmd->validate(params);
            if (!validation.is_ok()) {
                result.success = false;
                result.error_message = "Validation failed for parameter '" +
                    validation.error().parameter_name + "': " +
                    validation.error().message;
                return result;
            }

            result.output = cmd->execute(params);
            result.success = true;
        } catch (const std::exception& ex) {
            result.success = false;
            result.error_message = ex.what();
        } catch (...) {
            result.success = false;
            result.error_message = "Unknown error during command execution";
        }

        return result;
    }

    // ============================================================================
    // PlaceholderResolver Implementation
    // ============================================================================

    web::json::value PlaceholderResolver::resolve(
        const web::json::value& input,
        const std::vector<WorkflowStepResult>& previous_results) {

        if (input.is_string()) {
            std::string resolved = resolve_string(
                input.as_string(), previous_results);
            return web::json::value::string(resolved);
        }

        if (input.is_object()) {
            web::json::value out = web::json::value::object();
            for (const auto& field : input.as_object()) {
                out[field.first] = resolve(field.second, previous_results);
            }
            return out;
        }

        if (input.is_array()) {
            const auto& arr = input.as_array();
            web::json::value out = web::json::value::array(arr.size());
            for (size_t i = 0; i < arr.size(); ++i) {
                out[i] = resolve(arr.at(i), previous_results);
            }
            return out;
        }

        return input;
    }

    std::string PlaceholderResolver::resolve_string(
        const std::string& input,
        const std::vector<WorkflowStepResult>& previous_results) {

        // Capture the WHOLE dotted path in group 2. A repeated *capturing* group
        // (\.\w+)* would only capture its last iteration, so nested references
        // like {{step1.output.a.b}} would lose all but the final segment.
        static const std::regex placeholder_regex(
            R"(\{\{step(\d+)\.output((?:\.[\w\d_]+)*)\}\})");

        std::string result = input;
        std::smatch match;

        while (std::regex_search(result, match, placeholder_regex)) {
            int step_index = std::stoi(match[1].str()) - 1;
            std::string field_path = match[2].str();
            std::string replacement;

            if (step_index >= 0 &&
                step_index < static_cast<int>(previous_results.size())) {

                auto value = navigate_output(
                    previous_results[step_index].output, field_path);
                replacement = json_value_to_string(value);
            }

            result.replace(match.position(0), match.length(0), replacement);
        }

        return result;
    }

    web::json::value PlaceholderResolver::navigate_output(
        const web::json::value& output,
        const std::string& field_path) {

        if (field_path.empty()) {
            return output;
        }

        web::json::value current = output;
        size_t start = 1;  // Skip leading dot

        while (start < field_path.size()) {
            size_t next = field_path.find('.', start);
            std::string key = field_path.substr(
                start, next == std::string::npos ? std::string::npos : next - start);

            if (current.is_object() && current.has_field(key)) {
                current = current.at(key);
            } else {
                return web::json::value::null();
            }

            start = (next == std::string::npos) ? field_path.size() : next + 1;
        }

        return current;
    }

    std::string PlaceholderResolver::json_value_to_string(
        const web::json::value& value) {

        if (value.is_null()) {
            return "";
        }
        if (value.is_string()) {
            return value.as_string();
        }
        if (value.is_integer()) {
            return std::to_string(value.as_integer());
        }
        if (value.is_double()) {
            return std::to_string(value.as_double());
        }
        if (value.is_boolean()) {
            return value.as_bool() ? "true" : "false";
        }

        return "<non-scalar>";
    }

    // ============================================================================
    // WorkflowEngine Implementation
    // ============================================================================

    WorkflowEngine::WorkflowEngine(
        std::shared_ptr<Core::ICommandRegistry> registry,
        std::shared_ptr<IStepExecutor> executor,
        std::shared_ptr<Utils::ILogger> logger)
        : registry_(std::move(registry))
        , executor_(std::move(executor))
        , logger_(std::move(logger)) {

        if (!executor_) {
            executor_ = std::make_shared<SyncStepExecutor>();
        }
    }

    WorkflowEngine::WorkflowEngine(std::shared_ptr<Core::ICommandRegistry> registry)
        : WorkflowEngine(std::move(registry), std::make_shared<SyncStepExecutor>(), nullptr) {}

    WorkflowResult WorkflowEngine::execute(const WorkflowDefinition& definition) {
        WorkflowResult result;
        result.step_results.resize(definition.steps.size());
        std::mutex results_mutex;
        std::vector<std::future<void>> futures;

        for (size_t i = 0; i < definition.steps.size(); ++i) {
            const auto& step = definition.steps[i];

            log_step_start(step, static_cast<int>(i));

            if (step.parallel) {
                // Execute in parallel
                futures.push_back(std::async(std::launch::async,
                    [this, &step, &result, &results_mutex, i]() {
                        std::vector<WorkflowStepResult> snapshot;
                        {
                            std::lock_guard<std::mutex> lock(results_mutex);
                            snapshot = result.step_results;
                        }
                        auto step_result = execute_single_step(
                            step, snapshot, static_cast<int>(i));

                        std::lock_guard<std::mutex> lock(results_mutex);
                        result.step_results[i] = std::move(step_result);
                    }));
            } else {
                // Execute synchronously
                auto step_result = execute_single_step(
                    step, result.step_results, static_cast<int>(i));

                log_step_complete(step_result, static_cast<int>(i));

                result.step_results[i] = step_result;

                // Stop on failure for non-parallel steps
                if (!step_result.success) {
                    result.success = false;
                    result.error_message = step_result.error_message;
                    break;
                }
            }
        }

        // Wait for all parallel tasks
        for (auto& future : futures) {
            if (future.valid()) {
                future.get();
            }
        }

        // Check overall success
        result.success = std::ranges::all_of(result.step_results,
            [](const auto& r) { return r.success; });

        return result;
    }

    pplx::task<WorkflowResult> WorkflowEngine::execute_async(
        const WorkflowDefinition& definition) {

        return pplx::create_task([this, definition]() {
            return execute(definition);
        });
    }

    pplx::task<web::json::value> WorkflowEngine::execute_json(
        const web::json::value& workflow_json) {

        return pplx::create_task([this, workflow_json]() {
            // Validate input
            if (!workflow_json.has_field(U("steps")) ||
                !workflow_json.at(U("steps")).is_array()) {
                WorkflowResult error_result;
                error_result.success = false;
                WorkflowStepResult error_step;
                error_step.success = false;
                error_step.error_message = "No 'steps' array in workflow JSON";
                error_result.step_results.push_back(error_step);
                return error_result.to_json();
            }

            auto definition = WorkflowDefinition::from_json(workflow_json);
            auto result = execute(definition);
            return result.to_json();
        });
    }

    WorkflowResult WorkflowEngine::execute_yaml(const std::string& yaml_path) {
        try {
            YAML::Node yaml = YAML::LoadFile(yaml_path);
            auto json = Utils::yaml_to_json(yaml);
            auto definition = WorkflowDefinition::from_json(json);
            return execute(definition);
        } catch (const std::exception& ex) {
            WorkflowResult error_result;
            error_result.success = false;
            error_result.error_message = std::string("Failed to load YAML: ") + ex.what();
            return error_result;
        }
    }

    WorkflowResult WorkflowEngine::execute_yaml_string(const std::string& yaml_content) {
        try {
            YAML::Node yaml = YAML::Load(yaml_content);
            auto json = Utils::yaml_to_json(yaml);
            auto definition = WorkflowDefinition::from_json(json);
            return execute(definition);
        } catch (const std::exception& ex) {
            WorkflowResult error_result;
            error_result.success = false;
            error_result.error_message = std::string("Failed to parse YAML: ") + ex.what();
            return error_result;
        }
    }

    WorkflowStepResult WorkflowEngine::execute_single_step(
        const WorkflowStep& step,
        const std::vector<WorkflowStepResult>& previous_results,
        int step_index) {

        WorkflowStepResult result;
        result.step_index = step_index;
        result.command_name = step.command_name;

        // Get command
        auto cmd = registry_->get_command(step.command_name);
        if (!cmd) {
            result.success = false;
            result.error_message = "Command not found: " + step.command_name;
            return result;
        }

        // Resolve placeholders
        auto resolved_params = PlaceholderResolver::resolve(step.params, previous_results);

        // Execute
        result = executor_->execute_step(cmd, resolved_params);
        result.step_index = step_index;
        result.command_name = step.command_name;

        return result;
    }

    void WorkflowEngine::log_step_start(const WorkflowStep& step, int index) {
        if (logger_) {
            logger_->debug("Starting step " + std::to_string(index + 1) +
                          ": " + step.command_name +
                          (step.parallel ? " (parallel)" : ""));
        }
    }

    void WorkflowEngine::log_step_complete(const WorkflowStepResult& result, int index) {
        if (logger_) {
            if (result.success) {
                logger_->debug("Step " + std::to_string(index + 1) +
                              " completed successfully");
            } else {
                logger_->warn("Step " + std::to_string(index + 1) +
                             " failed: " + result.error_message);
            }
        }
    }

} // namespace Orcha::Workflow
