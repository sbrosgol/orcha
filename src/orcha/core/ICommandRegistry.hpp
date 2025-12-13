//
// ICommandRegistry.hpp - Interface for command registry
// Created as part of architectural improvements
//

#pragma once

#include <string>
#include <vector>
#include <memory>

namespace Orcha::Core {

    // Forward declaration
    class ICommand;

    /**
     * @interface ICommandRegistry
     * @brief Abstract interface for command registration and retrieval.
     *
     * This interface allows for dependency injection and testability
     * by abstracting the command registry implementation.
     */
    class ICommandRegistry {
    public:
        virtual ~ICommandRegistry() = default;

        /**
         * @brief Retrieve a command by name.
         * @param name The unique name of the command.
         * @return Pointer to the command, or nullptr if not found.
         */
        [[nodiscard]] virtual ICommand* get_command(const std::string& name) const = 0;

        /**
         * @brief List all registered command names.
         * @return Vector of command names.
         */
        [[nodiscard]] virtual std::vector<std::string> list_commands() const = 0;

        /**
         * @brief Check if a command exists.
         * @param name The command name to check.
         * @return True if command exists.
         */
        [[nodiscard]] virtual bool has_command(const std::string& name) const = 0;

        /**
         * @brief Get the number of registered commands.
         * @return Count of registered commands.
         */
        [[nodiscard]] virtual size_t command_count() const = 0;
    };

    /**
     * @interface IMutableCommandRegistry
     * @brief Extended interface that allows command registration.
     *
     * Separates read and write operations for better abstraction.
     */
    class IMutableCommandRegistry : public ICommandRegistry {
    public:
        /**
         * @brief Load a command from a shared library.
         * @param path Path to the shared library.
         * @return True if loading succeeded.
         */
        virtual bool load_command_library(const std::string& path) = 0;

        /**
         * @brief Register a command instance directly.
         * @param command The command to register (takes ownership).
         * @return True if registration succeeded.
         */
        virtual bool register_command(std::unique_ptr<ICommand> command) = 0;

        /**
         * @brief Unregister a command by name.
         * @param name The command name to unregister.
         * @return True if unregistration succeeded.
         */
        virtual bool unregister_command(const std::string& name) = 0;
    };

} // namespace Orcha::Core
