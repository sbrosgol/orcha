//
// YamlConfiguration.cpp - YAML-based configuration implementation
// Created as part of architectural improvements
//

#include "YamlConfiguration.hpp"
#include <algorithm>
#include <cstdlib>
#include <sstream>

#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char** environ;
#endif

namespace Orcha::Config {

    YamlConfiguration::YamlConfiguration(YAML::Node root)
        : root_(std::move(root)) {}

    bool YamlConfiguration::load_from_file(const std::filesystem::path& config_path) {
        try {
            if (!std::filesystem::exists(config_path)) {
                return false;
            }
            root_ = YAML::LoadFile(config_path.string());
            return true;
        } catch (const YAML::Exception&) {
            return false;
        }
    }

    bool YamlConfiguration::load_from_string(const std::string& yaml_content) {
        try {
            root_ = YAML::Load(yaml_content);
            return true;
        } catch (const YAML::Exception&) {
            return false;
        }
    }

    void YamlConfiguration::merge_environment(const std::string& prefix) {
        // Get all environment variables
        for (char** env = environ; *env != nullptr; ++env) {
            std::string env_var(*env);
            auto eq_pos = env_var.find('=');
            if (eq_pos == std::string::npos) continue;

            std::string name = env_var.substr(0, eq_pos);
            std::string value = env_var.substr(eq_pos + 1);

            // Check if it starts with our prefix
            if (name.rfind(prefix, 0) == 0) {
                // Convert ORCHA_SERVER_PORT -> server.port
                std::string key = name.substr(prefix.length());
                // Replace underscores with dots and convert to lowercase
                std::transform(key.begin(), key.end(), key.begin(), [](char c) {
                    return c == '_' ? '.' : static_cast<char>(std::tolower(c));
                });
                env_overrides_[key] = value;
            }
        }
    }

    YAML::Node YamlConfiguration::navigate_to_key(const std::string& key) const {
        if (key.empty()) {
            return root_;
        }

        // Check environment overrides first
        auto env_it = env_overrides_.find(key);
        if (env_it != env_overrides_.end()) {
            return YAML::Node(env_it->second);
        }

        // Navigate through dot-separated path
        YAML::Node current = root_;
        std::istringstream key_stream(key);
        std::string segment;

        while (std::getline(key_stream, segment, '.')) {
            if (!current.IsMap() || !current[segment]) {
                return YAML::Node();  // Return null node
            }
            current = current[segment];
        }

        return current;
    }

    void YamlConfiguration::set_at_key(const std::string& key, const YAML::Node& value) {
        if (key.empty()) {
            root_ = value;
            return;
        }

        // Navigate/create through dot-separated path
        std::vector<std::string> segments;
        std::istringstream key_stream(key);
        std::string segment;
        while (std::getline(key_stream, segment, '.')) {
            segments.push_back(segment);
        }

        YAML::Node current = root_;
        for (size_t i = 0; i < segments.size() - 1; ++i) {
            if (!current[segments[i]]) {
                current[segments[i]] = YAML::Node(YAML::NodeType::Map);
            }
            current = current[segments[i]];
        }

        current[segments.back()] = value;
    }

    std::optional<std::string> YamlConfiguration::get_string(const std::string& key) const {
        YAML::Node node = navigate_to_key(key);
        if (node && node.IsScalar()) {
            return node.as<std::string>();
        }
        return std::nullopt;
    }

    std::optional<int64_t> YamlConfiguration::get_int(const std::string& key) const {
        YAML::Node node = navigate_to_key(key);
        if (node && node.IsScalar()) {
            try {
                return node.as<int64_t>();
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<bool> YamlConfiguration::get_bool(const std::string& key) const {
        YAML::Node node = navigate_to_key(key);
        if (node && node.IsScalar()) {
            try {
                return node.as<bool>();
            } catch (...) {
                // Try string parsing for "true"/"false"
                std::string val = node.as<std::string>();
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                if (val == "true" || val == "yes" || val == "1") return true;
                if (val == "false" || val == "no" || val == "0") return false;
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<double> YamlConfiguration::get_double(const std::string& key) const {
        YAML::Node node = navigate_to_key(key);
        if (node && node.IsScalar()) {
            try {
                return node.as<double>();
            } catch (...) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::vector<std::string> YamlConfiguration::get_string_list(const std::string& key) const {
        std::vector<std::string> result;
        YAML::Node node = navigate_to_key(key);
        if (node && node.IsSequence()) {
            for (const auto& item : node) {
                if (item.IsScalar()) {
                    result.push_back(item.as<std::string>());
                }
            }
        }
        return result;
    }

    std::unique_ptr<IConfiguration> YamlConfiguration::get_section(const std::string& section) const {
        YAML::Node node = navigate_to_key(section);
        if (node && node.IsMap()) {
            return std::make_unique<YamlConfiguration>(node);
        }
        return std::make_unique<YamlConfiguration>(YAML::Node());
    }

    bool YamlConfiguration::has_key(const std::string& key) const {
        // Check env overrides
        if (env_overrides_.contains(key)) {
            return true;
        }

        YAML::Node node = navigate_to_key(key);
        return node && !node.IsNull();
    }

    std::vector<std::string> YamlConfiguration::get_keys() const {
        std::vector<std::string> keys;
        if (root_.IsMap()) {
            for (const auto& kv : root_) {
                keys.push_back(kv.first.as<std::string>());
            }
        }
        return keys;
    }

    void YamlConfiguration::set_string(const std::string& key, const std::string& value) {
        set_at_key(key, YAML::Node(value));
    }

    void YamlConfiguration::set_int(const std::string& key, int64_t value) {
        set_at_key(key, YAML::Node(value));
    }

    void YamlConfiguration::set_bool(const std::string& key, bool value) {
        set_at_key(key, YAML::Node(value));
    }

    void YamlConfiguration::set_double(const std::string& key, double value) {
        set_at_key(key, YAML::Node(value));
    }

    void YamlConfiguration::merge(const IConfiguration& other) {
        for (const auto& key : other.get_keys()) {
            if (auto val = other.get_string(key)) {
                set_string(key, *val);
            }
        }
    }

    std::unique_ptr<YamlConfiguration> YamlConfiguration::create(
        const std::filesystem::path& config_path,
        const std::string& env_prefix) {

        auto config = std::make_unique<YamlConfiguration>();
        config->load_from_file(config_path);
        config->merge_environment(env_prefix);
        return config;
    }

    std::unique_ptr<YamlConfiguration> YamlConfiguration::create_default() {
        auto config = std::make_unique<YamlConfiguration>();
        config->load_from_string(R"(
server:
  host: "0.0.0.0"
  port: 8070
  worker_threads: 4

logging:
  file: "./logs/orcha.log"
  level: INFO
  console_output: true

plugins:
  directory: "./commands"
  auto_reload: false
  scan_interval_ms: 5000
)");
        config->merge_environment("ORCHA_");
        return config;
    }

} // namespace Orcha::Config
