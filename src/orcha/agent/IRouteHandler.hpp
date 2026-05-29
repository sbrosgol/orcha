//
// IRouteHandler.hpp - Route handler interface
// Created as part of architectural improvements
//

#pragma once

#include <cpprest/http_listener.h>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace Orcha::Agent {

    /**
     * @brief True if @p path begins with @p prefix.
     */
    [[nodiscard]] inline bool path_starts_with(
        const std::string& path, const std::string& prefix) {
        return path.size() >= prefix.size() &&
               path.compare(0, prefix.size(), prefix) == 0;
    }

    /**
     * @brief Split a path into '/'-separated, non-empty segments.
     *
     * "/api/plugins/foo/reload" -> {"api", "plugins", "foo", "reload"}
     */
    [[nodiscard]] inline std::vector<std::string> split_path(const std::string& path) {
        std::vector<std::string> segments;
        std::string current;
        for (char c : path) {
            if (c == '/') {
                if (!current.empty()) {
                    segments.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            segments.push_back(current);
        }
        return segments;
    }

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
         * @brief A middleware inspects a request before handler dispatch.
         * @return True if the middleware handled (e.g. rejected) the request,
         *         short-circuiting further processing. False to continue.
         */
        using Middleware = std::function<bool(web::http::http_request&)>;

        /**
         * @brief Register a route handler.
         */
        void register_handler(std::shared_ptr<IRouteHandler> handler) {
            handlers_.push_back(std::move(handler));
        }

        /**
         * @brief Register a middleware. Middleware run in registration order
         *        before any handler is consulted.
         */
        void use(Middleware middleware) {
            middleware_.push_back(std::move(middleware));
        }

        /**
         * @brief Route a request to the appropriate handler.
         * @param request The HTTP request.
         * @return True if a handler (or middleware) processed the request.
         */
        bool route(web::http::http_request request) {
            // Run middleware first; any one may short-circuit the request.
            for (const auto& mw : middleware_) {
                if (mw(request)) {
                    return true;
                }
            }

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
        std::vector<Middleware> middleware_;
    };

} // namespace Orcha::Agent
