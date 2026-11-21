//
// AuthMiddleware.hpp - HTTP Basic authentication middleware for admin routes
// Part of the admin dashboard (Phase 1).
//

#pragma once

#include "IRouteHandler.hpp"
#include "HttpJson.hpp"
#include "AdminAccount.hpp"
#include "../config/AdminConfig.hpp"
#include "../config/ServerConfig.hpp"
#include "../utils/ILogger.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
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

    /// Paths under the admin prefix that answer without a login (first-run setup).
    inline constexpr const char* kSetupPath = "/api/setup";

    namespace detail {
        /// Another password check is running (see AdminAccount::check).
        inline HttpResponse login_busy_response() {
            return reply_error(status::TooManyRequests, "Too many login attempts; retry shortly")
                .with_header("Retry-After", "1");
        }
    } // namespace detail

    /**
     * @brief Build the admin login middleware for the given path prefixes.
     *
     * Requests outside @p protected_prefixes, and to kSetupPath, pass through.
     * Guarded requests need a login @p account accepts, unless it is Open
     * (auth_required=false on a loopback bind). In first-run setup every
     * guarded request is refused with `"setup_required": true`, so the
     * dashboard knows to show the setup form.
     */
    [[nodiscard]] inline Router::Middleware make_basic_auth(
        std::shared_ptr<const AdminAccount> account,
        std::vector<std::string> protected_prefixes,
        std::shared_ptr<Utils::ILogger> logger = nullptr) {

        return [account = std::move(account),
                prefixes = std::move(protected_prefixes),
                logger = std::move(logger)](
                   const HttpRequest& request) -> std::optional<HttpResponse> {

            const std::string& path = request.path;
            const bool guarded = std::ranges::any_of(prefixes,
                [&](const std::string& p) { return path_starts_with(path, p); });
            if (!guarded || path == kSetupPath) {
                return std::nullopt; // not our concern -> continue dispatch
            }

            const auto mode = account->mode();
            if (mode == AdminAccount::Mode::Open) {
                return std::nullopt;
            }
            if (mode == AdminAccount::Mode::Setup) {
                auto reply = detail::unauthorized_response();
                Orcha::Json body = {{"error", "Unauthorized: create the admin account first"},
                                    {"setup_required", true}};
                reply.body = body.dump();
                return reply;
            }
            if (auto value = request.header("Authorization")) {
                if (auto creds = detail::parse_basic_auth(*value)) {
                    const auto login = account->check(creds->user, creds->pass);
                    if (login == AdminAccount::Login::Ok) {
                        return std::nullopt; // authenticated -> continue dispatch
                    }
                    if (login == AdminAccount::Login::Busy) {
                        return detail::login_busy_response();
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
     * @param admin              The admin login, accepted too once it exists.
     * @param protected_prefixes Path prefixes requiring authentication.
     * @param logger             Optional logger for rejections.
     */
    [[nodiscard]] inline Router::Middleware make_api_auth(
        Config::ServerConfig server,
        std::shared_ptr<const AdminAccount> admin,
        std::vector<std::string> protected_prefixes,
        std::shared_ptr<Utils::ILogger> logger = nullptr) {

        return [server = std::move(server),
                admin = std::move(admin),
                prefixes = std::move(protected_prefixes),
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
                // Checked per request: first-run setup can create the login later.
                if (admin) {
                    if (auto creds = detail::parse_basic_auth(*value)) {
                        const auto login = admin->check(creds->user, creds->pass);
                        if (login == AdminAccount::Login::Ok) {
                            return std::nullopt;
                        }
                        if (login == AdminAccount::Login::Busy) {
                            return detail::login_busy_response();
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

    /// Same, from a password-only admin configuration (no stored account).
    [[nodiscard]] inline Router::Middleware make_api_auth(
        Config::ServerConfig server,
        Config::AdminConfig admin,
        std::vector<std::string> protected_prefixes,
        std::shared_ptr<Utils::ILogger> logger = nullptr) {
        admin.credentials_file.clear();
        const bool exposed = !server.is_loopback();
        return make_api_auth(std::move(server),
                             std::make_shared<const AdminAccount>(std::move(admin), exposed, logger),
                             std::move(protected_prefixes), std::move(logger));
    }

} // namespace Orcha::Agent
