//
// YamlConfiguration.hpp - YAML-based configuration implementation
// Created as part of architectural improvements
//

#pragma once

#include "IConfiguration.hpp"
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <unordered_map>

namespace Orcha::Config {

    /**
     * @class YamlConfiguration
     * @brief Configuration implementation using YAML files.
     *
     * Supports:
     * - Loading from YAML files
     * - Environment variable overrides
     * - Dot-notation key access (e.g., "server.port")
     * - Section extraction
     */
    class YamlConfiguration : public IMutableConfiguration {
    public:
        YamlConfiguration() = default;

        /**
         * @brief Load configuration from YAML file.
         * @param config_path Path to YAML configuration file.
         * @return True if loading succeeded.
         */
        bool load_from_file(const std::filesystem::path& config_path);

        /**
         * @brief Load configuration from YAML string.
         * @param yaml_content YAML content as string.
         * @return True if parsing succeeded.
         */
        bool load_from_string(const std::string& yaml_content);

        /**
         * @brief Merge environment variables with optional prefix.
         *
         * Environment variables are mapped to configuration keys:
         * - ORCHA_SERVER_PORT -> server.port
         * - ORCHA_LOGGING_LEVEL -> logging.level
         *
         * @param prefix Environment variable prefix (default: "ORCHA_").
         */
        void merge_environment(const std::string& prefix = "ORCHA_");

        // IConfiguration interface
        [[nodiscard]] std::optional<std::string> get_string(const std::string& key) const override;
        [[nodiscard]] std::optional<int64_t> get_int(const std::string& key) const override;
        [[nodiscard]] std::optional<bool> get_bool(const std::string& key) const override;
        [[nodiscard]] std::optional<double> get_double(const std::string& key) const override;
        [[nodiscard]] std::vector<std::string> get_string_list(const std::string& key) const override;
        [[nodiscard]] std::unique_ptr<IConfiguration> get_section(const std::string& section) const override;
        [[nodiscard]] bool has_key(const std::string& key) const override;
        [[nodiscard]] std::vector<std::string> get_keys() const override;

        // IMutableConfiguration interface
        void set_string(const std::string& key, const std::string& value) override;
        void set_int(const std::string& key, int64_t value) override;
        void set_bool(const std::string& key, bool value) override;
        void set_double(const std::string& key, double value) override;
        void merge(const IConfiguration& other) override;

        /**
         * @brief Factory method to create from file with environment overlay.
         */
        static std::unique_ptr<YamlConfiguration> create(
            const std::filesystem::path& config_path,
            const std::string& env_prefix = "ORCHA_");

        /**
         * @brief Create default configuration.
         */
        static std::unique_ptr<YamlConfiguration> create_default();

    public:
        // Used internally for get_section - consider this internal API
        explicit YamlConfiguration(YAML::Node root);

    private:

        [[nodiscard]] YAML::Node navigate_to_key(const std::string& key) const;
        void set_at_key(const std::string& key, const YAML::Node& value);

        YAML::Node root_;
        std::unordered_map<std::string, std::string> env_overrides_;
    };

    /**
     * @struct ServerConfig
     * @brief Strongly-typed server configuration.
     */
    struct ServerConfig {
        std::string host = "0.0.0.0";
        uint16_t port = 8070;
        size_t worker_threads = 4;

        static ServerConfig from_config(const IConfiguration& config) {
            ServerConfig cfg;
            cfg.host = config.get_string("server.host", "0.0.0.0");
            cfg.port = static_cast<uint16_t>(config.get_int("server.port", 8070));
            cfg.worker_threads = static_cast<size_t>(config.get_int("server.worker_threads", 4));
            return cfg;
        }
    };

    /**
     * @struct LoggingConfig
     * @brief Strongly-typed logging configuration.
     */
    struct LoggingConfig {
        std::string file = "./logs/orcha.log";
        std::string level = "INFO";
        bool console_output = true;

        static LoggingConfig from_config(const IConfiguration& config) {
            LoggingConfig cfg;
            cfg.file = config.get_string("logging.file", "./logs/orcha.log");
            cfg.level = config.get_string("logging.level", "INFO");
            cfg.console_output = config.get_bool("logging.console_output", true);
            return cfg;
        }
    };

    /**
     * @struct PluginConfig
     * @brief Strongly-typed plugin configuration.
     */
    struct PluginConfig {
        std::string directory = "./commands";
        bool auto_reload = false;
        int64_t scan_interval_ms = 5000;

        static PluginConfig from_config(const IConfiguration& config) {
            PluginConfig cfg;
            cfg.directory = config.get_string("plugins.directory", "./commands");
            cfg.auto_reload = config.get_bool("plugins.auto_reload", false);
            cfg.scan_interval_ms = config.get_int("plugins.scan_interval_ms", 5000);
            return cfg;
        }
    };

} // namespace Orcha::Config
