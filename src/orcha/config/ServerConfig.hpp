//
// ServerConfig.hpp - Strongly-typed HTTP server configuration
// Kept separate from YamlConfiguration.hpp so consumers (e.g. the agent layer)
// can depend on it without pulling in the yaml-cpp backend.
//

#pragma once

#include "IConfiguration.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Orcha::Config {

    /**
     * @struct ServerConfig
     * @brief Strongly-typed server configuration.
     *
     * The execution API (POST /workflow) is fail-closed: it requires
     * `Authorization: Bearer <api_token>` when a token is configured, and when
     * no token is configured it is only served if the server is bound to a
     * loopback address.
     */
    struct ServerConfig {
        std::string host = "127.0.0.1";
        uint16_t port = 8070;
        size_t worker_threads = 4;
        std::string api_token;          ///< Bearer token for POST /workflow.
        /// Longest a POST /workflow request may wait (`Prefer: wait=<s>`).
        int64_t max_wait_ms = 60000;

        static ServerConfig from_config(const IConfiguration& config) {
            ServerConfig cfg;
            cfg.host = config.get_string("server.host", "127.0.0.1");
            cfg.port = static_cast<uint16_t>(config.get_int("server.port", 8070));
            const auto workers = config.get_int("server.worker_threads", 4);
            cfg.worker_threads = workers > 0 ? static_cast<size_t>(workers) : 1;
            cfg.api_token = config.get_string("server.api_token", "");
            cfg.max_wait_ms = std::max<int64_t>(0, config.get_int("server.max_wait_ms", 60000));
            return cfg;
        }

        /// True if @p host only accepts connections from this machine.
        [[nodiscard]] static bool is_loopback_host(const std::string& host) {
            return host == "localhost" || host == "::1" || host == "[::1]" ||
                   host.rfind("127.", 0) == 0;
        }

        [[nodiscard]] bool is_loopback() const { return is_loopback_host(host); }
    };

} // namespace Orcha::Config
