//
// SlowTestPlugin.cpp - Test-only plugin whose execute() outlives an unload.
//
// Used by the registry lifetime regression test: the command is still running
// when it is unregistered, so its code must stay mapped until it returns.
// Also exercises the plugin ABI handshake (see ORCHA_TEST_LEGACY_ABI).
//
// The loader requires a plugin's command to be named like the plugin (its
// manifest "name", else its file name without "lib" and the extension). Tests
// copy this one library under many plugin names, so by default it names
// itself the same way, from where it was loaded. ORCHA_TEST_COMMAND_NAME
// fixes the name instead (a plugin claiming a command it was not named for).
//

#include "../../core/ICommand.hpp"

#include <dlfcn.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

extern "C" Orcha::Core::ICommand* create_command();

namespace {

    std::string own_plugin_name() {
#ifdef ORCHA_TEST_COMMAND_NAME
        return ORCHA_TEST_COMMAND_NAME;
#else
        Dl_info info{};
        // Fail loudly: a made-up fallback name could let a loader bug pass.
        if (dladdr(reinterpret_cast<void*>(&create_command), &info) == 0 || !info.dli_fname) return "<fixture-error: dladdr>";
        const std::filesystem::path library(info.dli_fname);
        std::ifstream manifest(library.parent_path() / "manifest.json");
        if (manifest) {
            try {
                const auto j = Orcha::Json::parse(manifest);
                if (j.contains("name") && j.at("name").is_string()) return j.at("name").get<std::string>();
                return "<fixture-error: manifest without name>";
            } catch (const std::exception&) {
                return "<fixture-error: unreadable manifest>";
            }
        }
        std::string stem = library.stem().string();
        if (stem.rfind("lib", 0) == 0) stem = stem.substr(3);
        return stem;
#endif
    }

} // namespace

class SlowTestPlugin final : public Orcha::Core::ICommand {
public:
    [[nodiscard]] std::string name() const override { return name_; }

    Orcha::Json execute(const Orcha::Json& params) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(params.value("ms", 200)));
        return Orcha::Json{{"done", true}};
    }

private:
    std::string name_ = own_plugin_name();
};

// The legacy variant (built with ORCHA_TEST_LEGACY_ABI) omits the ABI export,
// like a plugin compiled before versioning; the registry must refuse it.
#ifndef ORCHA_TEST_LEGACY_ABI
ORCHA_PLUGIN_ABI_EXPORT()
#endif

extern "C" Orcha::Core::ICommand* create_command() {
    return new SlowTestPlugin();
}

extern "C" void destroy_command(Orcha::Core::ICommand* command) {
    delete command;
}
