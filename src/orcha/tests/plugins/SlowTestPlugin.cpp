//
// SlowTestPlugin.cpp - Test-only plugin whose execute() outlives an unload.
//
// Used by the registry lifetime regression test: the command is still running
// when it is unregistered, so its code must stay mapped until it returns.
//

#include "../../core/ICommand.hpp"

#include <chrono>
#include <thread>

class SlowTestPlugin final : public Orcha::Core::ICommand {
public:
    [[nodiscard]] std::string name() const override { return "slow_test_plugin"; }

    Orcha::Json execute(const Orcha::Json& params) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(params.value("ms", 200)));
        return Orcha::Json{{"done", true}};
    }
};

extern "C" Orcha::Core::ICommand* create_command() {
    return new SlowTestPlugin();
}

extern "C" void destroy_command(Orcha::Core::ICommand* command) {
    delete command;
}
