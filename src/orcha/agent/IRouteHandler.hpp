//
// IRouteHandler.hpp - Route handler interface
// Created as part of architectural improvements
//

#pragma once

#include <cpprest/http_listener.h>
#include <string>
#include <vector>
#include <memory>

namespace Orcha::Agent {

    /**
     * @struct RouteInfo
     * @brief Information about a registered route.
     */
    struct RouteInfo {
        std::string method;  // "GET", "POST", etc.
        std::string path;
        std::string description;
    };

    /**
     * @interface IRouteHandler
     * @brief Interface for HTTP route handlers.
     *
     * Each route handler is responsible for a specific endpoint
     * or group of related endpoints.
     */
    class IRouteHandler {
    public:
        virtual ~IRouteHandler() = default;

        /**
         * @brief Check if this handler can handle the given request.
         * @param method HTTP method.
         * @param path Request path.
         * @return True if this handler should process the request.
         */
        [[nodiscard]] virtual bool can_handle(
            const std::string& method,
            const std::string& path) const = 0;

        /**
         * @brief Handle an HTTP request.
         * @param request The HTTP request to handle.
         *
         * The handler is responsible for calling request.reply().
         */
        virtual void handle(web::http::http_request request) = 0;

        /**
         * @brief Get information about routes handled.
         * @return Vector of route information.
         */
        [[nodiscard]] virtual std::vector<RouteInfo> get_routes() const = 0;
    };

    /**
     * @class Router
     * @brief Routes requests to appropriate handlers.
     */
    class Router {
    public:
        /**
         * @brief Register a route handler.
         */
        void register_handler(std::shared_ptr<IRouteHandler> handler) {
            handlers_.push_back(std::move(handler));
        }

        /**
         * @brief Route a request to the appropriate handler.
         * @param request The HTTP request.
         * @return True if a handler was found.
         */
        bool route(web::http::http_request request) {
            auto method = utility::conversions::to_utf8string(request.method());
            auto path = utility::conversions::to_utf8string(request.request_uri().path());

            for (const auto& handler : handlers_) {
                if (handler->can_handle(method, path)) {
                    handler->handle(std::move(request));
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Get all registered routes.
         */
        [[nodiscard]] std::vector<RouteInfo> get_all_routes() const {
            std::vector<RouteInfo> routes;
            for (const auto& handler : handlers_) {
                auto handler_routes = handler->get_routes();
                routes.insert(routes.end(),
                             handler_routes.begin(),
                             handler_routes.end());
            }
            return routes;
        }

    private:
        std::vector<std::shared_ptr<IRouteHandler>> handlers_;
    };

} // namespace Orcha::Agent
