//
// AdminConfig.hpp - Strongly-typed admin dashboard configuration
// Kept separate from YamlConfiguration.hpp so consumers (e.g. the agent layer)
// can depend on it without pulling in the yaml-cpp backend.
//

#pragma once

#include "IConfiguration.hpp"
#include <string>

namespace Orcha::Config {

    /**
     * @struct AdminConfig
     * @brief Configuration for the admin dashboard and its JSON API.
     *
     * When auth_required is true but password is empty, the admin surface is
     * disabled (fail closed) rather than exposed without authentication.
     */
    struct AdminConfig {
        bool enabled = true;            ///< Master switch for the dashboard + admin API.
        bool auth_required = true;      ///< If false, skip Basic auth (localhost/dev only).
        std::string username = "admin";
        std::string password;           ///< Empty + auth_required => admin surface disabled.
        std::string realm = "Orcha Admin";

        static AdminConfig from_config(const IConfiguration& config) {
            AdminConfig cfg;
            cfg.enabled       = config.get_bool("admin.enabled", true);
            cfg.auth_required = config.get_bool("admin.auth_required", true);
            cfg.username      = config.get_string("admin.username", "admin");
            cfg.password      = config.get_string("admin.password", "");
            cfg.realm         = config.get_string("admin.realm", "Orcha Admin");
            return cfg;
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
