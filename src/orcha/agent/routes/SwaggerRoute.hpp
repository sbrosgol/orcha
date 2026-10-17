//
// SwaggerRoute.hpp - Swagger/OpenAPI endpoint handlers
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../HttpJson.hpp"
#include "../swagger_embedded.hpp"
#include "../../core/ICommandRegistry.hpp"
#include "../../utils/ILogger.hpp"
#include "core/Version.hpp"

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

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            const std::string& path = request.path;

            if (path == "/swagger") {
                return serve_swagger_ui();
            } else if (path == "/swagger.json") {
                return serve_openapi_spec();
            } else if (path == "/sample") {
                return serve_sample_workflow();
            } else if (path == "/commands") {
                return serve_commands_list();
            }
            return reply_error(status::NotFound, "Unknown endpoint");
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
        HttpResponse serve_swagger_ui() {
            return HttpResponse::html(status::OK, Orcha::Agent::kSwaggerHtml);
        }

        HttpResponse serve_openapi_spec() {
            auto spec = generate_openapi_spec(registry_);
            return reply_json(status::OK, spec);
        }

        HttpResponse serve_sample_workflow() {
            Orcha::Json sample = Orcha::Json::object();
            Orcha::Json steps = Orcha::Json::array();

            Orcha::Json step0 = Orcha::Json::object();
            step0["command"] = "echo";
            Orcha::Json params = Orcha::Json::object();
            params["message"] = "Hello, Orcha!";
            step0["params"] = params;
            steps[0] = step0;

            sample["steps"] = steps;

            return reply_json(status::OK, sample);
        }

        HttpResponse serve_commands_list() {
            Orcha::Json result = Orcha::Json::object();

            auto commands = registry_->list_commands();
            Orcha::Json arr = Orcha::Json::array();

            for (size_t i = 0; i < commands.size(); ++i) {
                const auto& name = commands[i];
                Orcha::Json cmd_info = Orcha::Json::object();
                cmd_info["name"] = name;

                if (auto cmd = registry_->get_command(name)) {
                    auto meta = cmd->metadata();
                    cmd_info["version"] = meta.version;
                    cmd_info["description"] = meta.description;

                    if (!meta.parameters.empty()) {
                        Orcha::Json params = Orcha::Json::array();
                        for (size_t j = 0; j < meta.parameters.size(); ++j) {
                            params[j] = meta.parameters[j].to_json();
                        }
                        cmd_info["parameters"] = params;
                    }
                }

                arr[i] = cmd_info;
            }

            result["commands"] = arr;
            result["count"] = static_cast<int>(commands.size());

            return reply_json(status::OK, result);
        }

        // ====================================================================
        // OpenAPI spec generation
        // ====================================================================

        static Orcha::Json generate_openapi_spec(
            std::shared_ptr<Core::ICommandRegistry> registry) {

            using value = Orcha::Json;

            value spec = value::object();
            spec["openapi"] = "3.0.3";
            spec["info"] = build_info();
            spec["servers"] = build_servers();
            spec["tags"] = build_tags();
            spec["components"] = build_components();
            spec["paths"] = build_paths(registry);
            return spec;
        }

        // ---- Info / Servers / Tags ----------------------------------------

        static Orcha::Json build_info() {
            using value = Orcha::Json;
            value info = value::object();
            info["title"] = "Orcha API";
            info["version"] = Orcha::kVersion;
            info["description"] = 
                "Orcha is a plugin-based command orchestration engine. "
                "Public endpoints expose workflow execution and discovery; "
                "`/api/*` endpoints require Basic auth and are used by the "
                "admin dashboard at `/admin`.";

            value license = value::object();
            license["name"] = "MIT";
            info["license"] = license;
            return info;
        }

        static Orcha::Json build_servers() {
            using value = Orcha::Json;
            value servers = value::array();
            value server0 = value::object();
            server0["url"] = "/";
            server0["description"] = "Current host";
            servers[0] = server0;
            return servers;
        }

        static Orcha::Json build_tags() {
            using value = Orcha::Json;
            const std::pair<const char*, const char*> entries[] = {
                {"System",   "Health and discovery"},
                {"Workflow", "Ad-hoc workflow execution"},
                {"Commands", "Available command plugins"},
                {"Jobs",     "Saved workflow definitions (admin)"},
                {"Runs",     "Execution history (admin)"},
                {"Plugins",  "Plugin lifecycle (admin)"},
            };
            value tags = value::array();
            for (size_t i = 0; i < std::size(entries); ++i) {
                value t = value::object();
                t["name"] = entries[i].first;
                t["description"] = 
                    entries[i].second;
                tags[i] = t;
            }
            return tags;
        }

        // ---- Components: schemas + security -------------------------------

        static Orcha::Json build_components() {
            using value = Orcha::Json;
            value components = value::object();

            // Security schemes
            value security_schemes = value::object();
            value basic = value::object();
            basic["type"] = "http";
            basic["scheme"] = "basic";
            basic["description"] = 
                "Admin HTTP Basic credentials (see `admin.username` / `admin.password` in `orcha.yaml`).";
            security_schemes["basicAuth"] = basic;
            components["securitySchemes"] = security_schemes;

            // Schemas
            value schemas = value::object();
            schemas["Error"]            = schema_error();
            schemas["WorkflowStep"]     = schema_workflow_step();
            schemas["WorkflowRequest"]  = schema_workflow_request();
            schemas["StepResult"]       = schema_step_result();
            schemas["WorkflowResult"]   = schema_workflow_result();
            schemas["CommandParameter"] = schema_command_parameter();
            schemas["CommandInfo"]      = schema_command_info();
            schemas["CommandsList"]     = schema_commands_list();
            schemas["JobDefinition"]    = schema_job_definition();
            schemas["JobInput"]         = schema_job_input();
            schemas["JobsList"]         = schema_jobs_list();
            schemas["RunRecord"]        = schema_run_record();
            schemas["RunsList"]         = schema_runs_list();
            schemas["PluginInfo"]       = schema_plugin_info();
            schemas["PluginsList"]      = schema_plugins_list();
            schemas["PluginActionResult"] = schema_plugin_action_result();
            schemas["WatchStatus"]      = schema_watch_status();
            schemas["WatchUpdate"]      = schema_watch_update();
            components["schemas"] = schemas;

            return components;
        }

        // Convenience: a $ref string.
        static Orcha::Json ref(const std::string& name) {
            Orcha::Json r = Orcha::Json::object();
            r["$ref"] = "#/components/schemas/" + name;
            return r;
        }

        // Convenience: { "type": "string" } / "integer" / "boolean" / "object".
        static Orcha::Json type_of(const std::string& t) {
            Orcha::Json v = Orcha::Json::object();
            v["type"] = t;
            return v;
        }

        static Orcha::Json type_with_desc(const std::string& t,
                                                const std::string& desc) {
            auto v = type_of(t);
            v["description"] = desc;
            return v;
        }

        // ---- Schemas -------------------------------------------------------

        static Orcha::Json schema_error() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["error"] = type_of("string");
            s["properties"] = props;
            value req = value::array();
            req[0] = "error";
            s["required"] = req;
            return s;
        }

        static Orcha::Json schema_workflow_step() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["command"] = type_with_desc("string",
                "Registered command name (see GET /commands)");
            value params = type_of("object");
            params["additionalProperties"] = true;
            params["description"] = "Command-specific parameters";
            props["params"] = params;
            s["properties"] = props;
            value req = value::array();
            req[0] = "command";
            s["required"] = req;
            return s;
        }

        static Orcha::Json schema_workflow_request() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            value steps = value::object();
            steps["type"] = "array";
            steps["items"] = ref("WorkflowStep");
            props["steps"] = steps;
            s["properties"] = props;
            value req = value::array();
            req[0] = "steps";
            s["required"] = req;
            return s;
        }

        static Orcha::Json schema_step_result() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["success"] = type_of("boolean");
            props["output"] = type_of("object");
            props["error"] = type_of("string");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_workflow_result() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "array";
            s["items"] = ref("StepResult");
            return s;
        }

        static Orcha::Json schema_command_parameter() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["name"] = type_of("string");
            props["type"] = type_of("string");
            props["description"] = type_of("string");
            props["required"] = type_of("boolean");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_command_info() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["name"] = type_of("string");
            props["version"] = type_of("string");
            props["description"] = type_of("string");
            value params = value::object();
            params["type"] = "array";
            params["items"] = ref("CommandParameter");
            props["parameters"] = params;
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_commands_list() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            value items = value::object();
            items["type"] = "array";
            items["items"] = ref("CommandInfo");
            props["commands"] = items;
            props["count"] = type_of("integer");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_job_definition() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["id"] = type_of("string");
            props["name"] = type_of("string");
            props["description"] = type_of("string");
            props["definition"] = ref("WorkflowRequest");
            props["enabled"] = type_of("boolean");
            value cron = type_of("string");
            cron["nullable"] = true;
            cron["description"] = 
                "Cron expression (5-field, UTC) or null";
            props["schedule_cron"] = cron;
            props["created_at"] = type_of("string");
            props["updated_at"] = type_of("string");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_job_input() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["name"] = type_of("string");
            props["description"] = type_of("string");
            props["definition"] = ref("WorkflowRequest");
            props["enabled"] = type_of("boolean");
            value cron = type_of("string");
            cron["nullable"] = true;
            props["schedule_cron"] = cron;
            s["properties"] = props;
            value req = value::array();
            req[0] = "name";
            req[1] = "definition";
            s["required"] = req;
            return s;
        }

        static Orcha::Json schema_jobs_list() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            value items = value::object();
            items["type"] = "array";
            items["items"] = ref("JobDefinition");
            props["jobs"] = items;
            props["count"] = type_of("integer");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_run_record() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["id"] = type_of("string");
            value jid = type_of("string");
            jid["nullable"] = true;
            jid["description"] = "Null for ad-hoc /workflow runs";
            props["job_id"] = jid;
            value trig = type_of("string");
            value trig_enum = value::array();
            trig_enum[0] = "manual";
            trig_enum[1] = "api";
            trig_enum[2] = "schedule";
            trig["enum"] = trig_enum;
            props["trigger"] = trig;
            value st = type_of("string");
            value st_enum = value::array();
            st_enum[0] = "success";
            st_enum[1] = "failed";
            st["enum"] = st_enum;
            props["status"] = st;
            props["started_at"] = type_of("string");
            value fin = type_of("string");
            fin["nullable"] = true;
            props["finished_at"] = fin;
            props["result"] = ref("WorkflowResult");
            props["error"] = type_of("string");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_runs_list() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            value items = value::object();
            items["type"] = "array";
            items["items"] = ref("RunRecord");
            props["runs"] = items;
            props["count"] = type_of("integer");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_plugin_info() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["name"] = type_of("string");
            props["version"] = type_of("string");
            props["description"] = type_of("string");
            props["author"] = type_of("string");
            value status = type_of("string");
            value status_enum = value::array();
            status_enum[0] = "loaded";
            status_enum[1] = "available";
            status_enum[2] = "disabled";
            status["enum"] = status_enum;
            props["status"] = status;
            props["library_path"] = type_of("string");
            value deps = value::object();
            deps["type"] = "array";
            deps["items"] = type_of("string");
            props["dependencies"] = deps;
            value params = value::object();
            params["type"] = "array";
            params["items"] = ref("CommandParameter");
            props["parameters"] = params;
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_plugins_list() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["directory"] = type_of("string");
            props["watching"] = type_of("boolean");
            value items = value::object();
            items["type"] = "array";
            items["items"] = ref("PluginInfo");
            props["plugins"] = items;
            props["count"] = type_of("integer");
            value cmds = value::object();
            cmds["type"] = "array";
            cmds["items"] = type_of("string");
            props["commands"] = cmds;
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_plugin_action_result() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["success"] = type_of("boolean");
            props["message"] = type_of("string");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_watch_status() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["watching"] = type_of("boolean");
            s["properties"] = props;
            return s;
        }

        static Orcha::Json schema_watch_update() {
            using value = Orcha::Json;
            value s = value::object();
            s["type"] = "object";
            value props = value::object();
            props["enabled"] = type_of("boolean");
            s["properties"] = props;
            value req = value::array();
            req[0] = "enabled";
            s["required"] = req;
            return s;
        }

        // ---- Path builders -------------------------------------------------

        // Helper: a JSON response { description, content: { application/json: { schema: $ref } } }
        static Orcha::Json json_response(const std::string& description,
                                              const std::string& schema_name) {
            using value = Orcha::Json;
            value r = value::object();
            r["description"] = description;
            value content = value::object();
            value app_json = value::object();
            app_json["schema"] = ref(schema_name);
            content["application/json"] = app_json;
            r["content"] = content;
            return r;
        }

        static Orcha::Json text_response(const std::string& description) {
            using value = Orcha::Json;
            value r = value::object();
            r["description"] = description;
            value content = value::object();
            value text_plain = value::object();
            value schema = value::object();
            schema["type"] = "string";
            text_plain["schema"] = schema;
            content["text/plain"] = text_plain;
            r["content"] = content;
            return r;
        }

        static Orcha::Json simple_response(const std::string& description) {
            using value = Orcha::Json;
            value r = value::object();
            r["description"] = description;
            return r;
        }

        static Orcha::Json error_response(const std::string& description) {
            return json_response(description, "Error");
        }

        // Adds a `security: [{ basicAuth: [] }]` requirement.
        static Orcha::Json basic_auth_security() {
            using value = Orcha::Json;
            value sec = value::array();
            value s = value::object();
            s["basicAuth"] = value::array();
            sec[0] = s;
            return sec;
        }

        static Orcha::Json tag(const std::string& name) {
            using value = Orcha::Json;
            value arr = value::array();
            arr[0] = name;
            return arr;
        }

        static Orcha::Json path_param(const std::string& name,
                                           const std::string& description) {
            using value = Orcha::Json;
            value p = value::object();
            p["name"] = name;
            p["in"] = "path";
            p["required"] = true;
            p["description"] = description;
            p["schema"] = type_of("string");
            return p;
        }

        static Orcha::Json query_int(const std::string& name,
                                          const std::string& description,
                                          int default_value,
                                          int min_value,
                                          int max_value) {
            using value = Orcha::Json;
            value p = value::object();
            p["name"] = name;
            p["in"] = "query";
            p["required"] = false;
            p["description"] = description;
            value schema = value::object();
            schema["type"] = "integer";
            schema["default"] = default_value;
            schema["minimum"] = min_value;
            schema["maximum"] = max_value;
            p["schema"] = schema;
            return p;
        }

        static Orcha::Json json_body(const std::string& schema_name,
                                          bool required = true) {
            using value = Orcha::Json;
            value body = value::object();
            body["required"] = required;
            value content = value::object();
            value app_json = value::object();
            app_json["schema"] = ref(schema_name);
            content["application/json"] = app_json;
            body["content"] = content;
            return body;
        }

        // ---- All paths -----------------------------------------------------

        static Orcha::Json build_paths(
            std::shared_ptr<Core::ICommandRegistry> /*registry*/) {
            using value = Orcha::Json;
            value paths = value::object();

            // Public
            paths["/"]            = path_health();
            paths["/workflow"]    = path_workflow();
            paths["/commands"]    = path_commands();
            paths["/sample"]      = path_sample();

            // Admin: jobs
            paths["/api/jobs"]                  = path_jobs_collection();
            paths["/api/jobs/{id}"]             = path_job_item();
            paths["/api/jobs/{id}/run"]         = path_job_run();
            paths["/api/jobs/{id}/runs"]        = path_job_runs();

            // Admin: runs
            paths["/api/runs"]                  = path_runs_collection();
            paths["/api/runs/{id}"]             = path_run_item();

            // Admin: plugins
            paths["/api/plugins"]               = path_plugins_collection();
            paths["/api/plugins/{name}"]        = path_plugin_item();
            paths["/api/plugins/{name}/reload"] = path_plugin_action("reload", "Reload");
            paths["/api/plugins/{name}/enable"] = path_plugin_action("enable", "Enable");
            paths["/api/plugins/{name}/disable"]= path_plugin_action("disable", "Disable");
            paths["/api/plugins/_watch"]        = path_plugins_watch();

            return paths;
        }

        // -- Public paths ----------------------------------------------------

        static Orcha::Json path_health() {
            using value = Orcha::Json;
            value path = value::object();
            value op = value::object();
            op["summary"] = "Health check";
            op["description"] = 
                "Returns a plain-text banner with quick links.";
            op["tags"] = tag("System");
            value responses = value::object();
            responses["200"] = text_response("Banner");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        static Orcha::Json path_workflow() {
            using value = Orcha::Json;
            value path = value::object();
            value op = value::object();
            op["summary"] = "Execute a workflow";
            op["description"] = 
                "Executes the supplied steps and returns per-step results.";
            op["tags"] = tag("Workflow");
            op["requestBody"] = json_body("WorkflowRequest");
            value responses = value::object();
            responses["200"] = json_response("Workflow result", "WorkflowResult");
            responses["415"] = error_response("Unsupported media type");
            responses["500"] = error_response("Execution error");
            op["responses"] = responses;
            path["post"] = op;
            return path;
        }

        static Orcha::Json path_commands() {
            using value = Orcha::Json;
            value path = value::object();
            value op = value::object();
            op["summary"] = "List available commands";
            op["tags"] = tag("Commands");
            value responses = value::object();
            responses["200"] = json_response("Registered commands", "CommandsList");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        static Orcha::Json path_sample() {
            using value = Orcha::Json;
            value path = value::object();
            value op = value::object();
            op["summary"] = "Get sample workflow";
            op["description"] = 
                "Returns a minimal workflow payload usable with POST /workflow.";
            op["tags"] = tag("Workflow");
            value responses = value::object();
            responses["200"] = json_response("Sample workflow", "WorkflowRequest");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        // -- Jobs paths ------------------------------------------------------

        static Orcha::Json path_jobs_collection() {
            using value = Orcha::Json;
            value path = value::object();

            // GET
            value get_op = value::object();
            get_op["summary"] = "List jobs";
            get_op["tags"] = tag("Jobs");
            get_op["security"] = basic_auth_security();
            value get_resp = value::object();
            get_resp["200"] = json_response("Jobs", "JobsList");
            get_resp["401"] = error_response("Unauthorized");
            get_op["responses"] = get_resp;
            path["get"] = get_op;

            // POST
            value post_op = value::object();
            post_op["summary"] = "Create a job";
            post_op["tags"] = tag("Jobs");
            post_op["security"] = basic_auth_security();
            post_op["requestBody"] = json_body("JobInput");
            value post_resp = value::object();
            post_resp["201"] = json_response("Created", "JobDefinition");
            post_resp["400"] = error_response("Validation error");
            post_resp["401"] = error_response("Unauthorized");
            post_resp["409"] = error_response("Job name already exists");
            post_op["responses"] = post_resp;
            path["post"] = post_op;

            return path;
        }

        static Orcha::Json path_job_item() {
            using value = Orcha::Json;
            value path = value::object();
            value params = value::array();
            params[0] = path_param("id", "Job ID");
            path["parameters"] = params;

            value get_op = value::object();
            get_op["summary"] = "Get a job";
            get_op["tags"] = tag("Jobs");
            get_op["security"] = basic_auth_security();
            value get_resp = value::object();
            get_resp["200"] = json_response("Job", "JobDefinition");
            get_resp["404"] = error_response("Not found");
            get_op["responses"] = get_resp;
            path["get"] = get_op;

            value put_op = value::object();
            put_op["summary"] = "Update a job";
            put_op["tags"] = tag("Jobs");
            put_op["security"] = basic_auth_security();
            put_op["requestBody"] = json_body("JobInput");
            value put_resp = value::object();
            put_resp["200"] = json_response("Updated", "JobDefinition");
            put_resp["400"] = error_response("Validation error");
            put_resp["404"] = error_response("Not found");
            put_resp["409"] = error_response("Name conflict");
            put_op["responses"] = put_resp;
            path["put"] = put_op;

            value del_op = value::object();
            del_op["summary"] = "Delete a job";
            del_op["tags"] = tag("Jobs");
            del_op["security"] = basic_auth_security();
            value del_resp = value::object();
            del_resp["200"] = simple_response("Deleted");
            del_resp["404"] = error_response("Not found");
            del_op["responses"] = del_resp;
            path["delete"] = del_op;

            return path;
        }

        static Orcha::Json path_job_run() {
            using value = Orcha::Json;
            value path = value::object();
            value params = value::array();
            params[0] = path_param("id", "Job ID");
            path["parameters"] = params;

            value op = value::object();
            op["summary"] = "Run a job now";
            op["description"] = 
                "Synchronously executes the job and returns the resulting run record.";
            op["tags"] = tag("Jobs");
            op["security"] = basic_auth_security();
            value responses = value::object();
            responses["200"] = json_response("Run record", "RunRecord");
            responses["404"] = error_response("Not found");
            op["responses"] = responses;
            path["post"] = op;
            return path;
        }

        static Orcha::Json path_job_runs() {
            using value = Orcha::Json;
            value path = value::object();
            value params = value::array();
            params[0] = path_param("id", "Job ID");
            params[1] = query_int("limit", "Max rows to return", 50, 1, 1000);
            path["parameters"] = params;

            value op = value::object();
            op["summary"] = "Run history for a job";
            op["tags"] = tag("Runs");
            op["security"] = basic_auth_security();
            value responses = value::object();
            responses["200"] = json_response("Runs", "RunsList");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        // -- Runs paths ------------------------------------------------------

        static Orcha::Json path_runs_collection() {
            using value = Orcha::Json;
            value path = value::object();
            value params = value::array();
            params[0] = query_int("limit", "Max rows to return", 50, 1, 1000);
            path["parameters"] = params;

            value op = value::object();
            op["summary"] = "Recent runs (all jobs + ad-hoc)";
            op["tags"] = tag("Runs");
            op["security"] = basic_auth_security();
            value responses = value::object();
            responses["200"] = json_response("Runs", "RunsList");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        static Orcha::Json path_run_item() {
            using value = Orcha::Json;
            value path = value::object();
            value params = value::array();
            params[0] = path_param("id", "Run ID");
            path["parameters"] = params;

            value op = value::object();
            op["summary"] = "Get a run";
            op["tags"] = tag("Runs");
            op["security"] = basic_auth_security();
            value responses = value::object();
            responses["200"] = json_response("Run", "RunRecord");
            responses["404"] = error_response("Not found");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        // -- Plugin paths ----------------------------------------------------

        static Orcha::Json path_plugins_collection() {
            using value = Orcha::Json;
            value path = value::object();
            value op = value::object();
            op["summary"] = "List loaded and available plugins";
            op["tags"] = tag("Plugins");
            op["security"] = basic_auth_security();
            value responses = value::object();
            responses["200"] = json_response("Plugins", "PluginsList");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        static Orcha::Json path_plugin_item() {
            using value = Orcha::Json;
            value path = value::object();
            value params = value::array();
            params[0] = path_param("name", "Plugin name");
            path["parameters"] = params;

            value op = value::object();
            op["summary"] = "Get plugin metadata";
            op["tags"] = tag("Plugins");
            op["security"] = basic_auth_security();
            value responses = value::object();
            responses["200"] = json_response("Plugin", "PluginInfo");
            responses["404"] = error_response("Plugin not loaded");
            op["responses"] = responses;
            path["get"] = op;
            return path;
        }

        static Orcha::Json path_plugin_action(const std::string& action,
                                                   const std::string& verb) {
            using value = Orcha::Json;
            value path = value::object();
            value params = value::array();
            params[0] = path_param("name", "Plugin name");
            path["parameters"] = params;

            value op = value::object();
            op["summary"] = verb + " a plugin";
            op["description"] = 
                action == "enable"
                    ? "Loads an available plugin from disk and clears any persisted disable."
                : action == "disable"
                    ? "Unloads a plugin and persists it as disabled so it stays off across restarts."
                    : "Reloads a loaded plugin (does not touch the denylist).";
            op["tags"] = tag("Plugins");
            op["security"] = basic_auth_security();
            value responses = value::object();
            responses["200"] = json_response("Action succeeded", "PluginActionResult");
            responses["404"] = error_response("Plugin not found");
            responses["409"] = json_response("Action failed", "PluginActionResult");
            op["responses"] = responses;
            path["post"] = op;
            return path;
        }

        static Orcha::Json path_plugins_watch() {
            using value = Orcha::Json;
            value path = value::object();

            value get_op = value::object();
            get_op["summary"] = "Directory watcher status";
            get_op["tags"] = tag("Plugins");
            get_op["security"] = basic_auth_security();
            value get_resp = value::object();
            get_resp["200"] = json_response("Watch status", "WatchStatus");
            get_op["responses"] = get_resp;
            path["get"] = get_op;

            value put_op = value::object();
            put_op["summary"] = "Enable or disable the watcher";
            put_op["tags"] = tag("Plugins");
            put_op["security"] = basic_auth_security();
            put_op["requestBody"] = json_body("WatchUpdate");
            value put_resp = value::object();
            put_resp["200"] = json_response("Watch status", "WatchStatus");
            put_resp["400"] = error_response("Invalid body");
            put_op["responses"] = put_resp;
            path["put"] = put_op;

            return path;
        }

        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
