//
// ShellExec.cpp - Run a shell command, capture stdout/stderr and exit code.
//

#include "../../core/ICommand.hpp"
#include "core/Version.hpp"

#include <cpprest/json.h>

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

    web::json::value execute(const web::json::value& params) override {
        using namespace web;
        using namespace utility;

        json::value result;
        try {
            if (!params.has_field(U("cmd"))) {
                throw std::runtime_error("'cmd' parameter is required");
            }
            const std::string cmd = conversions::to_utf8string(params.at(U("cmd")).as_string());

            const bool capture_output = params.has_field(U("capture_output"))
                ? params.at(U("capture_output")).as_bool()
                : true;
            const bool check_exit = params.has_field(U("check_exit"))
                ? params.at(U("check_exit")).as_bool()
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
            result[U("success")]   = json::value::boolean(ok || !check_exit);
            result[U("exit_code")] = json::value::number(exit_code);
            result[U("output")]    = json::value::string(conversions::to_string_t(captured));

            if (check_exit && !ok) {
                result[U("error")] = json::value::string(
                    conversions::to_string_t("Command exited with code " + std::to_string(exit_code)));
            }
            return result;
        } catch (const std::exception& ex) {
            json::value err;
            err[U("success")] = json::value(false);
            err[U("error")]   = json::value::string(conversions::to_string_t(ex.what()));
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
