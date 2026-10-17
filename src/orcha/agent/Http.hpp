//
// Http.hpp - Transport-agnostic HTTP request/response types for the agent.
//
// These replace cpprestsdk's web::http::http_request / http_response across the
// route handlers. They are plain data with no dependency on the underlying
// server (Boost.Beast lives in HttpServer.cpp), so route headers stay light and
// the HTTP backend can change without touching handlers.
//

#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Orcha::Agent {

    /// HTTP status codes used by the agent (mirrors the cpprest names we relied on).
    namespace status {
        inline constexpr int OK = 200;
        inline constexpr int Created = 201;
        inline constexpr int BadRequest = 400;
        inline constexpr int Unauthorized = 401;
        inline constexpr int NotFound = 404;
        inline constexpr int MethodNotAllowed = 405;
        inline constexpr int Conflict = 409;
        inline constexpr int UnsupportedMediaType = 415;
        inline constexpr int InternalError = 500;
    } // namespace status

    /**
     * @struct HttpRequest
     * @brief A parsed inbound request handed to route handlers.
     *
     * @c path is decoded and normalized (leading `//` collapsed). @c query holds
     * the already-split query parameters. The server (HttpServer) is responsible
     * for populating all fields.
     */
    struct HttpRequest {
        std::string method;   ///< Upper-case verb, e.g. "GET".
        std::string path;     ///< Decoded, normalized path, e.g. "/api/jobs/abc".
        std::string body;     ///< Raw request body.
        std::vector<std::pair<std::string, std::string>> headers; ///< As received.
        std::map<std::string, std::string> query; ///< Parsed query parameters.

        /// Case-insensitive header lookup.
        [[nodiscard]] std::optional<std::string> header(std::string_view name) const {
            for (const auto& [k, v] : headers) {
                if (iequals(k, name)) {
                    return v;
                }
            }
            return std::nullopt;
        }

        /// Query parameter lookup.
        [[nodiscard]] std::optional<std::string> query_param(const std::string& name) const {
            if (auto it = query.find(name); it != query.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        static bool iequals(std::string_view a, std::string_view b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) ==
                              std::tolower(static_cast<unsigned char>(y));
                   });
        }
    };

    /**
     * @struct HttpResponse
     * @brief What a route handler returns; the server serializes it on the wire.
     */
    struct HttpResponse {
        int status = status::OK;
        std::string content_type = "text/plain; charset=utf-8";
        std::string body;

        HttpResponse() = default;
        HttpResponse(int code, std::string ct, std::string b)
            : status(code), content_type(std::move(ct)), body(std::move(b)) {}

        /// Convenience builders.
        static HttpResponse text(int code, std::string body) {
            return HttpResponse(code, "text/plain; charset=utf-8", std::move(body));
        }
        static HttpResponse html(int code, std::string body) {
            return HttpResponse(code, "text/html; charset=utf-8", std::move(body));
        }
    };

} // namespace Orcha::Agent
