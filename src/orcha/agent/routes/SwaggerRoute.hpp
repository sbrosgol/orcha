//
// SwaggerRoute.hpp - Swagger/OpenAPI endpoint handlers
// Created as part of architectural improvements
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../swagger_embedded.hpp"
#include "../../core/ICommandRegistry.hpp"
#include "../../utils/ILogger.hpp"

namespace Orcha::Agent::Routes {

    /**
     * @class SwaggerRoute
     * @brief Handles Swagger UI and OpenAPI spec endpoints.
     */
    class SwaggerRoute : public IRouteHandler {
    public:
        SwaggerRoute(std::shared_ptr<Core::ICommandRegistry> registry,
                    std::shared_ptr<Utils::ILogger> logger = nullptr)
            : registry_(std::move(registry))
            , logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            if (method != "GET") return false;
            return path == "/swagger" ||
                   path == "/swagger.json" ||
                   path == "/sample" ||
                   path == "/commands";
        }

        void handle(web::http::http_request request) override {
            auto path = utility::conversions::to_utf8string(request.request_uri().path());

            if (path == "/swagger") {
                serve_swagger_ui(request);
            } else if (path == "/swagger.json") {
                serve_openapi_spec(request);
            } else if (path == "/sample") {
                serve_sample_workflow(request);
            } else if (path == "/commands") {
                serve_commands_list(request);
            }
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "GET", .path = "/swagger", .description = "Swagger UI"},
                {.method = "GET", .path = "/swagger.json", .description = "OpenAPI specification"},
                {.method = "GET", .path = "/sample", .description = "Sample workflow payload"},
                {.method = "GET", .path = "/commands", .description = "List available commands"}
            };
        }

    private:
        void serve_swagger_ui(web::http::http_request request) {
            pplx::create_task([request]() {
                web::http::http_response resp(web::http::status_codes::OK);
                resp.headers().add(
                    web::http::header_names::content_type,
                    U("text/html; charset=utf-8"));
                resp.set_body(Orcha::Agent::kSwaggerHtml);
                request.reply(resp);
            });
        }

        void serve_openapi_spec(web::http::http_request request) {
            auto registry = registry_;

            pplx::create_task([request, registry]() {
                auto spec = generate_openapi_spec(registry);
                web::http::http_response resp(web::http::status_codes::OK);
                resp.headers().add(
                    web::http::header_names::content_type,
                    U("application/json"));
                resp.set_body(spec);
                request.reply(resp);
            });
        }

        void serve_sample_workflow(web::http::http_request request) {
            pplx::create_task([request]() {
                web::json::value sample = web::json::value::object();
                web::json::value steps = web::json::value::array(1);

                web::json::value step0 = web::json::value::object();
                step0[U("command")] = web::json::value::string(U("echo"));
                web::json::value params = web::json::value::object();
                params[U("message")] = web::json::value::string(U("Hello, Orcha!"));
                step0[U("params")] = params;
                steps[0] = step0;

                sample[U("steps")] = steps;

                web::http::http_response resp(web::http::status_codes::OK);
                resp.headers().add(
                    web::http::header_names::content_type,
                    U("application/json"));
                resp.set_body(sample);
                request.reply(resp);
            });
        }

        void serve_commands_list(web::http::http_request request) {
            auto registry = registry_;

            pplx::create_task([request, registry]() {
                web::json::value result = web::json::value::object();

                auto commands = registry->list_commands();
                web::json::value arr = web::json::value::array(commands.size());

                for (size_t i = 0; i < commands.size(); ++i) {
                    const auto& name = commands[i];
                    web::json::value cmd_info = web::json::value::object();
                    cmd_info[U("name")] = web::json::value::string(name);

                    // Get metadata if available
                    if (auto* cmd = registry->get_command(name)) {
                        auto meta = cmd->metadata();
                        cmd_info[U("version")] = web::json::value::string(meta.version);
                        cmd_info[U("description")] = web::json::value::string(meta.description);

                        if (!meta.parameters.empty()) {
                            web::json::value params = web::json::value::array(meta.parameters.size());
                            for (size_t j = 0; j < meta.parameters.size(); ++j) {
                                params[j] = meta.parameters[j].to_json();
                            }
                            cmd_info[U("parameters")] = params;
                        }
                    }

                    arr[i] = cmd_info;
                }

                result[U("commands")] = arr;
                result[U("count")] = web::json::value::number(static_cast<int>(commands.size()));

                web::http::http_response resp(web::http::status_codes::OK);
                resp.headers().add(
                    web::http::header_names::content_type,
                    U("application/json"));
                resp.set_body(result);
                request.reply(resp);
            });
        }

        static web::json::value generate_openapi_spec(
            std::shared_ptr<Core::ICommandRegistry> registry) {

            using web::json::value;

            value spec = value::object();
            spec[U("openapi")] = value::string(U("3.0.1"));

            // Info
            value info = value::object();
            info[U("title")] = value::string(U("Orcha API"));
            info[U("version")] = value::string(U("2.0.0"));
            info[U("description")] = value::string(
                U("Orcha Command Orchestration API with plugin support"));
            spec[U("info")] = info;

            // Servers
            value servers = value::array(1);
            value server0 = value::object();
            server0[U("url")] = value::string(U("/"));
            servers[0] = server0;
            spec[U("servers")] = servers;

            // Paths
            value paths = value::object();

            // GET /
            paths[U("/")] = create_health_path();

            // POST /workflow
            paths[U("/workflow")] = create_workflow_path();

            // GET /commands
            paths[U("/commands")] = create_commands_path(registry);

            // GET /sample
            paths[U("/sample")] = create_sample_path();

            spec[U("paths")] = paths;

            return spec;
        }

        static web::json::value create_health_path() {
            using web::json::value;

            value path = value::object();
            value get_op = value::object();
            get_op[U("summary")] = value::string(U("Health check"));
            get_op[U("tags")] = value::array(1);
            get_op[U("tags")][0] = value::string(U("System"));

            value responses = value::object();
            value resp200 = value::object();
            resp200[U("description")] = value::string(U("OK"));
            responses[U("200")] = resp200;
            get_op[U("responses")] = responses;

            path[U("get")] = get_op;
            return path;
        }

        static web::json::value create_workflow_path() {
            using web::json::value;

            value path = value::object();
            value post_op = value::object();
            post_op[U("summary")] = value::string(U("Execute a workflow"));
            post_op[U("tags")] = value::array(1);
            post_op[U("tags")][0] = value::string(U("Workflow"));

            // Request body
            value request_body = value::object();
            request_body[U("required")] = value::boolean(true);
            value content = value::object();
            value app_json = value::object();
            value schema = value::object();
            schema[U("type")] = value::string(U("object"));
            app_json[U("schema")] = schema;
            content[U("application/json")] = app_json;
            request_body[U("content")] = content;
            post_op[U("requestBody")] = request_body;

            // Responses
            value responses = value::object();
            value resp200 = value::object();
            resp200[U("description")] = value::string(U("Workflow result"));
            responses[U("200")] = resp200;
            post_op[U("responses")] = responses;

            path[U("post")] = post_op;
            return path;
        }

        static web::json::value create_commands_path(
            std::shared_ptr<Core::ICommandRegistry> registry) {

            using web::json::value;

            value path = value::object();
            value get_op = value::object();
            get_op[U("summary")] = value::string(U("List available commands"));
            get_op[U("tags")] = value::array(1);
            get_op[U("tags")][0] = value::string(U("Commands"));

            value responses = value::object();
            value resp200 = value::object();
            resp200[U("description")] = value::string(U("List of commands"));
            responses[U("200")] = resp200;
            get_op[U("responses")] = responses;

            path[U("get")] = get_op;
            return path;
        }

        static web::json::value create_sample_path() {
            using web::json::value;

            value path = value::object();
            value get_op = value::object();
            get_op[U("summary")] = value::string(U("Get sample workflow"));
            get_op[U("tags")] = value::array(1);
            get_op[U("tags")][0] = value::string(U("Workflow"));

            value responses = value::object();
            value resp200 = value::object();
            resp200[U("description")] = value::string(U("Sample workflow payload"));
            responses[U("200")] = resp200;
            get_op[U("responses")] = responses;

            path[U("get")] = get_op;
            return path;
        }

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
