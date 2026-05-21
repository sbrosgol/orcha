//
// HttpRequest.cpp - Perform an HTTP request and capture the response.
//

#include "../../core/ICommand.hpp"
#include "core/Version.hpp"

#include <cpprest/http_client.h>
#include <cpprest/json.h>

#include <chrono>
#include <stdexcept>
#include <string>

class HttpRequest final : public Orcha::Core::ICommand {
public:
    [[nodiscard]] std::string name() const override { return "http_request"; }

    web::json::value execute(const web::json::value& params) override {
        using namespace web;
        using namespace utility;

        try {
            json::value result;
            if (!params.has_field(U("url"))) {
                throw std::runtime_error("'url' parameter is required");
            }
            const auto url = params.at(U("url")).as_string();

            const std::string method_str = params.has_field(U("method"))
                ? conversions::to_utf8string(params.at(U("method")).as_string())
                : "GET";
            const auto method = resolve_method(method_str);

            http::http_request request(method);

            if (params.has_field(U("headers")) && params.at(U("headers")).is_object()) {
                for (const auto& kv : params.at(U("headers")).as_object()) {
                    if (kv.second.is_string()) {
                        request.headers().add(kv.first, kv.second.as_string());
                    }
                }
            }

            if (params.has_field(U("body"))) {
                const auto& body = params.at(U("body"));
                if (body.is_string()) {
                    request.set_body(body.as_string());
                } else {
                    // Accept structured JSON bodies too.
                    request.set_body(body);
                }
            }

            http::client::http_client_config config;
            if (params.has_field(U("timeout_ms"))) {
                config.set_timeout(std::chrono::milliseconds(
                    params.at(U("timeout_ms")).as_integer()));
            }

            http::client::http_client client(url, config);
            const auto response  = client.request(request).get();
            const auto status    = response.status_code();
            const auto body_str  = response.extract_string().get();

            result[U("success")]     = json::value::boolean(status >= 200 && status < 300);
            result[U("status_code")] = json::value::number(static_cast<int>(status));
            result[U("body")]        = json::value::string(body_str);

            json::value headers_out = json::value::object();
            for (const auto& h : response.headers()) {
                headers_out[h.first] = json::value::string(h.second);
            }
            result[U("headers")] = headers_out;
            return result;
        } catch (const std::exception& ex) {
            json::value err;
            err[U("success")] = json::value(false);
            err[U("error")]   = json::value::string(conversions::to_string_t(ex.what()));
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
        const web::json::value& params) const override {
        using R = Orcha::Core::Result<void, Orcha::Core::ValidationError>;
        if (!params.has_field(U("url"))) {
            return R::Err({"url", "Required parameter missing"});
        }
        if (!params.at(U("url")).is_string()) {
            return R::Err({"url", "Expected string type"});
        }
        if (params.has_field(U("method")) && !params.at(U("method")).is_string()) {
            return R::Err({"method", "Expected string type"});
        }
        if (params.has_field(U("headers")) && !params.at(U("headers")).is_object()) {
            return R::Err({"headers", "Expected object type"});
        }
        if (params.has_field(U("timeout_ms")) && !params.at(U("timeout_ms")).is_integer()) {
            return R::Err({"timeout_ms", "Expected integer type"});
        }
        return R::Ok();
    }

private:
    static web::http::method resolve_method(const std::string& m) {
        using namespace web::http;
        if (m == "GET")    return methods::GET;
        if (m == "POST")   return methods::POST;
        if (m == "PUT")    return methods::PUT;
        if (m == "PATCH")  return methods::PATCH;
        if (m == "DELETE") return methods::DEL;
        if (m == "HEAD")   return methods::HEAD;
        throw std::runtime_error("Unsupported HTTP method: " + m);
    }
};

extern "C" Orcha::Core::ICommand* create_command() {
    return new HttpRequest();
}
