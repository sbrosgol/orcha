//
// WorkflowRoute.hpp - Workflow execution API (bearer-token auth)
//
//   POST /workflow                    submit a workflow -> 202 + run resource
//   GET  /workflow/runs/{id}          run status/result
//   POST /workflow/runs/{id}/cancel   cancel a queued or running run
//
// The workflow is JSON, or YAML when sent with a YAML Content-Type
// (application/yaml, text/yaml, ...; see agent/HttpBody.hpp).
//
// Runs are asynchronous: POST /workflow answers 202 Accepted with a Location
// and the queued run. A client that prefers to wait sends `Prefer: wait=<s>`
// (RFC 7240); the server waits up to that long (capped by the configured
// maximum) and answers 200 with the finished run, or 202 if it is still going.
// Waiting is also refused (plain 202) when too many requests are already
// waiting, so waiters can never occupy every HTTP worker.
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../HttpBody.hpp"
#include "../HttpJson.hpp"
#include "RunResponses.hpp"
#include "../../core/JsonCompat.hpp"
#include "../../jobs/RunQueue.hpp"
#include "../../workflow/IWorkflowEngine.hpp"
#include "../../utils/ILogger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <limits>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace Orcha::Agent::Routes {

    /// How long POST /workflow may wait when the client sends `Prefer: wait`.
    struct WorkflowWaitOptions {
        /// Upper bound for `Prefer: wait=<seconds>`.
        std::chrono::milliseconds max_wait{std::chrono::seconds(60)};
        /// Requests allowed to wait at once (keep below the HTTP worker count;
        /// 0 = never wait, always answer 202).
        size_t max_waiters = 1;
    };

    /**
     * @class WorkflowRoute
     * @brief Handles the workflow execution API (POST /workflow, /workflow/runs/{id}[/cancel]).
     */
    class WorkflowRoute : public IRouteHandler {
    public:
        WorkflowRoute(std::shared_ptr<Jobs::RunQueue> runs,
                      std::shared_ptr<Utils::ILogger> logger = nullptr,
                      WorkflowWaitOptions wait = {})
            : runs_(std::move(runs))
            , logger_(std::move(logger))
            , wait_(wait) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            return (method == "POST" && path == "/workflow") ||
                   path_starts_with(path, std::string(kRuns) + "/");
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            if (request.path == "/workflow") {
                return submit(request);
            }
            const auto seg = split_path(request.path);   // {"workflow","runs",id[,"cancel"]}
            if (seg.size() == 3) {
                if (request.method != "GET") return reply_error(status::MethodNotAllowed, "Use GET");
                return get_run(seg[2]);
            }
            if (seg.size() == 4 && seg[3] == "cancel") {
                if (request.method != "POST") return reply_error(status::MethodNotAllowed, "Use POST");
                if (!visible(seg[2])) return reply_error(status::NotFound, "No such run: " + seg[2]);
                return reply_cancel(*runs_, seg[2], kRuns);
            }
            return reply_error(status::NotFound, "Unknown workflow endpoint");
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "POST", .path = "/workflow", .description = "Submit a workflow run"},
                {.method = "GET", .path = "/workflow/runs/{id}", .description = "Run status and result"},
                {.method = "POST", .path = "/workflow/runs/{id}/cancel", .description = "Cancel a run"}
            };
        }

        /**
         * @brief Seconds from a `Prefer` header's `wait=<n>` preference, if any.
         *        Preferences are comma-separated and case-insensitive, and may
         *        carry `;`-separated parameters, which are ignored (RFC 7240).
         *        Values too large to represent saturate rather than fail.
         */
        [[nodiscard]] static std::optional<long> preferred_wait(std::string_view prefer) {
            size_t start = 0;
            while (start <= prefer.size()) {
                size_t end = prefer.find(',', start);
                if (end == std::string_view::npos) end = prefer.size();
                std::string_view token = prefer.substr(start, end - start);
                while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.remove_prefix(1);
                if (token.size() > 5 &&
                    std::tolower(static_cast<unsigned char>(token[0])) == 'w' &&
                    std::tolower(static_cast<unsigned char>(token[1])) == 'a' &&
                    std::tolower(static_cast<unsigned char>(token[2])) == 'i' &&
                    std::tolower(static_cast<unsigned char>(token[3])) == 't' && token[4] == '=') {
                    std::string_view num = token.substr(5);
                    num = num.substr(0, num.find(';'));
                    while (!num.empty() && std::isspace(static_cast<unsigned char>(num.back()))) num.remove_suffix(1);
                    long seconds = 0;
                    auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), seconds);
                    if (ptr != num.data() + num.size() || num.empty()) return std::nullopt;
                    if (ec == std::errc::result_out_of_range) return std::numeric_limits<long>::max();
                    if (ec == std::errc{} && seconds >= 0) return seconds;
                    return std::nullopt;
                }
                start = end + 1;
            }
            return std::nullopt;
        }

    private:
        static constexpr const char* kRuns = "/workflow/runs";

        HttpResponse submit(const HttpRequest& request) {
            const auto content_type = request.header("Content-Type").value_or("");
            if (!is_json_media_type(content_type) && !is_yaml_media_type(content_type)) {
                return handle_unsupported_media_type();
            }

            Orcha::Json json;
            try {
                json = parse_request_body(request);
            } catch (const std::exception& ex) {
                return reply_bad_body(request, ex);
            }
            if (!json.is_object() || !json.contains("steps") || !json.at("steps").is_array()) {
                return reply_error(status::BadRequest, "Invalid workflow: expected a \"steps\" array");
            }
            try {
                // Rejects invalid settings (e.g. "rollback") before anything is queued.
                (void)Workflow::WorkflowDefinition::from_json(json);
            } catch (const std::exception& ex) {
                return reply_error(status::BadRequest,
                                   std::string("Invalid workflow: ") + ex.what());
            }

            auto submitted = runs_->submit(std::nullopt, "api", std::move(json));
            if (submitted.is_err()) {
                return reply_submit_error(submitted.error(), runs_->retry_after());
            }
            const Jobs::RunRecord queued = submitted.value();
            if (logger_) logger_->info("Accepted workflow run " + queued.id);

            const auto prefer = request.header("Prefer");
            const auto wait_s = prefer ? preferred_wait(*prefer) : std::nullopt;
            if (!wait_s || *wait_s == 0) {
                return reply_run_accepted(queued, kRuns);
            }

            // Honour the wait only while a waiter slot is free.
            struct WaiterSlot {
                std::atomic<size_t>& count;
                ~WaiterSlot() { count.fetch_sub(1); }
            };
            if (waiters_.fetch_add(1) >= wait_.max_waiters) {
                waiters_.fetch_sub(1);
                return reply_run_accepted(queued, kRuns);
            }
            WaiterSlot slot{waiters_};
            // Compare in seconds first: converting a huge value would overflow.
            const auto max_s = std::chrono::ceil<std::chrono::seconds>(wait_.max_wait).count();
            const auto wait_for = *wait_s >= max_s
                ? wait_.max_wait
                : std::chrono::milliseconds(std::chrono::seconds(*wait_s));
            auto finished = runs_->wait(queued.id, wait_for);

            if (finished) {
                return reply_run(*finished, kRuns);
            }
            auto current = runs_->get(queued.id);
            return reply_run_accepted(current ? *current : queued, kRuns);
        }

        HttpResponse get_run(const std::string& id) {
            auto run = runs_->get(id);
            if (!run || run->job_id) {
                return reply_error(status::NotFound, "No such run: " + id);
            }
            return reply_run(*run, kRuns);
        }

        /// API clients only see ad-hoc runs; job runs belong to the admin API.
        bool visible(const std::string& id) const {
            auto run = runs_->get(id);
            return run && !run->job_id;
        }

        static HttpResponse handle_unsupported_media_type() {
            Orcha::Json msg = Orcha::Json::object();
            msg["error"] = "Send the workflow as application/json or application/yaml";
            msg["hint"] = "See /swagger for API documentation and a sample payload";
            return reply_json(status::UnsupportedMediaType, msg);
        }

        std::shared_ptr<Jobs::RunQueue> runs_;
        std::shared_ptr<Utils::ILogger> logger_;
        WorkflowWaitOptions wait_;
        std::atomic<size_t> waiters_{0};
    };

} // namespace Orcha::Agent::Routes
