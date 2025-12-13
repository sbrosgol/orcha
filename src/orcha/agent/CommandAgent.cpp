//
// CommandAgent.cpp - HTTP agent implementation
// Updated to use route handlers
//

#include "CommandAgent.hpp"
#include "routes/HealthRoute.hpp"
#include "routes/WorkflowRoute.hpp"
#include "routes/SwaggerRoute.hpp"
#include "../workflow/WorkflowRunner.hpp"

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

namespace Orcha::Agent {

    // Adapter for legacy constructor
    class LegacyRegistryAdapter : public Core::ICommandRegistry {
    public:
        explicit LegacyRegistryAdapter(Core::CommandRegistry& registry)
            : registry_(registry) {}

        [[nodiscard]] Core::ICommand* get_command(const std::string& name) const override {
            return registry_.get_command(name);
        }

        [[nodiscard]] std::vector<std::string> list_commands() const override {
            return registry_.list_commands();
        }

        [[nodiscard]] bool has_command(const std::string& name) const override {
            return registry_.has_command(name);
        }

        [[nodiscard]] size_t command_count() const override {
            return registry_.command_count();
        }

    private:
        Core::CommandRegistry& registry_;
    };

    CommandAgent::CommandAgent(
        std::shared_ptr<Core::ICommandRegistry> registry,
        std::shared_ptr<Workflow::IWorkflowEngine> engine,
        std::shared_ptr<Utils::ILogger> logger)
        : registry_(std::move(registry))
        , engine_(std::move(engine))
        , logger_(std::move(logger)) {
        setup_default_routes();
    }

    CommandAgent::CommandAgent(Core::CommandRegistry& registry)
        : registry_(std::make_shared<LegacyRegistryAdapter>(registry))
        , engine_(std::make_shared<Workflow::WorkflowEngine>(registry_))
        , logger_(nullptr) {
        setup_default_routes();
    }

    CommandAgent::~CommandAgent() {
        stop();
    }

    void CommandAgent::setup_default_routes() {
        // Health check
        router_.register_handler(
            std::make_shared<Routes::HealthRoute>(logger_));

        // Workflow execution
        router_.register_handler(
            std::make_shared<Routes::WorkflowRoute>(engine_, logger_));

        // Swagger and commands
        router_.register_handler(
            std::make_shared<Routes::SwaggerRoute>(registry_, logger_));
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
