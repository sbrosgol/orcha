//
// ShellExec.cpp - Run a shell command, capture stdout/stderr and exit code.
//

#include "../../core/ICommand.hpp"
#include "core/Version.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#if defined(_WIN32)
  #define ORCHA_POPEN  _popen
  #define ORCHA_PCLOSE _pclose
#else
  #include <sys/wait.h>
  #define ORCHA_POPEN  popen
  #define ORCHA_PCLOSE pclose
#endif

class ShellExec final : public Orcha::Core::ICommand {
public:
    [[nodiscard]] std::string name() const override { return "shell_exec"; }

    Orcha::Json execute(const Orcha::Json& params) override {
        Orcha::Json result;
        try {
            if (!params.contains("cmd")) {
                throw std::runtime_error("'cmd' parameter is required");
            }
            const std::string cmd = params.at("cmd").get<std::string>();

            const bool capture_output = params.contains("capture_output")
                ? params.at("capture_output").get<bool>()
                : true;
            const bool check_exit = params.contains("check_exit")
                ? params.at("check_exit").get<bool>()
                : true;

            // Merge stderr into stdout so we capture both in a single stream.
            const std::string full_cmd = capture_output ? (cmd + " 2>&1") : cmd;

            std::string captured;
            int exit_code = 0;

            if (capture_output) {
                FILE* raw = ORCHA_POPEN(full_cmd.c_str(), "r");
                if (!raw) {
                    throw std::runtime_error("Failed to launch process");
                }
                std::array<char, 4096> buf{};
                while (std::fgets(buf.data(), static_cast<int>(buf.size()), raw) != nullptr) {
                    captured.append(buf.data());
                }
                const int rc = ORCHA_PCLOSE(raw);
#if defined(_WIN32)
                exit_code = rc;
#else
                exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
#endif
            } else {
                const int rc = std::system(full_cmd.c_str());
#if defined(_WIN32)
                exit_code = rc;
#else
                exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
#endif
            }

            const bool ok = (exit_code == 0);
            result["success"]   = (ok || !check_exit);
            result["exit_code"] = exit_code;
            result["output"]    = captured;

            if (check_exit && !ok) {
                result["error"] = "Command exited with code " + std::to_string(exit_code);
            }
            return result;
        } catch (const std::exception& ex) {
            Orcha::Json err;
            err["success"] = false;
            err["error"]   = std::string(ex.what());
            return err;
        }
    }

    [[nodiscard]] Orcha::Core::CommandMetadata metadata() const override {
        Orcha::Core::CommandMetadata meta;
        meta.name = "shell_exec";
        meta.version = Orcha::kVersion;
        meta.description = "Executes a shell command and captures its output";
        meta.author = "Orcha Team";
        meta.tags = {"utility", "shell", "process"};
        meta.supports_rollback = false;

        Orcha::Core::CommandParameter cmd_p;
        cmd_p.name = "cmd";
        cmd_p.type = "string";
        cmd_p.required = true;
        cmd_p.description = "Shell command line to execute";
        cmd_p.example = "echo hello";
        meta.parameters.push_back(cmd_p);

        Orcha::Core::CommandParameter cap_p;
        cap_p.name = "capture_output";
        cap_p.type = "bool";
        cap_p.required = false;
        cap_p.description = "Capture merged stdout/stderr in the output field";
        cap_p.default_value = "true";
        meta.parameters.push_back(cap_p);

        Orcha::Core::CommandParameter chk_p;
        chk_p.name = "check_exit";
        chk_p.type = "bool";
        chk_p.required = false;
        chk_p.description = "Treat a non-zero exit code as a failure";
        chk_p.default_value = "true";
        meta.parameters.push_back(chk_p);

        return meta;
    }
};

extern "C" Orcha::Core::ICommand* create_command() {
    return new ShellExec();
}
