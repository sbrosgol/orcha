//
// Created by Slava Brosgol on 24/06/2025.
// Updated to implement ICommandRegistry interface
//

#include "CommandRegistry.hpp"
#include <dlfcn.h>

namespace Orcha::Core {

    namespace {

        /// Owns a dlopen() handle; closing happens when the last owner lets go.
        struct SharedLibrary {
            void* handle = nullptr;

            explicit SharedLibrary(void* h) : handle(h) {}
            ~SharedLibrary() {
                if (handle) {
                    dlclose(handle);
                }
            }
            SharedLibrary(const SharedLibrary&) = delete;
            SharedLibrary& operator=(const SharedLibrary&) = delete;
        };

    } // namespace

    Result<void, std::string> CommandRegistry::try_load_command_library(const std::string& path) {
        // Every refusal is logged and returned, so callers (plugin install)
        // can tell the admin why.
        const auto fail = [this](std::string reason) {
            if (logger_) logger_->error(reason);
            return Result<void, std::string>::Err(std::move(reason));
        };
        void* handle = dlopen(path.c_str(), RTLD_NOW);
        if (!handle) {
            return fail("Failed to load plugin: " + path + " (" + dlerror() + ")");
        }
        auto library = std::make_shared<SharedLibrary>(handle);

        // Refuse plugins built against a different ICommand layout: calling
        // into a mismatched vtable is undefined behaviour.
        dlerror();
        const auto abi_func = reinterpret_cast<PluginAbiVersionFunc>(
            dlsym(handle, "orcha_plugin_abi_version"));
        if (!abi_func || abi_func() != kPluginAbiVersion) {
            return fail("Plugin " + path + " was built for plugin ABI " +
                        (abi_func ? std::to_string(abi_func()) : std::string("1 (unversioned)")) +
                        ", host requires " + std::to_string(kPluginAbiVersion) +
                        "; rebuild it against the current core/ICommand.hpp");
        }

        dlerror(); // Clear any existing error
        const auto create_func = reinterpret_cast<CreateCommandFunc>(dlsym(handle, "create_command"));
        if (const char* dlsym_error = dlerror()) {
            return fail("Cannot load symbol 'create_command' in " + path + ": " + dlsym_error);
        }
        // Optional: lets the plugin free the object with its own allocator.
        const auto destroy_func = reinterpret_cast<DestroyCommandFunc>(dlsym(handle, "destroy_command"));

        ICommand* raw = nullptr;
        try {
            raw = create_func();
        } catch (const std::exception& ex) {
            return fail("Plugin factory in " + path + " threw: " + ex.what());
        } catch (...) {
            return fail("Plugin factory in " + path + " threw an unknown exception");
        }
        if (!raw) {
            return fail("Plugin in " + path + " did not return a valid command.");
        }

        // The deleter owns the library: the command is destroyed first, then
        // the deleter (and with it the last library reference) is released,
        // so dlclose() can never run while the command is alive or executing.
        std::shared_ptr<ICommand> cmd(raw, [library, destroy_func](ICommand* p) {
            if (destroy_func) {
                destroy_func(p);
            } else {
                delete p;
            }
        });

        const std::string cmd_name = cmd->name();

        {
            std::unique_lock lock(mutex_);
            if (commands_.contains(cmd_name)) {
                return fail("Command '" + cmd_name + "' already registered, skipping."); // cmd and library are released here, in that order
            }
            commands_.emplace(cmd_name, std::move(cmd));
        }

        if (logger_) {
            logger_->info("Loaded command: " + cmd_name + " from " + path);
        }
        return Result<void, std::string>::Ok();
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

        commands_.emplace(cmd_name, std::move(command));

        if (logger_) {
            logger_->info("Registered command: " + cmd_name);
        }
        return true;
    }

    bool CommandRegistry::unregister_command(const std::string& name) {
        std::shared_ptr<ICommand> removed;
        {
            std::unique_lock lock(mutex_);
            auto it = commands_.find(name);
            if (it == commands_.end()) {
                return false;
            }
            removed = std::move(it->second);
            commands_.erase(it);
        }

        // Drop the registry's reference outside the lock. If this was the last
        // reference the command (and then its library) is released now;
        // otherwise that happens when in-flight executions finish.
        removed.reset();

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
