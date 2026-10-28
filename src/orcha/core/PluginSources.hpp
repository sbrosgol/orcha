//
// PluginSources.hpp - Where plugins come from: the bundled directory and the
// upload directory, and which copy of a plugin wins.
//
// plugins.directory holds the plugins that ship with Orcha (in Docker, part
// of the image, so they update with it). plugins.upload.directory holds
// plugins installed from the dashboard (in Docker, on the data volume, so
// they survive the container). An uploaded plugin overrides a bundled one
// with the same name; the bundled files are never modified, so removing the
// upload brings the bundled version back.
//
// The loader (PluginManager::load_plugins), the admin API (PluginAdminRoute)
// and the installer (PluginInstaller) all use scan(), so they agree on which
// copy of a plugin is "the" plugin.
//

#pragma once

#include "IPluginDiscovery.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Orcha::Core {

    struct PluginSources {
        std::filesystem::path bundled;    ///< plugins.directory
        std::filesystem::path uploaded;   ///< plugins.upload.directory; empty: none

        /// One plugin as it will be loaded.
        struct Entry {
            PluginMetadata meta;
            bool uploaded = false;
            std::optional<PluginMetadata> overrides;   ///< the bundled plugin it replaces
        };

        /// @p child is @p parent or inside it (lexically, after absolute()).
        [[nodiscard]] static bool within(const std::filesystem::path& child, const std::filesystem::path& parent) {
            if (child.empty() || parent.empty()) return false;
            std::error_code ec;
            const auto c = std::filesystem::absolute(child, ec).lexically_normal();
            const auto p = std::filesystem::absolute(parent, ec).lexically_normal();
            auto [pi, ci] = std::mismatch(p.begin(), p.end(), c.begin(), c.end());
            return pi == p.end() || (std::next(pi) == p.end() && pi->empty());
        }

        /// The two directories overlap (one inside the other): not allowed,
        /// discovery would see the same plugins twice.
        [[nodiscard]] bool overlapping() const {
            return !uploaded.empty() && (within(uploaded, bundled) || within(bundled, uploaded));
        }

        [[nodiscard]] bool is_uploaded(const PluginMetadata& meta) const {
            return !uploaded.empty() && within(meta.library_path, uploaded);
        }

        /// Plugins by name, an uploaded one overriding a bundled one. A
        /// directory that cannot be scanned contributes nothing.
        [[nodiscard]] std::vector<Entry> scan(const IPluginDiscovery& discovery) const {
            const auto scan_dir = [&](const std::filesystem::path& dir) {
                std::error_code ec;
                if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return std::vector<PluginMetadata>{};
                try {
                    return discovery.scan_plugins(dir);
                } catch (const std::exception&) {
                    return std::vector<PluginMetadata>{};
                }
            };
            std::map<std::string, Entry> by_name;
            std::vector<std::string> order;
            for (auto& meta : scan_dir(bundled)) {
                if (by_name.emplace(meta.name, Entry{meta, false, std::nullopt}).second) order.push_back(meta.name);
            }
            if (!overlapping()) {
                for (auto& meta : scan_dir(uploaded)) {
                    auto it = by_name.find(meta.name);
                    if (it == by_name.end()) {
                        by_name.emplace(meta.name, Entry{meta, true, std::nullopt});
                        order.push_back(meta.name);
                    } else if (!it->second.uploaded) {
                        it->second = Entry{meta, true, it->second.meta};
                    }
                }
            }
            std::vector<Entry> out;
            out.reserve(order.size());
            for (const auto& name : order) out.push_back(std::move(by_name.at(name)));
            return out;
        }
    };

} // namespace Orcha::Core
