#include "../../core/ICommand.hpp"
#include "../HttpClient.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Default CPython version downloaded when the caller doesn't pass `version`.
constexpr const char* kDefaultPythonVersion = "3.13.13";

// python-build-standalone publishes redistributable CPython tarballs for
// macOS/Linux under date-stamped GitHub release tags. The TAG appears in both
// the release path and the asset filename, so the default below bundles
// kDefaultPythonVersion. To download a different version, pass `release_tag`
// matching a release that includes it (see
// https://github.com/astral-sh/python-build-standalone/releases).
constexpr const char* kDefaultPbsReleaseTag = "20260510";

std::string make_download_url(const std::string& version, const std::string& release_tag) {
#if defined(_WIN32)
    (void)release_tag;
#if defined(_M_ARM64)
    const std::string arch = "arm64";
#else
    const std::string arch = "amd64";
#endif
    // python.org Windows "embeddable" package.
    return "https://www.python.org/ftp/python/" + version +
           "/python-" + version + "-embed-" + arch + ".zip";
#else
#  if defined(__APPLE__)
#    if defined(__arm64__)
    constexpr std::string triple = "aarch64-apple-darwin";
#    else
    const std::string triple = "x86_64-apple-darwin";
#    endif
#  else
#    if defined(__aarch64__)
    const std::string triple = "aarch64-unknown-linux-gnu";
#    else
    const std::string triple = "x86_64-unknown-linux-gnu";
#    endif
#  endif
    return "https://github.com/astral-sh/python-build-standalone/releases/download/" +
           release_tag + "/cpython-" + version + "+" + release_tag + "-" + triple +
           "-install_only.tar.gz";
#endif
}

const char* archive_name() {
#if defined(_WIN32)
    return "python-embed.zip";
#else
    return "python.tar.gz";
#endif
}

const char* extract_dir_name() {
#if defined(_WIN32)
    return "python_embed";
#else
    return "python";
#endif
}

}  // namespace

class PythonDownloader final : public Orcha::Core::ICommand {
public:
    Orcha::Json execute(const Orcha::Json& params) override {
        Orcha::Json result = Orcha::Json::object();

        std::string version = kDefaultPythonVersion;
        std::string release_tag = kDefaultPbsReleaseTag;
        if (params.is_object()) {
            if (params.contains("version") && params.at("version").is_string()) {
                version = params.at("version").get<std::string>();
            }
            if (params.contains("release_tag") && params.at("release_tag").is_string()) {
                release_tag = params.at("release_tag").get<std::string>();
            }
        }

        try {
            const std::string url = make_download_url(version, release_tag);
            const std::string archive = archive_name();
            const std::string out_dir = extract_dir_name();

            std::cout << "Downloading embedded Python " << version
                      << " from: " << url << std::endl;

            const Orcha::Http::Response response = Orcha::Http::get(url);
            if (response.status / 100 != 2) {
                throw std::runtime_error(
                    "HTTP " + std::to_string(response.status) + " for " + url);
            }

            {
                std::ofstream out(archive, std::ios::binary | std::ios::trunc);
                if (!out) {
                    throw std::runtime_error("Cannot open output file: " + archive);
                }
                out.write(response.body.data(),
                          static_cast<std::streamsize>(response.body.size()));
            }
            std::cout << "Download complete: " << archive << std::endl;

            // Fresh extraction directory each run so we don't mix versions.
            std::error_code ec;
            fs::remove_all(out_dir, ec);
            fs::create_directories(out_dir);

#if defined(_WIN32)
            const std::string unzip_cmd =
                "powershell -Command \"Expand-Archive -Path " + archive +
                " -DestinationPath " + out_dir + "\"";
            if (system(unzip_cmd.c_str()) != 0) {
                throw std::runtime_error("Unzip failed.");
            }
            const std::string py_path = out_dir + "\\python.exe";
#else
            const std::string untar_cmd = "tar -xzf " + archive + " -C " + out_dir;
            std::cout << "Extracting archive with: " << untar_cmd << std::endl;
            if (system(untar_cmd.c_str()) != 0) {
                throw std::runtime_error("Untar failed.");
            }

            // python-build-standalone `install_only` tarballs unpack into a
            // top-level `python/` directory with bin/, lib/, include/, etc.
            // Walk the tree to find the python3 binary so we don't hardcode
            // the exact internal layout.
            std::string py_path;
            for (const auto& entry : fs::recursive_directory_iterator(out_dir)) {
                if (!entry.is_regular_file()) continue;
                const auto name = entry.path().filename().string();
                if (name == "python3" || name == "python") {
                    py_path = entry.path().string();
                    break;
                }
            }
            if (py_path.empty()) {
                throw std::runtime_error(
                    "python binary not found in extracted folder: " + out_dir);
            }
            std::cout << "Setting executable permissions on: " << py_path << std::endl;
            if (system(("chmod +x \"" + py_path + "\"").c_str()) != 0) {
                throw std::runtime_error("chmod failed at: " + py_path);
            }
#endif

            std::cout << "Embedded Python is ready at: " << py_path << std::endl;
            result["path"] = py_path;
            result["version"] = version;
            result["success"] = true;
        } catch (const std::exception& ex) {
            std::cout << "Error: " << ex.what() << std::endl;
            result["success"] = false;
            result["error"] = ex.what();
        }

        return result;
    }

    [[nodiscard]] std::string name() const override { return "download_python"; }
};

extern "C" Orcha::Core::ICommand* create_command() {
    return new PythonDownloader();
}
