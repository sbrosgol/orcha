//
// main.cpp - Orcha application entry point
// Updated to use new architecture with DI and configuration
//

#include <filesystem>
#include <iostream>

// Core
#include "core/CommandRegistry.hpp"
#include "core/PluginManager.hpp"
#include "core/ServiceLocator.hpp"

// Config
#include "config/YamlConfiguration.hpp"

// Agent
#include "agent/CommandAgent.hpp"

// Workflow
#include "workflow/WorkflowEngine.hpp"

// Utils
#include "utils/Logger.hpp"

namespace fs = std::filesystem;

/**
 * @brief Bootstrap application services into the service locator.
 */
void bootstrap_services(Orcha::Core::ServiceLocator& services,
                       const Orcha::Config::IConfiguration& config) {
    using namespace Orcha;

    // Logger (singleton)
    auto& logger = Utils::Logger::instance();
    auto logging_config = Config::LoggingConfig::from_config(config);
    logger.set_log_file(logging_config.file);

    // For DI, we wrap the singleton in a shared_ptr
    auto logger_ptr = std::shared_ptr<Utils::ILogger>(&logger, [](Utils::ILogger*){});
    services.register_singleton<Utils::ILogger>(logger_ptr);

    // Command Registry (singleton)
    auto registry = std::make_shared<Core::CommandRegistry>(logger_ptr);
    services.register_singleton<Core::ICommandRegistry>(registry);
    services.register_singleton<Core::IMutableCommandRegistry>(registry);

    // Plugin Discovery (singleton)
    auto discovery = std::make_shared<Core::PluginDiscoveryService>();
    services.register_singleton<Core::IPluginDiscovery>(discovery);

    // Plugin Manager (singleton)
    auto plugin_manager = std::make_shared<Core::PluginManager>(
        registry, discovery, logger_ptr);
    services.register_singleton<Core::PluginManager>(plugin_manager);

    // Workflow Engine (factory - creates new instances)
    services.register_factory<Workflow::IWorkflowEngine>([registry, logger_ptr]() {
        return std::make_shared<Workflow::WorkflowEngine>(
            registry,
            std::make_shared<Workflow::SyncStepExecutor>(),
            logger_ptr);
    });
}

/**
 * @brief Load plugins from the configured directory.
 */
size_t load_plugins(Orcha::Core::ServiceLocator& services,
                   const Orcha::Config::IConfiguration& config) {
    using namespace Orcha;

    auto plugin_config = Config::PluginConfig::from_config(config);
    auto plugin_manager = services.get<Core::PluginManager>();
    auto logger = services.get<Utils::ILogger>();

    // Ensure commands directory exists
    if (!fs::exists(plugin_config.directory)) {
        logger->info("Creating missing commands directory: " + plugin_config.directory);
        fs::create_directories(plugin_config.directory);
    }

    // Load plugins with dependency resolution
    auto result = plugin_manager->load_plugins_from_directory(plugin_config.directory);

    // Handle dependency errors
    if (result.has_dependency_error()) {
        const auto& error = result.dependency_error.value();
        logger->error("Plugin dependency error: " + error.message);

        if (error.type == Core::DependencyError::Type::CircularDependency) {
            logger->error("Cannot proceed with circular dependencies. "
                         "Please fix plugin dependencies and restart.");
        }
    }

    if (result.loaded_count == 0) {
        logger->warn("No command plugins found in " + plugin_config.directory +
                    ". Place plugin libraries here to extend Orcha.");
    }

    // Log load order if plugins were loaded
    if (!result.load_order.empty()) {
        logger->info("Plugin load order: ");
        for (const auto& name : result.load_order) {
            logger->info("  - " + name);
        }
    }

    // Optionally start watching for changes
    if (plugin_config.auto_reload) {
        plugin_manager->start_watching(
            plugin_config.directory,
            std::chrono::milliseconds(plugin_config.scan_interval_ms));
    }

    return result.loaded_count;
}

/**
 * @brief Run in CLI mode - execute a workflow from a YAML file.
 */
int run_cli_mode(Orcha::Core::ServiceLocator& services, const std::string& yaml_path) {
    using namespace Orcha;

    auto logger = services.get<Utils::ILogger>();
    auto engine = services.get<Workflow::IWorkflowEngine>();

    logger->info("Running workflow from: " + yaml_path);

    // Use the new WorkflowEngine directly
    auto workflow_engine = std::dynamic_pointer_cast<Workflow::WorkflowEngine>(engine);
    if (!workflow_engine) {
        // Fallback: create one directly
        auto registry = services.get<Core::ICommandRegistry>();
        workflow_engine = std::make_shared<Workflow::WorkflowEngine>(registry);
    }

    auto result = workflow_engine->execute_yaml(yaml_path);

    // Print results
    for (size_t i = 0; i < result.step_results.size(); ++i) {
        const auto& step = result.step_results[i];
        std::cout << "Step " << (i + 1) << ": "
                  << "success=" << (step.success ? "true" : "false")
                  << " error=" << step.error_message
                  << " output=" << step.output.serialize()
                  << std::endl;
    }

    if (!result.success) {
        logger->error("Workflow failed: " + result.error_message);
        return 1;
    }

    logger->info("Workflow completed successfully");
    return 0;
}

/**
 * @brief Run in server mode - start HTTP server.
 */
int run_server_mode(Orcha::Core::ServiceLocator& services,
                   const Orcha::Config::IConfiguration& config) {
    using namespace Orcha;

    auto server_config = Config::ServerConfig::from_config(config);
    auto logger = services.get<Utils::ILogger>();
    auto registry = services.get<Core::ICommandRegistry>();
    auto engine = services.get<Workflow::IWorkflowEngine>();

    // Create agent with injected dependencies
    Agent::CommandAgent agent(registry, engine, logger);

    agent.start(server_config.port);

    std::cout << "[Orcha] Listening on http://" << server_config.host
              << ":" << server_config.port << "/\n";
    std::cout << "[Orcha] Available endpoints:\n";
    std::cout << "  GET  /          - Health check\n";
    std::cout << "  POST /workflow  - Execute workflow\n";
    std::cout << "  GET  /commands  - List commands\n";
    std::cout << "  GET  /swagger   - API documentation\n";
    std::cout << "[Orcha] Press Enter to exit...\n";

    std::cin.get();

    logger->info("Shutting down Orcha...");
    agent.stop();

    std::cout << "[Orcha] Shutdown complete.\n";
    return 0;
}

auto main(int argc, char* argv[]) -> int {
    using namespace Orcha;

    // Load configuration
    auto config = Config::YamlConfiguration::create_default();

    // Try to load from config file if it exists
    if (fs::exists("orcha.yaml")) {
        config->load_from_file("orcha.yaml");
    } else if (fs::exists("config/orcha.yaml")) {
        config->load_from_file("config/orcha.yaml");
    }

    // Apply environment variable overrides
    config->merge_environment("ORCHA_");

    // Create service locator and bootstrap services
    Core::ServiceLocator services;
    bootstrap_services(services, *config);

    auto logger = services.get<Utils::ILogger>();
    logger->info("Starting Orcha...");

    // Load plugins
    size_t plugin_count = load_plugins(services, *config);
    logger->info("Loaded " + std::to_string(plugin_count) + " plugins");

    // CLI mode if workflow path provided
    if (argc > 1) {
        int result = run_cli_mode(services, argv[1]);
        // Release all shared_ptrs before static destruction to avoid
        // destructor-order issues with Logger singleton
        services.clear();
        return result;
    }

    // Otherwise, run server mode
    int result = run_server_mode(services, *config);
    services.clear();
    return result;
}
