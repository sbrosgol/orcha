//
// ICommand.hpp - Command interface with metadata support
// Created by Slava Brosgol on 24/06/2025.
// Updated with metadata and validation support
//

#pragma once

#include <cpprest/json.h>
#include <string>
#include <vector>
#include <optional>
#include "Result.hpp"
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

        [[nodiscard]] web::json::value to_json() const {
            web::json::value obj;
            obj[U("name")] = web::json::value::string(name);
            obj[U("type")] = web::json::value::string(type);
            obj[U("required")] = web::json::value::boolean(required);
            if (description) {
                obj[U("description")] = web::json::value::string(*description);
            }
            if (default_value) {
                obj[U("default")] = web::json::value::string(*default_value);
            }
            if (example) {
                obj[U("example")] = web::json::value::string(*example);
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

        [[nodiscard]] web::json::value to_json() const {
            web::json::value obj;
            obj[U("name")] = web::json::value::string(name);
            obj[U("version")] = web::json::value::string(version);
            obj[U("description")] = web::json::value::string(description);

            if (!author.empty()) {
                obj[U("author")] = web::json::value::string(author);
            }

            if (!tags.empty()) {
                web::json::value arr = web::json::value::array(tags.size());
                for (size_t i = 0; i < tags.size(); ++i) {
                    arr[i] = web::json::value::string(tags[i]);
                }
                obj[U("tags")] = arr;
            }

            if (!parameters.empty()) {
                web::json::value params = web::json::value::array(parameters.size());
                for (size_t i = 0; i < parameters.size(); ++i) {
                    params[i] = parameters[i].to_json();
                }
                obj[U("parameters")] = params;
            }

            obj[U("supports_rollback")] = web::json::value::boolean(supports_rollback);

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
         */
        virtual web::json::value execute(const web::json::value& params) = 0;

        /**
         * @brief Rollback the command (undo execution).
         * @param params Original parameters used for execution.
         *
         * Default implementation does nothing.
         */
        virtual void rollback(const web::json::value&) {}

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
            const web::json::value& params) const {

            const auto& meta = metadata();

            for (const auto& param : meta.parameters) {
                if (param.required) {
                    if (!params.has_field(param.name)) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Required parameter missing"));
                    }
                }

                // Type validation for present parameters
                if (params.has_field(param.name)) {
                    const auto& value = params.at(param.name);

                    if (param.type == "string" && !value.is_string()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected string type"));
                    }
                    if (param.type == "int" && !value.is_integer()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected integer type"));
                    }
                    if (param.type == "bool" && !value.is_boolean()) {
                        return Result<void, ValidationError>::Err(
                            ValidationError(param.name, "Expected boolean type"));
                    }
                    if (param.type == "double" && !value.is_double() && !value.is_integer()) {
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
     * @brief Factory function type for creating commands.
     *
     * Plugins must export a function with this signature named "create_command".
     */
    extern "C" {
        typedef ICommand* (*CreateCommandFunc)();
    }

} // namespace Orcha::Core
