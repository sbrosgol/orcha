//
// PluginManager.hpp - Plugin lifecycle management
// Created as part of architectural improvements
//

#pragma once

#include "IPluginDiscovery.hpp"
#include "ICommandRegistry.hpp"
#include "../utils/ILogger.hpp"
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace Orcha::Core {

    /**
     * @class PluginDiscoveryService
     * @brief Default implementation of IPluginDiscovery.
     *
     * Discovers plugins by looking for manifest.json files or
     * directly scanning for library files.
     */
    class PluginDiscoveryService : public IPluginDiscovery {
    public:
        [[nodiscard]] std::vector<PluginMetadata> scan_plugins(
            const std::filesystem::path& directory) const override;

        [[nodiscard]] std::optional<PluginMetadata> get_plugin_info(
            const std::filesystem::path& library_path) const override;

    private:
        [[nodiscard]] std::optional<PluginMetadata> load_manifest(
            const std::filesystem::path& manifest_path) const;

        [[nodiscard]] PluginMetadata create_basic_metadata(
            const std::filesystem::path& library_path) const;
    };

    /**
     * @class PluginManager
     * @brief Manages plugin lifecycle including discovery, loading, and hot-reload.
     */
    class PluginManager {
    public:
        using PluginCallback = std::function<void(const std::string& plugin_name)>;

        /**
         * @brief Construct PluginManager with dependencies.
         */
        PluginManager(std::shared_ptr<IMutableCommandRegistry> registry,
                      std::shared_ptr<IPluginDiscovery> discovery,
                      std::shared_ptr<Utils::ILogger> logger);

        ~PluginManager();

        // Prevent copying
        PluginManager(const PluginManager&) = delete;
        PluginManager& operator=(const PluginManager&) = delete;

        /**
         * @brief Scan and load all plugins from directory.
         * @param directory Plugin directory path.
         * @return Number of plugins loaded.
         */
        size_t load_plugins_from_directory(const std::filesystem::path& directory);

        /**
         * @brief Load a specific plugin by path.
         * @param library_path Path to plugin library.
         * @return True if loading succeeded.
         */
        bool load_plugin(const std::filesystem::path& library_path);

        /**
         * @brief Unload a plugin by name.
         * @param name Plugin name.
         * @return True if unloading succeeded.
         */
        bool unload_plugin(const std::string& name);

        /**
         * @brief Reload a plugin (unload + load).
         * @param name Plugin name.
         * @return True if reload succeeded.
         */
        bool reload_plugin(const std::string& name);

        /**
         * @brief Get metadata for a loaded plugin.
         */
        [[nodiscard]] std::optional<PluginMetadata> get_plugin_metadata(
            const std::string& name) const;

        /**
         * @brief Get all loaded plugin metadata.
         */
        [[nodiscard]] std::vector<PluginMetadata> get_all_plugins() const;

        /**
         * @brief Start watching a directory for plugin changes.
         * @param directory Directory to watch.
         * @param interval_ms Scan interval in milliseconds.
         */
        void start_watching(const std::filesystem::path& directory,
                           std::chrono::milliseconds interval_ms);

        /**
         * @brief Stop watching for plugin changes.
         */
        void stop_watching();

        /**
         * @brief Check if watching is active.
         */
        [[nodiscard]] bool is_watching() const { return watching_.load(); }

        // Event callbacks
        void on_plugin_loaded(PluginCallback callback);
        void on_plugin_unloaded(PluginCallback callback);
        void on_plugin_error(std::function<void(const std::string&, const std::string&)> callback);

    private:
        void watch_thread_func();
        void notify_loaded(const std::string& name);
        void notify_unloaded(const std::string& name);
        void notify_error(const std::string& name, const std::string& error);

        std::shared_ptr<IMutableCommandRegistry> registry_;
        std::shared_ptr<IPluginDiscovery> discovery_;
        std::shared_ptr<Utils::ILogger> logger_;

        mutable std::mutex mutex_;
        std::unordered_map<std::string, PluginMetadata> loaded_plugins_;
        std::unordered_map<std::string, std::filesystem::file_time_type> plugin_timestamps_;

        // Watching
        std::atomic<bool> watching_{false};
        std::thread watch_thread_;
        std::filesystem::path watch_directory_;
        std::chrono::milliseconds watch_interval_{5000};

        // Callbacks
        std::vector<PluginCallback> on_loaded_callbacks_;
        std::vector<PluginCallback> on_unloaded_callbacks_;
        std::vector<std::function<void(const std::string&, const std::string&)>> on_error_callbacks_;
    };

} // namespace Orcha::Core
