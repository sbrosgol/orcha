//
// ShellExec.cpp - Run a shell command, capture stdout/stderr and exit code.
//

#include "../../core/ICommand.hpp"
#include "../Process.hpp"
#include "core/Version.hpp"

#include <string>

class ShellExec final : public Orcha::Core::ICommand {
public:
    [[nodiscard]] std::string name() const override { return "shell_exec"; }

    Orcha::Json execute(const Orcha::Json& params) override {
        return execute_with_control(params, {});
    }

    Orcha::Json execute_with_control(const Orcha::Json& params,
                                     const Orcha::Core::ExecutionControl& control) override {
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

            // On timeout/cancellation the process group is terminated.
            const auto run = Orcha::Process::run_shell(cmd, capture_output, control);

            if (run.timed_out || run.cancelled) {
                result["success"]   = false;
                result["exit_code"] = run.exit_code;
                result["output"]    = run.output;
                result["error"]     = run.timed_out
                    ? "Timed out; process terminated"
                    : "Cancelled; process terminated";
                return result;
            }

            const bool ok = (run.exit_code == 0);
            result["success"]   = (ok || !check_exit);
            result["exit_code"] = run.exit_code;
            result["output"]    = run.output;

            if (check_exit && !ok) {
                result["error"] = "Command exited with code " + std::to_string(run.exit_code);
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

ORCHA_PLUGIN_ABI_EXPORT()

extern "C" Orcha::Core::ICommand* create_command() {
    return new ShellExec();
}
