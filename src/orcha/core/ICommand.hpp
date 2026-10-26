//
// ICommand.hpp - Command interface with metadata support
// Created by Slava Brosgol on 24/06/2025.
// Updated with metadata and validation support
//

#pragma once

#include "Json.hpp"
#include <string>
#include <vector>
#include <optional>
#include "Result.hpp"
#include "ExecutionControl.hpp"
#include "core/Version.hpp"

namespace Orcha::Core {

    /**
     * @struct CommandParameter
     * @brief Describes a command parameter for documentation and validation.
     */
    struct CommandParameter {
        std::string name;
        std::string type;  // "string", "int", "bool", "double", "object", "array"
        bool required = false;
        std::optional<std::string> description;
        std::optional<std::string> default_value;
        std::optional<std::string> example;

        [[nodiscard]] Json to_json() const {
            Json obj = Json::object();
            obj["name"] = name;
            obj["type"] = type;
            obj["required"] = required;
            if (description) {
                obj["description"] = *description;
            }
            if (default_value) {
                obj["default"] = *default_value;
            }
            if (example) {
                obj["example"] = *example;
            }
            return obj;
        }
    };

    /**
     * @struct CommandMetadata
     * @brief Rich metadata about a command for documentation and validation.
     */
    struct CommandMetadata {
        std::string name;
        std::string version = Orcha::kVersion;
        std::string description;
        std::string author;
        std::vector<std::string> tags;
        std::vector<CommandParameter> parameters;
        bool supports_rollback = false;

        [[nodiscard]] Json to_json() const {
            Json obj = Json::object();
            obj["name"] = name;
            obj["version"] = version;
            obj["description"] = description;

            if (!author.empty()) {
                obj["author"] = author;
            }

            if (!tags.empty()) {
                Json arr = Json::array();
                for (const auto& tag : tags) {
                    arr.push_back(tag);
                }
                obj["tags"] = arr;
            }

            if (!parameters.empty()) {
                Json params = Json::array();
                for (const auto& param : parameters) {
                    params.push_back(param.to_json());
                }
                obj["parameters"] = params;
            }

            obj["supports_rollback"] = supports_rollback;

            return obj;
        }
    };

    /**
     * @struct ValidationError
     * @brief Error information from parameter validation.
     */
    struct ValidationError {
        std::string parameter_name;
        std::string message;

        ValidationError(std::string param, std::string msg)
            : parameter_name(std::move(param)), message(std::move(msg)) {}
    };

    /**
     * @class ICommand
     * @brief An interface representing a command in the Orcha namespace.
     *
     * ICommand serves as a base interface for defining commands, allowing for
     * a standard structure within the command design pattern. Implementing classes
     * can define specific command behaviors.
     *
     * This class is intended to be extended by other classes to provide
     * concrete implementations of the command logic.
     *
     * Part of the Orcha namespace.
     */
    class ICommand {
    public:
        virtual ~ICommand() = default;

        /**
         * @brief Get the unique name of this command.
         */
        [[nodiscard]] virtual std::string name() const = 0;

        /**
         * @brief Execute the command with given parameters.
         * @param params JSON object containing command parameters.
         * @return JSON result of execution.
         *
         * Failure contract: a command fails its workflow step either by
         * throwing, or by returning an object with `"success": false` (and
         * optionally an `"error"` string used as the step's error message).
         * Any other return value is a successful step.
         */
        virtual Json execute(const Json& params) = 0;

        /**
         * @brief Execute with cancellation and a deadline.
         * @param params  JSON object containing command parameters.
         * @param control Stop token and deadline for this execution.
         * @return JSON result of execution (same contract as execute()).
         *
         * The engine always calls this overload. The default ignores
         * @p control and calls execute(params); commands that block (child
         * processes, network I/O) should override it, poll
         * control.should_stop() and return promptly, e.g. with
         * { "success": false, "error": "Timed out" }. Such commands can
         * implement execute(params) as execute_with_control(params, {}).
         */
        virtual Json execute_with_control(const Json& params, const ExecutionControl& control) {
            (void)control;
            return execute(params);
        }

        /**
         * @brief Rollback the command (undo execution).
         * @param params Original parameters used for execution.
         *
         * Default implementation does nothing.
         */
        virtual void rollback(const Json&) {}

        /**
         * @brief Get rich metadata about this command.
         *
         * Default implementation returns basic metadata from name().
         * Override for full documentation.
         */
        [[nodiscard]] virtual CommandMetadata metadata() const {
            CommandMetadata meta;
            meta.name = name();
            meta.version = Orcha::kVersion;
            meta.description = "No description available";
            return meta;
        }

        /**
         * @brief Validate parameters before execution.
         * @param params Parameters to validate.
         * @return Ok if valid, or Error with validation details.
         *
         * Default implementation validates based on metadata().
         */
        [[nodiscard]] virtual Result<void, ValidationError> validate(
            const Json& params) const {

            const auto& meta = metadata();

            for (const auto& param : meta.parameters) {
                if (param.required) {
                    if (!params.contains(param.name)) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Required parameter missing"));
                    }
                }

                // Type validation for present parameters
                if (params.contains(param.name)) {
                    const auto& value = params.at(param.name);

                    if (param.type == "string" && !value.is_string()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected string type"));
                    }
                    if (param.type == "int" && !value.is_number_integer()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected integer type"));
                    }
                    if (param.type == "bool" && !value.is_boolean()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected boolean type"));
                    }
                    if (param.type == "double" && !value.is_number()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected number type"));
                    }
                    if (param.type == "object" && !value.is_object()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected object type"));
                    }
                    if (param.type == "array" && !value.is_array()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected array type"));
                    }
                }
            }

            return Result<void, ValidationError>::Ok();
        }
    };

    /**
     * @brief Plugin ABI version.
     *
     * Bumped whenever ICommand's layout changes (e.g. a new virtual). Plugins
     * export it via ORCHA_PLUGIN_ABI_EXPORT(); the registry refuses plugins
     * built against a different version instead of calling into a mismatched
     * vtable.
     *
     * 2: added ICommand::execute_with_control.
     */
    inline constexpr int kPluginAbiVersion = 2;

    /**
     * @brief Factory function type for creating commands.
     *
     * Plugins must export a function with this signature named "create_command".
     */
    extern "C" {
        typedef ICommand* (*CreateCommandFunc)();

        /**
         * @brief Optional destructor export ("destroy_command").
         *
         * When a plugin exports it, the host destroys commands through it so
         * the object is freed by the allocator that created it. Without it the
         * host falls back to `delete` (virtual destructor).
         */
        typedef void (*DestroyCommandFunc)(ICommand*);

        /// Required export ("orcha_plugin_abi_version"); see kPluginAbiVersion.
        typedef int (*PluginAbiVersionFunc)();
    }

} // namespace Orcha::Core

/**
 * @brief Export the plugin ABI version. Every plugin must expand this once at
 *        namespace scope, next to create_command().
 */
#define ORCHA_PLUGIN_ABI_EXPORT()                                   \
    extern "C" int orcha_plugin_abi_version() {                     \
        return ::Orcha::Core::kPluginAbiVersion;                    \
    }
