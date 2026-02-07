//
// Created by Slava Brosgol on 24/06/2025.
// Updated to implement ICommandRegistry interface
//

#include "CommandRegistry.hpp"
#include <dlfcn.h>
#include <algorithm>

namespace Orcha::Core {

    CommandRegistry::~CommandRegistry() {
        std::unique_lock lock(mutex_);
        commands_.clear();
        for (auto& plugin : plugins_) {
            plugin.command.reset();
            if (plugin.handle) {
                dlclose(plugin.handle);
            }
        }
    }

    bool CommandRegistry::load_command_library(const std::string& path) {
        void* handle = dlopen(path.c_str(), RTLD_NOW);
        if (!handle) {
            if (logger_) {
                logger_->error("Failed to load plugin: " + path + " (" + dlerror() + ")");
            }
            return false;
        }

        dlerror(); // Clear any existing error
        const auto create_func = reinterpret_cast<CreateCommandFunc>(dlsym(handle, "create_command"));
        if (const char* dlsym_error = dlerror()) {
            if (logger_) {
                logger_->error("Cannot load symbol 'create_command' in " + path + ": " + dlsym_error);
            }
            dlclose(handle);
            return false;
        }

        std::shared_ptr<ICommand> cmd(create_func());
        if (!cmd) {
            if (logger_) {
                logger_->error("Plugin in " + path + " did not return a valid command.");
            }
            dlclose(handle);
            return false;
        }

        const std::string cmd_name = cmd->name();

        {
            std::unique_lock lock(mutex_);

            // Check if command already exists
            if (commands_.contains(cmd_name)) {
                if (logger_) {
                    logger_->error("Command '" + cmd_name + "' already registered, skipping.");
                }
                dlclose(handle);
                return false;
            }

            commands_[cmd_name] = cmd;
            plugins_.push_back({handle, std::move(cmd), path});
        }

        if (logger_) {
            logger_->info("Loaded command: " + cmd_name + " from " + path);
        }
        return true;
    }

    bool CommandRegistry::register_command(std::shared_ptr<ICommand> command) {
        if (!command) {
            return false;
        }

        const std::string cmd_name = command->name();

        std::unique_lock lock(mutex_);

        if (commands_.contains(cmd_name)) {
            if (logger_) {
                logger_->error("Command '" + cmd_name + "' already registered.");
            }
            return false;
        }

        commands_[cmd_name] = command;
        plugins_.push_back({nullptr, std::move(command), ""});

        if (logger_) {
            logger_->info("Registered command: " + cmd_name);
        }
        return true;
    }

    bool CommandRegistry::unregister_command(const std::string& name) {
        std::unique_lock lock(mutex_);

        auto cmd_it = commands_.find(name);
        if (cmd_it == commands_.end()) {
            return false;
        }

        // Find and remove the plugin
        auto plugin_it = std::ranges::find_if(plugins_, [&name](const PluginHandle& p) {
            return p.command && p.command->name() == name;
        });

        if (plugin_it != plugins_.end()) {
            if (plugin_it->handle) {
                dlclose(plugin_it->handle);
            }
            plugins_.erase(plugin_it);
        }

        commands_.erase(cmd_it);
        if (logger_) {
            logger_->info("Unregistered command: " + name);
        }
        return true;
    }

    std::shared_ptr<ICommand> CommandRegistry::get_command(const std::string& name) const {
        std::shared_lock lock(mutex_);
        const auto it = commands_.find(name);
        return (it != commands_.end()) ? it->second : nullptr;
    }

    std::vector<std::string> CommandRegistry::list_commands() const {
        std::shared_lock lock(mutex_);
        std::vector<std::string> names;
        names.reserve(commands_.size());
        for (const auto& [name, _] : commands_) {
            names.push_back(name);
        }
        return names;
    }

    bool CommandRegistry::has_command(const std::string& name) const {
        std::shared_lock lock(mutex_);
        return commands_.contains(name);
    }

    size_t CommandRegistry::command_count() const {
        std::shared_lock lock(mutex_);
        return commands_.size();
    }

} // namespace Orcha::Core
