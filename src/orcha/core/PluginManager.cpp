//
// PluginManager.cpp - Plugin lifecycle management implementation
// Created as part of architectural improvements
//

#include "PluginManager.hpp"
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace Orcha::Core {

    // ============================================================================
    // PluginDiscoveryService Implementation
    // ============================================================================

    std::vector<PluginMetadata> PluginDiscoveryService::scan_plugins(
        const std::filesystem::path& directory) const {

        std::vector<PluginMetadata> plugins;

        if (!std::filesystem::exists(directory)) {
            return plugins;
        }

        const std::string ext = get_library_extension();

        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            // Check for manifest.json first
            if (entry.path().filename() == "manifest.json") {
                if (auto meta = load_manifest(entry.path())) {
                    plugins.push_back(*meta);
                }
            }
            // Or check for library files
            else if (entry.path().extension() == ext) {
                // Check if there's a manifest in the same directory
                auto manifest_path = entry.path().parent_path() / "manifest.json";
                if (!std::filesystem::exists(manifest_path)) {
                    // Create basic metadata from library
                    plugins.push_back(create_basic_metadata(entry.path()));
                }
            }
        }

        return plugins;
    }

    std::optional<PluginMetadata> PluginDiscoveryService::get_plugin_info(
        const std::filesystem::path& library_path) const {

        // Check for manifest in same directory
        auto manifest_path = library_path.parent_path() / "manifest.json";
        if (std::filesystem::exists(manifest_path)) {
            return load_manifest(manifest_path);
        }

        // Create basic metadata
        if (std::filesystem::exists(library_path)) {
            return create_basic_metadata(library_path);
        }

        return std::nullopt;
    }

    std::optional<PluginMetadata> PluginDiscoveryService::load_manifest(
        const std::filesystem::path& manifest_path) const {

        try {
            std::ifstream file(manifest_path);
            if (!file.is_open()) {
                return std::nullopt;
            }

            std::stringstream buffer;
            buffer << file.rdbuf();

            auto json = web::json::value::parse(buffer.str());
            auto meta = PluginMetadata::from_json(json, manifest_path.parent_path());
            meta.manifest_path = manifest_path;

            // If library_path not set, try to find it
            if (meta.library_path.empty()) {
                const std::string ext = get_library_extension();
                for (const auto& entry : std::filesystem::directory_iterator(manifest_path.parent_path())) {
                    if (entry.path().extension() == ext) {
                        meta.library_path = entry.path();
                        break;
                    }
                }
            }

            return meta;
        } catch (...) {
            return std::nullopt;
        }
    }

    PluginMetadata PluginDiscoveryService::create_basic_metadata(
        const std::filesystem::path& library_path) const {

        PluginMetadata meta;
        meta.library_path = library_path;

        // Extract name from library filename
        std::string filename = library_path.stem().string();

        // Remove common prefixes
        if (filename.rfind("lib", 0) == 0) {
            filename = filename.substr(3);
        }

        meta.name = filename;
        meta.description = "Plugin loaded from " + library_path.filename().string();

        return meta;
    }

    // ============================================================================
    // PluginManager Implementation
    // ============================================================================

    PluginManager::PluginManager(std::shared_ptr<IMutableCommandRegistry> registry,
                                 std::shared_ptr<IPluginDiscovery> discovery,
                                 std::shared_ptr<Utils::ILogger> logger)
        : registry_(std::move(registry))
        , discovery_(std::move(discovery))
        , logger_(std::move(logger)) {}

    PluginManager::~PluginManager() {
        stop_watching();
    }

    PluginManager::LoadResult PluginManager::load_plugins_from_directory(
        const std::filesystem::path& directory,
        bool strict_dependencies) {

        LoadResult result;

        // Scan for all plugins
        auto plugins = discovery_->scan_plugins(directory);

        if (plugins.empty()) {
            logger_->info("No plugins found in directory: " + directory.string());
            return result;
        }

        logger_->info("Discovered " + std::to_string(plugins.size()) +
                     " plugin(s) in " + directory.string());

        // Resolve dependencies and get load order
        auto resolve_result = DependencyResolver::resolve(plugins, strict_dependencies);

        if (resolve_result.is_err()) {
            const auto& error = resolve_result.error();
            result.dependency_error = error;

            logger_->error("Dependency resolution failed: " + error.message);

            if (error.type == DependencyError::Type::CircularDependency) {
                // Cannot proceed with circular dependencies
                return result;
            }

            // For missing dependencies in strict mode, we already returned error
            // In non-strict mode, resolve() would have succeeded
        }

        // Load plugins in dependency order
        const auto& ordered_plugins = resolve_result.value();

        logger_->info("Loading plugins in dependency order:");
        for (const auto& plugin : ordered_plugins) {
            std::string deps_str;
            if (!plugin.dependencies.empty()) {
                deps_str = " (depends on: ";
                for (size_t i = 0; i < plugin.dependencies.size(); ++i) {
                    if (i > 0) deps_str += ", ";
                    deps_str += plugin.dependencies[i];
                }
                deps_str += ")";
            }
            logger_->info("  -> " + plugin.name + deps_str);
        }

        for (const auto& plugin : ordered_plugins) {
            result.load_order.push_back(plugin.name);

            if (load_plugin(plugin.library_path)) {
                result.loaded_count++;
            } else {
                result.failed_count++;
                logger_->error("Failed to load plugin: " + plugin.name);
            }
        }

        logger_->info("Plugin loading complete: " +
                     std::to_string(result.loaded_count) + " loaded, " +
                     std::to_string(result.failed_count) + " failed");

        return result;
    }

    size_t PluginManager::load_plugins_from_directory_legacy(const std::filesystem::path& directory) {
        auto result = load_plugins_from_directory(directory, false);
        return result.loaded_count;
    }

    bool PluginManager::load_plugin(const std::filesystem::path& library_path) {
        // Get metadata
        auto meta_opt = discovery_->get_plugin_info(library_path);
        if (!meta_opt) {
            notify_error("unknown", "Could not get plugin metadata for " + library_path.string());
            return false;
        }

        auto& meta = *meta_opt;

        // Check if already loaded
        {
            std::lock_guard lock(mutex_);
            if (loaded_plugins_.contains(meta.name)) {
                logger_->warn("Plugin '" + meta.name + "' is already loaded");
                return false;
            }
        }

        // Snapshot registered command names so we can attribute the newly
        // registered command(s) to this plugin (the command name frequently
        // differs from the plugin/library name).
        std::unordered_set<std::string> before;
        for (auto& n : registry_->list_commands()) {
            before.insert(n);
        }

        // Load through registry
        if (!registry_->load_command_library(library_path.string())) {
            notify_error(meta.name, "Failed to load library: " + library_path.string());
            return false;
        }

        std::vector<std::string> new_commands;
        for (auto& n : registry_->list_commands()) {
            if (!before.contains(n)) {
                new_commands.push_back(n);
            }
        }

        // Track plugin
        {
            std::lock_guard lock(mutex_);
            loaded_plugins_[meta.name] = meta;
            plugin_commands_[meta.name] = new_commands;

            if (std::filesystem::exists(library_path)) {
                plugin_timestamps_[meta.name] = std::filesystem::last_write_time(library_path);
            }
        }

        logger_->info("Loaded plugin: " + meta.name + " v" + meta.version);
        notify_loaded(meta.name);

        return true;
    }

    bool PluginManager::unload_plugin(const std::string& name) {
        // Gather the commands this plugin registered. Do NOT hold the lock
        // while calling the registry or notify_*: those acquire other locks
        // (and notify_* re-acquires mutex_, which is non-recursive).
        std::vector<std::string> commands;
        {
            std::lock_guard lock(mutex_);
            auto it = loaded_plugins_.find(name);
            if (it == loaded_plugins_.end()) {
                return false;
            }
            auto cit = plugin_commands_.find(name);
            if (cit != plugin_commands_.end() && !cit->second.empty()) {
                commands = cit->second;
            } else {
                // Fallback for plugins loaded without command tracking.
                commands.push_back(name);
            }
        }

        for (const auto& cmd : commands) {
            if (!registry_->unregister_command(cmd)) {
                notify_error(name, "Failed to unregister command: " + cmd);
                return false;
            }
        }

        {
            std::lock_guard lock(mutex_);
            loaded_plugins_.erase(name);
            plugin_timestamps_.erase(name);
            plugin_commands_.erase(name);
        }

        logger_->info("Unloaded plugin: " + name);
        notify_unloaded(name);

        return true;
    }

    bool PluginManager::reload_plugin(const std::string& name) {
        std::filesystem::path library_path;

        {
            std::lock_guard lock(mutex_);
            auto it = loaded_plugins_.find(name);
            if (it == loaded_plugins_.end()) {
                return false;
            }
            library_path = it->second.library_path;
        }

        if (!unload_plugin(name)) {
            return false;
        }

        return load_plugin(library_path);
    }

    std::optional<PluginMetadata> PluginManager::get_plugin_metadata(const std::string& name) const {
        std::lock_guard lock(mutex_);
        auto it = loaded_plugins_.find(name);
        if (it != loaded_plugins_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::vector<PluginMetadata> PluginManager::get_all_plugins() const {
        std::lock_guard lock(mutex_);
        std::vector<PluginMetadata> result;
        result.reserve(loaded_plugins_.size());
        for (const auto& [name, meta] : loaded_plugins_) {
            result.push_back(meta);
        }
        return result;
    }

    void PluginManager::start_watching(const std::filesystem::path& directory,
                                       std::chrono::milliseconds interval_ms) {
        stop_watching();

        watch_directory_ = directory;
        watch_interval_ = interval_ms;
        watching_.store(true);

        watch_thread_ = std::thread(&PluginManager::watch_thread_func, this);

        logger_->info("Started watching plugin directory: " + directory.string());
    }

    void PluginManager::stop_watching() {
        if (watching_.exchange(false)) {
            if (watch_thread_.joinable()) {
                watch_thread_.join();
            }
            logger_->info("Stopped watching plugin directory");
        }
    }

    void PluginManager::watch_thread_func() {
        while (watching_.load()) {
            std::this_thread::sleep_for(watch_interval_);

            if (!watching_.load()) break;

            // Scan for changes
            auto plugins = discovery_->scan_plugins(watch_directory_);

            std::lock_guard lock(mutex_);

            for (const auto& plugin : plugins) {
                if (!std::filesystem::exists(plugin.library_path)) {
                    continue;
                }

                auto current_time = std::filesystem::last_write_time(plugin.library_path);

                auto it = plugin_timestamps_.find(plugin.name);
                if (it != plugin_timestamps_.end()) {
                    // Check if modified
                    if (current_time != it->second) {
                        logger_->info("Detected change in plugin: " + plugin.name);
                        // Note: Auto-reload would go here, but it's risky
                        // Just log for now - user can manually reload
                    }
                } else {
                    // New plugin
                    logger_->info("Detected new plugin: " + plugin.name);
                }
            }
        }
    }

    void PluginManager::on_plugin_loaded(PluginCallback callback) {
        std::lock_guard lock(mutex_);
        on_loaded_callbacks_.push_back(std::move(callback));
    }

    void PluginManager::on_plugin_unloaded(PluginCallback callback) {
        std::lock_guard lock(mutex_);
        on_unloaded_callbacks_.push_back(std::move(callback));
    }

    void PluginManager::on_plugin_error(
        std::function<void(const std::string&, const std::string&)> callback) {
        std::lock_guard lock(mutex_);
        on_error_callbacks_.push_back(std::move(callback));
    }

    void PluginManager::notify_loaded(const std::string& name) {
        std::vector<PluginCallback> callbacks;
        {
            std::lock_guard lock(mutex_);
            callbacks = on_loaded_callbacks_;
        }
        for (const auto& cb : callbacks) {
            cb(name);
        }
    }

    void PluginManager::notify_unloaded(const std::string& name) {
        std::vector<PluginCallback> callbacks;
        {
            std::lock_guard lock(mutex_);
            callbacks = on_unloaded_callbacks_;
        }
        for (const auto& cb : callbacks) {
            cb(name);
        }
    }

    void PluginManager::notify_error(const std::string& name, const std::string& error) {
        logger_->error("Plugin error [" + name + "]: " + error);

        std::vector<std::function<void(const std::string&, const std::string&)>> callbacks;
        {
            std::lock_guard lock(mutex_);
            callbacks = on_error_callbacks_;
        }
        for (const auto& cb : callbacks) {
            cb(name, error);
        }
    }

} // namespace Orcha::Core
