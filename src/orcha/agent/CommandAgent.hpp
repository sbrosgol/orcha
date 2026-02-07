//
// CommandAgent.hpp - HTTP agent with pluggable routes
// Updated to use route handlers
//

#pragma once

#include "IRouteHandler.hpp"
#include "../core/ICommandRegistry.hpp"
#include "../workflow/IWorkflowEngine.hpp"
#include "../workflow/WorkflowEngine.hpp"
#include "../utils/ILogger.hpp"
#include <cpprest/http_listener.h>
#include <memory>

namespace Orcha::Agent {

    /**
     * @class CommandAgent
     * @brief HTTP server exposing command and workflow execution.
     *
     * Uses pluggable route handlers for endpoint management.
     */
    class CommandAgent {
    public:
        /**
         * @brief Construct with full dependency injection.
         */
        CommandAgent(std::shared_ptr<Core::ICommandRegistry> registry,
                    std::shared_ptr<Workflow::IWorkflowEngine> engine,
                    std::shared_ptr<Utils::ILogger> logger = nullptr);

        ~CommandAgent();

        // Prevent copying
        CommandAgent(const CommandAgent&) = delete;
        CommandAgent& operator=(const CommandAgent&) = delete;

        /**
         * @brief Start the HTTP server.
         * @param port Port to listen on.
         */
        void start(unsigned short port);

        /**
         * @brief Stop the HTTP server.
         */
        void stop() const;

        /**
         * @brief Register a custom route handler.
         */
        void add_route(std::shared_ptr<IRouteHandler> handler);

        /**
         * @brief Get the router for external configuration.
         */
        Router& router() { return router_; }

    private:
        void handle_request(web::http::http_request request);
        void setup_default_routes();

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<Workflow::IWorkflowEngine> engine_;
        std::shared_ptr<Utils::ILogger> logger_;

        Router router_;
        std::unique_ptr<web::http::experimental::listener::http_listener> listener_;
    };

} // namespace Orcha::Agent
