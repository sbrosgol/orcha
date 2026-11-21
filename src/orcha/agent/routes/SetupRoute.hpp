//
// SetupRoute.hpp - First-run setup of the admin account.
//
// Endpoints (no login needed; the auth middleware lets kSetupPath through):
//   GET  /api/setup
//     200: { "setup_required": bool }
//   POST /api/setup   body: { "username": "...", "password": "..." }
//     201: { "username": "..." }        the account now exists; log in with it
//     400: invalid username or password  (password: at least 12 characters)
//     409: already set up (by config, a stored account, or a concurrent request)
//     429: another setup request is being processed (Retry-After: 1)
//     500: the account could not be saved (details in the server log)
//
// See AdminAccount.hpp for where credentials come from. Deliberately no setup
// code: the first person to submit this form becomes admin.
//

#pragma once

#include "../AdminAccount.hpp"
#include "../AuthMiddleware.hpp"
#include "../HttpBody.hpp"
#include "../HttpJson.hpp"
#include "../IRouteHandler.hpp"
#include "../../core/JsonCompat.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Orcha::Agent::Routes {

    class SetupRoute : public IRouteHandler {
    public:
        explicit SetupRoute(std::shared_ptr<AdminAccount> account,
                            std::shared_ptr<Utils::ILogger> logger = nullptr)
            : account_(std::move(account)), logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(const std::string& method, const std::string& path) const override {
            (void)method;
            return path == kSetupPath;
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            if (request.method == "GET") {
                return reply_json(status::OK, Orcha::Json{
                    {"setup_required", account_->mode() == AdminAccount::Mode::Setup}});
            }
            if (request.method != "POST") {
                return reply_error(status::MethodNotAllowed, "Use GET or POST /api/setup");
            }
            if (account_->mode() != AdminAccount::Mode::Setup) {
                return reply_error(status::Conflict, "The admin account is already set up");
            }
            if (!is_json_media_type(request.header("Content-Type").value_or(""))) {
                return reply_error(status::UnsupportedMediaType, "Send application/json");
            }
            Orcha::Json body;
            try {
                body = parse_json_bounded(request.body);
            } catch (const std::exception& ex) {
                return reply_error(status::BadRequest, std::string("Invalid JSON: ") + ex.what());
            }
            if (!body.is_object() || !body.contains("username") || !body.contains("password") ||
                !body.at("username").is_string() || !body.at("password").is_string()) {
                return reply_error(status::BadRequest,
                                   "Expected {\"username\": \"...\", \"password\": \"...\"}");
            }
            const auto user = body.at("username").get<std::string>();
            if (auto failed = account_->set_up(user, body.at("password").get<std::string>())) {
                using Kind = AdminAccount::SetupError::Kind;
                if (failed->kind == Kind::Busy) {
                    return reply_error(status::TooManyRequests, failed->message).with_header("Retry-After", "1");
                }
                const int code = failed->kind == Kind::Invalid ? status::BadRequest
                               : failed->kind == Kind::AlreadySet ? status::Conflict
                               : status::InternalError;
                return reply_error(code, failed->message);
            }
            return reply_json(status::Created, Orcha::Json{{"username", user}});
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "GET", .path = kSetupPath, .description = "Whether the admin account still needs setting up"},
                {.method = "POST", .path = kSetupPath, .description = "Create the admin account (first run only)"},
            };
        }

    private:
        std::shared_ptr<AdminAccount> account_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
