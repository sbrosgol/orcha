//
// PluginInstaller.hpp - Upload, review and install external plugins.
//
// Installing a plugin runs its code in this process, so uploads are:
//  * off unless plugins.upload.enabled, admin auth is required and at least
//    one Ed25519 public key is configured (fail closed, see disabled_reason());
//  * signed: Ed25519 over the library file name and the library and manifest
//    digests (PluginPackage::signing_message). The plugin's name and version
//    come from the signed manifest, or from the signed file name, so the
//    signature decides which plugin an upload becomes;
//  * staged: stage() checks everything that can be checked without running
//    the library (size, ELF/Mach-O format and arch, manifest, name,
//    signature) and stores the files in <upload directory>/.orcha-uploads,
//    a hidden directory discovery skips. Nothing is loaded.
//  * installed on request: install() puts the files in a new directory,
//    <upload directory>/<name>-<install id>, and loads them. A fresh path
//    means dlopen maps the new library even while a running workflow (or a
//    library that cannot be unloaded) keeps the old image.
//    - Replacing an uploaded plugin moves only its library, manifest and
//      provenance aside; if the new one fails to load, they are moved back
//      and reloaded.
//    - Replacing a bundled plugin (plugins.directory) overrides it: its files
//      are never touched (that directory may be read-only, e.g. part of a
//      Docker image), and a failed install just reloads it. PluginSources
//      decides which copy loads at start.
//
// Crash safety. Staging, backups and the journal live in the upload
// directory, so every move is an atomic rename and they survive with the
// uploaded plugins (e.g. on a Docker volume). install() fsyncs a journal
// before touching the plugin directory and removes it as the commit point;
// the plugin is also on the denylist while it loads, so even without a
// journal a plugin that crashed the server is not loaded again at start.
// recover_interrupted() (server start, before plugins load) undoes
// uncommitted installs and marks the upload as having crashed the server.
// A file lock serialises install and recovery across processes; the
// lifecycle mutex serialises them (and PluginAdminRoute enable/disable/
// reload) within this one.
//

#pragma once

#include "IPluginDiscovery.hpp"
#include "Json.hpp"
#include "PluginDenylist.hpp"
#include "PluginManager.hpp"
#include "PluginSources.hpp"
#include "Result.hpp"
#include "../utils/ILogger.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Orcha::Core {

    struct PluginUploadOptions {
        bool enabled = false;
        std::filesystem::path upload_directory;    ///< plugins.upload.directory: installs + staging
        std::filesystem::path bundled_directory;   ///< plugins.directory: read, never written
        std::vector<std::string> public_keys;   ///< base64 raw 32-byte Ed25519 keys
        std::uint64_t max_bytes = 32ull * 1024 * 1024;
        std::size_t max_staged = 20;
        bool admin_auth_required = true;        ///< uploads need an authenticated admin
    };

    enum class PluginUploadErrorKind { Disabled, Invalid, TooLarge, BadSignature, NotFound,
                                       Conflict, LoadFailed, Internal };

    struct PluginUploadError {
        PluginUploadErrorKind kind;
        std::string message;
        bool rolled_back = false;   ///< LoadFailed: the previous state is back in place
    };

    /// The files of one upload, as sent.
    struct PluginUploadRequest {
        std::string filename;                 ///< library file name, e.g. "libfoo.so" (signed)
        std::string library;                  ///< library bytes
        std::optional<std::string> manifest;  ///< manifest.json bytes
        std::string signature;                ///< 64-byte Ed25519 signature
        std::string user;                     ///< who uploaded (audit)
    };

    /// A staged upload, waiting for install or discard.
    struct StagedPlugin {
        std::string id;
        std::string name;
        std::string version;         ///< from the manifest; empty without one
        std::string description;
        std::string library_filename;
        std::uint64_t size = 0;
        std::string sha256;
        std::string format;
        std::string arch;
        std::string signer;          ///< key id of the key that verified the signature
        bool has_manifest = false;
        std::size_t parameters = 0;  ///< parameters declared in the manifest
        std::string uploaded_at;     ///< UTC ISO-8601
        std::string uploaded_by;
        std::optional<std::string> install_crashed_at;  ///< the server stopped while installing it

        // Relative to what is installed now (set by list()/stage()).
        std::optional<std::string> replaces_version;
        bool replaces_bundled = false;   ///< installing it overrides a bundled plugin
        std::string change = "new";  ///< new | upgrade | downgrade | reinstall | replace

        [[nodiscard]] Json to_json() const;
        [[nodiscard]] static StagedPlugin from_json(const Json& j);
    };

    struct InstalledPlugin {
        std::string name;
        std::string version;
        std::optional<std::string> previous_version;
        bool overrides_bundled = false;
    };

    class PluginInstaller {
    public:
        static constexpr const char* kStagingDirName = ".orcha-uploads";

        PluginInstaller(PluginUploadOptions options,
                        std::shared_ptr<PluginManager> manager,
                        std::shared_ptr<IPluginDiscovery> discovery,
                        std::shared_ptr<IPluginDenylist> denylist,
                        std::shared_ptr<Utils::ILogger> logger,
                        std::shared_ptr<std::timed_mutex> lifecycle_mutex);

        /// Empty when uploads are on; otherwise why not (shown to admins).
        [[nodiscard]] const std::string& disabled_reason() const noexcept { return disabled_reason_; }
        [[nodiscard]] bool enabled() const noexcept { return disabled_reason_.empty(); }

        /// {enabled, reason?, max_bytes, platform{format, arch, extension}, keys[]}
        [[nodiscard]] Json status() const;

        [[nodiscard]] Result<StagedPlugin, PluginUploadError> stage(const PluginUploadRequest& request);
        [[nodiscard]] std::vector<StagedPlugin> list() const;
        [[nodiscard]] Result<void, PluginUploadError> discard(const std::string& id, const std::string& user);
        [[nodiscard]] Result<InstalledPlugin, PluginUploadError> install(const std::string& id,
                                                                         const std::string& user);

        /// Undo installs a previous process did not commit. Call at server
        /// start, before plugins are loaded; skipped if another process holds
        /// the install lock.
        void recover_interrupted();

        /// Serialises plugin lifecycle changes; share it with other callers
        /// that load or unload plugins (PluginAdminRoute).
        [[nodiscard]] std::shared_ptr<std::timed_mutex> lifecycle_mutex() const { return lifecycle_; }

        /// Where uploaded plugins are installed (plugins.upload.directory).
        [[nodiscard]] const std::filesystem::path& upload_directory() const { return options_.upload_directory; }

        /// Where an uploaded plugin came from (written next to it on install),
        /// or nullopt for plugins that were not uploaded.
        [[nodiscard]] static std::optional<Json> provenance(const PluginMetadata& meta);

        /// "upgrade", "downgrade", "reinstall" or "replace" (unversioned) for
        /// installing @p next over @p current.
        [[nodiscard]] static std::string version_change(const std::string& current, const std::string& next);

    private:
        struct Existing {
            PluginMetadata meta;
            bool loaded = false;
            bool uploaded = false;   ///< lives in the upload directory (else bundled)
        };

        [[nodiscard]] std::filesystem::path staging_dir() const;
        [[nodiscard]] std::filesystem::path upload_dir(const std::string& id) const;
        [[nodiscard]] std::optional<Existing> find_existing(const std::string& name) const;
        [[nodiscard]] PluginSources sources() const { return {options_.bundled_directory, options_.upload_directory}; }
        [[nodiscard]] std::optional<std::string> verify_signature(const std::string& filename,
                                                                  const std::string& library_sha256,
                                                                  const std::optional<std::string>& manifest,
                                                                  const std::string& signature) const;
        [[nodiscard]] std::optional<StagedPlugin> read_staged(const std::string& id) const;
        [[nodiscard]] std::vector<StagedPlugin> list_unlocked() const;
        [[nodiscard]] bool has_unfinished_install() const;
        void recover_one(const std::filesystem::path& upload, const Json& journal);

        PluginUploadOptions options_;
        std::shared_ptr<PluginManager> manager_;
        std::shared_ptr<IPluginDiscovery> discovery_;
        std::shared_ptr<IPluginDenylist> denylist_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::shared_ptr<std::timed_mutex> lifecycle_;
        std::vector<std::string> keys_;   ///< decoded raw public keys
        std::string disabled_reason_;
    };

} // namespace Orcha::Core
