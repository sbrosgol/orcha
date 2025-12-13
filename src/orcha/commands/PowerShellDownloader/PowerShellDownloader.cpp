#include "../../core/ICommand.hpp"
#include <cpprest/json.h>
#include <string>
#include <cpprest/filestream.h>
#include <cpprest/http_client.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#if defined(_WIN32)
#if defined(_M_ARM64)
const std::string ps_url = "https://github.com/PowerShell/PowerShell/releases/download/v7.5.2/PowerShell-7.5.2-win-arm64.zip";
#else
const std::string ps_url = "https://github.com/PowerShell/PowerShell/releases/download/v7.5.2/PowerShell-7.5.2-win-x64.zip";
#endif
const std::string ps_archive = "PowerShell.zip";
const std::string ps_folder = "pwsh_win";
#else
#if defined(__APPLE__)
#if defined(__arm64__)
const std::string ps_url = "https://github.com/PowerShell/PowerShell/releases/download/v7.5.2/powershell-7.5.2-osx-arm64.tar.gz";
#else
const std::string ps_url = "https://github.com/PowerShell/PowerShell/releases/download/v7.5.2/powershell-7.5.2-osx-x64.tar.gz";
#endif
#else
#if defined(__aarch64__)
const std::string ps_url = "https://github.com/PowerShell/PowerShell/releases/download/v7.5.2/powershell-7.5.2-linux-arm64.tar.gz";
#else
const std::string ps_url = "https://github.com/PowerShell/PowerShell/releases/download/v7.5.2/powershell-7.5.2-linux-x64.tar.gz";
#endif
#endif
const std::string ps_archive = "powershell.tar.gz";
const std::string ps_folder = "pwsh";
#endif

class PowerShellDownloader final : public Orcha::Core::ICommand {
public:
    web::json::value execute(const web::json::value &params) override {
        using namespace web;
        using namespace utility;
        json::value result;

        try {
            http::client::http_client client(utility::conversions::to_string_t(ps_url));
            std::cout << "Downloading PowerShell Core from: " << ps_url << std::endl;
            const auto response = client.request(web::http::methods::GET).get();

            const concurrency::streams::ostream out = concurrency::streams::fstream::open_ostream(ps_archive).get();
            (void) response.body().read_to_end(out.streambuf()).wait();
            (void) out.close().wait();

            std::cout << "Download complete: " << ps_archive << std::endl;

            // Extract and find pwsh binary
#if defined(_WIN32)
            std::string unzip_cmd = "powershell -Command \"Expand-Archive -Path " + ps_archive + " -DestinationPath " + ps_folder + "\"";
            int unzip_result = system(unzip_cmd.c_str());
            if (unzip_result != 0) throw std::runtime_error("Unzip failed.");
            std::string ps_path = ps_folder + "\\pwsh.exe";
#else
            std::string mkdir_cmd = "mkdir -p " + ps_folder;
            system(mkdir_cmd.c_str());

            const std::string untar_cmd = "tar -xzf " + ps_archive + " -C " + ps_folder;
            std::cout << "Extracting archive with: " << untar_cmd << std::endl;
            int untar_result = system(untar_cmd.c_str());
            if (untar_result != 0)
                throw std::runtime_error("Untar failed.");

            std::string ps_path;

            for (const auto &entry: fs::recursive_directory_iterator(ps_folder)) {
                if (entry.path().filename() == "pwsh" && fs::is_regular_file(entry)) {
                    ps_path = entry.path().string();
                    break;
                }
            }

            if (ps_path.empty()) {
                throw std::runtime_error("pwsh binary not found in extracted folder: " + ps_folder);
            }

            std::cout << "Setting executable permissions on: " << ps_path << std::endl;
            int chmod_result = system(("chmod +x \"" + ps_path + "\"").c_str());
            if (chmod_result != 0)
                throw std::runtime_error("chmod failed at: " + ps_path);
#endif
            std::cout << "PowerShell Core is ready at: " << ps_path << std::endl;

            result[U("path")] = json::value::string(conversions::to_string_t(ps_path));
            result[U("success")] = json::value(true);
        } catch (const std::exception &ex) {
            std::cout << "Error: " << ex.what() << std::endl;
            result[U("success")] = json::value(false);
            result[U("error")] = json::value::string(ex.what());
        }

        return result;
    }

    [[nodiscard]] std::string name() const override { return "download_pwsh"; }
};

extern "C" Orcha::Core::ICommand *create_command() {
    return new PowerShellDownloader();
}