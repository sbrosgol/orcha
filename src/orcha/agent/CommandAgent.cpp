//
// CommandAgent.cpp - HTTP agent implementation
// Updated to use route handlers
//

#include "CommandAgent.hpp"
#include "routes/HealthRoute.hpp"
#include "routes/WorkflowRoute.hpp"
#include "routes/SwaggerRoute.hpp"
#include "routes/PluginAdminRoute.hpp"
#include "routes/PluginUploadRoute.hpp"
#include "routes/DashboardRoute.hpp"
#include "routes/JobRoute.hpp"
#include "routes/DefinitionRoute.hpp"
#include "routes/CircuitBreakerRoute.hpp"
#include "routes/SetupRoute.hpp"
#include "AuthMiddleware.hpp"
#include "HttpJson.hpp"

#include <iostream>

namespace Orcha::Agent {

    CommandAgent::CommandAgent(
        std::shared_ptr<Core::ICommandRegistry> registry,
        std::shared_ptr<Jobs::RunQueue> runs,
        std::shared_ptr<Utils::ILogger> logger,
        std::shared_ptr<Core::PluginManager> plugins,
        std::shared_ptr<Core::IPluginDiscovery> discovery,
        Config::AdminConfig admin,
        std::string plugin_dir,
        std::chrono::milliseconds watch_interval,
        std::shared_ptr<Jobs::JobService> jobs,
        std::shared_ptr<Core::IPluginDenylist> denylist,
        Config::ServerConfig server,
        std::shared_ptr<Core::CircuitBreakerRegistry> circuit_breakers,
        std::shared_ptr<Core::PluginInstaller> plugin_installer)
        : registry_(std::move(registry))
        , runs_(std::move(runs))
        , logger_(std::move(logger))
        , plugins_(std::move(plugins))
        , discovery_(std::move(discovery))
        , admin_(std::move(admin))
        , plugin_dir_(std::move(plugin_dir))
        , watch_interval_(watch_interval)
        , jobs_(std::move(jobs))
        , denylist_(std::move(denylist))
        , server_config_(std::move(server))
        , circuit_breakers_(std::move(circuit_breakers))
        , plugin_installer_(std::move(plugin_installer))
        , admin_account_(std::make_shared<AdminAccount>(admin_, !server_config_.is_loopback(), logger_)) {
        setup_default_routes();
    }

    CommandAgent::~CommandAgent() {
        stop();
    }

    void CommandAgent::setup_default_routes() {
        // The execution API runs arbitrary commands (e.g. shell_exec), so it is
        // never served unauthenticated on a reachable interface.
        router_.use(make_api_auth(server_config_, admin_account_, {"/workflow"}, logger_));
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
            std::make_shared<Routes::HealthRoute>(
                logger_, !jobs_ || jobs_->store()->persistent()));

        // Workflow execution API: runs are queued and recorded by the run queue
        router_.register_handler(
            std::make_shared<Routes::WorkflowRoute>(runs_, logger_, Routes::WorkflowWaitOptions{
                std::chrono::milliseconds(server_config_.max_wait_ms),
                // Leave at least one HTTP worker free for status polls and admin
                // (with a single worker, nobody may wait).
                server_config_.worker_threads > 1 ? server_config_.worker_threads - 1 : 0}));

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

        // Settings ignored on an exposed bind, or why the surface is off.
        const auto mode = admin_account_->mode();
        if (logger_) {
            for (const auto& note : admin_account_->notes()) {
                if (mode == AdminAccount::Mode::Disabled) logger_->error("Admin dashboard: " + note);
                else logger_->warn("Admin dashboard: " + note);
            }
        }
        // Fail closed: never expose config-mutating endpoints unauthenticated.
        if (mode == AdminAccount::Mode::Disabled) {
            if (logger_) logger_->warn("Admin dashboard NOT started (see above)");
            return;
        }

        // Admin login on /api/* (not on /api/setup; no-op when Open). The /admin
        // HTML shell is intentionally NOT gated: it carries no data and renders
        // its own login and setup views, which then authenticate the /api/*
        // calls. This avoids the browser's native Basic-auth dialog.
        router_.use(make_basic_auth(admin_account_, {"/api/"}, logger_));
        router_.register_handler(std::make_shared<Routes::SetupRoute>(admin_account_, logger_));

        // Uploads first: PluginAdminRoute matches every /api/plugins/* path.
        if (plugin_installer_) {
            router_.register_handler(std::make_shared<Routes::PluginUploadRoute>(plugin_installer_));
        }
        router_.register_handler(std::make_shared<Routes::PluginAdminRoute>(
            plugins_, discovery_, registry_, plugin_dir_, watch_interval_, logger_, denylist_,
            plugin_installer_ ? plugin_installer_->lifecycle_mutex() : nullptr,
            plugin_installer_ ? plugin_installer_->upload_directory() : std::filesystem::path{}));
        router_.register_handler(std::make_shared<Routes::DashboardRoute>(logger_));
        // JSON <-> YAML for the job editor's YAML tab (stateless).
        router_.register_handler(std::make_shared<Routes::DefinitionRoute>());

        // Jobs API (CRUD + run history). Gated by the same Basic-auth middleware.
        if (jobs_) {
            router_.register_handler(std::make_shared<Routes::JobRoute>(jobs_, runs_, logger_));
        }
        if (circuit_breakers_) {
            router_.register_handler(
                std::make_shared<Routes::CircuitBreakerRoute>(circuit_breakers_, logger_));
        }

        if (logger_) {
            logger_->info(std::string("Admin dashboard enabled at /admin (login: ") +
                          to_string(mode) + ")");
            if (mode == AdminAccount::Mode::Setup) {
                const std::string where = server_config_.host + ":" + std::to_string(server_config_.port);
                if (admin_account_->exposed()) {
                    logger_->warn("ADMIN ACCOUNT NOT SET UP: anyone who can reach " + where +
                                  " can open /admin and become admin (which can run commands on this host). "
                                  "Open /admin now to create the account, or set ORCHA_ADMIN_PASSWORD.");
                } else {
                    logger_->warn("Admin account not set up: open /admin on " + where +
                                  " to create it (or set ORCHA_ADMIN_PASSWORD).");
                }
            }
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

    void CommandAgent::stop() {
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
