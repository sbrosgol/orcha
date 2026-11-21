//
// AdminConfig.hpp - Strongly-typed admin dashboard configuration
// Kept separate from YamlConfiguration.hpp so consumers (e.g. the agent layer)
// can depend on it without pulling in the yaml-cpp backend.
//

#pragma once

#include "IConfiguration.hpp"
#include <string>
#include <vector>

namespace Orcha::Config {

    /**
     * @struct AdminConfig
     * @brief Configuration for the admin dashboard and its JSON API.
     *
     * Credentials come from admin.password when set; otherwise from the account
     * stored by first-run setup (credentials_file), see Agent::AdminAccount.
     * is_serviceable() describes only the password-based configuration.
     */
    struct AdminConfig {
        /// The placeholder password the docs and examples used. Never accepted
        /// on an exposed (non-loopback) bind.
        static constexpr const char* kPlaceholderPassword = "change-me";

        bool enabled = true;            ///< Master switch for the dashboard + admin API.
        bool auth_required = true;      ///< If false, skip Basic auth (loopback binds only).
        std::string username = "admin";
        std::string password;           ///< Empty => the stored account, or first-run setup.
        std::string realm = "Orcha Admin";
        /// Where first-run setup stores the admin account (a salted hash).
        /// Empty: no stored account, so no password means no admin surface.
        std::string credentials_file;

        static AdminConfig from_config(const IConfiguration& config) {
            AdminConfig cfg;
            cfg.enabled          = config.get_bool("admin.enabled", true);
            cfg.auth_required    = config.get_bool("admin.auth_required", true);
            cfg.username         = config.get_string("admin.username", "admin");
            cfg.password         = config.get_string("admin.password", "");
            cfg.realm            = config.get_string("admin.realm", "Orcha Admin");
            cfg.credentials_file = config.get_string("admin.credentials_file", "./orcha-admin.json");
            return cfg;
        }

        /**
         * @brief The settings that apply on a bind that is (or isn't) exposed.
         *
         * On a non-loopback bind, `auth_required: false` and the placeholder
         * password are ignored (as if unset, so the stored account or first-run
         * setup applies); each adjustment is described in @p notes. On a
         * loopback bind the configuration is used as written.
         */
        [[nodiscard]] AdminConfig for_bind(bool exposed, std::vector<std::string>* notes = nullptr) const {
            AdminConfig out = *this;
            if (!exposed) return out;
            if (!out.auth_required) {
                out.auth_required = true;
                if (notes) notes->push_back(
                    "admin.auth_required=false is ignored on a non-loopback bind; admin login is required");
            }
            if (out.password == kPlaceholderPassword) {
                out.password.clear();
                if (notes) notes->push_back(
                    std::string("admin.password is the placeholder '") + kPlaceholderPassword +
                    "' and is ignored on a non-loopback bind; set a real one (ORCHA_ADMIN_PASSWORD)");
            }
            return out;
        }

        /**
         * @brief Whether the admin surface should be served.
         *
         * Fail-closed: if auth is required but no password is configured, the
         * admin routes must not be registered.
         */
        [[nodiscard]] bool is_serviceable() const {
            if (!enabled) return false;
            if (auth_required && password.empty()) return false;
            return true;
        }
    };

} // namespace Orcha::Config
