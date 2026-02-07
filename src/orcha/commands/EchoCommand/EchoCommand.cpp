//
// EchoCommand.cpp - Echo command plugin
// Updated with metadata support
//

#include "../../core/ICommand.hpp"
#include "core/Version.hpp"
#include <cpprest/json.h>
#include <string>

/**
 * @class EchoCommand
 * @brief Simple command that echoes back the input message.
 *
 * This command serves as a basic example and testing utility.
 */
class EchoCommand final : public Orcha::Core::ICommand {
public:
    web::json::value execute(const web::json::value& params) override {
        const auto msg = params.has_field(U("message"))
            ? params.at(U("message")).as_string()
            : U("");

        web::json::value result;
        result[U("echoed")] = web::json::value::string(msg);
        return result;
    }

    [[nodiscard]] std::string name() const override {
        return "echo";
    }

    [[nodiscard]] Orcha::Core::CommandMetadata metadata() const override {
        Orcha::Core::CommandMetadata meta;
        meta.name = "echo";
        meta.version = Orcha::kVersion;
        meta.description = "Echoes back the input message";
        meta.author = "Orcha Team";
        meta.tags = {"utility", "debug"};
        meta.supports_rollback = false;

        // Parameter definition
        Orcha::Core::CommandParameter msg_param;
        msg_param.name = "message";
        msg_param.type = "string";
        msg_param.required = false;
        msg_param.description = "The message to echo back";
        msg_param.default_value = "";
        msg_param.example = "Hello, World!";

        meta.parameters.push_back(msg_param);

        return meta;
    }
};

extern "C" Orcha::Core::ICommand* create_command() {
    return new EchoCommand();
}
