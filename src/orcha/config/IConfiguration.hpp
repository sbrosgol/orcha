//
// IConfiguration.hpp - Configuration interface
// Created as part of architectural improvements
//

#pragma once

#include <string>
#include <optional>
#include <vector>
#include <memory>
#include <cstdint>

namespace Orcha::Config {

    /**
     * @interface IConfiguration
     * @brief Abstract interface for application configuration.
     *
     * Provides a unified way to access configuration values from
     * various sources (YAML files, environment variables, etc.).
     */
    class IConfiguration {
    public:
        virtual ~IConfiguration() = default;

        // String values
        [[nodiscard]] virtual std::optional<std::string> get_string(const std::string& key) const = 0;

        [[nodiscard]] virtual std::string get_string(const std::string& key,
                                                     const std::string& default_val) const {
            return get_string(key).value_or(default_val);
        }

        // Integer values
        [[nodiscard]] virtual std::optional<int64_t> get_int(const std::string& key) const = 0;

        [[nodiscard]] virtual int64_t get_int(const std::string& key, int64_t default_val) const {
            return get_int(key).value_or(default_val);
        }

        // Boolean values
        [[nodiscard]] virtual std::optional<bool> get_bool(const std::string& key) const = 0;

        [[nodiscard]] virtual bool get_bool(const std::string& key, bool default_val) const {
            return get_bool(key).value_or(default_val);
        }

        // Double values
        [[nodiscard]] virtual std::optional<double> get_double(const std::string& key) const = 0;

        [[nodiscard]] virtual double get_double(const std::string& key, double default_val) const {
            return get_double(key).value_or(default_val);
        }

        // Array of strings
        [[nodiscard]] virtual std::vector<std::string> get_string_list(const std::string& key) const = 0;

        // Sections (nested configuration)
        [[nodiscard]] virtual std::unique_ptr<IConfiguration> get_section(const std::string& section) const = 0;

        // Check if key exists
        [[nodiscard]] virtual bool has_key(const std::string& key) const = 0;

        // Get all keys at current level
        [[nodiscard]] virtual std::vector<std::string> get_keys() const = 0;
    };

    /**
     * @interface IMutableConfiguration
     * @brief Extended interface allowing configuration modification.
     */
    class IMutableConfiguration : public IConfiguration {
    public:
        virtual void set_string(const std::string& key, const std::string& value) = 0;
        virtual void set_int(const std::string& key, int64_t value) = 0;
        virtual void set_bool(const std::string& key, bool value) = 0;
        virtual void set_double(const std::string& key, double value) = 0;

        // Merge another configuration (for overlay patterns)
        virtual void merge(const IConfiguration& other) = 0;
    };

} // namespace Orcha::Config
