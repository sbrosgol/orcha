//
// PluginAdminRoute.hpp - Admin API for plugin management (/api/plugins)
// Part of the admin dashboard (Phase 1).
//
// Exposes the existing PluginManager capabilities over HTTP. Because
// PluginManager::unload_plugin() forgets a plugin's library path, the set of
// "available" plugins is recomputed from disk via IPluginDiscovery and diffed
// against the currently-loaded set.
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../HttpJson.hpp"
#include "../../core/PluginManager.hpp"
#include "../../core/IPluginDiscovery.hpp"
#include "../../core/ICommandRegistry.hpp"
#include "../../core/PluginDenylist.hpp"
#include "../../utils/ILogger.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace Orcha::Agent::Routes {

    /**
     * @class PluginAdminRoute
     * @brief Handles plugin administration endpoints under /api/plugins.
     */
    class PluginAdminRoute : public IRouteHandler {
    public:
        PluginAdminRoute(std::shared_ptr<Core::PluginManager> manager,
                         std::shared_ptr<Core::IPluginDiscovery> discovery,
                         std::shared_ptr<Core::ICommandRegistry> registry,
                         std::string plugin_directory,
                         std::chrono::milliseconds watch_interval,
                         std::shared_ptr<Utils::ILogger> logger = nullptr,
                         std::shared_ptr<Core::IPluginDenylist> denylist = nullptr)
            : manager_(std::move(manager))
            , discovery_(std::move(discovery))
            , registry_(std::move(registry))
            , directory_(std::move(plugin_directory))
            , watch_interval_(watch_interval)
            , logger_(std::move(logger))
            , denylist_(std::move(denylist)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            (void)method;
            return path == kPrefix || path_starts_with(path, kPrefix + "/");
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            const std::string& method = request.method;
            const auto segments = split_path(request.path); // e.g. {"api","plugins","foo","reload"}

            // /api/plugins
            if (segments.size() == 2) {
                if (method == "GET") {
                    return serve_list();
                }
                return reply_error(status::MethodNotAllowed,
                                   "Use GET /api/plugins");
            }

            const std::string& resource = segments[2];

            // /api/plugins/_watch
            if (resource == "_watch") {
                if (method == "GET") {
                    return serve_watch_status();
                }
                if (method == "PUT") {
                    return set_watch(request);
                }
                return reply_error(status::MethodNotAllowed,
                                   "Use GET or PUT /api/plugins/_watch");
            }

            // /api/plugins/{name}
            if (segments.size() == 3) {
                if (method == "GET") {
                    return serve_plugin(resource);
                }
                return reply_error(status::MethodNotAllowed,
                                   "Use GET /api/plugins/{name}");
            }

            // /api/plugins/{name}/{action}
            if (segments.size() == 4 && method == "POST") {
                return perform_action(resource, segments[3]);
            }

            return reply_error(status::NotFound, "Unknown plugins endpoint");
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "GET",  .path = "/api/plugins",                 .description = "List loaded and available plugins"},
                {.method = "GET",  .path = "/api/plugins/{name}",          .description = "Get plugin metadata"},
                {.method = "POST", .path = "/api/plugins/{name}/reload",   .description = "Reload a loaded plugin"},
                {.method = "POST", .path = "/api/plugins/{name}/disable",  .description = "Unload a plugin"},
                {.method = "POST", .path = "/api/plugins/{name}/enable",   .description = "Load an available plugin"},
                {.method = "GET",  .path = "/api/plugins/_watch",          .description = "Get directory watcher status"},
                {.method = "PUT",  .path = "/api/plugins/_watch",          .description = "Enable/disable the directory watcher"}
            };
        }

    private:
        inline static const std::string kPrefix = "/api/plugins";

        // ---- Endpoint implementations ----

        HttpResponse serve_list() {
            using value = Orcha::Json;

            auto loaded = manager_->get_all_plugins();
            std::unordered_set<std::string> loaded_names;
            for (const auto& m : loaded) loaded_names.insert(m.name);

            value plugins = value::array();
            size_t idx = 0;

            for (const auto& meta : loaded) {
                plugins[idx++] = plugin_to_json(meta, "loaded");
            }

            // Available-but-not-loaded from disk. Denylisted ones are tagged
            // "disabled" (won't auto-load on restart) vs plain "available".
            for (const auto& meta : discovery_->scan_plugins(directory_)) {
                if (!loaded_names.contains(meta.name)) {
                    const bool denied = denylist_ && denylist_->contains(meta.name);
                    plugins[idx++] = plugin_to_json(meta, denied ? "disabled" : "available");
                }
            }

            // All registered command names. Plugin->command attribution is
            // not tracked today (a plugin's command name often differs from
            // its library/plugin name), so this is surfaced at the top level.
            value commands = value::array();
            if (registry_) {
                auto names = registry_->list_commands();
                for (size_t i = 0; i < names.size(); ++i) {
                    commands[i] = names[i];
                }
            }

            value result = value::object();
            result["directory"] = directory_;
            result["watching"] = manager_->is_watching();
            result["plugins"] = plugins;
            result["count"] = static_cast<int>(idx);
            result["commands"] = commands;

            return reply_json(status::OK, result);
        }

        HttpResponse serve_plugin(const std::string& name) {
            if (auto meta = manager_->get_plugin_metadata(name)) {
                return reply_json(status::OK, plugin_to_json(*meta, "loaded"));
            }
            return reply_error(status::NotFound, "Plugin not loaded: " + name);
        }

        HttpResponse serve_watch_status() {
            Orcha::Json result = Orcha::Json::object();
            result["watching"] = manager_->is_watching();
            return reply_json(status::OK, result);
        }

        HttpResponse set_watch(const HttpRequest& request) {
            bool enabled = false;
            try {
                Orcha::Json body = Orcha::Json::parse(request.body);
                if (body.contains("enabled") && body.at("enabled").is_boolean()) {
                    enabled = body.at("enabled").get<bool>();
                } else {
                    return reply_error(status::BadRequest,
                                       "Body must be { \"enabled\": true|false }");
                }
            } catch (const std::exception& ex) {
                return reply_error(status::BadRequest,
                                   std::string("Invalid JSON body: ") + ex.what());
            }

            if (enabled) {
                manager_->start_watching(directory_, watch_interval_);
            } else {
                manager_->stop_watching();
            }

            Orcha::Json result = Orcha::Json::object();
            result["watching"] = manager_->is_watching();
            return reply_json(status::OK, result);
        }

        HttpResponse perform_action(const std::string& name, const std::string& action) {
            bool ok = false;
            std::string message;

            if (action == "reload") {
                // Reload does NOT touch the denylist.
                ok = manager_->reload_plugin(name);
                message = ok ? "Reloaded " + name
                             : "Reload failed; plugin not loaded: " + name;
            } else if (action == "disable") {
                ok = manager_->unload_plugin(name);
                if (ok && denylist_) denylist_->add(name); // persist: stays off across restarts
                message = ok ? "Disabled " + name
                             : "Disable failed; plugin not loaded: " + name;
            } else if (action == "enable") {
                // Resolve the library path from disk (manager has no record
                // of unloaded plugins).
                std::filesystem::path lib_path;
                for (const auto& meta : discovery_->scan_plugins(directory_)) {
                    if (meta.name == name) {
                        lib_path = meta.library_path;
                        break;
                    }
                }
                if (lib_path.empty()) {
                    return reply_error(status::NotFound,
                                       "No available plugin named: " + name);
                }
                if (denylist_) denylist_->remove(name); // clear persisted disable
                ok = manager_->load_plugin(lib_path);
                message = ok ? "Enabled " + name
                             : "Enable failed (already loaded or load error): " + name;
            } else {
                return reply_error(status::NotFound, "Unknown action: " + action);
            }

            if (!ok && logger_) {
                logger_->warn("Plugin admin action '" + action + "' failed for " + name);
            }

            Orcha::Json result = Orcha::Json::object();
            result["success"] = ok;
            result["message"] = message;
            return reply_json(ok ? status::OK : status::Conflict, result);
        }

        // ---- Helpers ----

        static Orcha::Json plugin_to_json(
            const Core::PluginMetadata& meta,
            const std::string& status) {

            Orcha::Json obj = meta.to_json(); // name/version/description/author/tags/parameters
            obj["status"] = status;
            obj["library_path"] = meta.library_path.string();

            Orcha::Json deps = Orcha::Json::array();
            for (size_t i = 0; i < meta.dependencies.size(); ++i) {
                deps[i] = meta.dependencies[i];
            }
            obj["dependencies"] = deps;

            return obj;
        }

        std::shared_ptr<Core::PluginManager> manager_;
        std::shared_ptr<Core::IPluginDiscovery> discovery_;
        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::string directory_;
        std::chrono::milliseconds watch_interval_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::shared_ptr<Core::IPluginDenylist> denylist_;
    };

} // namespace Orcha::Agent::Routes
