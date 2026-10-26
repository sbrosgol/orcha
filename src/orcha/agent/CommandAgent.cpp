//
// CommandAgent.cpp - HTTP agent implementation
// Updated to use route handlers
//

#include "CommandAgent.hpp"
#include "routes/HealthRoute.hpp"
#include "routes/WorkflowRoute.hpp"
#include "routes/SwaggerRoute.hpp"
#include "routes/PluginAdminRoute.hpp"
#include "routes/DashboardRoute.hpp"
#include "routes/JobRoute.hpp"
#include "AuthMiddleware.hpp"
#include "HttpJson.hpp"

#include <iostream>

namespace Orcha::Agent {

    CommandAgent::CommandAgent(
        std::shared_ptr<Core::ICommandRegistry> registry,
        std::shared_ptr<Workflow::IWorkflowEngine> engine,
        std::shared_ptr<Utils::ILogger> logger,
        std::shared_ptr<Core::PluginManager> plugins,
        std::shared_ptr<Core::IPluginDiscovery> discovery,
        Config::AdminConfig admin,
        std::string plugin_dir,
        std::chrono::milliseconds watch_interval,
        std::shared_ptr<Jobs::JobService> jobs,
        std::shared_ptr<Core::IPluginDenylist> denylist,
        Config::ServerConfig server)
        : registry_(std::move(registry))
        , engine_(std::move(engine))
        , logger_(std::move(logger))
        , plugins_(std::move(plugins))
        , discovery_(std::move(discovery))
        , admin_(std::move(admin))
        , plugin_dir_(std::move(plugin_dir))
        , watch_interval_(watch_interval)
        , jobs_(std::move(jobs))
        , denylist_(std::move(denylist))
        , server_config_(std::move(server)) {
        setup_default_routes();
    }

    CommandAgent::~CommandAgent() {
        stop();
    }

    void CommandAgent::setup_default_routes() {
        // The execution API runs arbitrary commands (e.g. shell_exec), so it is
        // never served unauthenticated on a reachable interface.
        router_.use(make_api_auth(server_config_, admin_, {"/workflow"}, logger_));
        if (logger_) {
            if (!server_config_.api_token.empty()) {
                logger_->info("POST /workflow requires a bearer token (server.api_token)");
            } else if (server_config_.is_loopback()) {
                logger_->info("POST /workflow is open to local clients only (bound to " +
                              server_config_.host + ")");
            } else {
                logger_->warn("POST /workflow is DISABLED: bound to " + server_config_.host +
                              " without server.api_token (set ORCHA_SERVER_API_TOKEN)");
            }
        }

        // Health check
        router_.register_handler(
            std::make_shared<Routes::HealthRoute>(logger_));

        // Workflow execution (records each ad-hoc run when a job service is present)
        router_.register_handler(
            std::make_shared<Routes::WorkflowRoute>(engine_, logger_, jobs_));

        // Swagger and commands
        router_.register_handler(
            std::make_shared<Routes::SwaggerRoute>(registry_, logger_));

        setup_admin_routes();
    }

    void CommandAgent::setup_admin_routes() {
        // Admin dashboard requires a plugin manager + discovery to be useful.
        if (!plugins_ || !discovery_) {
            return;
        }

        if (!admin_.enabled) {
            if (logger_) {
                logger_->info("Admin dashboard disabled by configuration");
            }
            return;
        }

        // Fail closed: never expose config-mutating endpoints unauthenticated.
        if (!admin_.is_serviceable()) {
            if (logger_) {
                logger_->warn(
                    "Admin dashboard NOT started: auth is required but no "
                    "admin.password is configured (set admin.password or "
                    "ORCHA_ADMIN_PASSWORD, or set admin.auth_required=false).");
            }
            return;
        }

        // Guard the admin API with Basic auth (no-op when auth_required=false).
        // The /admin HTML shell is intentionally NOT gated: it carries no data
        // and renders its own custom login view, which then authenticates the
        // /api/* calls. This avoids the browser's native Basic-auth dialog.
        router_.use(make_basic_auth(admin_, {"/api/"}, logger_));

        router_.register_handler(std::make_shared<Routes::PluginAdminRoute>(
            plugins_, discovery_, registry_, plugin_dir_, watch_interval_, logger_, denylist_));
        router_.register_handler(std::make_shared<Routes::DashboardRoute>(logger_));

        // Jobs API (CRUD + run history). Gated by the same Basic-auth middleware.
        if (jobs_) {
            router_.register_handler(std::make_shared<Routes::JobRoute>(jobs_, logger_));
        }

        if (logger_) {
            logger_->info(std::string("Admin dashboard enabled at /admin (auth ") +
                          (admin_.auth_required ? "required" : "disabled") + ")");
        }
    }

    void CommandAgent::add_route(std::shared_ptr<IRouteHandler> handler) {
        router_.register_handler(std::move(handler));
    }

    void CommandAgent::start() {
        const auto port = server_config_.port;
        server_ = std::make_unique<HttpServer>(
            [this](const HttpRequest& request) { return handle_request(request); },
            logger_,
            static_cast<unsigned int>(server_config_.worker_threads));

        if (logger_) {
            logger_->info("CommandAgent starting on port " + std::to_string(port));
        }

        try {
            server_->start(server_config_.host, port);
            std::cout << "[Orcha] CommandAgent listening on " << server_config_.host
                      << ":" << port << '\n';

            if (logger_) {
                logger_->info("CommandAgent listening on " + server_config_.host + ":" +
                              std::to_string(port));
            }
        } catch (const std::exception& ex) {
            std::cerr << "[Orcha] Failed to start HTTP server on "
                      << server_config_.host << ":" << port << ": " << ex.what() << '\n';

            if (logger_) {
                logger_->error(
                    "Failed to start HTTP server on " + server_config_.host + ":" +
                    std::to_string(port) + ": " + ex.what());
            }
            throw;
        }
    }

    void CommandAgent::stop() const {
        if (server_) {
            server_->stop();
        }
    }

    HttpResponse CommandAgent::handle_request(const HttpRequest& request) {
        // Note: the `GET //admin` leading-slash defense now lives in the server's
        // target parsing (HttpServer.cpp), so request.path is already normalized.
        if (logger_) {
            logger_->info("Received " + request.method +
                          " request at path: " + request.path);
        }

        if (auto resp = router_.route(request)) {
            return *resp;
        }

        // No handler found - return 404.
        Orcha::Json error = Orcha::Json::object();
        error["error"] = "Endpoint not found";
        error["path"] = request.path;
        return reply_json(status::NotFound, error);
    }

} // namespace Orcha::Agent
