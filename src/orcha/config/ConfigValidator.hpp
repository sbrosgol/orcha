//
// ConfigValidator.hpp - Configuration validation at startup
//

#pragma once

#include "IConfiguration.hpp"
#include <vector>
#include <string>
#include <functional>
#include <regex>

namespace Orcha::Config {

    /**
     * @struct ValidationIssue
     * @brief Represents a single configuration validation issue.
     */
    struct ValidationIssue {
        enum class Severity { Error, Warning };

        Severity severity;
        std::string key;
        std::string message;
        std::optional<std::string> suggestion;

        [[nodiscard]] std::string to_string() const {
            std::string result = (severity == Severity::Error ? "[ERROR] " : "[WARN] ");
            result += key + ": " + message;
            if (suggestion) {
                result += " (suggestion: " + *suggestion + ")";
            }
            return result;
        }
    };

    /**
     * @struct ValidationResult
     * @brief Result of configuration validation.
     */
    struct ValidationResult {
        bool valid = true;
        std::vector<ValidationIssue> issues;

        void add_error(const std::string& key, const std::string& message,
                      const std::optional<std::string>& suggestion = std::nullopt) {
            valid = false;
            issues.push_back({ValidationIssue::Severity::Error, key, message, suggestion});
        }

        void add_warning(const std::string& key, const std::string& message,
                        const std::optional<std::string>& suggestion = std::nullopt) {
            issues.push_back({ValidationIssue::Severity::Warning, key, message, suggestion});
        }

        [[nodiscard]] std::vector<std::string> get_error_messages() const {
            std::vector<std::string> errors;
            for (const auto& issue : issues) {
                if (issue.severity == ValidationIssue::Severity::Error) {
                    errors.push_back(issue.to_string());
                }
            }
            return errors;
        }

        [[nodiscard]] std::vector<std::string> get_warning_messages() const {
            std::vector<std::string> warnings;
            for (const auto& issue : issues) {
                if (issue.severity == ValidationIssue::Severity::Warning) {
                    warnings.push_back(issue.to_string());
                }
            }
            return warnings;
        }
    };

    /**
     * @class ConfigValidator
     * @brief Validates application configuration at startup.
     *
     * Provides comprehensive validation with detailed error messages,
     * suggestions, and support for custom validation rules.
     */
    class ConfigValidator {
    public:
        using CustomValidator = std::function<void(const IConfiguration&, ValidationResult&)>;

        /**
         * @brief Validate the complete configuration.
         * @param config Configuration to validate.
         * @return Validation result with any issues found.
         */
        [[nodiscard]] ValidationResult validate(const IConfiguration& config) const {
            ValidationResult result;

            validate_server_config(config, result);
            validate_plugin_config(config, result);
            validate_logging_config(config, result);
            validate_workflow_config(config, result);

            // Run custom validators
            for (const auto& validator : custom_validators_) {
                validator(config, result);
            }

            return result;
        }

        /**
         * @brief Add a custom validation rule.
         */
        void add_validator(CustomValidator validator) {
            custom_validators_.push_back(std::move(validator));
        }

        /**
         * @brief Validate and throw if invalid.
         */
        void validate_or_throw(const IConfiguration& config) const {
            auto result = validate(config);
            if (!result.valid) {
                std::string message = "Configuration validation failed:\n";
                for (const auto& error : result.get_error_messages()) {
                    message += "  " + error + "\n";
                }
                throw std::runtime_error(message);
            }
        }

    private:
        static void validate_server_config(const IConfiguration& config, ValidationResult& result) {
            // Server port validation
            if (auto port = config.get_int("server.port")) {
                if (*port < 1 || *port > 65535) {
                    result.add_error("server.port",
                        "Port must be between 1 and 65535, got: " + std::to_string(*port),
                        "Use a port like 8080 or 8070");
                }
                if (*port < 1024) {
                    result.add_warning("server.port",
                        "Port " + std::to_string(*port) + " requires root privileges",
                        "Consider using a port >= 1024");
                }
            }

            // Server host validation
            if (const auto host = config.get_string("server.host")) {
                if (host->empty()) {
                    result.add_error("server.host",
                        "Server host cannot be empty",
                        "Use '0.0.0.0' for all interfaces or '127.0.0.1' for localhost");
                }
                // Basic IP/hostname validation
                // Thread-safe per C++11 [stmt.dcl]/4; compiled once on first call
                static const std::regex ip_regex(
                    R"(^(\d{1,3}\.){3}\d{1,3}$|^localhost$|^[\w\-\.]+$)");
                if (!std::regex_match(*host, ip_regex)) {
                    result.add_warning("server.host",
                        "Host '" + *host + "' may not be a valid hostname or IP");
                }
            }

            // Worker threads validation
            if (const auto workers = config.get_int("server.worker_threads")) {
                if (*workers < 1) {
                    result.add_error("server.worker_threads",
                        "Worker threads must be at least 1",
                        "Use a value between 1 and number of CPU cores");
                }
                if (*workers > 64) {
                    result.add_warning("server.worker_threads",
                        "High thread count (" + std::to_string(*workers) + ") may cause performance issues",
                        "Consider using fewer threads");
                }
            }
        }

        static void validate_plugin_config(const IConfiguration& config, ValidationResult& result) {
            // Plugin directory validation
            if (auto dir = config.get_string("plugins.directory")) {
                if (dir->empty()) {
                    result.add_error("plugins.directory",
                        "Plugin directory cannot be empty",
                        "Use a relative path like 'commands' or absolute path");
                }
                // Check for potentially dangerous paths
                if (dir->find("..") != std::string::npos) {
                    result.add_warning("plugins.directory",
                        "Plugin directory contains '..', ensure this is intentional");
                }
            }

            // Auto-reload validation
            if (config.get_bool("plugins.auto_reload", false)) {
                auto interval = config.get_int("plugins.scan_interval_ms", 5000);
                if (interval < 1000) {
                    result.add_warning("plugins.scan_interval_ms",
                        "Very short scan interval (" + std::to_string(interval) + "ms) may impact performance",
                        "Use at least 1000ms (1 second)");
                }
            }
        }

        static void validate_logging_config(const IConfiguration& config, ValidationResult& result) {
            // Log level validation
            if (auto level = config.get_string("logging.level")) {
                static const std::vector<std::string> valid_levels =
                    {"trace", "debug", "info", "warn", "warning", "error", "fatal", "off"};

                std::string lower_level = *level;
                std::transform(lower_level.begin(), lower_level.end(), lower_level.begin(),
                    [](unsigned char c) { return std::tolower(c); });

                bool valid = false;
                for (const auto& valid_level : valid_levels) {
                    if (lower_level == valid_level) {
                        valid = true;
                        break;
                    }
                }

                if (!valid) {
                    result.add_error("logging.level",
                        "Invalid log level: '" + *level + "'",
                        "Use one of: trace, debug, info, warn, error, fatal, off");
                }
            }

            // Log file path validation
            if (auto file = config.get_string("logging.file")) {
                if (file->empty()) {
                    result.add_warning("logging.file",
                        "Empty log file path, logs will only go to console");
                }
            }
        }

        static void validate_workflow_config(const IConfiguration& config, ValidationResult& result) {
            // Default timeout validation
            if (auto timeout = config.get_int("workflow.default_timeout_ms")) {
                if (*timeout < 0) {
                    result.add_error("workflow.default_timeout_ms",
                        "Timeout cannot be negative",
                        "Use 0 for no timeout or a positive value in milliseconds");
                }
                if (*timeout > 0 && *timeout < 100) {
                    result.add_warning("workflow.default_timeout_ms",
                        "Very short timeout (" + std::to_string(*timeout) + "ms) may cause frequent failures");
                }
            }

            // Run queue (every top-level run is admitted through it)
            if (auto v = config.get_int("workflow.max_concurrent_runs")) {
                if (*v < 1) {
                    result.add_error("workflow.max_concurrent_runs", "Must be at least 1");
                } else if (*v > 256) {
                    result.add_warning("workflow.max_concurrent_runs",
                        "Each concurrent run uses its own thread (" + std::to_string(*v) + " configured)",
                        "Runs mostly wait on child processes and network; a few dozen is usually plenty");
                }
            }
            if (auto v = config.get_int("jobs.max_runs"); v && *v < 0) {
                result.add_error("jobs.max_runs", "Cannot be negative",
                                 "Use 0 to keep every run");
            }
            if (auto v = config.get_int("workflow.max_queued_runs"); v && *v < 0) {
                result.add_error("workflow.max_queued_runs", "Cannot be negative",
                                 "Use 0 to reject runs whenever every runner is busy");
            }
            if (auto v = config.get_int("server.max_wait_ms"); v && *v < 0) {
                result.add_error("server.max_wait_ms", "Cannot be negative",
                                 "Use 0 to never wait (POST /workflow always answers 202)");
            }

            // Circuit breaker thresholds (workflows opt in with "circuit_breaker": true)
            for (const char* key : {"workflow.circuit_breaker.failure_threshold",
                                    "workflow.circuit_breaker.success_threshold"}) {
                if (auto value = config.get_int(key); value && *value < 1) {
                    result.add_error(key, "Must be at least 1");
                }
            }
            if (auto reset = config.get_int("workflow.circuit_breaker.reset_timeout_ms");
                reset && *reset < 0) {
                result.add_error("workflow.circuit_breaker.reset_timeout_ms",
                    "Cannot be negative", "Use the time in ms an open circuit waits before probing");
            }
        }

        std::vector<CustomValidator> custom_validators_;
    };

} // namespace Orcha::Config
