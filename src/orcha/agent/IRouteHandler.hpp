//
// IRouteHandler.hpp - Route handler interface
// Created as part of architectural improvements
//

#pragma once

#include "Http.hpp"
#include <optional>
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
         * @brief Handle an HTTP request and produce a response.
         * @param request The parsed HTTP request.
         * @return The response to send.
         */
        [[nodiscard]] virtual HttpResponse handle(const HttpRequest& request) = 0;

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
         * @return A response to short-circuit processing (e.g. a 401), or
         *         std::nullopt to continue to the next middleware/handler.
         */
        using Middleware =
            std::function<std::optional<HttpResponse>(const HttpRequest&)>;

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
         * @return The handler/middleware response, or std::nullopt if no handler
         *         matched (the caller should then produce a 404).
         */
        [[nodiscard]] std::optional<HttpResponse> route(const HttpRequest& request) {
            // Run middleware first; any one may short-circuit the request.
            for (const auto& mw : middleware_) {
                if (auto resp = mw(request)) {
                    return resp;
                }
            }

            for (const auto& handler : handlers_) {
                if (handler->can_handle(request.method, request.path)) {
                    return handler->handle(request);
                }
            }
            return std::nullopt;
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
