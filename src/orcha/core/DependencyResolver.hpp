//
// DependencyResolver.hpp - Topological sorting for plugin dependencies
//

#pragma once

#include "IPluginDiscovery.hpp"
#include "Result.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <algorithm>

namespace Orcha::Core {

    /**
     * @struct DependencyError
     * @brief Error information for dependency resolution failures.
     */
    struct DependencyError {
        enum class Type {
            CircularDependency,
            MissingDependency
        };

        Type type;
        std::string message;
        std::vector<std::string> involved_plugins;

        [[nodiscard]] static DependencyError circular(std::vector<std::string> cycle) {
            DependencyError err;
            err.type = Type::CircularDependency;
            err.involved_plugins = std::move(cycle);

            std::string cycle_str;
            for (size_t i = 0; i < err.involved_plugins.size(); ++i) {
                if (i > 0) cycle_str += " -> ";
                cycle_str += err.involved_plugins[i];
            }
            if (!err.involved_plugins.empty()) {
                cycle_str += " -> " + err.involved_plugins[0];
            }

            err.message = "Circular dependency detected: " + cycle_str;
            return err;
        }

        [[nodiscard]] static DependencyError missing(const std::string& plugin,
                                                      const std::string& missing_dep) {
            DependencyError err;
            err.type = Type::MissingDependency;
            err.involved_plugins = {plugin, missing_dep};
            err.message = "Plugin '" + plugin + "' depends on '" + missing_dep +
                         "' which is not available";
            return err;
        }
    };

    /**
     * @class DependencyResolver
     * @brief Resolves plugin load order using topological sorting (Kahn's algorithm).
     *
     * This class takes a list of plugins with their declared dependencies and
     * produces a load order that ensures dependencies are loaded before dependents.
     * It detects circular dependencies and missing dependencies.
     */
    class DependencyResolver {
    public:
        using ResolveResult = Result<std::vector<PluginMetadata>, DependencyError>;

        /**
         * @brief Resolve plugin dependencies and return ordered list.
         * @param plugins List of plugins to order.
         * @param strict If true, fail on missing dependencies. If false, warn and skip.
         * @return Ordered plugin list or error.
         *
         * Uses Kahn's algorithm for topological sorting:
         * 1. Build adjacency list and compute in-degrees
         * 2. Start with nodes that have no dependencies (in-degree = 0)
         * 3. Process each node, reducing in-degrees of dependents
         * 4. If not all nodes processed, there's a cycle
         */
        [[nodiscard]] static ResolveResult resolve(
            const std::vector<PluginMetadata>& plugins,
            bool strict = true) {

            if (plugins.empty()) {
                return ResolveResult::Ok({});
            }

            // Build plugin name -> metadata mapping
            std::unordered_map<std::string, const PluginMetadata*> plugin_map;
            for (const auto& plugin : plugins) {
                plugin_map[plugin.name] = &plugin;
            }

            // Check for missing dependencies first (if strict)
            if (strict) {
                for (const auto& plugin : plugins) {
                    for (const auto& dep : plugin.dependencies) {
                        if (!plugin_map.contains(dep)) {
                            return ResolveResult::Err(
                                DependencyError::missing(plugin.name, dep));
                        }
                    }
                }
            }

            // Build adjacency list: dependency -> list of dependents
            // A depends on B means edge B -> A (B must come before A)
            std::unordered_map<std::string, std::vector<std::string>> dependents;
            std::unordered_map<std::string, int> in_degree;

            // Initialize in-degrees
            for (const auto& plugin : plugins) {
                in_degree[plugin.name] = 0;
                dependents[plugin.name] = {};
            }

            // Build graph
            for (const auto& plugin : plugins) {
                for (const auto& dep : plugin.dependencies) {
                    // Skip missing dependencies in non-strict mode
                    if (!plugin_map.contains(dep)) {
                        continue;
                    }
                    dependents[dep].push_back(plugin.name);
                    in_degree[plugin.name]++;
                }
            }

            // Kahn's algorithm
            std::queue<std::string> ready;
            for (const auto& [name, degree] : in_degree) {
                if (degree == 0) {
                    ready.push(name);
                }
            }

            std::vector<PluginMetadata> ordered;
            ordered.reserve(plugins.size());

            while (!ready.empty()) {
                const std::string current = ready.front();
                ready.pop();

                // Add to ordered result
                if (auto it = plugin_map.find(current); it != plugin_map.end()) {
                    ordered.push_back(*it->second);
                }

                // Reduce in-degree of dependents
                for (const auto& dependent : dependents[current]) {
                    if (--in_degree[dependent] == 0) {
                        ready.push(dependent);
                    }
                }
            }

            // Check for cycle - if not all nodes processed, there's a cycle
            if (ordered.size() != plugins.size()) {
                auto cycle = find_cycle(plugins, plugin_map);
                return ResolveResult::Err(DependencyError::circular(std::move(cycle)));
            }

            return ResolveResult::Ok(std::move(ordered));
        }

        /**
         * @brief Check if dependencies can be satisfied.
         * @param plugins List of plugins to check.
         * @return True if all dependencies can be resolved.
         */
        [[nodiscard]] static bool can_resolve(const std::vector<PluginMetadata>& plugins) {
            return resolve(plugins, true).is_ok();
        }

        /**
         * @brief Get list of plugins that have no dependencies.
         * @param plugins List of plugins.
         * @return Plugins with empty dependency list.
         */
        [[nodiscard]] static std::vector<PluginMetadata> get_roots(
            const std::vector<PluginMetadata>& plugins) {

            std::vector<PluginMetadata> roots;
            std::unordered_set<std::string> available;

            for (const auto& plugin : plugins) {
                available.insert(plugin.name);
            }

            for (const auto& plugin : plugins) {
                bool has_available_deps = false;
                for (const auto& dep : plugin.dependencies) {
                    if (available.contains(dep)) {
                        has_available_deps = true;
                        break;
                    }
                }
                if (!has_available_deps) {
                    roots.push_back(plugin);
                }
            }

            return roots;
        }

    private:
        /**
         * @brief Find a cycle in the dependency graph using DFS.
         */
        [[nodiscard]] static std::vector<std::string> find_cycle(
            const std::vector<PluginMetadata>& plugins,
            const std::unordered_map<std::string, const PluginMetadata*>& plugin_map) {

            enum class State { Unvisited, Visiting, Visited };
            std::unordered_map<std::string, State> state;
            std::vector<std::string> path;

            for (const auto& plugin : plugins) {
                state[plugin.name] = State::Unvisited;
            }

            std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool {
                state[node] = State::Visiting;
                path.push_back(node);

                auto it = plugin_map.find(node);
                if (it != plugin_map.end()) {
                    for (const auto& dep : it->second->dependencies) {
                        if (!plugin_map.contains(dep)) continue;

                        if (state[dep] == State::Visiting) {
                            // Found cycle - extract it
                            auto cycle_start = std::find(path.begin(), path.end(), dep);
                            return true;
                        }

                        if (state[dep] == State::Unvisited) {
                            if (dfs(dep)) return true;
                        }
                    }
                }

                state[node] = State::Visited;
                path.pop_back();
                return false;
            };

            for (const auto& plugin : plugins) {
                if (state[plugin.name] == State::Unvisited) {
                    path.clear();
                    if (dfs(plugin.name)) {
                        // Extract cycle from path
                        // Find where the cycle starts
                        for (size_t i = 0; i < path.size(); ++i) {
                            auto it = plugin_map.find(path[i]);
                            if (it != plugin_map.end()) {
                                for (const auto& dep : it->second->dependencies) {
                                    auto cycle_start = std::find(path.begin(), path.end(), dep);
                                    if (cycle_start != path.end() &&
                                        std::distance(path.begin(), cycle_start) <= static_cast<long>(i)) {
                                        return std::vector<std::string>(cycle_start, path.end());
                                    }
                                }
                            }
                        }
                        return path;
                    }
                }
            }

            return {}; // Should not reach here if cycle exists
        }
    };

} // namespace Orcha::Core
