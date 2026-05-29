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

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

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
        std::shared_ptr<Core::IPluginDenylist> denylist)
        : registry_(std::move(registry))
        , engine_(std::move(engine))
        , logger_(std::move(logger))
        , plugins_(std::move(plugins))
        , discovery_(std::move(discovery))
        , admin_(std::move(admin))
        , plugin_dir_(std::move(plugin_dir))
        , watch_interval_(watch_interval)
        , jobs_(std::move(jobs))
        , denylist_(std::move(denylist)) {
        setup_default_routes();
    }

    CommandAgent::~CommandAgent() {
        stop();
    }

    void CommandAgent::setup_default_routes() {
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

    void CommandAgent::start(unsigned short port) {
        const auto uri_string = utility::conversions::to_string_t(
            "http://0.0.0.0:" + std::to_string(port) + "/");
        const uri_builder uri(uri_string);

        listener_ = std::make_unique<http_listener>(uri.to_uri());
        listener_->support([this](const http_request& request) {
            handle_request(request);
        });

        if (logger_) {
            logger_->info("CommandAgent starting on port " + std::to_string(port));
        }

        try {
            listener_->open().wait();
            std::cout << "[Orcha] CommandAgent listening on port " << port << '\n';

            if (logger_) {
                logger_->info("CommandAgent listening on port " + std::to_string(port));
            }
        } catch (const std::exception& ex) {
            std::cerr << "[Orcha] Failed to open HTTP listener on port "
                      << port << ": " << ex.what() << '\n';

            if (logger_) {
                logger_->error(
                    "Failed to open HTTP listener on port " +
                    std::to_string(port) + ": " + ex.what());
            }
            throw;
        }
    }

    void CommandAgent::stop() const {
        if (listener_) {
            listener_->close().wait();
        }
    }

    void CommandAgent::handle_request(http_request request) {
        auto path = request.request_uri().path();
        auto method = request.method();

        if (logger_) {
            logger_->info(
                "Received " + utility::conversions::to_utf8string(method) +
                " request at path: " + utility::conversions::to_utf8string(path));
        }

        // Try to route the request
        if (router_.route(request)) {
            return;
        }

        // No handler found - return 404
        pplx::create_task([request]() {
            http_response resp(status_codes::NotFound);
            resp.headers().add(header_names::content_type, U("application/json"));

            json::value error;
            error[U("error")] = json::value::string(U("Endpoint not found"));
            error[U("path")] = json::value::string(request.request_uri().path());
            resp.set_body(error);

            request.reply(resp);
        });
    }

} // namespace Orcha::Agent
