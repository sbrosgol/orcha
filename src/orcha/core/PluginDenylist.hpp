//
// PluginDenylist.hpp - Persistent set of disabled plugin names (Phase 3).
//
// When a plugin is disabled through the admin API its name is added here and
// persisted to a file, so it stays unloaded across restarts (the PluginManager
// skips denylisted plugins when scanning the plugin directory). Enabling removes
// it. Reload does NOT touch the denylist.
//

#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Orcha::Core {

    /**
     * @interface IPluginDenylist
     * @brief Persistent set of plugin names that must not be auto-loaded.
     */
    class IPluginDenylist {
    public:
        virtual ~IPluginDenylist() = default;
        [[nodiscard]] virtual bool contains(const std::string& name) const = 0;
        virtual void add(const std::string& name) = 0;
        virtual void remove(const std::string& name) = 0;
        [[nodiscard]] virtual std::vector<std::string> list() const = 0;
    };

    /**
     * @class FilePluginDenylist
     * @brief File-backed IPluginDenylist (one plugin name per line).
     */
    class FilePluginDenylist : public IPluginDenylist {
    public:
        explicit FilePluginDenylist(std::filesystem::path path)
            : path_(std::move(path)) {
            load();
        }

        [[nodiscard]] bool contains(const std::string& name) const override {
            std::lock_guard lock(mutex_);
            return names_.count(name) > 0;
        }

        void add(const std::string& name) override {
            std::lock_guard lock(mutex_);
            if (names_.insert(name).second) save();
        }

        void remove(const std::string& name) override {
            std::lock_guard lock(mutex_);
            if (names_.erase(name) > 0) save();
        }

        [[nodiscard]] std::vector<std::string> list() const override {
            std::lock_guard lock(mutex_);
            return {names_.begin(), names_.end()};
        }

    private:
        void load() {
            std::ifstream in(path_);
            if (!in) return;
            std::string line;
            while (std::getline(in, line)) {
                // trim trailing CR/whitespace
                while (!line.empty() &&
                       (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                    line.pop_back();
                }
                if (!line.empty()) names_.insert(line);
            }
        }

        void save() const {
            std::error_code ec;
            if (path_.has_parent_path()) {
                std::filesystem::create_directories(path_.parent_path(), ec);
            }
            std::ofstream out(path_, std::ios::trunc);
            for (const auto& n : names_) out << n << '\n';
        }

        std::filesystem::path path_;
        mutable std::mutex mutex_;
        std::set<std::string> names_;
    };

} // namespace Orcha::Core
