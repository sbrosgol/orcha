#include "../../core/ICommand.hpp"
#include "../HttpClient.hpp"
#include <string>
#include <filesystem>
#include <fstream>
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
    Orcha::Json execute(const Orcha::Json &params) override {
        (void) params;
        Orcha::Json result = Orcha::Json::object();

        try {
            std::cout << "Downloading PowerShell Core from: " << ps_url << std::endl;
            const Orcha::Http::Response response = Orcha::Http::get(ps_url);
            if (response.status / 100 != 2) {
                throw std::runtime_error(
                    "HTTP " + std::to_string(response.status) + " for " + ps_url);
            }

            {
                std::ofstream out(ps_archive, std::ios::binary | std::ios::trunc);
                if (!out) {
                    throw std::runtime_error("Cannot open output file: " + ps_archive);
                }
                out.write(response.body.data(),
                          static_cast<std::streamsize>(response.body.size()));
            }

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

            result["path"] = ps_path;
            result["success"] = true;
        } catch (const std::exception &ex) {
            std::cout << "Error: " << ex.what() << std::endl;
            result["success"] = false;
            result["error"] = ex.what();
        }

        return result;
    }

    [[nodiscard]] std::string name() const override { return "download_pwsh"; }
};

extern "C" Orcha::Core::ICommand *create_command() {
    return new PowerShellDownloader();
}