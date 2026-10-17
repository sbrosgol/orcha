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

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            (void)request;
            if (logger_) {
                logger_->debug("Serving admin dashboard");
            }
            return HttpResponse::html(status::OK,
                                      std::string(Orcha::Agent::kDashboardHtml));
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
