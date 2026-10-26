//
// CommandAgent.hpp - HTTP agent with pluggable routes
// Updated to use route handlers
//

#pragma once

#include "IRouteHandler.hpp"
#include "HttpServer.hpp"
#include "../core/ICommandRegistry.hpp"
#include "../core/PluginManager.hpp"
#include "../core/IPluginDiscovery.hpp"
#include "../core/PluginDenylist.hpp"
#include "../core/CircuitBreaker.hpp"
#include "../workflow/IWorkflowEngine.hpp"
#include "../workflow/WorkflowEngine.hpp"
#include "../jobs/JobService.hpp"
#include "../utils/ILogger.hpp"
#include "../config/AdminConfig.hpp"
#include "../config/ServerConfig.hpp"
#include <chrono>
#include <memory>
#include <string>

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
         *
         * The trailing parameters enable the admin dashboard. When @p plugins
         * is null or @p admin is not serviceable (disabled, or auth required
         * with no password), the admin routes and auth middleware are skipped.
         *
         * @param registry        Command registry (read side).
         * @param engine          Workflow engine.
         * @param logger          Optional logger.
         * @param plugins         Plugin manager (enables /api/plugins + /admin).
         * @param discovery       Plugin discovery (lists available-on-disk plugins).
         * @param admin           Admin dashboard configuration.
         * @param plugin_dir      Plugin directory (for enable/watch).
         * @param watch_interval  Directory scan interval used when enabling the watcher.
         * @param jobs            Job service (enables /api/jobs and run recording).
         * @param denylist        Persistent plugin denylist.
         * @param server          Bind address, worker pool size and the API token
         *                        guarding POST /workflow (fail closed).
         * @param circuit_breakers Shared per-command breakers (enables
         *                        /api/circuit-breakers).
         */
        CommandAgent(std::shared_ptr<Core::ICommandRegistry> registry,
                    std::shared_ptr<Workflow::IWorkflowEngine> engine,
                    std::shared_ptr<Utils::ILogger> logger = nullptr,
                    std::shared_ptr<Core::PluginManager> plugins = nullptr,
                    std::shared_ptr<Core::IPluginDiscovery> discovery = nullptr,
                    Config::AdminConfig admin = {},
                    std::string plugin_dir = "./commands",
                    std::chrono::milliseconds watch_interval = std::chrono::milliseconds(5000),
                    std::shared_ptr<Jobs::JobService> jobs = nullptr,
                    std::shared_ptr<Core::IPluginDenylist> denylist = nullptr,
                    Config::ServerConfig server = {},
                    std::shared_ptr<Core::CircuitBreakerRegistry> circuit_breakers = nullptr);

        ~CommandAgent();

        // Prevent copying
        CommandAgent(const CommandAgent&) = delete;
        CommandAgent& operator=(const CommandAgent&) = delete;

        /**
         * @brief Start the HTTP server on the configured host and port.
         * @throws std::runtime_error if the address cannot be bound.
         */
        void start();

        /**
         * @brief Stop the HTTP server.
         */
        void stop();

        /**
         * @brief Register a custom route handler.
         */
        void add_route(std::shared_ptr<IRouteHandler> handler);

        /**
         * @brief Get the router for external configuration.
         */
        Router& router() { return router_; }

    private:
        HttpResponse handle_request(const HttpRequest& request);
        void setup_default_routes();
        void setup_admin_routes();

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<Workflow::IWorkflowEngine> engine_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::shared_ptr<Core::PluginManager> plugins_;
        std::shared_ptr<Core::IPluginDiscovery> discovery_;
        Config::AdminConfig admin_;
        std::string plugin_dir_;
        std::chrono::milliseconds watch_interval_;
        std::shared_ptr<Jobs::JobService> jobs_;
        std::shared_ptr<Core::IPluginDenylist> denylist_;
        Config::ServerConfig server_config_;
        std::shared_ptr<Core::CircuitBreakerRegistry> circuit_breakers_;

        Router router_;
        std::unique_ptr<HttpServer> server_;
    };

} // namespace Orcha::Agent
