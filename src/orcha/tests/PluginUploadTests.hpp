//
// PluginUploadTests.hpp - Plugin uploads: package checks (core/PluginPackage),
// staged install with upgrade/rollback and crash recovery
// (core/PluginInstaller), the HTTP API (PluginUploadRoute) and config.
//
// Installer tests use real temporary directories and the real test plugins:
// ORCHA_TEST_SLOW_PLUGIN_PATH (loads) and ORCHA_TEST_LEGACY_PLUGIN_PATH (built
// without the ABI export, so the registry refuses it after dlopen), which is
// how a failed upgrade is produced.
//

#pragma once

#include "../agent/routes/PluginAdminRoute.hpp"
#include "../agent/routes/PluginUploadRoute.hpp"
#include "../config/ConfigValidator.hpp"
#include "../config/PluginUploadConfig.hpp"
#include "../config/YamlConfiguration.hpp"
#include "../core/CommandRegistry.hpp"
#include "../core/PluginInstaller.hpp"
#include "../core/PluginManager.hpp"
#include "../core/PluginPackage.hpp"
#include "mocks/MockLogger.hpp"

#include "Assertions.hpp"
#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

namespace Orcha::Tests {

    namespace upload_tests {
        namespace fs = std::filesystem;
        namespace pkg = Core::PluginPackage;
        using K = Core::PluginUploadErrorKind;

        inline std::string from_hex(std::string_view hex) {
            std::string out;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
            }
            return out;
        }

        inline std::string read_file(const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        inline void write_file(const fs::path& p, const std::string& bytes) {
            fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << bytes;
        }

        inline std::size_t logged(const Mocks::MockLogger& logger, const std::string& needle) {
            const auto entries = logger.get_entries();
            return static_cast<std::size_t>(std::ranges::count_if(entries, [&](const auto& e) {
                return e.message.find(needle) != std::string::npos;
            }));
        }

        /// An Ed25519 key pair that signs like scripts/orcha-plugin-sign.sh.
        class TestKey {
        public:
            TestKey() {
                EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
                ORCHA_ASSERT(ctx && EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_keygen(ctx, &key_) == 1);
                EVP_PKEY_CTX_free(ctx);
                std::size_t len = 32;
                public_.resize(32);
                ORCHA_ASSERT(EVP_PKEY_get_raw_public_key(key_, reinterpret_cast<unsigned char*>(public_.data()), &len) == 1);
            }
            ~TestKey() { EVP_PKEY_free(key_); }
            TestKey(const TestKey&) = delete;
            TestKey& operator=(const TestKey&) = delete;

            [[nodiscard]] std::string public_base64() const { return pkg::base64_encode(public_); }
            [[nodiscard]] std::string id() const { return pkg::key_id(public_); }

            [[nodiscard]] std::string sign(const std::string& filename, const std::string& library,
                                           const std::optional<std::string>& manifest = std::nullopt) const {
                const auto message = pkg::signing_message(filename, pkg::sha256_hex(library),
                                                          pkg::sha256_hex(manifest.value_or("")));
                EVP_MD_CTX* ctx = EVP_MD_CTX_new();
                std::string sig(64, '\0');
                std::size_t len = sig.size();
                ORCHA_ASSERT(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key_) == 1);
                ORCHA_ASSERT(EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                                            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1);
                EVP_MD_CTX_free(ctx);
                return sig;
            }

        private:
            EVP_PKEY* key_ = nullptr;
            std::string public_;
        };

        /// A bundled and an upload directory, a real manager and an installer.
        struct Env {
            fs::path root;
            fs::path bundled;   ///< plugins.directory
            fs::path plugins;   ///< plugins.upload.directory (where installs go)
            fs::path staging;
            TestKey key;
            std::shared_ptr<Mocks::MockLogger> logger = std::make_shared<Mocks::MockLogger>();
            std::shared_ptr<Core::CommandRegistry> registry = std::make_shared<Core::CommandRegistry>(logger);
            std::shared_ptr<Core::PluginDiscoveryService> discovery = std::make_shared<Core::PluginDiscoveryService>();
            std::shared_ptr<Core::PluginManager> manager;
            std::shared_ptr<Core::FilePluginDenylist> denylist;
            std::shared_ptr<Core::PluginInstaller> installer;

            Env() {
                std::random_device rd;
                root = fs::temp_directory_path() / ("orcha-upload-test-" + std::to_string(rd()));
                bundled = root / "bundled";
                plugins = root / "uploads";
                staging = plugins / Core::PluginInstaller::kStagingDirName;
                fs::create_directories(bundled);
                fs::create_directories(plugins);
                manager = std::make_shared<Core::PluginManager>(registry, discovery, logger);
                denylist = std::make_shared<Core::FilePluginDenylist>(root / "denylist.txt");
                installer = make(options());
            }
            ~Env() {
                for (const auto& p : manager->get_all_plugins()) (void)manager->unload_plugin(p.name);
                std::error_code ec;
                fs::remove_all(root, ec);
            }

            [[nodiscard]] Core::PluginUploadOptions options() const {
                Core::PluginUploadOptions o;
                o.enabled = true;
                o.upload_directory = plugins;
                o.bundled_directory = bundled;
                o.public_keys = {{key.public_base64(), {}, false}};
                return o;
            }
            [[nodiscard]] std::shared_ptr<Core::PluginInstaller> make(Core::PluginUploadOptions o) const {
                return std::make_shared<Core::PluginInstaller>(std::move(o), manager, discovery, denylist,
                                                               logger, nullptr);
            }

            /// An upload of @p library_path as @p filename, signed with this key.
            [[nodiscard]] Core::PluginUploadRequest upload(const std::string& library_path, const std::string& filename,
                                                           std::optional<std::string> manifest) const {
                Core::PluginUploadRequest r;
                r.filename = filename;
                r.library = read_file(library_path);
                r.manifest = std::move(manifest);
                r.signature = key.sign(filename, r.library, r.manifest);
                r.user = "tester";
                return r;
            }

            /// Stage then install; fails the test on any error.
            Core::InstalledPlugin install(const Core::PluginUploadRequest& r) {
                auto staged = installer->stage(r);
                if (staged.is_err()) throw std::runtime_error("stage: " + staged.error().message);
                auto done = installer->install(staged.value().id, "tester");
                if (done.is_err()) throw std::runtime_error("install: " + done.error().message);
                return done.value();
            }

            /// Directories holding @p name's installs (<plugins>/<name>-<id>).
            [[nodiscard]] std::vector<fs::path> install_dirs(const std::string& name) const {
                std::vector<fs::path> out;
                for (const auto& e : fs::directory_iterator(plugins)) {
                    if (e.is_directory() && e.path().filename().string().starts_with(name + "-")) out.push_back(e.path());
                }
                return out;
            }
        };

        inline std::string lib_name(const std::string& stem) {
            return "lib" + stem + Core::IPluginDiscovery::get_library_extension();
        }

        inline std::string manifest(const std::string& name, const std::string& version) {
            return R"({"name":")" + name + R"(","version":")" + version + R"(","description":"Upload test"})";
        }
    } // namespace upload_tests

    // ========== Package checks ==========

    inline void test_plugin_package_checks() {
        using namespace upload_tests;
        // base64
        for (const std::string s : {std::string(), std::string("f"), std::string("fo"), std::string("foo"),
                                    std::string("foob"), std::string("fooba"), std::string("foobar"),
                                    std::string("\0\xff\x10", 3)}) {
            ORCHA_ASSERT(pkg::base64_decode(pkg::base64_encode(s)) == s);
        }
        ORCHA_ASSERT(pkg::base64_encode("foobar") == "Zm9vYmFy");
        ORCHA_ASSERT(pkg::base64_decode("Zm9v\nYmE=") == "fooba");
        ORCHA_ASSERT(!pkg::base64_decode("Zm9v*"));
        ORCHA_ASSERT(!pkg::base64_decode("Zg==Zg"));
        ORCHA_ASSERT(!pkg::base64_decode("Zh=="));   // non-zero padding bits: not canonical

        // SHA-256 ("abc", FIPS 180-2)
        ORCHA_ASSERT(pkg::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        // Ed25519, RFC 8032 section 7.1 tests 1 and 2.
        const auto pk1 = from_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        const auto sig1 = from_hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        ORCHA_ASSERT(pkg::verify_ed25519(pk1, sig1, ""));
        const auto pk2 = from_hex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c");
        const auto sig2 = from_hex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00");
        ORCHA_ASSERT(pkg::verify_ed25519(pk2, sig2, std::string(1, '\x72')));
        ORCHA_ASSERT(!pkg::verify_ed25519(pk2, sig2, std::string(1, '\x73')));   // other message
        ORCHA_ASSERT(!pkg::verify_ed25519(pk1, sig2, std::string(1, '\x72')));   // other key
        ORCHA_ASSERT(!pkg::verify_ed25519(pk1.substr(1), sig1, ""));             // wrong sizes
        ORCHA_ASSERT(pkg::key_id(pk1).size() == 16);

        // Golden vectors from scripts/orcha-plugin-sign.sh (OpenSSL 3 CLI): the
        // script and signing_message() must produce the same signed bytes.
        const auto script_key = *pkg::base64_decode("g6lxoFAKtt9aRy3gzH3iWnARD/txkgswH9mAk0CuR4w=");
        const auto golden = pkg::signing_message("libgolden.so", pkg::sha256_hex("orcha golden library\n"),
                                                 pkg::sha256_hex(R"({"name":"golden","version":"1.0.0"})"));
        ORCHA_ASSERT(pkg::verify_ed25519(script_key, *pkg::base64_decode(
            "TQM1ukQv9qaJeWdg9ifjUrv947n4u/QHZsZ43fHtg8hY72Snr0xPjqSw+S+FMXVacx8E4FAPSBwNAA5A2yNQAw=="), golden));
        const auto no_manifest_sig = *pkg::base64_decode(
            "4g57okYuCm28k9DMlwGumAKbv8QaEpwgM/c8Rn0wDfytaYquAs9vj3w9yHBBva0yXYaKal06qJ8TvOQGRkRwCg==");
        ORCHA_ASSERT(pkg::verify_ed25519(script_key, no_manifest_sig,
            pkg::signing_message("libnoman.so", pkg::sha256_hex("x"), pkg::sha256_hex(""))));
        // The file name is part of what is signed.
        ORCHA_ASSERT(!pkg::verify_ed25519(script_key, no_manifest_sig,
            pkg::signing_message("libother.so", pkg::sha256_hex("x"), pkg::sha256_hex(""))));

        // Names
        for (const char* ok : {"echo", "http_request", "a", "My-Plugin.v2"}) ORCHA_ASSERT(pkg::is_valid_plugin_name(ok));
        for (const char* bad : {"", "../x", "a/b", ".hidden", "_x", "a..b", "a b", "x\\y", "x.so", "x.dylib", "x.part"}) {
            ORCHA_ASSERT(!pkg::is_valid_plugin_name(bad));
        }
        ORCHA_ASSERT(!pkg::is_valid_plugin_name(std::string(65, 'a')));
        ORCHA_ASSERT(pkg::plugin_name_from_filename("libEchoCommand.dylib") == "EchoCommand");
        ORCHA_ASSERT(pkg::plugin_name_from_filename("tool.so") == "tool");

        // Key-scope globs
        ORCHA_ASSERT(pkg::name_matches("acme-*", "acme-billing") && pkg::name_matches("acme-*", "acme-"));
        ORCHA_ASSERT(!pkg::name_matches("acme-*", "xacme-billing") && !pkg::name_matches("acme-*", "acme"));
        ORCHA_ASSERT(pkg::name_matches("*", "anything") && pkg::name_matches("a?c", "abc") && !pkg::name_matches("a?c", "ac"));
        ORCHA_ASSERT(pkg::name_matches("*-db-*", "pg-db-backup") && !pkg::name_matches("*-db-*", "pg-db"));
        ORCHA_ASSERT(pkg::name_matches("billing", "billing") && !pkg::name_matches("billing", "billing2"));
        ORCHA_ASSERT(!pkg::name_matches("a*a*a*a*a*b", std::string(40, 'a')));   // no blow-up

        // Version changes
        using I = Core::PluginInstaller;
        ORCHA_ASSERT(I::version_change("1.0.0", "1.1.0") == "upgrade");
        ORCHA_ASSERT(I::version_change("1.10.0", "1.9.0") == "downgrade");
        ORCHA_ASSERT(I::version_change("2.0", "2.0.0") == "reinstall");
        ORCHA_ASSERT(I::version_change("1.0.0-rc1", "1.0.0-rc2") == "upgrade");
        ORCHA_ASSERT(I::version_change("", "1.0") == "replace" && I::version_change("1.0", "") == "replace");

        // Binary headers: a real plugin built here loads; others are refused unread.
#ifdef ORCHA_TEST_SLOW_PLUGIN_PATH
        const auto real = pkg::inspect_library(read_file(ORCHA_TEST_SLOW_PLUGIN_PATH));
        ORCHA_ASSERT(real.loadable_here && real.arch.find(pkg::host_platform().arch) != std::string::npos);
#endif
        std::string elf(64, '\0');
        elf.replace(0, 4, "\x7f" "ELF");
        elf[4] = 2; elf[5] = 1; elf[16] = 3; elf[18] = 62;      // 64-bit LE ET_DYN x86_64
        const auto elf_info = pkg::inspect_library(elf);
        ORCHA_ASSERT(elf_info.format == "ELF" && elf_info.arch == "x86_64");
        if (pkg::host_platform().format != "ELF" || pkg::host_platform().arch != "x86_64") {
            ORCHA_ASSERT(!elf_info.loadable_here && !elf_info.problem.empty());
        }
        elf[16] = 2;                                            // ET_EXEC: not a library
        ORCHA_ASSERT(pkg::inspect_library(elf).problem.find("Not a shared library") == 0);
        std::string macho(32, '\0');
        macho.replace(0, 4, "\xcf\xfa\xed\xfe");                // MH_MAGIC_64
        macho.replace(4, 4, std::string("\x07\x00\x00\x01", 4));   // x86_64 (has NULs: explicit length)
        macho[12] = 6;                                          // MH_DYLIB
        const auto macho_info = pkg::inspect_library(macho);
        ORCHA_ASSERT(macho_info.format == "Mach-O" && macho_info.arch == "x86_64");
        macho[12] = 2;                                          // MH_EXECUTE
        ORCHA_ASSERT(!pkg::inspect_library(macho).loadable_here);
        ORCHA_ASSERT(pkg::inspect_library("#!/bin/sh\necho hi\n").problem.find("Not a shared library") == 0);
        ORCHA_ASSERT(!pkg::inspect_library("").loadable_here);
        std::cout << "[PASS] test_plugin_package_checks\n";
    }

    // ========== Installer ==========

    inline void test_plugin_uploads_fail_closed() {
        using namespace upload_tests;
        Env env;
        ORCHA_ASSERT(env.installer->enabled());
        ORCHA_ASSERT(fs::is_directory(env.staging));
        const auto reason = [&](auto change) {
            auto o = env.options();
            change(o);
            return env.make(o)->disabled_reason();
        };
        ORCHA_ASSERT(reason([](auto& o) { o.enabled = false; }).find("plugins.upload.enabled") != std::string::npos);
        ORCHA_ASSERT(reason([](auto& o) { o.admin_auth_required = false; }).find("admin.auth_required") != std::string::npos);
        ORCHA_ASSERT(reason([](auto& o) { o.public_keys.clear(); }).find("No signing keys") == 0);
        ORCHA_ASSERT(reason([](auto& o) { o.public_keys = {{"not-a-key", {}, false}}; }).find("public_keys[0]") != std::string::npos);
        ORCHA_ASSERT(reason([&](auto& o) { o.public_keys = {{env.key.public_base64(), {"bad/glob"}, false}}; })
                         .find("not a name pattern") != std::string::npos);
        ORCHA_ASSERT(reason([](auto& o) { o.config_errors = {"plugins.upload.public_keys[1] must be a key"}; })
                         .find("public_keys[1]") != std::string::npos);
        ORCHA_ASSERT(reason([](auto& o) { o.upload_directory.clear(); }).find("plugins.upload.directory") != std::string::npos);
        ORCHA_ASSERT(reason([&](auto& o) { o.upload_directory = env.bundled / "uploads"; }).find("inside each other") != std::string::npos);
        ORCHA_ASSERT(reason([&](auto& o) { o.bundled_directory = env.plugins / "x"; }).find("inside each other") != std::string::npos);

        auto off = env.make([&] { auto o = env.options(); o.enabled = false; return o; }());
        auto staged = off->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), std::nullopt));
        ORCHA_ASSERT(staged.is_err() && staged.error().kind == K::Disabled);
        ORCHA_ASSERT(off->status()["enabled"] == false && off->status().contains("reason"));
        std::cout << "[PASS] test_plugin_uploads_fail_closed\n";
    }

    inline void test_plugin_upload_rejects_bad_uploads() {
        using namespace upload_tests;
        Env env;
        const auto kind_of = [&](const Core::PluginUploadRequest& r) {
            auto res = env.installer->stage(r);
            return res.is_err() ? res.error().kind : K::Internal;
        };
        const auto good = [&] { return env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), manifest("slowtest", "1.0.0")); };

        auto r = good(); r.filename = "slowtest.txt";               ORCHA_ASSERT(kind_of(r) == K::Invalid);
        r = good(); r.filename = "../" + lib_name("x");             ORCHA_ASSERT(kind_of(r) == K::Invalid);
        r = good(); r.filename = "li\nb" + lib_name("x");           ORCHA_ASSERT(kind_of(r) == K::Invalid);
        r = good(); r.library = "#!/bin/sh\nrm -rf /\n"; r.signature = env.key.sign(r.filename, r.library, r.manifest);
        ORCHA_ASSERT(kind_of(r) == K::Invalid);                      // not a library, refused unread
        r = good(); r.signature[0] ^= 1;                             ORCHA_ASSERT(kind_of(r) == K::BadSignature);
        r = good(); r.manifest = manifest("slowtest", "9.9.9");      ORCHA_ASSERT(kind_of(r) == K::BadSignature);  // swapped manifest
        r = good(); r.signature.resize(10);                          ORCHA_ASSERT(kind_of(r) == K::BadSignature);
        TestKey stranger;
        r = good(); r.signature = stranger.sign(r.filename, r.library, r.manifest); ORCHA_ASSERT(kind_of(r) == K::BadSignature);
        // Rename-and-replace: a library signed as libslowtest without a manifest
        // must not become another plugin by uploading it as libvictim.
        r = env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), std::nullopt);
        r.filename = lib_name("victim");                             ORCHA_ASSERT(kind_of(r) == K::BadSignature);
        r = env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), std::string(R"({"version":"1"})"));
        ORCHA_ASSERT(kind_of(r) == K::Invalid);                      // manifest without name
        r = env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), manifest("../../evil", "1"));
        ORCHA_ASSERT(kind_of(r) == K::Invalid);
        r = env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), manifest("evil.so", "1"));
        ORCHA_ASSERT(kind_of(r) == K::Invalid);
        r = env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), std::string(R"({"name":"x","entry_point":"other.so"})"));
        ORCHA_ASSERT(kind_of(r) == K::Invalid);
        r = env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), std::string("{not json"));
        ORCHA_ASSERT(kind_of(r) == K::Invalid);

        auto small = env.options();
        small.max_bytes = 100;
        auto res = env.make(small)->stage(good());
        ORCHA_ASSERT(res.is_err() && res.error().kind == K::TooLarge);

        ORCHA_ASSERT(env.installer->list().empty());
        // Every refusal is audited.
        ORCHA_ASSERT(logged(*env.logger, "Plugin upload refused") >= 15);
        std::cout << "[PASS] test_plugin_upload_rejects_bad_uploads\n";
    }

    inline void test_plugin_upload_install_upgrade_and_rollback() {
        using namespace upload_tests;
        Env env;
        env.denylist->add("slowtest");   // installing it is an explicit enable

        // Stage: stored in the hidden staging directory, nothing loaded or discovered.
        auto staged = env.installer->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"),
                                                      manifest("slowtest", "1.0.0")));
        ORCHA_ASSERT(staged.is_ok());
        const auto s = staged.value();
        ORCHA_ASSERT(s.name == "slowtest" && s.version == "1.0.0" && s.uploaded_by == "tester");
        ORCHA_ASSERT(!s.replaces_version && s.change == "new" && s.signer == env.key.id());
        ORCHA_ASSERT(env.installer->list().size() == 1 && env.install_dirs("slowtest").empty());
        ORCHA_ASSERT(env.discovery->scan_plugins(env.plugins).empty());   // .orcha-uploads is skipped
        ORCHA_ASSERT(!env.manager->get_plugin_metadata("slowtest"));

        // Install: a new <name>-<id> directory, loaded, staging cleaned,
        // denylist cleared, provenance recorded.
        auto installed = env.installer->install(s.id, "tester");
        ORCHA_ASSERT(installed.is_ok() && !installed.value().previous_version);
        ORCHA_ASSERT(env.install_dirs("slowtest").size() == 1);
        const auto v1_dir = env.install_dirs("slowtest").front();
        ORCHA_ASSERT(v1_dir.filename() == "slowtest-" + s.id.substr(0, 8));
        ORCHA_ASSERT(fs::exists(v1_dir / lib_name("slowtest")) && fs::exists(v1_dir / "manifest.json"));
        const auto v1 = env.manager->get_plugin_metadata("slowtest");
        ORCHA_ASSERT(v1 && v1->version == "1.0.0");
        ORCHA_ASSERT(env.registry->get_command("slow_test_plugin") != nullptr);
        ORCHA_ASSERT(env.installer->list().empty() && !fs::exists(env.staging / s.id));
        ORCHA_ASSERT(!env.denylist->contains("slowtest"));
        const auto origin = Core::PluginInstaller::provenance(*v1);
        ORCHA_ASSERT(origin && (*origin)["signer"] == env.key.id() && (*origin)["sha256"] == s.sha256 &&
                     (*origin)["installed_by"] == "tester");

        // A failing upgrade (plugin ABI mismatch) restores and reloads v1.
        write_file(v1_dir / "settings.json", "{}");   // not the plugin's: must survive everything
        auto bad = env.installer->stage(env.upload(ORCHA_TEST_LEGACY_PLUGIN_PATH, lib_name("slowtest_legacy"),
                                                   manifest("slowtest", "2.0.0")));
        ORCHA_ASSERT(bad.is_ok() && bad.value().replaces_version == "1.0.0" && bad.value().change == "upgrade");
        auto failed = env.installer->install(bad.value().id, "tester");
        ORCHA_ASSERT(failed.is_err() && failed.error().kind == K::LoadFailed);
        ORCHA_ASSERT(failed.error().rolled_back);
        ORCHA_ASSERT(failed.error().message.find("plugin ABI") != std::string::npos);
        ORCHA_ASSERT(env.manager->get_plugin_metadata("slowtest")->version == "1.0.0");
        ORCHA_ASSERT(env.registry->get_command("slow_test_plugin") != nullptr);
        ORCHA_ASSERT(fs::exists(v1_dir / lib_name("slowtest")) && fs::exists(v1_dir / "manifest.json"));
        ORCHA_ASSERT(fs::exists(v1_dir / "settings.json"));
        ORCHA_ASSERT(Core::PluginInstaller::provenance(*env.manager->get_plugin_metadata("slowtest")).has_value());
        ORCHA_ASSERT(env.install_dirs("slowtest").size() == 1);   // the failed one's directory is gone
        ORCHA_ASSERT(!env.denylist->contains("slowtest"));
        ORCHA_ASSERT(env.installer->list().size() == 1);          // kept for the admin to discard
        ORCHA_ASSERT(env.installer->discard(bad.value().id, "tester").is_ok() && env.installer->list().empty());
        ORCHA_ASSERT(logged(*env.logger, "Plugin upload discarded") == 1);

        // A good upgrade while a workflow still holds v1: the new path means
        // dlopen maps the new library (not v1's image again).
        auto busy = env.registry->get_command("slow_test_plugin");
        auto up = env.install(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), manifest("slowtest", "2.0.0")));
        ORCHA_ASSERT(up.previous_version == "1.0.0");
        const auto v2 = env.manager->get_plugin_metadata("slowtest");
        ORCHA_ASSERT(v2 && v2->version == "2.0.0" && v2->library_path.parent_path() != v1_dir);
        auto now = env.registry->get_command("slow_test_plugin");
        ORCHA_ASSERT(now != nullptr && now != busy);
        // v1's own files moved out; the rest of its directory stays.
        ORCHA_ASSERT(!fs::exists(v1_dir / lib_name("slowtest")) && !fs::exists(v1_dir / "manifest.json"));
        ORCHA_ASSERT(fs::exists(v1_dir / "settings.json"));
        busy.reset();

        // Unknown or malformed ids.
        ORCHA_ASSERT(env.installer->install("0123456789abcdef0123456789abcdef", "t").error().kind == K::NotFound);
        ORCHA_ASSERT(env.installer->discard("../../plugins", "t").error().kind == K::NotFound);
        std::cout << "[PASS] test_plugin_upload_install_upgrade_and_rollback\n";
    }

    inline void test_plugin_upload_refuses_unsafe_installs() {
        using namespace upload_tests;
        Env env;
        // Tampered at rest: someone with access to the staging area swaps the library.
        auto staged = env.installer->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), std::nullopt));
        ORCHA_ASSERT(staged.is_ok() && staged.value().name == "slowtest" && !staged.value().has_manifest);
        ORCHA_ASSERT(staged.value().version.empty());
        const auto id = staged.value().id;
        const auto lib = env.staging / id / lib_name("slowtest");
        const auto original = read_file(lib);
        write_file(lib, read_file(ORCHA_TEST_LEGACY_PLUGIN_PATH));
        auto installed = env.installer->install(id, "tester");
        ORCHA_ASSERT(installed.is_err() && installed.error().kind == K::Invalid);
        ORCHA_ASSERT(env.install_dirs("slowtest").empty() && !env.manager->get_plugin_metadata("slowtest"));
        write_file(lib, original);

        // Another process holds the install lock.
        {
            const int fd = ::open((env.staging / ".lock").c_str(), O_RDWR | O_CREAT, 0600);
            ORCHA_ASSERT(fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0);
            auto locked = env.installer->install(id, "tester");
            ::close(fd);
            ORCHA_ASSERT(locked.is_err() && locked.error().kind == K::Conflict);
            ORCHA_ASSERT(locked.error().message.find("Another Orcha process") == 0);
        }

        // An unfinished install (journal left behind) blocks installs and its discard.
        write_file(env.staging / id / "install.json", "{}");
        auto blocked = env.installer->install(id, "tester");
        ORCHA_ASSERT(blocked.is_err() && blocked.error().message.find("did not finish") != std::string::npos);
        ORCHA_ASSERT(env.installer->discard(id, "t").error().kind == K::Conflict);
        fs::remove(env.staging / id / "install.json");

        // A plugin directory that holds other libraries is not taken apart.
        const auto shared = env.plugins / "shared";
        fs::create_directories(shared);
        fs::copy_file(ORCHA_TEST_SLOW_PLUGIN_PATH, shared / lib_name("slowtest"));
        write_file(shared / "manifest.json", manifest("slowtest", "0.9.0"));
        write_file(shared / lib_name("other"), "not loaded");
        auto conflict = env.installer->install(id, "tester");
        ORCHA_ASSERT(conflict.is_err() && conflict.error().kind == K::Conflict);
        ORCHA_ASSERT(fs::exists(shared / lib_name("slowtest")) && fs::exists(shared / lib_name("other")));
        std::cout << "[PASS] test_plugin_upload_refuses_unsafe_installs\n";
    }

    inline void test_plugin_install_recovers_after_crash() {
        using namespace upload_tests;
        Env env;
        // Simulate a server that died while loading v2: v1's files are in the
        // backup, v2's directory exists, the plugin is denylisted, the journal stays.
        auto staged = env.installer->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"),
                                                      manifest("slowtest", "2.0.0")));
        ORCHA_ASSERT(staged.is_ok());
        const auto upload = env.staging / staged.value().id;
        const auto v1_dir = env.plugins / "slowtest-old";
        const auto v2_dir = env.plugins / "slowtest-new";
        write_file(upload / "previous" / lib_name("slowtest"), "v1 library");
        write_file(upload / "previous" / "manifest.json", "v1 manifest");
        write_file(v1_dir / "settings.json", "kept");
        write_file(v2_dir / lib_name("slowtest"), "v2 library");
        env.denylist->add("slowtest");
        const Json restore = Json::array({
            Json::array({(upload / "previous" / lib_name("slowtest")).string(), (v1_dir / lib_name("slowtest")).string()}),
            Json::array({(upload / "previous" / "manifest.json").string(), (v1_dir / "manifest.json").string()})});
        write_file(upload / "install.json", Json{{"name", "slowtest"}, {"was_denied", false},
                                                  {"created_dir", v2_dir.string()}, {"restore", restore}}.dump());
        // A journal pointing outside the plugin directory is never acted on.
        write_file(env.root / "precious" / "data.bin", "keep");
        const auto evil = env.staging / "cccccccccccccccccccccccccccccccc";
        write_file(evil / "install.json", Json{{"name", "x"}, {"created_dir", (env.root / "precious").string()},
                                                {"restore", Json::array()}}.dump());
        // An upload that never finished writing.
        write_file(env.staging / ".tmp-eeee" / "partial", "x");

        env.installer->recover_interrupted();
        ORCHA_ASSERT(read_file(v1_dir / lib_name("slowtest")) == "v1 library");
        ORCHA_ASSERT(read_file(v1_dir / "manifest.json") == "v1 manifest");
        ORCHA_ASSERT(read_file(v1_dir / "settings.json") == "kept");
        ORCHA_ASSERT(!fs::exists(v2_dir) && !fs::exists(upload / "previous") && !fs::exists(upload / "install.json"));
        ORCHA_ASSERT(!env.denylist->contains("slowtest"));
        // The upload is kept and marked: retrying it is a deliberate choice.
        auto marked = env.installer->list();
        ORCHA_ASSERT(marked.size() == 1 && marked.front().install_crashed_at.has_value());
        // The bad journal is left alone (and blocks installs until someone looks).
        ORCHA_ASSERT(read_file(env.root / "precious" / "data.bin") == "keep" && fs::exists(evil / "install.json"));
        ORCHA_ASSERT(logged(*env.logger, "outside plugins.upload.directory") >= 1);
        ORCHA_ASSERT(!fs::exists(env.staging / ".tmp-eeee"));
        std::cout << "[PASS] test_plugin_install_recovers_after_crash\n";
    }

    inline void test_plugin_sources() {
        using namespace upload_tests;
        Env env;
        write_file(env.bundled / "a" / "manifest.json", manifest("alpha", "1.0.0"));
        write_file(env.bundled / "a" / lib_name("alpha"), "x");
        write_file(env.bundled / "b" / "manifest.json", manifest("beta", "1.0.0"));
        write_file(env.bundled / "b" / lib_name("beta"), "x");
        write_file(env.plugins / "beta-1" / "manifest.json", manifest("beta", "2.0.0"));
        write_file(env.plugins / "beta-1" / lib_name("beta"), "x");
        write_file(env.plugins / "gamma-1" / "manifest.json", manifest("gamma", "0.1.0"));
        write_file(env.plugins / "gamma-1" / lib_name("gamma"), "x");
        write_file(env.staging / "staged" / lib_name("hidden"), "x");   // staging is never a plugin

        const Core::PluginSources src{env.bundled, env.plugins};
        const auto entries = src.scan(*env.discovery);
        ORCHA_ASSERT(entries.size() == 3);
        const auto find = [&](const std::string& n) {
            return *std::ranges::find_if(entries, [&](const auto& e) { return e.meta.name == n; });
        };
        ORCHA_ASSERT(!find("alpha").uploaded && !find("alpha").overrides);
        ORCHA_ASSERT(find("beta").uploaded && find("beta").meta.version == "2.0.0");
        ORCHA_ASSERT(find("beta").overrides && find("beta").overrides->version == "1.0.0");
        ORCHA_ASSERT(find("gamma").uploaded && !find("gamma").overrides);
        ORCHA_ASSERT(src.is_uploaded(find("beta").meta) && !src.is_uploaded(find("alpha").meta));

        ORCHA_ASSERT(!src.overlapping());
        ORCHA_ASSERT((Core::PluginSources{env.root, env.plugins}.overlapping()));
        ORCHA_ASSERT((Core::PluginSources{env.plugins / "x", env.plugins}.overlapping()));
        ORCHA_ASSERT(!(Core::PluginSources{env.bundled, {}}.overlapping()));
        ORCHA_ASSERT((Core::PluginSources{env.bundled, env.root / "missing"}.scan(*env.discovery).size() == 2));
        std::cout << "[PASS] test_plugin_sources\n";
    }

    inline void test_plugin_upload_overrides_bundled() {
        using namespace upload_tests;
        Env env;
        // A bundled plugin, loaded the way the server starts.
        const auto bundled_dir = env.bundled / "SlowTest";
        fs::create_directories(bundled_dir);
        fs::copy_file(ORCHA_TEST_SLOW_PLUGIN_PATH, bundled_dir / lib_name("slowtest"));
        write_file(bundled_dir / "manifest.json", manifest("slowtest", "1.0.0"));
        const auto bundled_lib = read_file(bundled_dir / lib_name("slowtest"));
        const Core::PluginSources sources{env.bundled, env.plugins};
        ORCHA_ASSERT(env.manager->load_plugins(sources).loaded_count == 1);
        // Overriding a bundled plugin needs a key that allows it.
        env.installer = env.make([&] { auto o = env.options(); o.public_keys[0].override_bundled = true; return o; }());
        ORCHA_ASSERT(env.manager->get_plugin_metadata("slowtest")->version == "1.0.0");

        const auto bundled_untouched = [&] {
            return fs::exists(bundled_dir / "manifest.json") && read_file(bundled_dir / lib_name("slowtest")) == bundled_lib &&
                   std::distance(fs::directory_iterator(bundled_dir), fs::directory_iterator{}) == 2;
        };

        // A failing override reloads the bundled version; nothing is left in uploads.
        auto bad = env.installer->stage(env.upload(ORCHA_TEST_LEGACY_PLUGIN_PATH, lib_name("slowtest_legacy"),
                                                   manifest("slowtest", "2.0.0")));
        ORCHA_ASSERT(bad.is_ok() && bad.value().replaces_bundled && bad.value().change == "upgrade");
        auto failed = env.installer->install(bad.value().id, "tester");
        ORCHA_ASSERT(failed.is_err() && failed.error().kind == K::LoadFailed && failed.error().rolled_back);
        ORCHA_ASSERT(env.manager->get_plugin_metadata("slowtest")->version == "1.0.0");
        ORCHA_ASSERT(bundled_untouched() && env.install_dirs("slowtest").empty());
        ORCHA_ASSERT(env.installer->discard(bad.value().id, "t").is_ok());

        // A good one overrides it: loaded from the upload directory, bundled files untouched.
        auto up = env.install(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), manifest("slowtest", "2.0.0")));
        ORCHA_ASSERT(up.overrides_bundled && up.previous_version == "1.0.0");
        const auto now = env.manager->get_plugin_metadata("slowtest");
        ORCHA_ASSERT(now->version == "2.0.0" && sources.is_uploaded(*now));
        ORCHA_ASSERT(bundled_untouched());

        // The admin API says so.
        Agent::Routes::PluginAdminRoute admin(env.manager, env.discovery, env.registry, env.bundled.string(),
                                              std::chrono::milliseconds(5000), env.logger, env.denylist,
                                              nullptr, env.plugins);
        Agent::HttpRequest list;
        list.method = "GET";
        list.path = "/api/plugins";
        const auto listed = Json::parse(admin.handle(list).body);
        const auto it = std::ranges::find_if(listed["plugins"], [](const Json& p) { return p["name"] == "slowtest"; });
        ORCHA_ASSERT(it != listed["plugins"].end());
        ORCHA_ASSERT((*it)["source"] == "uploaded" && (*it)["overrides"]["version"] == "1.0.0" && (*it)["version"] == "2.0.0");

        // After a restart the upload still wins.
        for (const auto& p : env.manager->get_all_plugins()) (void)env.manager->unload_plugin(p.name);
        auto registry = std::make_shared<Core::CommandRegistry>(env.logger);
        Core::PluginManager restarted(registry, env.discovery, env.logger);
        ORCHA_ASSERT(restarted.load_plugins(sources).loaded_count == 1);
        ORCHA_ASSERT(restarted.get_plugin_metadata("slowtest")->version == "2.0.0");
        ORCHA_ASSERT(logged(*env.logger, "overrides the bundled 1.0.0") >= 1);
        for (const auto& p : restarted.get_all_plugins()) (void)restarted.unload_plugin(p.name);
        std::cout << "[PASS] test_plugin_upload_overrides_bundled\n";
    }

    inline void test_plugin_upload_key_scopes() {
        using namespace upload_tests;
        Env env;
        const auto scoped = [&](std::vector<std::string> plugins, bool override_bundled) {
            auto o = env.options();
            o.public_keys = {{env.key.public_base64(), std::move(plugins), override_bundled}};
            return env.make(o);
        };
        // Name scope: only acme-* names.
        auto acme = scoped({"acme-*"}, false);
        auto refused = acme->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), manifest("slowtest", "1.0.0")));
        ORCHA_ASSERT(refused.is_err() && refused.error().kind == K::NotAllowed);
        ORCHA_ASSERT(refused.error().message == "Key " + env.key.id() + " may not sign plugin \"slowtest\" (it may sign: acme-*)");
        auto ok = acme->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), manifest("acme-slow", "1.0.0")));
        ORCHA_ASSERT(ok.is_ok() && ok.value().allowed_by == "acme-*");
        ORCHA_ASSERT(acme->status()["keys"][0]["plugins"] == Json::array({"acme-*"}));
        ORCHA_ASSERT(acme->status()["keys"][0]["override_bundled"] == false);

        // Bundled plugins need override_bundled, also when an upload already overrides them.
        write_file(env.bundled / "Bundled" / "manifest.json", manifest("bundled-x", "1.0.0"));
        fs::copy_file(ORCHA_TEST_SLOW_PLUGIN_PATH, env.bundled / "Bundled" / lib_name("bundled-x"));
        auto plain = scoped({}, false);
        auto no = plain->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), manifest("bundled-x", "2.0.0")));
        ORCHA_ASSERT(no.is_err() && no.error().kind == K::NotAllowed);
        ORCHA_ASSERT(no.error().message.find("override_bundled: true") != std::string::npos);
        auto mayoverride = scoped({"bundled-*"}, true);
        auto yes = mayoverride->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), manifest("bundled-x", "2.0.0")));
        ORCHA_ASSERT(yes.is_ok() && yes.value().replaces_bundled);
        ORCHA_ASSERT(yes.value().allowed_by == "bundled-*, may override bundled plugins");

        // Checked again at install: tightening the config blocks staged uploads.
        auto installed = plain->install(yes.value().id, "tester");
        ORCHA_ASSERT(installed.is_err() && installed.error().kind == K::NotAllowed);
        ORCHA_ASSERT(env.install_dirs("bundled-x").empty() && !env.manager->get_plugin_metadata("bundled-x"));
        ORCHA_ASSERT(logged(*env.logger, "Plugin install refused") == 1);
        ORCHA_ASSERT(logged(*env.logger, "Plugin upload refused: Key") >= 1);

        // New names and uploaded ones need no flag (any-name key).
        ORCHA_ASSERT(plain->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("x"), manifest("fresh", "1.0.0"))).is_ok());

        // Over HTTP a scope refusal is 403.
        ORCHA_ASSERT(Agent::Routes::PluginUploadRoute::status_for(K::NotAllowed) == Agent::status::Forbidden);
        std::cout << "[PASS] test_plugin_upload_key_scopes\n";
    }

    inline void test_plugin_downgrade_needs_confirmation() {
        using namespace upload_tests;
        Env env;
        const auto stage = [&](const std::string& version) {
            auto r = env.installer->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), manifest("slowtest", version)));
            if (r.is_err()) throw std::runtime_error(r.error().message);
            return r.value();
        };
        env.install(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), manifest("slowtest", "1.0.0")));
        const auto later = stage("1.5.0");                       // an upgrade when staged
        env.install(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), manifest("slowtest", "2.0.0")));

        // Older than what is installed now: refused until confirmed, nothing changes.
        auto refused = env.installer->install(later.id, "tester");
        ORCHA_ASSERT(refused.is_err() && refused.error().kind == K::Conflict);
        ORCHA_ASSERT(refused.error().requires_confirmation == "allow_downgrade");
        ORCHA_ASSERT(refused.error().message == "1.5.0 is older than the installed 2.0.0; confirm the downgrade to install it");
        ORCHA_ASSERT(env.manager->get_plugin_metadata("slowtest")->version == "2.0.0");
        ORCHA_ASSERT(env.installer->list().size() == 1);
        ORCHA_ASSERT(logged(*env.logger, "downgrade was not confirmed") == 1);

        // Confirmed: installed, and audited as a confirmed downgrade.
        auto confirmed = env.installer->install(later.id, "tester", /*allow_downgrade=*/true);
        ORCHA_ASSERT(confirmed.is_ok() && confirmed.value().previous_version == "2.0.0");
        ORCHA_ASSERT(env.manager->get_plugin_metadata("slowtest")->version == "1.5.0");
        ORCHA_ASSERT(logged(*env.logger, "(confirmed downgrade)") == 1);

        // Upgrades, reinstalls and unversioned uploads need no confirmation; the flag changes nothing for them.
        ORCHA_ASSERT(env.installer->install(stage("1.6.0").id, "tester", true).is_ok());
        ORCHA_ASSERT(env.installer->install(stage("1.6.0").id, "tester").is_ok());
        auto unversioned = env.installer->stage(env.upload(ORCHA_TEST_SLOW_PLUGIN_PATH, lib_name("slowtest"), std::nullopt));
        ORCHA_ASSERT(unversioned.is_ok() && unversioned.value().change == "replace");

        // Over HTTP: the 409 says what to confirm; the body must be well-formed.
        Agent::Routes::PluginUploadRoute route(env.installer);
        const auto post = [&](const std::string& path, const std::string& body) {
            Agent::HttpRequest r;
            r.method = "POST";
            r.path = path;
            r.body = body;
            return route.handle(r);
        };
        const auto older = "/api/plugins/_uploads/" + stage("1.0.0").id + "/install";
        auto conflict = post(older, "");
        ORCHA_ASSERT_EQ(conflict.status, Agent::status::Conflict);
        ORCHA_ASSERT(Json::parse(conflict.body)["requires"] == "allow_downgrade");
        ORCHA_ASSERT_EQ(post(older, R"({"allow_downgrade":"yes"})").status, Agent::status::BadRequest);
        ORCHA_ASSERT_EQ(post(older, "[true]").status, Agent::status::BadRequest);
        ORCHA_ASSERT_EQ(post(older, R"({"allow_downgrade":false})").status, Agent::status::Conflict);
        auto ok = post(older, R"({"allow_downgrade":true})");
        ORCHA_ASSERT_EQ(ok.status, Agent::status::OK);
        ORCHA_ASSERT(Json::parse(ok.body)["previous_version"] == "1.6.0");
        std::cout << "[PASS] test_plugin_downgrade_needs_confirmation\n";
    }

    // ========== HTTP ==========

    inline void test_plugin_upload_route() {
        using namespace upload_tests;
        Env env;
        Agent::Routes::PluginUploadRoute route(env.installer);
        const auto req = [](std::string method, std::string path, std::string body = "") {
            Agent::HttpRequest r;
            r.method = std::move(method);
            r.path = std::move(path);
            r.body = std::move(body);
            r.headers.emplace_back("Authorization", "Basic YWRtaW46c2VjcmV0");   // admin:secret
            r.headers.emplace_back("Content-Type", "application/json");
            return r;
        };
        ORCHA_ASSERT(route.can_handle("POST", "/api/plugins/_uploads"));
        ORCHA_ASSERT(route.can_handle("POST", "/api/plugins/_uploads/abc/install"));
        ORCHA_ASSERT(!route.can_handle("GET", "/api/plugins"));
        ORCHA_ASSERT(!route.can_handle("GET", "/api/plugins/_uploadsx"));

        auto status = Json::parse(route.handle(req("GET", "/api/plugins/_uploads")).body);
        ORCHA_ASSERT(status["enabled"] == true && status["uploads"].empty() && status["keys"].size() == 1);
        ORCHA_ASSERT(status["platform"]["extension"] == Core::IPluginDiscovery::get_library_extension());

        // The signature arrives as a .sig file (base64 text), base64-encoded again.
        const auto library = read_file(ORCHA_TEST_SLOW_PLUGIN_PATH);
        const auto man = manifest("routed", "1.2.3");
        const Json body = {{"library", {{"filename", lib_name("routed")}, {"data", pkg::base64_encode(library)}}},
                           {"manifest", pkg::base64_encode(man)},
                           {"signature", pkg::base64_encode(pkg::base64_encode(env.key.sign(lib_name("routed"), library, man)) + "\n")}};
        auto created = route.handle(req("POST", "/api/plugins/_uploads", body.dump()));
        ORCHA_ASSERT_EQ(created.status, Agent::status::Created);
        const auto staged = Json::parse(created.body);
        ORCHA_ASSERT(staged["name"] == "routed" && staged["uploaded_by"] == "admin" && staged["change"] == "new");

        auto installed = route.handle(req("POST", "/api/plugins/_uploads/" + staged["id"].get<std::string>() + "/install"));
        ORCHA_ASSERT_EQ(installed.status, Agent::status::OK);
        ORCHA_ASSERT(Json::parse(installed.body)["version"] == "1.2.3");
        ORCHA_ASSERT(env.manager->get_plugin_metadata("routed").has_value());

        // Error mapping.
        Json forged = body;
        forged["signature"] = pkg::base64_encode(std::string(64, 'x'));
        ORCHA_ASSERT_EQ(route.handle(req("POST", "/api/plugins/_uploads", forged.dump())).status,
                        Agent::status::UnprocessableEntity);
        ORCHA_ASSERT_EQ(route.handle(req("POST", "/api/plugins/_uploads", R"({"library":{}})")).status,
                        Agent::status::BadRequest);
        ORCHA_ASSERT_EQ(route.handle(req("POST", "/api/plugins/_uploads", R"({"library":{"filename":"a.so","data":"!!"},"signature":""})")).status,
                        Agent::status::BadRequest);
        ORCHA_ASSERT_EQ(route.handle(req("DELETE", "/api/plugins/_uploads/0123456789abcdef0123456789abcdef")).status,
                        Agent::status::NotFound);
        ORCHA_ASSERT_EQ(route.handle(req("PUT", "/api/plugins/_uploads")).status, Agent::status::MethodNotAllowed);

        auto off_options = env.options();
        off_options.enabled = false;
        Agent::Routes::PluginUploadRoute off(env.make(off_options));
        auto refused = off.handle(req("POST", "/api/plugins/_uploads", body.dump()));
        ORCHA_ASSERT_EQ(refused.status, Agent::status::Forbidden);
        ORCHA_ASSERT(Json::parse(refused.body)["error"].get<std::string>().find("plugins.upload.enabled") != std::string::npos);
        std::cout << "[PASS] test_plugin_upload_route\n";
    }

    inline void test_plugin_upload_config() {
        const auto load = [](const std::string& yaml) {
            Config::YamlConfiguration config;
            ORCHA_ASSERT(config.load_from_string(yaml));
            return config;
        };
        const auto has = [](const std::vector<std::string>& messages, const std::string& needle) {
            return std::ranges::any_of(messages, [&](const auto& m) { return m.find(needle) != std::string::npos; });
        };
        // Defaults: off, nothing to warn about.
        auto defaults = Config::PluginUploadConfig::from_config(load("plugins:\n  directory: commands\n"));
        ORCHA_ASSERT(!defaults.enabled && defaults.public_keys.empty() && defaults.max_bytes == 32ll * 1024 * 1024);
        ORCHA_ASSERT(defaults.directory == "./uploaded-plugins");
        auto nested = Config::ConfigValidator{}.validate(load("plugins:\n  directory: commands\n  upload:\n    directory: commands/up\n"));
        ORCHA_ASSERT(!nested.valid && has(nested.get_error_messages(), "plugins.upload.directory"));
        ORCHA_ASSERT(Config::ConfigValidator{}.validate(load("plugins:\n  directory: commands\n")).issues.empty());

        // Keys as a YAML list (plain or scoped) or one comma/space separated string (the env form).
        const auto names = [](const Config::PluginUploadConfig& c) {
            std::vector<std::string> out;
            for (const auto& k : c.public_keys) out.push_back(k.key);
            return out;
        };
        auto listed = Config::PluginUploadConfig::from_config(load(
            "plugins:\n  upload:\n    public_keys:\n      - a\n"
            "      - key: b\n        plugins: [\"acme-*\", billing]\n        override_bundled: true\n"));
        ORCHA_ASSERT(listed.errors.empty() && names(listed) == std::vector<std::string>({"a", "b"}));
        ORCHA_ASSERT(listed.public_keys[0].plugins.empty() && !listed.public_keys[0].override_bundled);
        ORCHA_ASSERT(listed.public_keys[1].plugins == std::vector<std::string>({"acme-*", "billing"}));
        ORCHA_ASSERT(listed.public_keys[1].override_bundled);
        auto joined = Config::PluginUploadConfig::from_config(load("plugins:\n  upload:\n    public_keys: \"a, b c\"\n"));
        ORCHA_ASSERT(names(joined) == std::vector<std::string>({"a", "b", "c"}));
        // Malformed entries are errors (uploads stay off), not guesses.
        auto broken = Config::PluginUploadConfig::from_config(load(
            "plugins:\n  upload:\n    public_keys:\n      - {plugins: [x]}\n      - {key: k, plugins: []}\n"
            "      - {key: k, override_bundled: yes}\n      - {key: k, scope: x}\n"));
        ORCHA_ASSERT(broken.errors.size() == 4);
        ORCHA_ASSERT(has(broken.errors, "public_keys[0] must be a key") && has(broken.errors, "public_keys[1].plugins"));
        ORCHA_ASSERT(has(broken.errors, "override_bundled must be true or false") && has(broken.errors, "unknown setting \"scope\""));
        auto invalid = Config::ConfigValidator{}.validate(load("plugins:\n  upload:\n    enabled: true\n    public_keys:\n      - {plugins: [x]}\n"));
        ORCHA_ASSERT(!invalid.valid && has(invalid.get_error_messages(), "public_keys[0]"));

        auto on = Config::ConfigValidator{}.validate(load("plugins:\n  upload:\n    enabled: true\nadmin:\n  auth_required: false\n"));
        ORCHA_ASSERT(on.valid);
        ORCHA_ASSERT(has(on.get_warning_messages(), "Plugin uploads are on"));
        ORCHA_ASSERT(has(on.get_warning_messages(), "admin.auth_required"));
        ORCHA_ASSERT(has(on.get_warning_messages(), "No signing keys"));
        auto bad = Config::ConfigValidator{}.validate(load("plugins:\n  upload:\n    max_bytes: 0\n"));
        ORCHA_ASSERT(!bad.valid && has(bad.get_error_messages(), "plugins.upload.max_bytes"));
        auto big = Config::ConfigValidator{}.validate(load("plugins:\n  upload:\n    max_bytes: 60000000\n"));
        ORCHA_ASSERT(big.valid && has(big.get_warning_messages(), "64 MB"));
        std::cout << "[PASS] test_plugin_upload_config\n";
    }

    inline void run_plugin_upload_tests() {
        std::cout << "\n-- Plugin uploads --\n";
        test_plugin_package_checks();
        test_plugin_upload_config();
#if defined(ORCHA_TEST_SLOW_PLUGIN_PATH) && defined(ORCHA_TEST_LEGACY_PLUGIN_PATH)
        test_plugin_uploads_fail_closed();
        test_plugin_upload_rejects_bad_uploads();
        test_plugin_upload_install_upgrade_and_rollback();
        test_plugin_upload_refuses_unsafe_installs();
        test_plugin_install_recovers_after_crash();
        test_plugin_sources();
        test_plugin_upload_overrides_bundled();
        test_plugin_upload_key_scopes();
        test_plugin_downgrade_needs_confirmation();
        test_plugin_upload_route();
#endif
    }

} // namespace Orcha::Tests
