//
// DependencyResolverTests.hpp - Unit tests for plugin dependency resolution
//

#pragma once

#include "../core/DependencyResolver.hpp"
#include <cassert>
#include <iostream>
#include <algorithm>

namespace Orcha::Tests {

    /**
     * @class DependencyResolverTestFixture
     * @brief Test fixture for dependency resolution tests.
     */
    class DependencyResolverTestFixture {
    public:
        virtual void SetUp() {
            plugins_.clear();
        }

        virtual void TearDown() {
            plugins_.clear();
        }
        /**
         * @brief Create a plugin with specified dependencies.
         */
        static Core::PluginMetadata make_plugin(
            const std::string& name,
            const std::vector<std::string>& deps = {}) {

            Core::PluginMetadata meta;
            meta.name = name;
            meta.version = "1.0.0";
            meta.dependencies = deps;
            meta.library_path = "/plugins/lib" + name + ".dylib";
            return meta;
        }

        /**
         * @brief Add a plugin to the test set.
         */
        void add_plugin(const std::string& name,
                       const std::vector<std::string>& deps = {}) {
            plugins_.push_back(make_plugin(name, deps));
        }

        /**
         * @brief Get index of plugin in ordered result.
         */
        static size_t index_of(const std::vector<Core::PluginMetadata>& ordered,
                               const std::string& name) {
            for (size_t i = 0; i < ordered.size(); ++i) {
                if (ordered[i].name == name) return i;
            }
            throw std::runtime_error("Plugin not found: " + name);
        }

        /**
         * @brief Assert that 'before' comes before 'after' in ordering.
         */
        static void assert_order(const std::vector<Core::PluginMetadata>& ordered,
                                const std::string& before,
                                const std::string& after) {
            size_t idx_before = index_of(ordered, before);
            size_t idx_after = index_of(ordered, after);

            if (idx_before >= idx_after) {
                throw std::runtime_error(
                    "Expected '" + before + "' to come before '" + after +
                    "' but got indices " + std::to_string(idx_before) +
                    " and " + std::to_string(idx_after));
            }
        }

        std::vector<Core::PluginMetadata> plugins_;
    };

    // ========== Test Cases ==========

    /**
     * @brief Test: Empty plugin list should resolve successfully.
     */
    inline void test_empty_plugins() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_ok() && "Empty list should resolve");
        assert(result.value().empty() && "Result should be empty");

        std::cout << "[PASS] test_empty_plugins\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Single plugin with no dependencies.
     */
    inline void test_single_plugin_no_deps() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("echo");

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_ok() && "Single plugin should resolve");
        assert(result.value().size() == 1 && "Should have one plugin");
        assert(result.value()[0].name == "echo" && "Should be 'echo'");

        std::cout << "[PASS] test_single_plugin_no_deps\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Multiple independent plugins (no dependencies).
     */
    inline void test_independent_plugins() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("echo");
        fixture.add_plugin("postgres");
        fixture.add_plugin("http");

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_ok() && "Independent plugins should resolve");
        assert(result.value().size() == 3 && "Should have three plugins");

        std::cout << "[PASS] test_independent_plugins\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Linear dependency chain (A -> B -> C).
     */
    inline void test_linear_dependency_chain() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        // C depends on B, B depends on A
        fixture.add_plugin("A");
        fixture.add_plugin("B", {"A"});
        fixture.add_plugin("C", {"B"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_ok() && "Linear chain should resolve");
        auto& ordered = result.value();
        assert(ordered.size() == 3 && "Should have three plugins");

        // A must come before B, B must come before C
        fixture.assert_order(ordered, "A", "B");
        fixture.assert_order(ordered, "B", "C");

        std::cout << "[PASS] test_linear_dependency_chain\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Diamond dependency pattern.
     *        A
     *       / \
     *      B   C
     *       \ /
     *        D
     */
    inline void test_diamond_dependencies() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("A");
        fixture.add_plugin("B", {"A"});
        fixture.add_plugin("C", {"A"});
        fixture.add_plugin("D", {"B", "C"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_ok() && "Diamond pattern should resolve");
        auto& ordered = result.value();
        assert(ordered.size() == 4 && "Should have four plugins");

        // A must come before B and C
        fixture.assert_order(ordered, "A", "B");
        fixture.assert_order(ordered, "A", "C");
        // B and C must come before D
        fixture.assert_order(ordered, "B", "D");
        fixture.assert_order(ordered, "C", "D");

        std::cout << "[PASS] test_diamond_dependencies\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Simple circular dependency (A -> B -> A).
     */
    inline void test_circular_dependency_simple() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("A", {"B"});
        fixture.add_plugin("B", {"A"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_err() && "Circular dependency should fail");
        assert(result.error().type == Core::DependencyError::Type::CircularDependency &&
               "Should be circular dependency error");

        std::cout << "[PASS] test_circular_dependency_simple\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Complex circular dependency (A -> B -> C -> A).
     */
    inline void test_circular_dependency_complex() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("A", {"C"});
        fixture.add_plugin("B", {"A"});
        fixture.add_plugin("C", {"B"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_err() && "Circular dependency should fail");
        assert(result.error().type == Core::DependencyError::Type::CircularDependency &&
               "Should be circular dependency error");

        std::cout << "[PASS] test_circular_dependency_complex\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Self-dependency (A -> A).
     */
    inline void test_self_dependency() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("A", {"A"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_err() && "Self-dependency should fail");
        assert(result.error().type == Core::DependencyError::Type::CircularDependency &&
               "Should be circular dependency error");

        std::cout << "[PASS] test_self_dependency\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Missing dependency in strict mode.
     */
    inline void test_missing_dependency_strict() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("A", {"nonexistent"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_, true);

        assert(result.is_err() && "Missing dependency should fail in strict mode");
        assert(result.error().type == Core::DependencyError::Type::MissingDependency &&
               "Should be missing dependency error");
        assert(result.error().involved_plugins[0] == "A" && "Should report plugin A");
        assert(result.error().involved_plugins[1] == "nonexistent" && "Should report missing dep");

        std::cout << "[PASS] test_missing_dependency_strict\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Missing dependency in non-strict mode (should skip).
     */
    inline void test_missing_dependency_non_strict() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("A", {"nonexistent"});
        fixture.add_plugin("B");

        auto result = Core::DependencyResolver::resolve(fixture.plugins_, false);

        assert(result.is_ok() && "Missing dependency should succeed in non-strict mode");
        assert(result.value().size() == 2 && "Should have both plugins");

        std::cout << "[PASS] test_missing_dependency_non_strict\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Multiple roots (several independent starting points).
     */
    inline void test_multiple_roots() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        // Two independent chains: A->B and C->D
        fixture.add_plugin("A");
        fixture.add_plugin("B", {"A"});
        fixture.add_plugin("C");
        fixture.add_plugin("D", {"C"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_ok() && "Multiple roots should resolve");
        auto& ordered = result.value();
        assert(ordered.size() == 4 && "Should have four plugins");

        // A before B, C before D
        fixture.assert_order(ordered, "A", "B");
        fixture.assert_order(ordered, "C", "D");

        std::cout << "[PASS] test_multiple_roots\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: get_roots utility function.
     */
    inline void test_get_roots() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        fixture.add_plugin("A");
        fixture.add_plugin("B", {"A"});
        fixture.add_plugin("C");
        fixture.add_plugin("D", {"B", "C"});

        auto roots = Core::DependencyResolver::get_roots(fixture.plugins_);

        assert(roots.size() == 2 && "Should have two roots (A and C)");

        bool has_a = std::any_of(roots.begin(), roots.end(),
                                  [](const auto& p) { return p.name == "A"; });
        bool has_c = std::any_of(roots.begin(), roots.end(),
                                  [](const auto& p) { return p.name == "C"; });

        assert(has_a && "Should contain A as root");
        assert(has_c && "Should contain C as root");

        std::cout << "[PASS] test_get_roots\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: can_resolve utility function.
     */
    inline void test_can_resolve() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        // Valid case
        fixture.add_plugin("A");
        fixture.add_plugin("B", {"A"});

        assert(Core::DependencyResolver::can_resolve(fixture.plugins_) &&
               "Valid deps should be resolvable");

        // Add circular
        fixture.plugins_.clear();
        fixture.add_plugin("X", {"Y"});
        fixture.add_plugin("Y", {"X"});

        assert(!Core::DependencyResolver::can_resolve(fixture.plugins_) &&
               "Circular deps should not be resolvable");

        std::cout << "[PASS] test_can_resolve\n";
        fixture.TearDown();
    }

    /**
     * @brief Test: Complex real-world scenario.
     */
    inline void test_complex_scenario() {
        DependencyResolverTestFixture fixture;
        fixture.SetUp();

        // Simulate real plugins:
        // - logging: no deps (core utility)
        // - config: no deps (core utility)
        // - database: depends on logging, config
        // - cache: depends on logging
        // - auth: depends on database, cache
        // - api: depends on auth, logging

        fixture.add_plugin("logging");
        fixture.add_plugin("config");
        fixture.add_plugin("database", {"logging", "config"});
        fixture.add_plugin("cache", {"logging"});
        fixture.add_plugin("auth", {"database", "cache"});
        fixture.add_plugin("api", {"auth", "logging"});

        auto result = Core::DependencyResolver::resolve(fixture.plugins_);

        assert(result.is_ok() && "Complex scenario should resolve");
        auto& ordered = result.value();
        assert(ordered.size() == 6 && "Should have six plugins");

        // Verify ordering constraints
        fixture.assert_order(ordered, "logging", "database");
        fixture.assert_order(ordered, "config", "database");
        fixture.assert_order(ordered, "logging", "cache");
        fixture.assert_order(ordered, "database", "auth");
        fixture.assert_order(ordered, "cache", "auth");
        fixture.assert_order(ordered, "auth", "api");
        fixture.assert_order(ordered, "logging", "api");

        std::cout << "[PASS] test_complex_scenario\n";
        fixture.TearDown();
    }

    /**
     * @brief Run all dependency resolver tests.
     */
    inline void run_dependency_resolver_tests() {
        std::cout << "\n=== Running Dependency Resolver Tests ===\n\n";

        test_empty_plugins();
        test_single_plugin_no_deps();
        test_independent_plugins();
        test_linear_dependency_chain();
        test_diamond_dependencies();
        test_circular_dependency_simple();
        test_circular_dependency_complex();
        test_self_dependency();
        test_missing_dependency_strict();
        test_missing_dependency_non_strict();
        test_multiple_roots();
        test_get_roots();
        test_can_resolve();
        test_complex_scenario();

        std::cout << "\n=== All Dependency Resolver Tests Passed ===\n";
    }

} // namespace Orcha::Tests
