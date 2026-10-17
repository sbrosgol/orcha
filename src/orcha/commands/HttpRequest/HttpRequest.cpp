//
// HttpRequest.cpp - Perform an HTTP request and capture the response.
//

#include "../../core/ICommand.hpp"
#include "../HttpClient.hpp"
#include "core/Version.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

class HttpRequest final : public Orcha::Core::ICommand {
public:
    [[nodiscard]] std::string name() const override { return "http_request"; }

    Orcha::Json execute(const Orcha::Json& params) override {
        try {
            if (!params.contains("url")) {
                throw std::runtime_error("'url' parameter is required");
            }

            Orcha::Http::Request req;
            req.url = params.at("url").get<std::string>();
            req.method = normalize_method(params.value("method", std::string("GET")));

            if (params.contains("headers") && params.at("headers").is_object()) {
                for (const auto& [key, value] : params.at("headers").items()) {
                    if (value.is_string()) {
                        req.headers.emplace_back(key, value.get<std::string>());
                    }
                }
            }

            if (params.contains("body")) {
                const auto& body = params.at("body");
                // Accept either a raw string body or a structured JSON body.
                req.body = body.is_string() ? body.get<std::string>() : body.dump();
            }

            if (params.contains("timeout_ms")) {
                req.timeout = std::chrono::milliseconds(
                    params.at("timeout_ms").get<long long>());
            }

            const Orcha::Http::Response response = Orcha::Http::perform(req);

            Orcha::Json result = Orcha::Json::object();
            result["success"]     = response.status >= 200 && response.status < 300;
            result["status_code"] = response.status;
            result["body"]        = response.body;

            Orcha::Json headers_out = Orcha::Json::object();
            for (const auto& [k, v] : response.headers) {
                headers_out[k] = v;
            }
            result["headers"] = headers_out;
            return result;
        } catch (const std::exception& ex) {
            Orcha::Json err = Orcha::Json::object();
            err["success"] = false;
            err["error"]   = ex.what();
            return err;
        }
    }

    [[nodiscard]] Orcha::Core::CommandMetadata metadata() const override {
        Orcha::Core::CommandMetadata meta;
        meta.name = "http_request";
        meta.version = Orcha::kVersion;
        meta.description = "Performs an HTTP request and returns the response";
        meta.author = "Orcha Team";
        meta.tags = {"http", "network", "utility"};
        meta.supports_rollback = false;

        Orcha::Core::CommandParameter url_p;
        url_p.name = "url";
        url_p.type = "string";
        url_p.required = true;
        url_p.description = "URL to request";
        url_p.example = "https://api.example.com/status";
        meta.parameters.push_back(url_p);

        Orcha::Core::CommandParameter method_p;
        method_p.name = "method";
        method_p.type = "string";
        method_p.required = false;
        method_p.description = "HTTP method (GET, POST, PUT, PATCH, DELETE, HEAD)";
        method_p.default_value = "GET";
        meta.parameters.push_back(method_p);

        Orcha::Core::CommandParameter headers_p;
        headers_p.name = "headers";
        headers_p.type = "object";
        headers_p.required = false;
        headers_p.description = "Request headers as a JSON object of string values";
        meta.parameters.push_back(headers_p);

        Orcha::Core::CommandParameter body_p;
        body_p.name = "body";
        body_p.type = "string";
        body_p.required = false;
        body_p.description = "Request body (string; JSON values are also accepted)";
        meta.parameters.push_back(body_p);

        Orcha::Core::CommandParameter timeout_p;
        timeout_p.name = "timeout_ms";
        timeout_p.type = "int";
        timeout_p.required = false;
        timeout_p.description = "Request timeout in milliseconds";
        meta.parameters.push_back(timeout_p);

        return meta;
    }

    // The default validate() would reject non-string bodies; override to permit
    // either string or structured JSON, while still checking required/url types.
    [[nodiscard]] Orcha::Core::Result<void, Orcha::Core::ValidationError> validate(
        const Orcha::Json& params) const override {
        using R = Orcha::Core::Result<void, Orcha::Core::ValidationError>;
        if (!params.contains("url")) {
            return R::Err({"url", "Required parameter missing"});
        }
        if (!params.at("url").is_string()) {
            return R::Err({"url", "Expected string type"});
        }
        if (params.contains("method") && !params.at("method").is_string()) {
            return R::Err({"method", "Expected string type"});
        }
        if (params.contains("headers") && !params.at("headers").is_object()) {
            return R::Err({"headers", "Expected object type"});
        }
        if (params.contains("timeout_ms") && !params.at("timeout_ms").is_number_integer()) {
            return R::Err({"timeout_ms", "Expected integer type"});
        }
        return R::Ok();
    }

private:
    // Normalize/validate the method name; Boost.Beast maps the verb itself.
    static std::string normalize_method(std::string m) {
        for (auto& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (m == "GET" || m == "POST" || m == "PUT" || m == "PATCH" ||
            m == "DELETE" || m == "HEAD") {
            return m;
        }
        throw std::runtime_error("Unsupported HTTP method: " + m);
    }
};

extern "C" Orcha::Core::ICommand* create_command() {
    return new HttpRequest();
}
