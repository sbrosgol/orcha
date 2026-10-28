//
// main.cpp - Orcha application entry point
// Updated to use new architecture with DI and configuration
//

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <thread>
#ifdef _WIN32
#include <io.h>
#define orcha_isatty _isatty
#define orcha_fileno _fileno
#else
#include <unistd.h>
#define orcha_isatty ::isatty
#define orcha_fileno ::fileno
#endif

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>

// Core
#include "core/CommandRegistry.hpp"
#include "core/PluginManager.hpp"
#include "core/PluginDenylist.hpp"
#include "core/PluginInstaller.hpp"
#include "core/ServiceLocator.hpp"

// Config
#include "config/YamlConfiguration.hpp"
#include "config/PluginUploadConfig.hpp"

// Agent
#include "agent/CommandAgent.hpp"

// Workflow
#include "workflow/WorkflowEngine.hpp"

// Jobs
#include "jobs/SqliteJobStore.hpp"
#include "jobs/JobService.hpp"
#include "jobs/RunJobCommand.hpp"
#include "jobs/JobScheduler.hpp"
#include "jobs/RunQueue.hpp"

// Utils
#include "utils/Logger.hpp"

// Version
#include "core/Version.hpp"

namespace fs = std::filesystem;

/**
 * @brief Bootstrap application services into the service locator.
 * @param run_cancel Cancels every workflow run when stopped (shutdown).
 */
void bootstrap_services(Orcha::Core::ServiceLocator& services,
                       const Orcha::Config::IConfiguration& config,
                       std::stop_token run_cancel) {
    using namespace Orcha;

    // Logger (singleton)
    auto& logger = Utils::Logger::instance();
    auto logging_config = Config::LoggingConfig::from_config(config);
    logger.set_log_file(logging_config.file);
    logger.set_console_output(logging_config.console_output);
    if (auto level = Utils::parse_log_level(logging_config.level)) {
        logger.set_level(*level);
    } else {
        logger.warn("Unknown logging.level '" + logging_config.level + "'; using INFO");
    }

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

    // Persistent plugin denylist: disabled plugins stay unloaded across restarts.
    auto denylist = std::make_shared<Core::FilePluginDenylist>(
        config.get_string("plugins.denylist_path", "./orcha-disabled-plugins.txt"));
    services.register_singleton<Core::IPluginDenylist>(denylist);
    plugin_manager->set_denylist(denylist);

    // Plugin uploads from the dashboard: off unless configured (fail closed).
    {
        const auto upload = Config::PluginUploadConfig::from_config(config);
        Core::PluginUploadOptions options;
        options.enabled = upload.enabled;
        options.plugin_directory = Config::PluginConfig::from_config(config).directory;
        options.public_keys = upload.public_keys;
        options.max_bytes = static_cast<std::uint64_t>(std::max<int64_t>(1, upload.max_bytes));
        options.admin_auth_required = Config::AdminConfig::from_config(config).auth_required;
        services.register_singleton<Core::PluginInstaller>(std::make_shared<Core::PluginInstaller>(
            std::move(options), plugin_manager, discovery, denylist, logger_ptr, nullptr));
    }

    // Workflow Engine (factory - creates new instances)
    Workflow::EngineOptions engine_options;
    engine_options.default_step_timeout = std::chrono::milliseconds(
        std::max<int64_t>(0, config.get_int("workflow.default_timeout_ms", 0)));
    engine_options.cancel = run_cancel;

    // Circuit breakers are per command and shared by every engine, so their
    // state persists across runs; workflows opt in with "circuit_breaker": true.
    auto circuit_breakers = std::make_shared<Core::CircuitBreakerRegistry>();
    services.register_singleton<Core::CircuitBreakerRegistry>(circuit_breakers);
    engine_options.circuit_breakers = circuit_breakers;
    engine_options.circuit_breaker_config.failure_threshold = static_cast<size_t>(
        std::max<int64_t>(1, config.get_int("workflow.circuit_breaker.failure_threshold", 5)));
    engine_options.circuit_breaker_config.success_threshold = static_cast<size_t>(
        std::max<int64_t>(1, config.get_int("workflow.circuit_breaker.success_threshold", 1)));
    engine_options.circuit_breaker_config.reset_timeout = std::chrono::milliseconds(
        std::max<int64_t>(0, config.get_int("workflow.circuit_breaker.reset_timeout_ms", 30000)));
    auto engine_factory = [registry, logger_ptr, engine_options]() {
        return std::make_shared<Workflow::WorkflowEngine>(
            registry,
            std::make_shared<Workflow::SyncStepExecutor>(),
            logger_ptr,
            engine_options);
    };
    services.register_factory<Workflow::IWorkflowEngine>(
        std::function<std::shared_ptr<Workflow::IWorkflowEngine>()>(engine_factory));

    // Jobs store. Every run is recorded, so the server always needs one: if
    // the configured database cannot be opened, fall back to an in-memory
    // store (jobs and run history then do not survive a restart).
    auto jobs_db = config.get_string("jobs.db_path", "./orcha-jobs.db");
    std::shared_ptr<Jobs::SqliteJobStore> job_store;
    try {
        job_store = std::make_shared<Jobs::SqliteJobStore>(jobs_db, logger_ptr);
    } catch (const std::exception& ex) {
        logger.error(std::string("Job store unavailable: ") + ex.what() +
                     "; using an in-memory store - jobs and run history will not persist");
        job_store = std::make_shared<Jobs::SqliteJobStore>(":memory:", logger_ptr);
    }
    job_store->set_max_runs(static_cast<size_t>(
        std::max<int64_t>(0, config.get_int("jobs.max_runs", 10000))));
    services.register_singleton<Jobs::IJobStore>(job_store);
    auto job_service = std::make_shared<Jobs::JobService>(
        job_store, engine_factory, logger_ptr, run_cancel);
    services.register_singleton<Jobs::JobService>(job_service);

    // Every top-level run (API, dashboard, scheduler) is admitted through the
    // bounded run queue and executed by its own runner threads.
    Jobs::RunQueue::Options queue_options;
    queue_options.max_concurrent = static_cast<size_t>(
        std::max<int64_t>(1, config.get_int("workflow.max_concurrent_runs", 4)));
    queue_options.max_queued = static_cast<size_t>(
        std::max<int64_t>(0, config.get_int("workflow.max_queued_runs", 64)));
    services.register_singleton<Jobs::RunQueue>(
        std::make_shared<Jobs::RunQueue>(job_service, queue_options, logger_ptr));

    // Register the built-in run_job command so workflows can invoke other
    // jobs as sub-workflows (inline, in the parent's runner slot). Holds a
    // weak_ptr to JobService (no ref cycle).
    if (!registry->register_command(
            std::make_shared<Jobs::RunJobCommand>(job_service))) {
        logger.warn("Could not register the 'run_job' command");
    }
}

/**
 * @brief Load plugins from the configured directory.
 */
size_t load_plugins(Orcha::Core::ServiceLocator& services,
                   const Orcha::Config::IConfiguration& config,
                   bool server_mode) {
    using namespace Orcha;

    auto plugin_config = Config::PluginConfig::from_config(config);
    auto plugin_manager = services.get<Core::PluginManager>();
    auto logger = services.get<Utils::ILogger>();

    // Ensure commands directory exists
    if (!fs::exists(plugin_config.directory)) {
        logger->info("Creating missing commands directory: " + plugin_config.directory);
        fs::create_directories(plugin_config.directory);
    }

    // Server start: undo a plugin install the previous server did not finish
    // (e.g. the new plugin crashed it while loading) before anything is
    // loaded. Not in CLI mode, which must never touch a live server's install.
    if (auto installer = services.try_get<Core::PluginInstaller>(); installer && server_mode) {
        installer->recover_interrupted();
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
                  << " output=" << step.output.dump()
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
                   const Orcha::Config::IConfiguration& config,
                   std::stop_source& run_cancel) {
    using namespace Orcha;

    auto server_config = Config::ServerConfig::from_config(config);
    auto plugin_config = Config::PluginConfig::from_config(config);
    auto admin_config = Config::AdminConfig::from_config(config);
    auto logger = services.get<Utils::ILogger>();
    auto registry = services.get<Core::ICommandRegistry>();
    auto runs = services.get<Jobs::RunQueue>();
    auto plugins = services.get<Core::PluginManager>();
    auto discovery = services.get<Core::IPluginDiscovery>();
    auto job_service = services.get<Jobs::JobService>();
    auto denylist = services.try_get<Core::IPluginDenylist>();
    auto circuit_breakers = services.try_get<Core::CircuitBreakerRegistry>();
    auto plugin_installer = services.try_get<Core::PluginInstaller>();

    // Create agent with injected dependencies (including the admin dashboard).
    Agent::CommandAgent agent(
        registry, runs, logger,
        plugins, discovery, admin_config,
        plugin_config.directory,
        std::chrono::milliseconds(plugin_config.scan_interval_ms),
        job_service, denylist, server_config, circuit_breakers, plugin_installer);

    try {
        agent.start();
    } catch (const std::exception&) {
        // CommandAgent::start already reported the cause (e.g. address in use
        // or an invalid server.host); exit cleanly instead of std::terminate.
        return 1;
    }

    // Runs a previous server left queued/running can never finish now. Done
    // only once this server owns the port (a second instance that failed to
    // bind must not touch the live one's runs), and never to this process's
    // own runs.
    if (const auto n = job_service->store()->mark_interrupted_runs(
            "Interrupted: the server stopped before the run finished"); n > 0) {
        logger->warn("Marked " + std::to_string(n) +
                     " unfinished run(s) from a previous server as interrupted");
    }

    // Cron scheduler (Phase 3): fire enabled jobs whose schedule_cron is due.
    std::unique_ptr<Jobs::JobScheduler> scheduler;
    const bool scheduler_enabled = config.get_bool("jobs.scheduler_enabled", true);
    const auto scheduler_tick =
        std::chrono::seconds(config.get_int("jobs.scheduler_tick_seconds", 30));
    if (scheduler_enabled) {
        scheduler = std::make_unique<Jobs::JobScheduler>(job_service, runs, logger, scheduler_tick);
        scheduler->start();
    }

    const bool admin_on = admin_config.is_serviceable();

    std::cout << "[Orcha] Listening on http://" << server_config.host
              << ":" << server_config.port << "/\n";
    std::cout << "[Orcha] Available endpoints:\n";
    std::cout << "  GET  /          - Health check\n";
    std::cout << "  POST /workflow  - Submit a workflow run (202; poll /workflow/runs/{id})"
              << (!server_config.api_token.empty() ? " (Bearer token)"
                  : server_config.is_loopback() ? " (local clients only)"
                  : " (DISABLED: set server.api_token)") << "\n";
    std::cout << "  GET  /commands  - List commands\n";
    std::cout << "  GET  /swagger   - API documentation\n";
    if (admin_on) {
        std::cout << "  GET  /admin     - Admin dashboard"
                  << (admin_config.auth_required ? " (Basic auth)" : "") << "\n";
        std::cout << "  *    /api/plugins - Plugin admin API\n";
        std::cout << "  *    /api/circuit-breakers - Circuit breaker state/reset\n";
        std::cout << "  *    /api/jobs, /api/runs - Jobs and run history admin API\n";
    } else {
        std::cout << "  (admin dashboard disabled - see logs)\n";
    }
    if (scheduler) {
        std::cout << "  [scheduler] cron scheduling active (tick "
                  << scheduler_tick.count() << "s)\n";
    }
    // Wait for shutdown: SIGTERM/SIGINT, or Enter on an interactive stdin. In a
    // detached environment (Docker `up -d`, systemd, nohup) stdin is closed and
    // only signals apply. Signals are delivered through asio::signal_set, which
    // handles them outside signal context (a raw handler may only touch
    // lock-free atomics, so it cannot wake a waiting thread safely).
    // Shared ownership: the detached stdin thread may outlive this function.
    auto shutdown_ioc = std::make_shared<boost::asio::io_context>();
    boost::asio::signal_set signals(*shutdown_ioc, SIGINT, SIGTERM);
    std::string shutdown_reason = "stdin";
    signals.async_wait([&shutdown_reason, shutdown_ioc](const boost::system::error_code& ec, int sig) {
        if (!ec) {
            shutdown_reason = (sig == SIGINT) ? "SIGINT" : "SIGTERM";
        }
        shutdown_ioc->stop();
    });

    const bool interactive = orcha_isatty(orcha_fileno(stdin)) != 0;
    if (interactive) {
        std::cout << "[Orcha] Press Enter (or Ctrl-C) to exit...\n";
        std::thread([shutdown_ioc] {
            std::cin.get();
            boost::asio::post(*shutdown_ioc, [shutdown_ioc] { shutdown_ioc->stop(); });
        }).detach();
    } else {
        std::cout << "[Orcha] Running detached; send SIGTERM to exit.\n";
    }

    shutdown_ioc->run();   // returns on the first signal or Enter

    // Restore default dispositions: a second Ctrl-C now terminates immediately
    // if a graceful shutdown ever gets stuck.
    boost::system::error_code ignored;
    signals.clear(ignored);
    logger->info("Shutdown requested (" + shutdown_reason + ")");

    logger->info("Shutting down Orcha...");
    // Stop submitting; cancel every run (engines then skip rollback); stop the
    // run queue (queued runs are cancelled, runners joined); then drain the
    // HTTP server. Cancelling before runs->stop() makes the runs see a
    // shutdown, not a user cancel, so they are not rolled back.
    if (scheduler) scheduler->stop();
    run_cancel.request_stop();
    runs->stop();
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
    std::stop_source run_cancel;
    bootstrap_services(services, *config, run_cancel.get_token());

    auto logger = services.get<Utils::ILogger>();
    logger->info("Starting Orcha v" + std::string(Orcha::kVersion) + "...");

    // Load plugins
    size_t plugin_count = load_plugins(services, *config, /*server_mode=*/argc <= 1);
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
    int result = run_server_mode(services, *config, run_cancel);
    services.clear();
    return result;
}
