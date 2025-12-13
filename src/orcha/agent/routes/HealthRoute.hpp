//
// HealthRoute.hpp - Health check endpoint handler
// Created as part of architectural improvements
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../../utils/ILogger.hpp"

namespace Orcha::Agent::Routes {

    /**
     * @class HealthRoute
     * @brief Handles the health check endpoint (GET /).
     */
    class HealthRoute : public IRouteHandler {
    public:
        explicit HealthRoute(std::shared_ptr<Utils::ILogger> logger = nullptr)
            : logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            return method == "GET" && (path == "/" || path.empty());
        }

        void handle(web::http::http_request request) override {
            if (logger_) {
                logger_->debug("Health check endpoint hit");
            }

            pplx::create_task([request]() {
                web::http::http_response resp(web::http::status_codes::OK);
                resp.headers().add(
                    web::http::header_names::content_type,
                    "text/plain; charset=utf-8");
                resp.set_body(
                    "Orcha Command Agent is running.\n"
                    "POST your workflow JSON (application/json) to /workflow\n"
                    "OpenAPI UI: /swagger\n"
                    "Spec: /swagger.json\n"
                    "Commands: /commands\n");
                request.reply(resp);
            });
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {{
                .method = "GET",
                .path = "/",
                .description = "Health check endpoint"
            }};
        }

    private:
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
