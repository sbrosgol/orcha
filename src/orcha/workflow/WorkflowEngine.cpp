//
// WorkflowEngine.cpp - Refactored workflow engine implementation
// Created as part of architectural improvements
//

#include "WorkflowEngine.hpp"
#include "../core/Json.hpp"
#include "../utils/YamlToJson.hpp"
#include <yaml-cpp/yaml.h>
#include <regex>
#include <algorithm>
#include <charconv>
#include <ranges>

namespace Orcha::Workflow {

    // ============================================================================
    // SyncStepExecutor Implementation
    // ============================================================================

    WorkflowStepResult SyncStepExecutor::execute_step(
        const std::shared_ptr<Core::ICommand>& cmd,
        const Orcha::Json& params) {

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

            // Commands may report failure in-band instead of throwing:
            // { "success": false, "error": "..." }. Honour it, otherwise a
            // failed shell command / HTTP 500 would count as a successful step.
            const auto& out = result.output;
            if (out.is_object() && out.contains("success") &&
                out.at("success").is_boolean() && !out.at("success").get<bool>()) {
                result.success = false;
                result.error_message =
                    (out.contains("error") && out.at("error").is_string())
                        ? out.at("error").get<std::string>()
                        : "Command reported failure";
                return result;
            }
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

    Orcha::Json PlaceholderResolver::resolve(
        const Orcha::Json& input,
        const std::vector<WorkflowStepResult>& previous_results) {

        if (input.is_string()) {
            std::string resolved = resolve_string(
                input.get<std::string>(), previous_results);
            return resolved;
        }

        if (input.is_object()) {
            Orcha::Json out = Orcha::Json::object();
            for (const auto& [key, value] : input.items()) {
                out[key] = resolve(value, previous_results);
            }
            return out;
        }

        if (input.is_array()) {
            Orcha::Json out = Orcha::Json::array();
            for (const auto& elem : input) {
                out.push_back(resolve(elem, previous_results));
            }
            return out;
        }

        return input;
    }

    namespace {

        // {{stepN.output.a.b}}  -> group 1 = N
        // {{steps.<name>.output.a.b}} -> group 2 = name ("steps." prefix avoids
        // any clash with the positional "stepN" form). Group 3 = the WHOLE
        // dotted field path (a repeated capturing group would only keep its
        // last iteration).
        const std::regex& placeholder_pattern() {
            static const std::regex re(
                R"(\{\{(?:step(\d+)|steps\.([A-Za-z_]\w*))\.output((?:\.\w+)*)\}\})");
            return re;
        }

        /// Parse a 1-based step position; nullopt on overflow or 0.
        std::optional<size_t> parse_position(const std::ssub_match& m) {
            size_t value = 0;
            const char* first = &*m.first;
            const char* last = first + m.length();
            auto [ptr, ec] = std::from_chars(first, last, value);
            if (ec != std::errc{} || ptr != last || value == 0) {
                return std::nullopt;
            }
            return value;
        }

    } // namespace

    std::string PlaceholderResolver::resolve_string(
        const std::string& input,
        const std::vector<WorkflowStepResult>& previous_results) {

        // Single left-to-right pass over the ORIGINAL input. Substituted text
        // is never re-scanned, so a step output that itself contains
        // "{{step1.output...}}" cannot cause re-expansion or an endless loop.
        std::string result;
        result.reserve(input.size());
        size_t copied_up_to = 0;

        const auto& re = placeholder_pattern();
        for (std::sregex_iterator it(input.begin(), input.end(), re), end; it != end; ++it) {
            const std::smatch& match = *it;
            result.append(input, copied_up_to,
                          static_cast<size_t>(match.position(0)) - copied_up_to);
            copied_up_to = static_cast<size_t>(match.position(0) + match.length(0));

            const std::string field_path = match[3].str();
            const WorkflowStepResult* source = nullptr;

            if (match[1].matched) {
                if (auto pos = parse_position(match[1]);
                    pos && *pos <= previous_results.size()) {
                    source = &previous_results[*pos - 1];
                }
            } else {
                // Walk in reverse so the MOST RECENT step with this name wins
                // when names are duplicated (consistent with the dependency
                // analysis, which also binds a name to its latest step).
                const std::string step_name = match[2].str();
                for (const auto& prev : std::views::reverse(previous_results)) {
                    if (!prev.name.empty() && prev.name == step_name) {
                        source = &prev;
                        break;
                    }
                }
            }

            if (source) {
                result += json_value_to_string(navigate_output(source->output, field_path));
            }
        }
        result.append(input, copied_up_to);
        return result;
    }

    Orcha::Json PlaceholderResolver::navigate_output(
        const Orcha::Json& output,
        const std::string& field_path) {

        if (field_path.empty()) {
            return output;
        }

        Orcha::Json current = output;
        size_t start = 1;  // Skip leading dot

        while (start < field_path.size()) {
            size_t next = field_path.find('.', start);
            std::string key = field_path.substr(
                start, next == std::string::npos ? std::string::npos : next - start);

            if (current.is_object() && current.contains(key)) {
                current = current.at(key);
            } else {
                return Orcha::Json(nullptr);
            }

            start = (next == std::string::npos) ? field_path.size() : next + 1;
        }

        return current;
    }

    std::string PlaceholderResolver::json_value_to_string(
        const Orcha::Json& value) {

        if (value.is_null()) {
            return "";
        }
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (value.is_number_integer()) {
            return std::to_string(value.get<long long>());
        }
        if (value.is_number_float()) {
            return std::to_string(value.get<double>());
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? "true" : "false";
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

    Orcha::Json WorkflowEngine::execute_json(const Orcha::Json& workflow_json) {
        // Validate input
        if (!workflow_json.contains("steps") ||
            !workflow_json.at("steps").is_array()) {
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
        result.name = step.name.value_or("");  // enables {{steps.<name>.output}} refs

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
