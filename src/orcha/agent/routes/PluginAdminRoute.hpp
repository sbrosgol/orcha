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
#include "../../core/PluginManager.hpp"
#include "../../core/IPluginDiscovery.hpp"
#include "../../core/ICommandRegistry.hpp"
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
                         std::shared_ptr<Utils::ILogger> logger = nullptr)
            : manager_(std::move(manager))
            , discovery_(std::move(discovery))
            , registry_(std::move(registry))
            , directory_(std::move(plugin_directory))
            , watch_interval_(watch_interval)
            , logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method,
            const std::string& path) const override {
            (void)method;
            return path == kPrefix || path_starts_with(path, kPrefix + "/");
        }

        void handle(web::http::http_request request) override {
            const auto method = utility::conversions::to_utf8string(request.method());
            const auto path = utility::conversions::to_utf8string(request.request_uri().path());
            const auto segments = split_path(path); // e.g. {"api","plugins","foo","reload"}

            // /api/plugins
            if (segments.size() == 2) {
                if (method == "GET") {
                    return serve_list(request);
                }
                return reply_error(request, web::http::status_codes::MethodNotAllowed,
                                   "Use GET /api/plugins");
            }

            const std::string& resource = segments[2];

            // /api/plugins/_watch
            if (resource == "_watch") {
                if (method == "GET") {
                    return serve_watch_status(request);
                }
                if (method == "PUT") {
                    return set_watch(request);
                }
                return reply_error(request, web::http::status_codes::MethodNotAllowed,
                                   "Use GET or PUT /api/plugins/_watch");
            }

            // /api/plugins/{name}
            if (segments.size() == 3) {
                if (method == "GET") {
                    return serve_plugin(request, resource);
                }
                return reply_error(request, web::http::status_codes::MethodNotAllowed,
                                   "Use GET /api/plugins/{name}");
            }

            // /api/plugins/{name}/{action}
            if (segments.size() == 4 && method == "POST") {
                return perform_action(request, resource, segments[3]);
            }

            reply_error(request, web::http::status_codes::NotFound, "Unknown plugins endpoint");
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

        void serve_list(web::http::http_request request) {
            auto manager = manager_;
            auto discovery = discovery_;
            auto registry = registry_;
            auto directory = directory_;

            pplx::create_task([=]() {
                using web::json::value;

                auto loaded = manager->get_all_plugins();
                std::unordered_set<std::string> loaded_names;
                for (const auto& m : loaded) loaded_names.insert(m.name);

                value plugins = value::array();
                size_t idx = 0;

                for (const auto& meta : loaded) {
                    plugins[idx++] = plugin_to_json(meta, "loaded");
                }

                // Available-but-not-loaded from disk.
                for (const auto& meta : discovery->scan_plugins(directory)) {
                    if (!loaded_names.contains(meta.name)) {
                        plugins[idx++] = plugin_to_json(meta, "available");
                    }
                }

                // All registered command names. Plugin->command attribution is
                // not tracked today (a plugin's command name often differs from
                // its library/plugin name), so this is surfaced at the top level.
                value commands = value::array();
                if (registry) {
                    auto names = registry->list_commands();
                    for (size_t i = 0; i < names.size(); ++i) {
                        commands[i] = value::string(
                            utility::conversions::to_string_t(names[i]));
                    }
                }

                value result = value::object();
                result[U("directory")] = value::string(utility::conversions::to_string_t(directory));
                result[U("watching")] = value::boolean(manager->is_watching());
                result[U("plugins")] = plugins;
                result[U("count")] = value::number(static_cast<int>(idx));
                result[U("commands")] = commands;

                reply_json(request, web::http::status_codes::OK, result);
            });
        }

        void serve_plugin(web::http::http_request request, std::string name) {
            auto manager = manager_;

            pplx::create_task([=]() {
                if (auto meta = manager->get_plugin_metadata(name)) {
                    reply_json(request, web::http::status_codes::OK,
                               plugin_to_json(*meta, "loaded"));
                } else {
                    reply_error(request, web::http::status_codes::NotFound,
                                "Plugin not loaded: " + name);
                }
            });
        }

        void serve_watch_status(web::http::http_request request) {
            auto manager = manager_;
            pplx::create_task([=]() {
                web::json::value result = web::json::value::object();
                result[U("watching")] = web::json::value::boolean(manager->is_watching());
                reply_json(request, web::http::status_codes::OK, result);
            });
        }

        void set_watch(web::http::http_request request) {
            auto manager = manager_;
            auto directory = directory_;
            auto interval = watch_interval_;
            auto logger = logger_;

            request.extract_json().then([=](pplx::task<web::json::value> body_task) {
                bool enabled = false;
                try {
                    auto body = body_task.get();
                    if (body.has_field(U("enabled")) && body.at(U("enabled")).is_boolean()) {
                        enabled = body.at(U("enabled")).as_bool();
                    } else {
                        return reply_error(request, web::http::status_codes::BadRequest,
                                           "Body must be { \"enabled\": true|false }");
                    }
                } catch (const std::exception& ex) {
                    return reply_error(request, web::http::status_codes::BadRequest,
                                       std::string("Invalid JSON body: ") + ex.what());
                }

                if (enabled) {
                    manager->start_watching(directory, interval);
                } else {
                    manager->stop_watching();
                }

                web::json::value result = web::json::value::object();
                result[U("watching")] = web::json::value::boolean(manager->is_watching());
                reply_json(request, web::http::status_codes::OK, result);
            });
        }

        void perform_action(web::http::http_request request,
                             std::string name, std::string action) {
            auto manager = manager_;
            auto discovery = discovery_;
            auto directory = directory_;
            auto logger = logger_;

            pplx::create_task([=]() {
                bool ok = false;
                std::string message;

                if (action == "reload") {
                    ok = manager->reload_plugin(name);
                    message = ok ? "Reloaded " + name
                                 : "Reload failed; plugin not loaded: " + name;
                } else if (action == "disable") {
                    ok = manager->unload_plugin(name);
                    message = ok ? "Disabled " + name
                                 : "Disable failed; plugin not loaded: " + name;
                } else if (action == "enable") {
                    // Resolve the library path from disk (manager has no record
                    // of unloaded plugins).
                    std::filesystem::path lib_path;
                    for (const auto& meta : discovery->scan_plugins(directory)) {
                        if (meta.name == name) {
                            lib_path = meta.library_path;
                            break;
                        }
                    }
                    if (lib_path.empty()) {
                        return reply_error(request, web::http::status_codes::NotFound,
                                           "No available plugin named: " + name);
                    }
                    ok = manager->load_plugin(lib_path);
                    message = ok ? "Enabled " + name
                                 : "Enable failed (already loaded or load error): " + name;
                } else {
                    return reply_error(request, web::http::status_codes::NotFound,
                                       "Unknown action: " + action);
                }

                if (!ok && logger) {
                    logger->warn("Plugin admin action '" + action + "' failed for " + name);
                }

                web::json::value result = web::json::value::object();
                result[U("success")] = web::json::value::boolean(ok);
                result[U("message")] = web::json::value::string(
                    utility::conversions::to_string_t(message));
                reply_json(request,
                           ok ? web::http::status_codes::OK
                              : web::http::status_codes::Conflict,
                           result);
            });
        }

        // ---- Helpers ----

        static web::json::value plugin_to_json(
            const Core::PluginMetadata& meta,
            const std::string& status) {

            web::json::value obj = meta.to_json(); // name/version/description/author/tags/parameters
            obj[U("status")] = web::json::value::string(
                utility::conversions::to_string_t(status));
            obj[U("library_path")] = web::json::value::string(
                utility::conversions::to_string_t(meta.library_path.string()));

            web::json::value deps = web::json::value::array(meta.dependencies.size());
            for (size_t i = 0; i < meta.dependencies.size(); ++i) {
                deps[i] = web::json::value::string(
                    utility::conversions::to_string_t(meta.dependencies[i]));
            }
            obj[U("dependencies")] = deps;

            return obj;
        }

        static void reply_json(web::http::http_request request,
                               web::http::status_code code,
                               const web::json::value& body) {
            web::http::http_response resp(code);
            resp.headers().add(web::http::header_names::content_type, U("application/json"));
            resp.set_body(body);
            request.reply(resp);
        }

        static void reply_error(web::http::http_request request,
                                web::http::status_code code,
                                const std::string& message) {
            web::json::value body = web::json::value::object();
            body[U("error")] = web::json::value::string(
                utility::conversions::to_string_t(message));
            reply_json(request, code, body);
        }

        std::shared_ptr<Core::PluginManager> manager_;
        std::shared_ptr<Core::IPluginDiscovery> discovery_;
        std::shared_ptr<Core::ICommandRegistry> registry_;
        std::string directory_;
        std::chrono::milliseconds watch_interval_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
