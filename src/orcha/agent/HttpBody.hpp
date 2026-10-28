//
// HttpBody.hpp - Request bodies in JSON or YAML.
//
// Admin endpoints that take a definition (jobs, definition conversion) accept
// YAML when the request says so with its Content-Type, and JSON otherwise.
// Both are bounded by nesting depth. YAML is also capped in size (yaml-cpp
// builds a node tree of ~100x the input) and in values after alias expansion.
// A bad body becomes a 400 that carries the line and column of a YAML error;
// an oversized one a 413.
//

#pragma once

#include "Http.hpp"
#include "HttpJson.hpp"
#include "../core/JsonCompat.hpp"
#include "../utils/YamlJson.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <type_traits>

namespace Orcha::Agent {

    /// Largest YAML body accepted (also the dashboard's import limit).
    inline constexpr std::size_t kMaxYamlBodyBytes = Utils::YamlLimits{}.max_bytes;

    /// Thrown by parse_request_body() for a body over its size limit (413).
    class RequestBodyTooLarge : public std::runtime_error {
    public:
        explicit RequestBodyTooLarge(std::size_t limit)
            : std::runtime_error("Body is larger than " + std::to_string(limit) + " bytes") {}
    };

    namespace detail {
        /// "Text/YAML; charset=utf-8" -> "Text/YAML".
        inline std::string_view media_type(std::string_view content_type) {
            auto type = content_type.substr(0, content_type.find(';'));
            while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) {
                type.remove_suffix(1);
            }
            while (!type.empty() && std::isspace(static_cast<unsigned char>(type.front()))) {
                type.remove_prefix(1);
            }
            return type;
        }
    } // namespace detail

    /// True for the YAML media types in use (RFC 9512 plus the legacy x- forms).
    [[nodiscard]] inline bool is_yaml_media_type(std::string_view content_type) {
        static constexpr std::array<std::string_view, 4> kYamlTypes{
            "application/yaml", "application/x-yaml", "text/yaml", "text/x-yaml"};
        return std::ranges::any_of(kYamlTypes, [type = detail::media_type(content_type)](std::string_view t) {
            return HttpRequest::iequals(type, t);
        });
    }

    [[nodiscard]] inline bool is_json_media_type(std::string_view content_type) {
        return HttpRequest::iequals(detail::media_type(content_type), "application/json");
    }

    [[nodiscard]] inline bool is_yaml_body(const HttpRequest& request) {
        return is_yaml_media_type(request.header("Content-Type").value_or(""));
    }

    /**
     * @brief Parse the request body as YAML (YAML Content-Type) or JSON.
     *
     * With nlohmann::ordered_json the body is also capped at kMaxYamlBodyBytes
     * and 10,000 keys per object (its member lookup is linear).
     *
     * @throws RequestBodyTooLarge, Utils::YamlError, JsonDepthError or J::parse_error.
     */
    template <class J = Orcha::Json>
    [[nodiscard]] J parse_request_body(const HttpRequest& request) {
        constexpr bool ordered = std::is_same_v<J, nlohmann::ordered_json>;
        const bool yaml = is_yaml_body(request);
        if ((yaml || ordered) && request.body.size() > kMaxYamlBodyBytes) {
            throw RequestBodyTooLarge(kMaxYamlBodyBytes);
        }
        if (yaml) return Utils::parse_yaml<J>(request.body);
        if constexpr (ordered) return parse_json_bounded<J>(request.body, kMaxRequestJsonDepth, 10'000);
        return parse_json_bounded<J>(request.body);
    }

    /// 400 (413 if too large) for a body parse_request_body() rejected:
    /// `{error, line?, column?}`.
    [[nodiscard]] inline HttpResponse reply_bad_body(const HttpRequest& request,
                                                     const std::exception& ex) {
        if (dynamic_cast<const RequestBodyTooLarge*>(&ex)) {
            return reply_error(status::PayloadTooLarge, ex.what());
        }
        Orcha::Json body = Orcha::Json::object();
        body["error"] = std::string(is_yaml_body(request) ? "Invalid YAML body: "
                                                          : "Invalid JSON body: ") + ex.what();
        if (const auto* yaml = dynamic_cast<const Utils::YamlError*>(&ex);
            yaml && yaml->line() > 0) {
            body["line"] = yaml->line();
            body["column"] = yaml->column();
        }
        return reply_json(status::BadRequest, body);
    }

} // namespace Orcha::Agent
