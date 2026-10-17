//
// WorkflowRoute.hpp - Workflow execution endpoint handler
// Created as part of architectural improvements
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../HttpJson.hpp"
#include "../../workflow/IWorkflowEngine.hpp"
#include "../../jobs/JobService.hpp"
#include "../../utils/ILogger.hpp"

namespace Orcha::Agent::Routes {

    /**
     * @class WorkflowRoute
     * @brief Handles workflow execution (POST /workflow).
     */
    class WorkflowRoute : public IRouteHandler {
    public:
        WorkflowRoute(std::shared_ptr<Workflow::IWorkflowEngine> engine,
                     std::shared_ptr<Utils::ILogger> logger = nullptr,
                     std::shared_ptr<Jobs::JobService> jobs = nullptr)
            : engine_(std::move(engine))
            , logger_(std::move(logger))
            , jobs_(std::move(jobs)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            return method == "POST" && path == "/workflow";
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            const auto content_type = request.header("Content-Type").value_or("");
            // Accept "application/json" with or without a charset suffix.
            if (content_type.rfind("application/json", 0) != 0) {
                return handle_unsupported_media_type();
            }

            if (logger_) {
                logger_->info("Executing workflow from POST /workflow");
            }

            try {
                Orcha::Json json = Orcha::Json::parse(request.body);
                Orcha::Json result = engine_->execute_json(json);

                // Record the ad-hoc run (no job id) when a job service exists.
                if (jobs_) {
                    try {
                        jobs_->record_run(std::nullopt, "api",
                                          all_steps_succeeded(result), result, "");
                    } catch (...) { /* recording must never break the response */ }
                }
                if (logger_) {
                    logger_->debug("Workflow execution completed");
                }
                return reply_json(status::OK, result);
            } catch (const std::exception& ex) {
                if (logger_) {
                    logger_->error(std::string("Workflow execution error: ") + ex.what());
                }
                return reply_error(status::InternalError, ex.what());
            }
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {{
                .method = "POST",
                .path = "/workflow",
                .description = "Execute a workflow"
            }};
        }

    private:
        /// The /workflow result is a JSON array of step results; the run
        /// succeeded if every step reports success (an empty array counts as success).
        static bool all_steps_succeeded(const Orcha::Json& result) {
            if (!result.is_array()) return false;
            for (const auto& step : result) {
                if (!step.contains("success") || !step.at("success").get<bool>()) {
                    return false;
                }
            }
            return true;
        }

        static HttpResponse handle_unsupported_media_type() {
            Orcha::Json msg = Orcha::Json::object();
            msg["error"] = "Only application/json is accepted for /workflow";
            msg["hint"] = "See /swagger for API documentation and a sample payload";
            return reply_json(status::UnsupportedMediaType, msg);
        }

        std::shared_ptr<Workflow::IWorkflowEngine> engine_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::shared_ptr<Jobs::JobService> jobs_;
    };

} // namespace Orcha::Agent::Routes
