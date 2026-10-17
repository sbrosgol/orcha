//
// EchoCommand.cpp - Echo command plugin
// Updated with metadata support
//

#include "../../core/ICommand.hpp"
#include "core/Version.hpp"
#include <string>

/**
 * @class EchoCommand
 * @brief Simple command that echoes back the input message.
 *
 * This command serves as a basic example and testing utility.
 */
class EchoCommand final : public Orcha::Core::ICommand {
public:
    Orcha::Json execute(const Orcha::Json& params) override {
        const std::string msg = params.contains("message")
            ? params.at("message").get<std::string>()
            : "";

        Orcha::Json result;
        result["echoed"] = msg;
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
