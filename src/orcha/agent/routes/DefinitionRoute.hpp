//
// DefinitionRoute.hpp - Convert workflow definitions between JSON and YAML.
//
// Endpoint (under the admin auth gate):
//   POST /api/definitions/convert
//     body: a definition as YAML (YAML Content-Type) or JSON
//     200:  { "definition": <JSON>, "yaml": "<YAML text>" }
//     400:  { "error", "line"?, "column"? }
//
// The admin job editor uses this for its YAML tab, so the dashboard needs no
// YAML parser of its own and reads YAML exactly as the CLI and jobs API do
// (utils/YamlJson.hpp). Key order is kept in both directions, so bodies are
// capped (kMaxYamlBodyBytes, 10,000 keys per object; 413/400). The definition
// is not validated as a workflow here (the editor converts half-written
// definitions); saving a job does that.
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../HttpBody.hpp"
#include "../HttpJson.hpp"

#include <string>
#include <vector>

namespace Orcha::Agent::Routes {

    class DefinitionRoute : public IRouteHandler {
    public:
        static constexpr const char* kConvertPath = "/api/definitions/convert";

        [[nodiscard]] bool can_handle(
            const std::string& method, const std::string& path) const override {
            (void)method;
            return path == kConvertPath;
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            if (request.method != "POST") {
                return reply_error(status::MethodNotAllowed, "Use POST /api/definitions/convert");
            }
            const auto content_type = request.header("Content-Type").value_or("");
            if (!is_yaml_media_type(content_type) && !is_json_media_type(content_type)) {
                return reply_error(status::UnsupportedMediaType,
                                   "Send the definition as application/yaml or application/json");
            }

            nlohmann::ordered_json definition;
            try {
                definition = parse_request_body<nlohmann::ordered_json>(request);
            } catch (const std::exception& ex) {
                return reply_bad_body(request, ex);
            }
            if (!definition.is_object()) {
                return reply_error(status::BadRequest,
                                   "A definition must be a mapping (an object with 'steps')");
            }

            nlohmann::ordered_json out = nlohmann::ordered_json::object();
            try {
                out["yaml"] = Utils::json_to_yaml(definition);
            } catch (const std::exception& ex) {
                return reply_error(status::BadRequest, ex.what());
            }
            out["definition"] = std::move(definition);
            return reply_json(status::OK, out);
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "POST", .path = kConvertPath,
                 .description = "Convert a workflow definition between JSON and YAML"}
            };
        }
    };

} // namespace Orcha::Agent::Routes
