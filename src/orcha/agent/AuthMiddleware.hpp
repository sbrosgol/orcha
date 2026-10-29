//
// AuthMiddleware.hpp - HTTP Basic authentication middleware for admin routes
// Part of the admin dashboard (Phase 1).
//

#pragma once

#include "IRouteHandler.hpp"
#include "HttpJson.hpp"
#include "../config/AdminConfig.hpp"
#include "../config/ServerConfig.hpp"
#include "../utils/ILogger.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Orcha::Agent {

    namespace detail {

        /**
         * @brief Decode a standard base64 string. Returns nullopt on malformed input.
         *
         * Hand-rolled to avoid OpenSSL EVP padding quirks; the inputs here are
         * tiny (credential pairs), so performance is irrelevant.
         */
        [[nodiscard]] inline std::optional<std::string> base64_decode(const std::string& input) {
            auto decode_char = [](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                if (c == '+') return 62;
                if (c == '/') return 63;
                return -1; // padding or invalid
            };

            std::string out;
            int buffer = 0;
            int bits = 0;

            for (char c : input) {
                if (c == '=') break;      // padding -> end of data
                if (c == '\n' || c == '\r' || c == ' ') continue; // tolerate whitespace
                int val = decode_char(c);
                if (val < 0) {
                    return std::nullopt;  // invalid character
                }
                buffer = (buffer << 6) | val;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
                }
            }
            return out;
        }

        /**
         * @brief Length-aware, content-constant-time string comparison.
         *
         * Avoids leaking the position of the first mismatch via timing. A
         * length difference is not hidden (and need not be for credentials).
         */
        [[nodiscard]] inline bool constant_time_equals(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) {
                return false;
            }
            unsigned char diff = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
            }
            return diff == 0;
        }

        struct Credentials {
            std::string user;
            std::string pass;
        };

        /**
         * @brief Parse an "Authorization: Basic <base64(user:pass)>" header value.
         */
        [[nodiscard]] inline std::optional<Credentials> parse_basic_auth(const std::string& header) {
            constexpr std::string_view kScheme = "Basic ";
            if (header.size() <= kScheme.size() ||
                header.compare(0, kScheme.size(), kScheme) != 0) {
                return std::nullopt;
            }

            auto decoded = base64_decode(header.substr(kScheme.size()));
            if (!decoded) {
                return std::nullopt;
            }

            auto colon = decoded->find(':');
            if (colon == std::string::npos) {
                return std::nullopt;
            }

            Credentials creds;
            creds.user = decoded->substr(0, colon);
            creds.pass = decoded->substr(colon + 1);
            return creds;
        }

        /**
         * @brief Reply with a 401 (JSON) but WITHOUT a WWW-Authenticate header.
         *
         * Omitting the challenge header is deliberate: a "WWW-Authenticate: Basic"
         * header would make the browser pop its native login dialog. The admin
         * dashboard renders its own custom login view instead and attaches the
         * Authorization header to its fetch() calls.
         */
        inline HttpResponse unauthorized_response() {
            Orcha::Json body = Orcha::Json::object();
            body["error"] = "Unauthorized";
            return reply_json(status::Unauthorized, body);
        }

    } // namespace detail

    /**
     * @brief Build a Basic-auth middleware that guards the given path prefixes.
     *
     * Requests to paths outside @p protected_prefixes pass through untouched, as
     * do all requests when @p cfg.auth_required is false. Guarded requests must
     * carry valid credentials matching @p cfg, otherwise a 401 challenge is sent.
     *
     * @param cfg                Admin configuration (credentials + realm).
     * @param protected_prefixes Path prefixes requiring authentication.
     * @param logger             Optional logger for auth failures.
     */
    [[nodiscard]] inline Router::Middleware make_basic_auth(
        Config::AdminConfig cfg,
        std::vector<std::string> protected_prefixes,
        std::shared_ptr<Utils::ILogger> logger = nullptr) {

        return [cfg = std::move(cfg),
                prefixes = std::move(protected_prefixes),
                logger = std::move(logger)](
                   const HttpRequest& request) -> std::optional<HttpResponse> {

            const std::string& path = request.path;

            const bool guarded = std::ranges::any_of(prefixes
                                                     ,
                                                     [&](const std::string& p) { return path_starts_with(path, p); });

            if (!guarded || !cfg.auth_required) {
                return std::nullopt; // not our concern -> continue dispatch
            }

            if (auto value = request.header("Authorization")) {
                if (auto creds = detail::parse_basic_auth(*value)) {
                    if (detail::constant_time_equals(creds->user, cfg.username) &&
                        detail::constant_time_equals(creds->pass, cfg.password)) {
                        return std::nullopt; // authenticated -> continue dispatch
                    }
                }
            }

            if (logger) {
                logger->warn("Rejected unauthenticated admin request to " + path);
            }
            return detail::unauthorized_response(); // handled (rejected)
        };
    }

    /**
     * @brief Build the fail-closed guard for the execution API (POST /workflow).
     *
     * For requests under @p protected_prefixes:
     * - If @p server.api_token is set, the request must carry
     *   `Authorization: Bearer <token>` or valid admin Basic credentials
     *   (when the admin surface is serviceable with auth); otherwise 401.
     * - If no token is set, the request is allowed only when the server is
     *   bound to a loopback address; otherwise 503 (the API is disabled
     *   rather than exposed unauthenticated).
     *
     * @param server             Server configuration (host + api_token).
     * @param admin              Admin configuration (Basic credentials accepted too).
     * @param protected_prefixes Path prefixes requiring authentication.
     * @param logger             Optional logger for rejections.
     */
    [[nodiscard]] inline Router::Middleware make_api_auth(
        Config::ServerConfig server,
        Config::AdminConfig admin,
        std::vector<std::string> protected_prefixes,
        std::shared_ptr<Utils::ILogger> logger = nullptr) {

        const bool accept_admin_basic = admin.is_serviceable() && admin.auth_required;

        return [server = std::move(server),
                admin = std::move(admin),
                prefixes = std::move(protected_prefixes),
                accept_admin_basic,
                logger = std::move(logger)](
                   const HttpRequest& request) -> std::optional<HttpResponse> {

            const std::string& path = request.path;
            const bool guarded = std::ranges::any_of(prefixes
                                                     ,
                                                     [&](const std::string& p) { return path_starts_with(path, p); });
            if (!guarded) {
                return std::nullopt;
            }

            if (server.api_token.empty()) {
                if (server.is_loopback()) {
                    return std::nullopt; // local-only bind: no token required
                }
                if (logger) {
                    logger->warn("Rejected " + path + ": server.api_token is not set and "
                                 "the server is bound to non-loopback host " + server.host);
                }
                return reply_error(status::ServiceUnavailable,
                    "The workflow API is disabled: set server.api_token "
                    "(ORCHA_SERVER_API_TOKEN) to enable it on a non-loopback host");
            }

            if (auto value = request.header("Authorization")) {
                constexpr std::string_view kBearer = "Bearer ";
                if (value->size() > kBearer.size() &&
                    value->compare(0, kBearer.size(), kBearer) == 0 &&
                    detail::constant_time_equals(value->substr(kBearer.size()),
                                                 server.api_token)) {
                    return std::nullopt;
                }
                if (accept_admin_basic) {
                    if (auto creds = detail::parse_basic_auth(*value)) {
                        if (detail::constant_time_equals(creds->user, admin.username) &&
                            detail::constant_time_equals(creds->pass, admin.password)) {
                            return std::nullopt;
                        }
                    }
                }
            }

            if (logger) {
                logger->warn("Rejected unauthenticated API request to " + path);
            }
            return detail::unauthorized_response();
        };
    }

} // namespace Orcha::Agent
