//
// WorkflowEngine.cpp - Refactored workflow engine implementation
// Created as part of architectural improvements
//

#include "WorkflowEngine.hpp"
#include "../core/Json.hpp"
#include "ExecutionContext.hpp"
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
        const Orcha::Json& params,
        const Core::ExecutionControl& control) {

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

            result.output = cmd->execute_with_control(params, control);

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

    PlaceholderResolver::References PlaceholderResolver::references(const Orcha::Json& input) {
        References refs;
        collect_references(input, refs);
        return refs;
    }

    void PlaceholderResolver::collect_references(const Orcha::Json& input, References& out) {
        if (input.is_string()) {
            const auto& str = input.get_ref<const std::string&>();
            const auto& re = placeholder_pattern();
            for (std::sregex_iterator it(str.begin(), str.end(), re), end; it != end; ++it) {
                if ((*it)[1].matched) {
                    if (auto pos = parse_position((*it)[1])) {
                        out.positions.push_back(*pos);
                    }
                } else {
                    out.names.push_back((*it)[2].str());
                }
            }
        } else if (input.is_structured()) {
            for (const auto& elem : input) {
                collect_references(elem, out);
            }
        }
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
        std::shared_ptr<Utils::ILogger> logger,
        EngineOptions options)
        : registry_(std::move(registry))
        , executor_(std::move(executor))
        , logger_(std::move(logger))
        , options_(std::move(options)) {

        if (!executor_) {
            executor_ = std::make_shared<SyncStepExecutor>();
        }
    }

    WorkflowEngine::WorkflowEngine(std::shared_ptr<Core::ICommandRegistry> registry)
        : WorkflowEngine(std::move(registry), std::make_shared<SyncStepExecutor>(), nullptr) {}

    namespace {

        /// For each step, the earlier steps whose output it references.
        std::vector<std::vector<size_t>> step_dependencies(const WorkflowDefinition& def) {
            std::vector<std::vector<size_t>> deps(def.steps.size());
            for (size_t i = 0; i < def.steps.size(); ++i) {
                const auto refs = PlaceholderResolver::references(def.steps[i].params);
                for (size_t pos : refs.positions) {
                    if (pos >= 1 && pos - 1 < i) {
                        deps[i].push_back(pos - 1);
                    }
                }
                for (const auto& name : refs.names) {
                    // Bind to the latest earlier step with this name.
                    for (size_t j = i; j-- > 0;) {
                        if (def.steps[j].name == name) {
                            deps[i].push_back(j);
                            break;
                        }
                    }
                }
            }
            return deps;
        }

    } // namespace

    WorkflowResult WorkflowEngine::execute(const WorkflowDefinition& definition) {
        using Clock = Core::ExecutionControl::Clock;
        using std::chrono::milliseconds;

        const size_t step_count = definition.steps.size();
        const auto deps = step_dependencies(definition);

        // Whole-run deadline, if the workflow sets timeout_ms.
        const auto started = Clock::now();
        const std::optional<milliseconds> run_limit =
            definition.timeout_ms && *definition.timeout_ms > 0
                ? std::optional(milliseconds(*definition.timeout_ms)) : std::nullopt;
        const std::optional<Clock::time_point> run_deadline =
            run_limit ? std::optional(started + *run_limit) : std::nullopt;

        // The run is cancelled by the engine's token (shutdown) or by the
        // enclosing run's token when this is a nested run_job. Installing the
        // run token in the ExecutionContext lets nested runs inherit it.
        std::stop_source run_stop;
        std::stop_callback on_engine_cancel(options_.cancel, [&run_stop] { run_stop.request_stop(); });
        std::stop_callback on_parent_cancel(ExecutionContext::stop_token(),
                                            [&run_stop] { run_stop.request_stop(); });
        ExecutionContext::StopScope stop_scope(run_stop.get_token());

        WorkflowResult result;
        result.step_results.resize(step_count);
        // Guards result.step_results: `parallel: true` steps run concurrently
        // with the steps that follow them.
        std::mutex results_mutex;
        // Completion signal of each launched `parallel: true` step.
        std::vector<std::shared_future<void>> in_flight(step_count);
        const auto context = ExecutionContext::capture();

        // Runs step i after every step it references has finished, so a
        // reference to a still-running parallel step never sees a blank result.
        auto run_step = [&](size_t i) -> WorkflowStepResult {
            for (size_t dep : deps[i]) {
                if (in_flight[dep].valid()) {
                    in_flight[dep].wait();
                }
            }

            const auto& step = definition.steps[i];
            WorkflowStepResult step_result;

            // The step's deadline is the earlier of its own limit and the run's.
            const milliseconds step_limit = step.timeout_ms && *step.timeout_ms > 0
                ? milliseconds(*step.timeout_ms) : options_.default_step_timeout;
            const auto step_start = Clock::now();
            std::optional<Clock::time_point> deadline = run_deadline;
            bool run_deadline_binds = run_deadline.has_value();
            if (step_limit > milliseconds::zero() &&
                (!deadline || step_start + step_limit < *deadline)) {
                deadline = step_start + step_limit;
                run_deadline_binds = false;
            }
            const auto timeout_message = [&] {
                return run_deadline_binds
                    ? "Workflow timed out after " + std::to_string(run_limit->count()) + " ms"
                    : "Step timed out after " + std::to_string(step_limit.count()) + " ms";
            };

            if (run_stop.stop_requested()) {
                step_result.success = false;
                step_result.error_message = "Cancelled";
            } else if (deadline && step_start >= *deadline) {
                step_result.success = false;
                step_result.error_message = timeout_message();
            } else {
                const Core::ExecutionControl control(run_stop.get_token(), deadline);
                try {
                    Orcha::Json params;
                    {
                        std::lock_guard lock(results_mutex);
                        params = PlaceholderResolver::resolve(step.params, result.step_results);
                    }
                    step_result = execute_single_step(step, params, static_cast<int>(i), control);
                } catch (const std::exception& ex) {
                    step_result.success = false;
                    step_result.error_message = ex.what();
                }

                if (deadline && Clock::now() > *deadline) {
                    // Returned after its deadline (it ignored or overran the
                    // control): its result is not trusted.
                    step_result.success = false;
                    step_result.error_message = timeout_message();
                } else if (!step_result.success && run_stop.stop_requested() &&
                           step_result.error_message.rfind("Cancelled", 0) != 0) {
                    step_result.error_message = step_result.error_message.empty()
                        ? std::string("Cancelled")
                        : "Cancelled: " + step_result.error_message;
                }
            }
            step_result.step_index = static_cast<int>(i);
            step_result.command_name = step.command_name;
            step_result.name = step.name.value_or("");

            log_step_complete(step_result, static_cast<int>(i));
            {
                std::lock_guard lock(results_mutex);
                result.step_results[i] = step_result;
            }
            return step_result;
        };

        auto wait_all = [&in_flight] {
            for (auto& f : in_flight) {
                if (f.valid()) {
                    f.wait();
                }
            }
        };

        try {
            for (size_t i = 0; i < step_count; ++i) {
                const auto& step = definition.steps[i];
                log_step_start(step, static_cast<int>(i));

                if (step.parallel) {
                    // in_flight is pre-sized and only element i is written
                    // here; tasks read elements < i, so there is no race.
                    in_flight[i] = std::async(std::launch::async, [run_step, context, i] {
                        ExecutionContext::Scope scope(context);
                        (void)run_step(i);
                    }).share();
                    continue;
                }

                auto step_result = run_step(i);
                // Stop on failure for non-parallel steps
                if (!step_result.success) {
                    break;
                }
            }
        } catch (...) {
            wait_all();   // tasks reference locals of this frame
            throw;
        }
        wait_all();

        std::lock_guard lock(results_mutex);
        result.success = std::ranges::all_of(result.step_results,
            [](const auto& r) { return r.success; });
        if (!result.success) {
            for (const auto& r : result.step_results) {
                if (!r.success && !r.error_message.empty()) {
                    result.error_message = r.error_message;
                    break;
                }
            }
        }

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
        const Orcha::Json& resolved_params,
        int step_index,
        const Core::ExecutionControl& control) {

        WorkflowStepResult result;
        result.step_index = step_index;
        result.command_name = step.command_name;

        // Get command. The shared_ptr keeps a plugin's library loaded for the
        // whole execution, even if the plugin is disabled meanwhile.
        auto cmd = registry_->get_command(step.command_name);
        if (!cmd) {
            result.success = false;
            result.error_message = "Command not found: " + step.command_name;
            return result;
        }

        result = executor_->execute_step(cmd, resolved_params, control);
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
