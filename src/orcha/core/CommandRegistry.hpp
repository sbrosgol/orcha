//
// Created by Slava Brosgol on 24/06/2025.
// Updated to implement ICommandRegistry interface
//
#pragma once

#include "ICommandRegistry.hpp"
#include "ICommand.hpp"
#include "../utils/ILogger.hpp"
#include <unordered_map>
#include <shared_mutex>

namespace Orcha::Core {

    /**
     * @class CommandRegistry
     * @brief Concrete implementation of command registry with plugin loading.
     *
     * Thread-safe command registry that supports dynamic loading of
     * command plugins from shared libraries.
     *
     * Lifetime: a command loaded from a plugin library keeps that library
     * mapped for as long as ANY shared_ptr to the command is alive (the
     * library handle is owned by the command's deleter). Unregistering a
     * command that a workflow is still executing therefore only drops the
     * registry's reference; the library is closed after the last user lets go.
     */
    class CommandRegistry : public IMutableCommandRegistry {
    public:
        explicit CommandRegistry(std::shared_ptr<Utils::ILogger> logger = nullptr)
            : logger_(std::move(logger)) {}
        ~CommandRegistry() override = default;

        // Prevent copying
        CommandRegistry(const CommandRegistry&) = delete;
        CommandRegistry& operator=(const CommandRegistry&) = delete;

        // Cannot move due to std::shared_mutex
        CommandRegistry(CommandRegistry&&) = delete;
        CommandRegistry& operator=(CommandRegistry&&) = delete;

        // ICommandRegistry interface
        [[nodiscard]] std::shared_ptr<ICommand> get_command(const std::string& name) const override;
        [[nodiscard]] std::vector<std::string> list_commands() const override;
        [[nodiscard]] bool has_command(const std::string& name) const override;
        [[nodiscard]] size_t command_count() const override;

        // IMutableCommandRegistry interface
        [[nodiscard]] Result<void, std::string> try_load_command_library(const std::string& path) override;
        [[nodiscard]] bool register_command(std::shared_ptr<ICommand> command) override;
        [[nodiscard]] bool unregister_command(const std::string& name) override;

    private:
        mutable std::shared_mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<ICommand>> commands_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Core
