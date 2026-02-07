//
// IPluginDiscovery.hpp - Plugin discovery interface
// Created as part of architectural improvements
//

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <cpprest/json.h>
#include "ICommand.hpp"

namespace Orcha::Core {

    /**
     * @struct PluginMetadata
     * @brief Metadata describing a command plugin.
     */
    struct PluginMetadata {
        std::string name;
        std::string version = "1.0.0";
        std::string description;
        std::string author;
        std::vector<std::string> tags;
        std::vector<std::string> dependencies;
        std::vector<CommandParameter> parameters;
        std::filesystem::path library_path;
        std::filesystem::path manifest_path;

        [[nodiscard]] web::json::value to_json() const {
            web::json::value obj;
            obj[U("name")] = web::json::value::string(name);
            obj[U("version")] = web::json::value::string(version);
            obj[U("description")] = web::json::value::string(description);
            if (!author.empty()) {
                obj[U("author")] = web::json::value::string(author);
            }

            if (!tags.empty()) {
                web::json::value arr = web::json::value::array(tags.size());
                for (size_t i = 0; i < tags.size(); ++i) {
                    arr[i] = web::json::value::string(tags[i]);
                }
                obj[U("tags")] = arr;
            }

            if (!parameters.empty()) {
                web::json::value params = web::json::value::array(parameters.size());
                for (size_t i = 0; i < parameters.size(); ++i) {
                    params[i] = parameters[i].to_json();
                }
                obj[U("parameters")] = params;
            }

            return obj;
        }

        static PluginMetadata from_json(const web::json::value& json,
                                        const std::filesystem::path& base_path) {
            PluginMetadata meta;
            if (json.has_field(U("name"))) {
                meta.name = json.at(U("name")).as_string();
            }
            if (json.has_field(U("version"))) {
                meta.version = json.at(U("version")).as_string();
            }
            if (json.has_field(U("description"))) {
                meta.description = json.at(U("description")).as_string();
            }
            if (json.has_field(U("author"))) {
                meta.author = json.at(U("author")).as_string();
            }
            if (json.has_field(U("entry_point"))) {
                meta.library_path = base_path / json.at(U("entry_point")).as_string();
            }
            if (json.has_field(U("tags")) && json.at(U("tags")).is_array()) {
                for (const auto& tag : json.at(U("tags")).as_array()) {
                    meta.tags.push_back(tag.as_string());
                }
            }
            if (json.has_field(U("dependencies")) && json.at(U("dependencies")).is_array()) {
                for (const auto& dep : json.at(U("dependencies")).as_array()) {
                    meta.dependencies.push_back(dep.as_string());
                }
            }
            if (json.has_field(U("parameters")) && json.at(U("parameters")).is_array()) {
                for (const auto& param : json.at(U("parameters")).as_array()) {
                    CommandParameter cp;
                    if (param.has_field(U("name"))) {
                        cp.name = param.at(U("name")).as_string();
                    }
                    if (param.has_field(U("type"))) {
                        cp.type = param.at(U("type")).as_string();
                    }
                    if (param.has_field(U("required"))) {
                        cp.required = param.at(U("required")).as_bool();
                    }
                    if (param.has_field(U("description"))) {
                        cp.description = param.at(U("description")).as_string();
                    }
                    if (param.has_field(U("default"))) {
                        cp.default_value = param.at(U("default")).as_string();
                    }
                    if (param.has_field(U("example"))) {
                        cp.example = param.at(U("example")).as_string();
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
