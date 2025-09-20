#include "CommandAgent.hpp"
#include "orcha_pch.hpp"
#include "swagger_embedded.hpp"
#include "../utils/YamlToJson.hpp"
#include <yaml-cpp/yaml.h>
#include <filesystem>

using namespace web;
using namespace web::http;
using namespace web::http::experimental::listener;

namespace Orcha::Agent {

    CommandAgent::CommandAgent(Orcha::Core::CommandRegistry& registry)
        : registry_(registry), runner_(registry) {}

    void CommandAgent::start(unsigned short port) {
        const auto uri_string = utility::conversions::to_string_t("http://0.0.0.0:" + std::to_string(port) + "/");
        const uri_builder uri(uri_string);

        listener_ = std::make_unique<http_listener>(uri.to_uri());
        listener_->support([this](const http_request& request) { handle_request(request); });

        Utils::Logger::info("CommandAgent starting on port " + std::to_string(port));
        try {
            (void)listener_->open().wait();
            std::cout << "[Orcha] CommandAgent listening on port " << port << std::endl;
            Utils::Logger::instance().log(Utils::LogLevel::INFO, 
                "CommandAgent listening on port " + std::to_string(port));
        } catch (const std::exception& ex) {
            std::cerr << "[Orcha] Failed to open HTTP listener on port " << port << ": " << ex.what() << std::endl;
            Utils::Logger::instance().log(Utils::LogLevel::ERROR, 
                std::string("Failed to open HTTP listener on port ") + std::to_string(port) + ": " + ex.what());
            throw;
        }
    }

    void CommandAgent::stop() const {
        if (listener_)
            (void)listener_->close().wait();
    }

    void CommandAgent::handle_request(http_request request) const {
        auto path = request.request_uri().path();
        auto method = request.method();
        Utils::Logger::instance().log(Utils::LogLevel::INFO, 
            "Received " + utility::conversions::to_utf8string(method) + " request at path: " + utility::conversions::to_utf8string(path));

        if (request.method() == methods::GET && (path == U("/") || path.empty())) {
            pplx::create_task([request] {
                http_response resp(status_codes::OK);
                resp.headers().add(header_names::content_type, "text/plain; charset=utf-8");
                resp.set_body("Orcha Command Agent is running.\nPOST your workflow JSON (application/json) to /workflow\nOpenAPI UI: /swagger\nSpec: /swagger.json\n");
                (void)request.reply(resp);
                Utils::Logger::instance().log(Utils::LogLevel::DEBUG, "Health check endpoint hit.");
            }).then([](const pplx::task<void>& t) { 
                try { 
                    t.get(); 
                } catch (...) {} 
            });
        } else if (request.method() == methods::GET && path == U("/swagger.json")) {
            // Serve minimal OpenAPI (Swagger) spec
            pplx::create_task([request] {
                using web::json::value;
                // servers
                auto servers = web::json::value::array(1);
                value server0 = value::object();
                server0[U("url")] = value::string(U("/"));
                servers[0] = server0;

                // paths object
                value paths = value::object();

                // GET /
                {
                    value getRoot = value::object();
                    value getOp = value::object();
                    getOp[U("summary")] = value::string(U("Health check"));

                    web::json::value responses = value::object();
                    web::json::value resp200 = value::object();
                    resp200[U("description")] = value::string(U("OK"));
                    web::json::value content = value::object();
                    web::json::value textPlain = value::object();
                    web::json::value schema = value::object();
                    schema[U("type")] = value::string(U("string"));
                    textPlain[U("schema")] = schema;
                    content[U("text/plain")] = textPlain;
                    resp200[U("content")] = content;
                    responses[U("200")] = resp200;
                    getOp[U("responses")] = responses;

                    getRoot[U("get")] = getOp;
                    paths[U("/")] = getRoot;
                }

                // POST /workflow
                {
                    web::json::value postWf = value::object();
                    web::json::value postOp = value::object();
                    postOp[U("summary")] = value::string(U("Run a workflow"));

                    // requestBody
                    web::json::value requestBody = value::object();
                    requestBody[U("required")] = value::boolean(true);
                    web::json::value content = value::object();
                    // application/json
                    {
                        web::json::value c = value::object();
                        web::json::value schema = value::object();
                        schema[U("type")] = value::string(U("object"));
                        c[U("schema")] = schema;
                        // Attach example converted from the sample YAML file if available
                        try {
                            namespace fs = std::filesystem;
                            std::string p1 = "sample_workflows/test-flow.yaml";
                            std::string p2 = "../sample_workflows/test-flow.yaml";
                            std::string use = fs::exists(p1) ? p1 : (fs::exists(p2) ? p2 : "");
                            if (!use.empty()) {
                                YAML::Node yaml = YAML::LoadFile(use);
                                web::json::value ex = Orcha::Utils::yaml_to_json(yaml);
                                c[U("example")] = ex;
                            }
                        } catch (...) {
                            // ignore example load errors
                        }
                        content[U("application/json")] = c;
                    }
                    requestBody[U("content")] = content;
                    postOp[U("requestBody")] = requestBody;

                    // responses
                    web::json::value responses = value::object();
                    web::json::value resp200 = value::object();
                    resp200[U("description")] = value::string(U("Workflow result"));
                    web::json::value outContent = value::object();
                    web::json::value appJson = value::object();

                    // Schema: array of step result objects
                    web::json::value outSchema = value::object();
                    outSchema[U("type")] = value::string(U("array"));
                    web::json::value itemSchema = value::object();
                    itemSchema[U("type")] = value::string(U("object"));
                    web::json::value props = value::object();
                    {
                        web::json::value p = value::object();
                        p[U("type")] = value::string(U("boolean"));
                        props[U("success")] = p;
                    }
                    {
                        web::json::value p = value::object();
                        p[U("type")] = value::string(U("string"));
                        props[U("error_message")] = p;
                    }
                    {
                        web::json::value p = value::object();
                        p[U("type")] = value::string(U("object"));
                        props[U("output")] = p;
                    }
                    itemSchema[U("properties")] = props;
                    outSchema[U("items")] = itemSchema;
                    appJson[U("schema")] = outSchema;

                    // Example: provided sample output
                    web::json::value exampleArr = value::array(3);
                    {
                        web::json::value obj = value::object();
                        obj[U("error_message")] = value::string(U(""));
                        obj[U("success")] = value::boolean(true);
                        web::json::value output = value::object();
                        output[U("echoed")] = value::string(U("Starting PowerShell Core download..."));
                        obj[U("output")] = output;
                        exampleArr[0] = obj;
                    }
                    {
                        web::json::value obj = value::object();
                        obj[U("error_message")] = value::string(U(""));
                        obj[U("success")] = value::boolean(true);
                        web::json::value output = value::object();
                        output[U("path")] = value::string(U("pwsh/pwsh"));
                        output[U("success")] = value::boolean(true);
                        obj[U("output")] = output;
                        exampleArr[1] = obj;
                    }
                    {
                        web::json::value obj = value::object();
                        obj[U("error_message")] = value::string(U(""));
                        obj[U("success")] = value::boolean(true);
                        web::json::value output = value::object();
                        output[U("echoed")] = value::string(U("PowerShell was downloaded to pwsh/pwsh"));
                        obj[U("output")] = output;
                        exampleArr[2] = obj;
                    }
                    appJson[U("example")] = exampleArr;

                    outContent[U("application/json")] = appJson;
                    resp200[U("content")] = outContent;
                    responses[U("200")] = resp200;
                    postOp[U("responses")] = responses;

                    postWf[U("post")] = postOp;
                    paths[U("/workflow")] = postWf;
                }

                // root spec
                web::json::value spec = value::object();
                spec[U("openapi")] = value::string(U("3.0.1"));
                web::json::value info = value::object();
                info[U("title")] = value::string(U("Orcha API"));
                info[U("version")] = value::string(U("1.0.0"));
                spec[U("info")] = info;
                spec[U("servers")] = servers;
                spec[U("paths")] = paths;

                http_response resp(status_codes::OK);
                resp.headers().add(header_names::content_type, U("application/json"));
                resp.set_body(spec);
                (void)request.reply(resp);
            }).then([](const pplx::task<void>& t) {
                try { t.get(); } catch (...) {}
            });
        } else if (request.method() == methods::GET && path == U("/swagger")) {
            // Serve Swagger UI HTML from embedded file
            pplx::create_task([request] {
                http_response resp(status_codes::OK);
                resp.headers().add(header_names::content_type, U("text/html; charset=utf-8"));
                resp.set_body(Orcha::Agent::kSwaggerHtml);
                (void)request.reply(resp);
            }).then([](const pplx::task<void>& t) {
                try { t.get(); } catch (...) {}
            });
        } else if (request.method() == methods::POST && path == U("/workflow")) {
            if (auto content_type = request.headers().content_type(); content_type == U("application/json")) {
                request.extract_json().then([this, request](const web::json::value& json) {
                    runner_.run_and_report_json(json)
                        .then([request](const web::json::value& result) {
                            (void)request.reply(status_codes::OK, result);
                        });
                });
            } else {
                // Enforce JSON-only payloads
                pplx::create_task([request] {
                    http_response resp(status_codes::UnsupportedMediaType);
                    resp.headers().add(header_names::content_type, U("application/json"));
                    web::json::value msg = web::json::value::object();
                    msg[U("error")] = web::json::value::string(U("Only application/json is accepted for /workflow"));
                    msg[U("hint")] = web::json::value::string(U("See /swagger for API documentation and a sample payload"));
                    resp.set_body(msg);
                    (void)request.reply(resp);
                }).then([](const pplx::task<void>& t) { try { t.get(); } catch (...) {} });
            }
        } else {
            pplx::create_task([request] {
                request.reply(status_codes::NotFound, "Endpoint not found.");
            }).then([](const pplx::task<void>& t) { 
                try { 
                    t.get(); 
                } catch (...) {} 
            });
        }
    }

} // namespace Orcha::Agent
