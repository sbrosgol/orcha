#include <filesystem>
#include "core/CommandRegistry.hpp"
#include "agent/CommandAgent.hpp"
#include "workflow/WorkflowRunner.hpp"

namespace fs = std::filesystem;

auto main(int argc, char* argv[]) -> int {
    Orcha::Utils::Logger::instance().set_log_file("./logs2/orcha.log");
    Orcha::Utils::Logger::instance().log(Orcha::Utils::LogLevel::INFO, "Starting Orcha...");

    Orcha::Core::CommandRegistry registry;
    const std::string commands_dir = "./commands";
    const std::string ext =
#if defined(_WIN32)
        ".dll";
#elif defined(__APPLE__)
        ".dylib";
#else
        ".so";
#endif

    // Ensure commands directory exists
    if (!fs::exists(commands_dir)) {
        std::cout << "[Orcha] Creating missing commands directory: " << commands_dir << std::endl;
        fs::create_directory(commands_dir);
    }

    // Load all shared library plugins in commands/
    bool found_plugin = false;
    for (const auto& entry : fs::recursive_directory_iterator(commands_dir)) {
        if (entry.path().extension() == ext) {
            registry.load_command_library(entry.path().string());
            found_plugin = true;
        }
    }
    if (!found_plugin) {
        std::cout << "[Orcha] No command plugins found in " << commands_dir
                  << ". Place plugin libraries here to extend Orcha.\n";
    }

    // CLI workflow mode
    if (argc > 1) {
        std::vector<Orcha::Workflow::WorkflowStepResult> step_results;
        if (const Orcha::Workflow::WorkflowRunner runner(registry); !runner.run(argv[1], step_results)) {
            std::cout << "[Orcha] Workflow failed!\n";
        }
        for (size_t i = 0; i < step_results.size(); ++i) {
            std::cout << "Step " << (i + 1) << ": success=" << step_results[i].success
                      << " error=" << step_results[i].error_message
                      << " output=" << step_results[i].output.serialize() << std::endl;
        }
        return 0;
    }

    // HTTP server mode
    Orcha::Agent::CommandAgent agent(registry);
    agent.start(8070);

    std::cout << "[Orcha] Listening on http://localhost:8070/\n";
    std::cout << "[Orcha] Press Enter to exit...\n";

    std::cin.get();

    Orcha::Utils::Logger::instance().log(Orcha::Utils::LogLevel::INFO, "Shutting down Orcha...");
    agent.stop();
    std::cout << "[Orcha] Shutdown complete.\n";
    return 0;
}
