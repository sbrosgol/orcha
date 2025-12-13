//
// WorkflowRoute.hpp - Workflow execution endpoint handler
// Created as part of architectural improvements
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../../workflow/IWorkflowEngine.hpp"
#include "../../utils/ILogger.hpp"

namespace Orcha::Agent::Routes {

    /**
     * @class WorkflowRoute
     * @brief Handles workflow execution (POST /workflow).
     */
    class WorkflowRoute : public IRouteHandler {
    public:
        WorkflowRoute(std::shared_ptr<Workflow::IWorkflowEngine> engine,
                     std::shared_ptr<Utils::ILogger> logger = nullptr)
            : engine_(std::move(engine))
            , logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            return method == "POST" && path == "/workflow";
        }

        void handle(web::http::http_request request) override {
            auto content_type = request.headers().content_type();

            if (content_type != U("application/json")) {
                handle_unsupported_media_type(request);
                return;
            }

            if (logger_) {
                logger_->info("Executing workflow from POST /workflow");
            }

            auto engine = engine_;
            auto logger = logger_;

            request.extract_json()
                .then([engine, logger, request](web::json::value json) {
                    return engine->execute_json(json);
                })
                .then([request, logger](web::json::value result) {
                    request.reply(web::http::status_codes::OK, result);
                    if (logger) {
                        logger->debug("Workflow execution completed");
                    }
                })
                .then([request, logger](pplx::task<void> t) {
                    try {
                        t.get();
                    } catch (const std::exception& ex) {
                        if (logger) {
                            logger->error(std::string("Workflow execution error: ") + ex.what());
                        }
                        web::json::value error;
                        error[U("error")] = web::json::value::string(ex.what());
                        request.reply(web::http::status_codes::InternalError, error);
                    }
                });
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {{
                .method = "POST",
                .path = "/workflow",
                .description = "Execute a workflow"
            }};
        }

    private:
        void handle_unsupported_media_type(web::http::http_request request) {
            pplx::create_task([request]() {
                web::http::http_response resp(web::http::status_codes::UnsupportedMediaType);
                resp.headers().add(
                    web::http::header_names::content_type,
                    U("application/json"));

                web::json::value msg;
                msg[U("error")] = web::json::value::string(
                    U("Only application/json is accepted for /workflow"));
                msg[U("hint")] = web::json::value::string(
                    U("See /swagger for API documentation and a sample payload"));
                resp.set_body(msg);

                request.reply(resp);
            });
        }

        std::shared_ptr<Workflow::IWorkflowEngine> engine_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
