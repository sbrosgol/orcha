//
// MockCommandRegistry.hpp - Mock command registry for testing
// Created as part of test infrastructure
//

#pragma once

#include "../../core/ICommandRegistry.hpp"
#include "../../core/ICommand.hpp"
#include <unordered_map>
#include <memory>
#include <vector>
#include <functional>

namespace Orcha::Tests::Mocks {

    /**
     * @class MockCommandRegistry
     * @brief Mock implementation of ICommandRegistry for testing.
     */
    class MockCommandRegistry : public Core::ICommandRegistry {
    public:
        // ========== ICommandRegistry Interface ==========

        [[nodiscard]] Core::ICommand* get_command(const std::string& name) const override {
            if (get_command_calls_) {
                get_command_calls_(name);
            }

            auto it = commands_.find(name);
            return (it != commands_.end()) ? it->second.get() : nullptr;
        }

        [[nodiscard]] std::vector<std::string> list_commands() const override {
            std::vector<std::string> names;
            names.reserve(commands_.size());
            for (const auto& [name, _] : commands_) {
                names.push_back(name);
            }
            return names;
        }

        [[nodiscard]] bool has_command(const std::string& name) const override {
            return commands_.contains(name);
        }

        [[nodiscard]] size_t command_count() const override {
            return commands_.size();
        }

        // ========== Mock Helpers ==========

        /**
         * @brief Add a mock command to the registry.
         */
        void add_command(std::unique_ptr<Core::ICommand> cmd) {
            auto name = cmd->name();
            commands_[name] = std::move(cmd);
        }

        /**
         * @brief Add a mock command using a factory.
         */
        template<typename CommandType, typename... Args>
        void add_command(Args&&... args) {
            auto cmd = std::make_unique<CommandType>(std::forward<Args>(args)...);
            add_command(std::move(cmd));
        }

        /**
         * @brief Clear all commands.
         */
        void clear() {
            commands_.clear();
        }

        /**
         * @brief Set callback to track get_command calls.
         */
        void on_get_command(std::function<void(const std::string&)> callback) {
            get_command_calls_ = std::move(callback);
        }

    private:
        std::unordered_map<std::string, std::unique_ptr<Core::ICommand>> commands_;
        std::function<void(const std::string&)> get_command_calls_;
    };

    /**
     * @class MockCommand
     * @brief Simple mock command for testing.
     */
    class MockCommand : public Core::ICommand {
    public:
        explicit MockCommand(std::string name,
                           std::function<web::json::value(const web::json::value&)> execute_fn = nullptr)
            : name_(std::move(name))
            , execute_fn_(std::move(execute_fn)) {}

        [[nodiscard]] std::string name() const override {
            return name_;
        }

        web::json::value execute(const web::json::value& params) override {
            ++execute_count_;
            last_params_ = params;

            if (execute_fn_) {
                return execute_fn_(params);
            }

            // Default: return empty object
            return web::json::value::object();
        }

        [[nodiscard]] Core::CommandMetadata metadata() const override {
            Core::CommandMetadata meta;
            meta.name = name_;
            meta.description = "Mock command for testing";
            return meta;
        }

        // ========== Mock Verification ==========

        int execute_count() const { return execute_count_; }
        const web::json::value& last_params() const { return last_params_; }

        void reset_stats() {
            execute_count_ = 0;
            last_params_ = web::json::value();
        }

    private:
        std::string name_;
        std::function<web::json::value(const web::json::value&)> execute_fn_;
        int execute_count_ = 0;
        web::json::value last_params_;
    };

    /**
     * @class FailingCommand
     * @brief Mock command that always throws.
     */
    class FailingCommand : public Core::ICommand {
    public:
        explicit FailingCommand(std::string name, std::string error_message = "Command failed")
            : name_(std::move(name))
            , error_message_(std::move(error_message)) {}

        [[nodiscard]] std::string name() const override {
            return name_;
        }

        web::json::value execute(const web::json::value&) override {
            throw std::runtime_error(error_message_);
        }

    private:
        std::string name_;
        std::string error_message_;
    };

} // namespace Orcha::Tests::Mocks
