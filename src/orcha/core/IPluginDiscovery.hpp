//
// IPluginDiscovery.hpp - Plugin discovery interface
// Created as part of architectural improvements
//

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include "Json.hpp"
#include "ICommand.hpp"

namespace Orcha::Core {

    /**
     * @struct PluginMetadata
     * @brief Metadata describing a command plugin.
     */
    struct PluginMetadata {
        std::string name;
        std::string version = kVersion;
        std::string description;
        std::string author;
        std::vector<std::string> tags;
        std::vector<std::string> dependencies;
        std::vector<CommandParameter> parameters;
        std::filesystem::path library_path;
        std::filesystem::path manifest_path;

        [[nodiscard]] Json to_json() const {
            Json obj = Json::object();
            obj["name"] = name;
            obj["version"] = version;
            obj["description"] = description;
            if (!author.empty()) {
                obj["author"] = author;
            }

            if (!tags.empty()) {
                Json arr = Json::array();
                for (const auto& tag : tags) {
                    arr.push_back(tag);
                }
                obj["tags"] = arr;
            }

            if (!parameters.empty()) {
                Json params = Json::array();
                for (const auto& param : parameters) {
                    params.push_back(param.to_json());
                }
                obj["parameters"] = params;
            }

            return obj;
        }

        static PluginMetadata from_json(const Json& json,
                                        const std::filesystem::path& base_path) {
            PluginMetadata meta;
            if (json.contains("name")) {
                meta.name = json.at("name").get<std::string>();
            }
            if (json.contains("version")) {
                meta.version = json.at("version").get<std::string>();
            }
            if (json.contains("description")) {
                meta.description = json.at("description").get<std::string>();
            }
            if (json.contains("author")) {
                meta.author = json.at("author").get<std::string>();
            }
            if (json.contains("entry_point")) {
                meta.library_path = base_path / json.at("entry_point").get<std::string>();
            }
            if (json.contains("tags") && json.at("tags").is_array()) {
                for (const auto& tag : json.at("tags")) {
                    meta.tags.push_back(tag.get<std::string>());
                }
            }
            if (json.contains("dependencies") && json.at("dependencies").is_array()) {
                for (const auto& dep : json.at("dependencies")) {
                    meta.dependencies.push_back(dep.get<std::string>());
                }
            }
            if (json.contains("parameters") && json.at("parameters").is_array()) {
                for (const auto& param : json.at("parameters")) {
                    CommandParameter cp;
                    if (param.contains("name")) {
                        cp.name = param.at("name").get<std::string>();
                    }
                    if (param.contains("type")) {
                        cp.type = param.at("type").get<std::string>();
                    }
                    if (param.contains("required")) {
                        cp.required = param.at("required").get<bool>();
                    }
                    if (param.contains("description")) {
                        cp.description = param.at("description").get<std::string>();
                    }
                    if (param.contains("default")) {
                        cp.default_value = param.at("default").get<std::string>();
                    }
                    if (param.contains("example")) {
                        cp.example = param.at("example").get<std::string>();
                    }
                    meta.parameters.push_back(cp);
                }
            }
            return meta;
        }
    };

    /**
     * @interface IPluginDiscovery
     * @brief Interface for discovering and loading plugin metadata.
     */
    class IPluginDiscovery {
    public:
        virtual ~IPluginDiscovery() = default;

        /**
         * @brief Scan a directory for plugins.
         * @param directory Directory to scan.
         * @return Vector of discovered plugin metadata.
         */
        [[nodiscard]] virtual std::vector<PluginMetadata> scan_plugins(
            const std::filesystem::path& directory) const = 0;

        /**
         * @brief Get metadata for a specific plugin library.
         * @param library_path Path to the plugin library.
         * @return Plugin metadata if found.
         */
        [[nodiscard]] virtual std::optional<PluginMetadata> get_plugin_info(
            const std::filesystem::path& library_path) const = 0;

        /**
         * @brief Get platform-specific library extension.
         */
        [[nodiscard]] static std::string get_library_extension() {
#if defined(_WIN32)
            return ".dll";
#elif defined(__APPLE__)
            return ".dylib";
#else
            return ".so";
#endif
        }
    };

} // namespace Orcha::Core
