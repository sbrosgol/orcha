//
// DashboardRoute.hpp - Serves the embedded admin dashboard at /admin
// Part of the admin dashboard (Phase 1).
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../dashboard_embedded.hpp"
#include "../../utils/ILogger.hpp"

namespace Orcha::Agent::Routes {

    /**
     * @class DashboardRoute
     * @brief Serves the embedded admin UI (GET /admin).
     */
    class DashboardRoute : public IRouteHandler {
    public:
        explicit DashboardRoute(std::shared_ptr<Utils::ILogger> logger = nullptr)
            : logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            return method == "GET" && (path == "/admin" || path == "/admin/");
        }

        void handle(web::http::http_request request) override {
            if (logger_) {
                logger_->debug("Serving admin dashboard");
            }
            pplx::create_task([request]() {
                web::http::http_response resp(web::http::status_codes::OK);
                resp.headers().add(
                    web::http::header_names::content_type,
                    U("text/html; charset=utf-8"));
                resp.set_body(Orcha::Agent::kDashboardHtml);
                request.reply(resp);
            });
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {{
                .method = "GET",
                .path = "/admin",
                .description = "Admin dashboard UI"
            }};
        }

    private:
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
