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
        /// @param persistent False when runs are kept in memory only (the job
        ///        database could not be opened); the page then says so.
        explicit HealthRoute(std::shared_ptr<Utils::ILogger> logger = nullptr,
                             bool persistent = true)
            : logger_(std::move(logger)), persistent_(persistent) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            return method == "GET" && (path == "/" || path.empty());
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            (void)request;
            if (logger_) {
                logger_->debug("Health check endpoint hit");
            }
            std::string body =
                "Orcha Command Agent is running.\n"
                "POST your workflow JSON (application/json) to /workflow\n"
                "OpenAPI UI: /swagger\n"
                "Spec: /swagger.json\n"
                "Commands: /commands\n";
            if (!persistent_) {
                body += "Warning: the job database could not be opened; jobs and run history "
                        "are kept in memory and lost on restart\n";
            }
            return HttpResponse::text(status::OK, body);
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
        bool persistent_ = true;
    };

} // namespace Orcha::Agent::Routes
