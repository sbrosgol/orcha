#include "WorkflowRunner.hpp"
#include "orcha_pch.hpp"
#include <mutex>
#include <future>
#include <algorithm>
#include <complex>
#include <iostream>
#include <pplx/pplxtasks.h>

#include "YamlToJson.hpp"

namespace Orcha::Workflow {
    web::json::value WorkflowRunner::resolve_placeholders(const web::json::value &input,
                                                          const std::vector<WorkflowStepResult> &previous_results) {
        if (input.is_string()) {
            utility::string_t s = input.as_string();
#if defined(_WIN32)
            static const std::wregex re(L"\\{\\{step(\\d+)\\.output(\\.[\\w\\d_]+)*\\}\\}");
            std::wsmatch match;
            while (std::regex_search(s, match, re)) {
                int idx = std::stoi(match[1].str()) - 1;
                std::wstring field_path = match[2].str();
                std::wstring value = L"";
                if (idx >= 0 && idx < (int) previous_results.size()) {
                    auto out = previous_results[idx].output;
                    if (!field_path.empty()) {
                        size_t start = 1;
                        while (start < field_path.size()) {
                            size_t next = field_path.find(L'.', start);
                            std::wstring key = field_path.substr(start, next - start);
                            if (out.is_object() && out.has_field(key)) {
                                out = out.at(key);
                            } else {
                                out = web::json::value::null();
                                break;
                            }
                            start = (next == std::wstring::npos) ? field_path.size() : next + 1;
                        }
                        if (!out.is_null()) {
                            if (out.is_string()) value = out.as_string();
                            else if (out.is_integer()) value = std::to_wstring(out.as_integer());
                            else if (out.is_double()) value = std::to_wstring(out.as_double());
                            else if (out.is_boolean()) value = out.as_bool() ? L"true" : L"false";
                            else value = L"<non-scalar>";
                        }
                    }
                }
                s.replace(match.position(0), match.length(0), value);
            }
#else
            static const std::regex re(R"(\{\{step(\d+)\.output(\.[\w\d_]+)*\}\})");
            std::smatch match;
            while (std::regex_search(s, match, re)) {
                int idx = std::stoi(match[1].str()) - 1;
                std::string field_path = match[2].str();
                std::string value;
                if (int prev_idx = static_cast<int>(previous_results.size()); idx >= 0 && idx < prev_idx) {
                    auto out = previous_results[idx].output;
                    if (!field_path.empty()) {
                        size_t start = 1;
                        while (start < field_path.size()) {
                            size_t next = field_path.find('.', start);
                            std::string key = field_path.substr(start, next - start);
                            if (out.is_object() && out.has_field(key)) {
                                out = out.at(key);
                            } else {
                                out = web::json::value::null();
                                break;
                            }
                            start = (next == std::string::npos) ? field_path.size() : next + 1;
                        }
                        if (!out.is_null()) {
                            if (out.is_string()) value = out.as_string();
                            else if (out.is_integer()) value = std::to_string(out.as_integer());
                            else if (out.is_double()) value = std::to_string(out.as_double());
                            else if (out.is_boolean()) value = out.as_bool() ? "true" : "false";
                            else value = "<non-scalar>";
                        }
                    }
                }
                s.replace(match.position(0), match.length(0), value);
            }
#endif
            return web::json::value::string(s);
        }

        if (input.is_object()) {
            auto out = input;
            for (auto &f: out.as_object())
                out[f.first] = resolve_placeholders(f.second, previous_results);
            return out;
        }
        if (input.is_array()) {
            auto out = input;
            for (size_t i = 0; i < out.size(); ++i)
                out[i] = resolve_placeholders(out[i], previous_results);
            return out;
        }
        return input;
    }

    bool WorkflowRunner::run(const std::string &yaml_path, std::vector<WorkflowStepResult> &step_results) const {
        YAML::Node config = YAML::LoadFile(yaml_path);
        if (!config["steps"]) {
            std::cerr << "No 'steps' defined in YAML." << std::endl;
            return false;
        }
        std::mutex results_mutex;
        std::vector<std::future<void> > futures;

        for (const auto &step: config["steps"]) {
            bool parallel = step["parallel"] && step["parallel"].as<bool>();
            auto cmd_name = step["command"].as<std::string>();
            auto *cmd = registry_.get_command(cmd_name);
            WorkflowStepResult result;

            if (!cmd) {
                result.success = false;
                result.error_message = "Command not found: " + cmd_name;
                std::lock_guard<std::mutex> lock(results_mutex);
                step_results.push_back(result);
                break;
            }

            web::json::value params;
            if (step["params"]) {
                params = Utils::yaml_to_json(step["params"]);
                params = resolve_placeholders(params, step_results);
            }

            if (parallel) {
                futures.push_back(std::async(std::launch::async, [this, params, cmd, &results_mutex, &step_results]() mutable {
                    WorkflowStepResult local_result;
                    try {
                        local_result.output = cmd->execute(params);
                        local_result.success = true;
                    } catch (const std::exception &ex) {
                        local_result.success = false;
                        local_result.error_message = ex.what();
                    }
                    std::lock_guard<std::mutex> lock(results_mutex);
                    step_results.push_back(std::move(local_result));
                }));
            } else {
                try {
                    result.output = cmd->execute(params);
                    result.success = true;
                } catch (const std::exception &ex) {
                    result.success = false;
                    result.error_message = ex.what();
                }
                {
                    std::lock_guard<std::mutex> lock(results_mutex);
                    step_results.push_back(result);
                }
                if (!result.success) break;
            }
        }

        for (auto &fut: futures) {
            if (fut.valid())
                fut.get();
        }
        return std::ranges::all_of(step_results, [](const auto &r) { return r.success; });
    }

    pplx::task<web::json::value> WorkflowRunner::run_and_report_async(const std::string &yaml_content) const {
        return pplx::create_task([=, this] {
            YAML::Node config = YAML::Load(yaml_content);
            std::vector<WorkflowStepResult> results;
            std::mutex results_mutex;
            std::vector<std::future<void> > futures;

            if (!config["steps"]) {
                web::json::value error = web::json::value::array();
                error[0][U("success")] = web::json::value(false);
                error[0][U("error_message")] = web::json::value::string("No 'steps' defined in YAML.");
                error[0][U("output")] = web::json::value::null();
                return error;
            }

            for (const auto &step: config["steps"]) {
                bool parallel = step["parallel"] && step["parallel"].as<bool>();
                auto cmd_name = step["command"].as<std::string>();
                auto *cmd = registry_.get_command(cmd_name);
                WorkflowStepResult result;

                if (!cmd) {
                    result.success = false;
                    result.error_message = "Command not found: " + cmd_name;
                    std::lock_guard<std::mutex> lock(results_mutex);
                    results.push_back(result);
                    break;
                }

                web::json::value params;
                if (step["params"]) {
                    params = Utils::yaml_to_json(step["params"]);
                    params = resolve_placeholders(params, results);
                }

                if (parallel) {
                    futures.push_back(std::async(std::launch::async, [this, params, cmd, &results_mutex, &results]() mutable {
                        WorkflowStepResult local_result;
                        try {
                            local_result.output = cmd->execute(params);
                            local_result.success = true;
                        } catch (const std::exception &ex) {
                            local_result.success = false;
                            local_result.error_message = ex.what();
                        }
                        std::lock_guard<std::mutex> lock(results_mutex);
                        results.push_back(std::move(local_result));
                    }));
                } else {
                    try {
                        result.output = cmd->execute(params);
                        result.success = true;
                    } catch (const std::exception &ex) {
                        result.success = false;
                        result.error_message = ex.what();
                    }
                    {
                        std::lock_guard<std::mutex> lock(results_mutex);
                        results.push_back(result);
                    }
                    if (!result.success) break;
                }
            }

            for (auto &fut: futures) {
                if (fut.valid())
                    fut.get();
            }

            web::json::value arr = web::json::value::array();
            for (size_t i = 0; i < results.size(); ++i) {
                arr[i][U("success")] = web::json::value(results[i].success);
                arr[i][U("error_message")] = web::json::value::string(results[i].error_message);
                arr[i][U("output")] = results[i].output;
            }
            return arr;
        });
    }

    pplx::task<web::json::value> WorkflowRunner::run_and_report_json(const web::json::value& workflow_json) const {
        // The logic will mirror run_and_report_async, but uses workflow_json directly
        return pplx::create_task([=, this] {
            std::vector<WorkflowStepResult> results;
            std::mutex results_mutex;
            std::vector<std::future<void>> futures;

            if (!workflow_json.has_field(U("steps")) || !workflow_json.at(U("steps")).is_array()) {
                web::json::value error = web::json::value::array();
                error[0][U("success")] = web::json::value(false);
                error[0][U("error_message")] = web::json::value::string("No 'steps' array in JSON.");
                error[0][U("output")] = web::json::value::null();
                return error;
            }

            auto steps = workflow_json.at(U("steps")).as_array();
            for (const auto& step : steps) {
                bool parallel = step.has_field(U("parallel")) && step.at(U("parallel")).as_bool();
                std::string cmd_name = step.at(U("command")).as_string();
                auto* cmd = registry_.get_command(cmd_name);
                WorkflowStepResult result;

                if (!cmd) {
                    result.success = false;
                    result.error_message = "Command not found: " + cmd_name;
                    std::lock_guard<std::mutex> lock(results_mutex);
                    results.push_back(result);
                    break;
                }

                web::json::value params = step.has_field(U("params")) ? 
                    step.at(U("params")) : web::json::value::object();
                params = resolve_placeholders(params, results);

                if (parallel) {
                    futures.push_back(std::async(std::launch::async, [this, params, cmd, &results_mutex, &results]() mutable {
                        WorkflowStepResult local_result;
                        try {
                            local_result.output = cmd->execute(params);
                            local_result.success = true;
                        } catch (const std::exception& ex) {
                            local_result.success = false;
                            local_result.error_message = ex.what();
                        }
                        std::lock_guard<std::mutex> lock(results_mutex);
                        results.push_back(std::move(local_result));
                    }));
                } else {
                    try {
                        result.output = cmd->execute(params);
                        result.success = true;
                    } catch (const std::exception& ex) {
                        result.success = false;
                        result.error_message = ex.what();
                    }
                    {
                        std::lock_guard<std::mutex> lock(results_mutex);
                        results.push_back(result);
                    }
                    if (!result.success) break;
                }
            }

            for (auto& fut : futures) {
                if (fut.valid()) fut.get();
            }

            web::json::value arr = web::json::value::array();
            for (size_t i = 0; i < results.size(); ++i) {
                arr[i][U("success")] = web::json::value(results[i].success);
                arr[i][U("error_message")] = web::json::value::string(results[i].error_message);
                arr[i][U("output")] = results[i].output;
            }
            return arr;
        });
    }

} // namespace Orcha::Workflow
