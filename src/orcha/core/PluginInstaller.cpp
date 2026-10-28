//
// PluginInstaller.cpp - see PluginInstaller.hpp.
//

#include "PluginInstaller.hpp"
#include "JsonCompat.hpp"
#include "PluginPackage.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <system_error>

namespace Orcha::Core {

    namespace fs = std::filesystem;
    namespace pkg = PluginPackage;
    using UploadResult = Result<StagedPlugin, PluginUploadError>;
    using InstallResult = Result<InstalledPlugin, PluginUploadError>;
    using Kind = PluginUploadErrorKind;

    namespace {

        constexpr const char* kUploadFile = "upload.json";
        constexpr const char* kJournalFile = "install.json";
        constexpr const char* kSignatureFile = "signature.bin";
        constexpr const char* kManifestFile = "manifest.json";
        constexpr const char* kLockFile = ".lock";
        constexpr std::size_t kMaxManifestBytes = 1024 * 1024;
        // Plugin changes wait this long for each other, then answer 409, so a
        // slow plugin initialiser cannot park every HTTP worker.
        constexpr auto kLockWait = std::chrono::seconds(5);
        constexpr const char* kBusy = "Another plugin change is in progress; try again shortly";

        PluginUploadError err(Kind kind, std::string message) { return {kind, std::move(message)}; }

        std::string read_file(const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error("cannot read " + path.string());
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

        void fsync_path(const fs::path& path) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) throw std::system_error(errno, std::generic_category(), "open " + path.string());
            const int rc = ::fsync(fd);
            ::close(fd);
            if (rc != 0) throw std::system_error(errno, std::generic_category(), "fsync " + path.string());
        }

        /// Write and fsync (file, then its directory): survives a power loss.
        void write_durable(const fs::path& path, const std::string& bytes) {
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                out.flush();
                if (!out) throw std::runtime_error("cannot write " + path.string());
            }
            fsync_path(path);
            fsync_path(path.parent_path());
        }

        fs::path normal(const fs::path& p) {
            std::error_code ec;
            auto abs = fs::absolute(p, ec);
            if (ec) abs = p;
            auto canon = fs::weakly_canonical(abs, ec);
            return (ec ? abs : canon).lexically_normal();
        }

        /// @p child is strictly below @p parent.
        bool strictly_within(const fs::path& child, const fs::path& parent) {
            const auto c = normal(child), p = normal(parent);
            auto [pi, ci] = std::mismatch(p.begin(), p.end(), c.begin(), c.end());
            const bool within = pi == p.end() || (std::next(pi) == p.end() && pi->empty());
            return within && c != p;
        }

        bool valid_id(const std::string& id) {
            return id.size() == 32 && std::ranges::all_of(id, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        }

        std::string new_id() {
            std::random_device rd;
            std::uniform_int_distribution<int> nibble(0, 15);
            std::string id(32, '0');
            for (auto& c : id) c = "0123456789abcdef"[nibble(rd)];
            return id;
        }

        std::string now_iso() {
            const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm{};
            gmtime_r(&t, &tm);
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            return out.str();
        }

        /// Exclusive flock() on a file: serialises install/recovery across processes.
        class InstallLock {
        public:
            explicit InstallLock(const fs::path& path)
                : fd_(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
                held_ = fd_ >= 0 && ::flock(fd_, LOCK_EX | LOCK_NB) == 0;
            }
            ~InstallLock() { if (fd_ >= 0) ::close(fd_); }   // closing releases the lock
            InstallLock(const InstallLock&) = delete;
            InstallLock& operator=(const InstallLock&) = delete;
            [[nodiscard]] bool held() const noexcept { return held_; }
        private:
            int fd_;
            bool held_ = false;
        };

        bool numeric(const std::string& s) {
            return !s.empty() && s.size() <= 18 &&
                   std::ranges::all_of(s, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        }

        std::string provenance_file(const std::string& name) { return ".orcha-provenance-" + name + ".json"; }

    } // namespace

    // ---- StagedPlugin ------------------------------------------------------

    Json StagedPlugin::to_json() const {
        Json j = Json::object();
        j["id"] = id;
        j["name"] = name;
        j["version"] = version;
        j["description"] = description;
        j["library_filename"] = library_filename;
        j["size"] = size;
        j["sha256"] = sha256;
        j["format"] = format;
        j["arch"] = arch;
        j["signer"] = signer;
        j["allowed_by"] = allowed_by;
        j["has_manifest"] = has_manifest;
        j["parameters"] = parameters;
        j["uploaded_at"] = uploaded_at;
        j["uploaded_by"] = uploaded_by;
        j["install_crashed_at"] = install_crashed_at ? Json(*install_crashed_at) : Json(nullptr);
        j["replaces_version"] = replaces_version ? Json(*replaces_version) : Json(nullptr);
        j["replaces_bundled"] = replaces_bundled;
        j["change"] = change;
        return j;
    }

    StagedPlugin StagedPlugin::from_json(const Json& j) {
        StagedPlugin s;
        s.id = j.at("id").get<std::string>();
        s.name = j.at("name").get<std::string>();
        s.version = j.value("version", "");
        s.description = j.value("description", "");
        s.library_filename = j.at("library_filename").get<std::string>();
        s.size = j.value("size", std::uint64_t{0});
        s.sha256 = j.at("sha256").get<std::string>();
        s.format = j.value("format", "");
        s.arch = j.value("arch", "");
        s.signer = j.value("signer", "");
        s.allowed_by = j.value("allowed_by", "");
        s.has_manifest = j.value("has_manifest", false);
        s.parameters = j.value("parameters", std::size_t{0});
        s.uploaded_at = j.value("uploaded_at", "");
        s.uploaded_by = j.value("uploaded_by", "");
        if (j.contains("install_crashed_at") && j.at("install_crashed_at").is_string()) {
            s.install_crashed_at = j.at("install_crashed_at").get<std::string>();
        }
        return s;
    }

    // ---- PluginInstaller ---------------------------------------------------

    PluginInstaller::PluginInstaller(PluginUploadOptions options,
                                     std::shared_ptr<PluginManager> manager,
                                     std::shared_ptr<IPluginDiscovery> discovery,
                                     std::shared_ptr<IPluginDenylist> denylist,
                                     std::shared_ptr<Utils::ILogger> logger,
                                     std::shared_ptr<std::timed_mutex> lifecycle_mutex)
        : options_(std::move(options))
        , manager_(std::move(manager))
        , discovery_(std::move(discovery))
        , denylist_(std::move(denylist))
        , logger_(std::move(logger))
        , lifecycle_(lifecycle_mutex ? std::move(lifecycle_mutex) : std::make_shared<std::timed_mutex>()) {
        if (!options_.config_errors.empty()) disabled_reason_ = options_.config_errors.front() + ".";
        for (std::size_t i = 0; i < options_.public_keys.size() && disabled_reason_.empty(); ++i) {
            const auto& trust = options_.public_keys[i];
            auto key = pkg::base64_decode(trust.key);
            if (!key || key->size() != 32) {
                disabled_reason_ = "plugins.upload.public_keys[" + std::to_string(i) +
                                   "] is not a base64 Ed25519 public key (32 bytes).";
                break;
            }
            const auto bad_glob = std::ranges::find_if(trust.plugins, [](const std::string& g) {
                return g.empty() || g.size() > 64 || !std::ranges::all_of(g, [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '*' || c == '?';
                });
            });
            if (bad_glob != trust.plugins.end()) {
                disabled_reason_ = "plugins.upload.public_keys[" + std::to_string(i) + "].plugins: \"" + *bad_glob +
                                   "\" is not a name pattern (letters, digits, . _ - and the wildcards * ?).";
                break;
            }
            keys_.push_back({*key, pkg::key_id(*key), trust});
        }
        if (!disabled_reason_.empty()) {
            keys_.clear();   // a bad key setting already explains it
            // a bad key already explains it
        } else if (!options_.enabled) {
            disabled_reason_ = "Plugin uploads are off. Set plugins.upload.enabled: true to allow them.";
        } else if (!options_.admin_auth_required) {
            disabled_reason_ = "Plugin uploads need admin authentication (admin.auth_required: true).";
        } else if (pkg::host_platform().format.empty()) {
            disabled_reason_ = "This platform does not support plugin uploads.";
        } else if (keys_.empty()) {
            disabled_reason_ = "No signing keys are configured. Add one to plugins.upload.public_keys "
                               "(scripts/orcha-plugin-sign.sh keygen prints it).";
        } else if (options_.upload_directory.empty()) {
            disabled_reason_ = "plugins.upload.directory must be set.";
        } else if (sources().overlapping()) {
            disabled_reason_ = "plugins.upload.directory and plugins.directory must not be inside each other.";
        } else {
            std::error_code ec;
            fs::create_directories(staging_dir(), ec);
            if (ec) disabled_reason_ = "Cannot create " + staging_dir().string() + ": " + ec.message();
        }
        if (!disabled_reason_.empty() && options_.enabled && logger_) {
            logger_->warn("Plugin uploads are disabled: " + disabled_reason_);
        }
    }

    Json PluginInstaller::status() const {
        const auto host = pkg::host_platform();
        Json j = Json::object();
        j["enabled"] = enabled();
        if (!enabled()) j["reason"] = disabled_reason_;
        j["max_bytes"] = options_.max_bytes;
        j["platform"] = {{"format", host.format}, {"arch", host.arch},
                         {"extension", IPluginDiscovery::get_library_extension()}};
        Json keys = Json::array();
        for (const auto& k : keys_) {
            keys.push_back({{"id", k.id},
                            {"plugins", k.trust.plugins.empty() ? Json(nullptr) : Json(k.trust.plugins)},
                            {"override_bundled", k.trust.override_bundled}});
        }
        j["keys"] = keys;
        return j;
    }

    fs::path PluginInstaller::staging_dir() const { return options_.upload_directory / kStagingDirName; }
    fs::path PluginInstaller::upload_dir(const std::string& id) const { return staging_dir() / id; }

    std::optional<PluginInstaller::Existing> PluginInstaller::find_existing(const std::string& name) const {
        const auto src = sources();
        if (auto meta = manager_->get_plugin_metadata(name)) return Existing{*meta, true, src.is_uploaded(*meta)};
        for (auto& entry : src.scan(*discovery_)) {
            if (entry.meta.name == name) return Existing{std::move(entry.meta), false, entry.uploaded};
        }
        return std::nullopt;
    }

    std::string PluginInstaller::version_change(const std::string& current, const std::string& next) {
        if (current.empty() || next.empty()) return "replace";
        // Compare dot-separated parts: numerically when both are numbers.
        std::istringstream a(current), b(next);
        std::string pa, pb;
        while (true) {
            const bool ha = static_cast<bool>(std::getline(a, pa, '.'));
            const bool hb = static_cast<bool>(std::getline(b, pb, '.'));
            if (!ha && !hb) return "reinstall";
            if (!ha) pa = "0";
            if (!hb) pb = "0";
            int cmp = 0;
            if (numeric(pa) && numeric(pb)) {
                const auto x = std::stoll(pa), y = std::stoll(pb);
                cmp = x < y ? -1 : x > y ? 1 : 0;
            } else {
                cmp = pa < pb ? -1 : pa > pb ? 1 : 0;
            }
            if (cmp != 0) return cmp < 0 ? "upgrade" : "downgrade";
        }
    }

    const PluginInstaller::Key* PluginInstaller::verify_signature(
        const std::string& filename, const std::string& library_sha256,
        const std::optional<std::string>& manifest, const std::string& signature) const {
        const auto message = pkg::signing_message(filename, library_sha256,
                                                  pkg::sha256_hex(manifest.value_or("")));
        for (const auto& key : keys_) {
            if (pkg::verify_ed25519(key.raw, signature, message)) return &key;
        }
        return nullptr;
    }

    bool PluginInstaller::is_bundled(const std::string& name) const {
        // The bundled directory alone: an upload overriding it does not hide it.
        const PluginSources bundled_only{options_.bundled_directory, {}};
        return std::ranges::any_of(bundled_only.scan(*discovery_),
                                   [&](const auto& entry) { return entry.meta.name == name; });
    }

    std::optional<PluginUploadError> PluginInstaller::check_allowed(const Key& key, const std::string& name,
                                                                    std::string* allowed_by) const {
        const auto& scope = key.trust.plugins;
        auto match = std::ranges::find_if(scope, [&](const std::string& g) { return pkg::name_matches(g, name); });
        if (!scope.empty() && match == scope.end()) {
            std::string list;
            for (const auto& g : scope) list += (list.empty() ? "" : ", ") + g;
            return err(Kind::NotAllowed, "Key " + key.id + " may not sign plugin \"" + name +
                                         "\" (it may sign: " + list + ")");
        }
        if (!key.trust.override_bundled && is_bundled(name)) {
            return err(Kind::NotAllowed, "\"" + name + "\" is a bundled plugin; overriding it needs a key with "
                                         "override_bundled: true (key " + key.id + " does not allow it)");
        }
        if (allowed_by) {
            *allowed_by = scope.empty() ? "any name" : *match;
            if (key.trust.override_bundled) *allowed_by += ", may override bundled plugins";
        }
        return std::nullopt;
    }

    std::optional<StagedPlugin> PluginInstaller::read_staged(const std::string& id) const {
        if (!valid_id(id)) return std::nullopt;
        try {
            auto staged = StagedPlugin::from_json(parse_json_bounded(read_file(upload_dir(id) / kUploadFile)));
            if (staged.id != id) return std::nullopt;
            return staged;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    std::optional<Json> PluginInstaller::provenance(const PluginMetadata& meta) {
        if (meta.library_path.empty()) return std::nullopt;
        try {
            const auto path = meta.library_path.parent_path() / provenance_file(meta.name);
            if (!fs::exists(path)) return std::nullopt;
            auto j = parse_json_bounded(read_file(path));
            if (j.is_object() && j.value("library", "") == meta.library_path.filename().string()) return j;
            return std::nullopt;   // a different library has replaced it since
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    Result<StagedPlugin, PluginUploadError> PluginInstaller::stage(const PluginUploadRequest& request) {
        // Everything up to the signature check is pure: done without the lock.
        const auto sha = request.library.empty() ? std::string() : pkg::sha256_hex(request.library);
        // Every refusal is audited, like every success.
        const auto refuse = [&](PluginUploadError e) {
            if (logger_) {
                logger_->warn("Plugin upload refused: " + e.message + " (file " + request.filename +
                              ", sha256 " + sha + ", by " + request.user + ")");
            }
            return UploadResult::Err(std::move(e));
        };
        if (!enabled()) return refuse(err(Kind::Disabled, disabled_reason_));

        const auto ext = IPluginDiscovery::get_library_extension();
        const auto& filename = request.filename;
        const bool odd_chars = std::ranges::any_of(filename, [](char c) {
            return static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == '/' || c == '\\';
        });
        if (filename.empty() || filename.size() > 128 || odd_chars ||
            filename.front() == '.' || fs::path(filename).extension() != ext) {
            return refuse(err(Kind::Invalid, "Upload a " + ext + " library for this server (a plain file name ending in " + ext + ")"));
        }
        if (request.library.empty()) return refuse(err(Kind::Invalid, "The library is empty"));
        if (request.library.size() > options_.max_bytes) {
            return refuse(err(Kind::TooLarge, "The library is larger than " +
                              std::to_string(options_.max_bytes) + " bytes"));
        }
        const auto binary = pkg::inspect_library(request.library);
        if (!binary.loadable_here) return refuse(err(Kind::Invalid, binary.problem));

        StagedPlugin staged;
        staged.name = pkg::plugin_name_from_filename(filename);
        if (request.manifest) {
            if (request.manifest->size() > kMaxManifestBytes) {
                return refuse(err(Kind::Invalid, "manifest.json is larger than 1 MB"));
            }
            try {
                const auto manifest = parse_json_bounded(*request.manifest);
                if (!manifest.is_object() || !manifest.contains("name") || !manifest.at("name").is_string()) {
                    return refuse(err(Kind::Invalid, "manifest.json needs a \"name\" string"));
                }
                if (manifest.contains("entry_point") && manifest.at("entry_point") != filename) {
                    return refuse(err(Kind::Invalid, "manifest.json entry_point must be \"" + filename + "\""));
                }
                const auto meta = PluginMetadata::from_json(manifest, {});
                staged.name = meta.name;
                staged.version = manifest.contains("version") ? meta.version : "";
                staged.description = meta.description;
                staged.parameters = meta.parameters.size();
                staged.has_manifest = true;
            } catch (const std::exception& ex) {
                return refuse(err(Kind::Invalid, std::string("manifest.json: ") + ex.what()));
            }
        }
        if (!pkg::is_valid_plugin_name(staged.name)) {
            return refuse(err(Kind::Invalid, "Invalid plugin name \"" + staged.name +
                              "\": use letters, digits, '.', '_' or '-' (up to 64), not ending in a library extension"));
        }
        if (request.signature.size() != 64) {
            return refuse(err(Kind::BadSignature, "The signature must be a 64-byte Ed25519 signature"));
        }
        const auto* signer = verify_signature(filename, sha, request.manifest, request.signature);
        if (!signer) {
            return refuse(err(Kind::BadSignature,
                "The signature does not match these files (the file name is signed too), "
                "or was made with a key this server does not trust"));
        }
        if (auto denied = check_allowed(*signer, staged.name, &staged.allowed_by)) return refuse(std::move(*denied));
        std::unique_lock lock(*lifecycle_, kLockWait);
        if (!lock.owns_lock()) return refuse(err(Kind::Conflict, kBusy));
        if (list_unlocked().size() >= options_.max_staged) {
            return refuse(err(Kind::Conflict, "Too many staged uploads (" +
                              std::to_string(options_.max_staged) + "); install or discard some first"));
        }

        staged.id = new_id();
        staged.library_filename = filename;
        staged.size = request.library.size();
        staged.sha256 = sha;
        staged.format = binary.format;
        staged.arch = binary.arch;
        staged.signer = signer->id;
        staged.uploaded_at = now_iso();
        staged.uploaded_by = request.user;

        // Written under a dot-name, then renamed: list() never sees half an upload.
        const auto tmp = staging_dir() / (".tmp-" + staged.id);
        try {
            fs::create_directories(tmp);
            write_durable(tmp / filename, request.library);
            if (request.manifest) write_durable(tmp / kManifestFile, *request.manifest);
            write_durable(tmp / kSignatureFile, request.signature);
            write_durable(tmp / kUploadFile, staged.to_json().dump(2));
            fs::rename(tmp, upload_dir(staged.id));
        } catch (const std::exception& ex) {
            std::error_code ignored;
            fs::remove_all(tmp, ignored);
            return refuse(err(Kind::Internal, std::string("Could not store the upload: ") + ex.what()));
        }
        if (auto existing = find_existing(staged.name)) {
            staged.replaces_version = existing->meta.version;
            staged.replaces_bundled = !existing->uploaded;
            staged.change = version_change(existing->meta.version, staged.version);
        }
        if (logger_) {
            logger_->warn("Plugin upload staged: " + staged.name + " " + staged.version + " (" +
                          staged.library_filename + ", sha256 " + staged.sha256 + ", key " +
                          staged.signer + ") by " + staged.uploaded_by);
        }
        return UploadResult::Ok(std::move(staged));
    }

    std::vector<StagedPlugin> PluginInstaller::list() const {
        // Uploads appear and disappear by rename and scan() does not throw,
        // so listing needs no lock (and never waits behind an install).
        return list_unlocked();
    }

    std::vector<StagedPlugin> PluginInstaller::list_unlocked() const {
        std::vector<StagedPlugin> out;
        std::error_code ec;
        if (!fs::is_directory(staging_dir(), ec)) return out;
        for (const auto& entry : fs::directory_iterator(staging_dir(), ec)) {
            const auto id = entry.path().filename().string();
            if (!entry.is_directory() || id.starts_with('.')) continue;
            if (auto staged = read_staged(id)) out.push_back(std::move(*staged));
        }
        if (!out.empty()) {
            // One scan for all of them; what is loaded wins over what is on disk.
            const auto src = sources();
            std::map<std::string, std::pair<std::string, bool>> installed;   // name -> {version, uploaded}
            for (const auto& entry : src.scan(*discovery_)) installed[entry.meta.name] = {entry.meta.version, entry.uploaded};
            for (const auto& meta : manager_->get_all_plugins()) installed[meta.name] = {meta.version, src.is_uploaded(meta)};
            for (auto& s : out) {
                if (auto it = installed.find(s.name); it != installed.end()) {
                    s.replaces_version = it->second.first;
                    s.replaces_bundled = !it->second.second;
                    s.change = version_change(it->second.first, s.version);
                }
            }
        }
        std::ranges::sort(out, std::greater{}, &StagedPlugin::uploaded_at);
        return out;
    }

    Result<void, PluginUploadError> PluginInstaller::discard(const std::string& id, const std::string& user) {
        std::unique_lock lock(*lifecycle_, kLockWait);
        if (!lock.owns_lock()) return Result<void, PluginUploadError>::Err(err(Kind::Conflict, kBusy));
        auto staged = read_staged(id);
        if (!staged) return Result<void, PluginUploadError>::Err(err(Kind::NotFound, "No staged upload " + id));
        if (fs::exists(upload_dir(id) / kJournalFile)) {
            return Result<void, PluginUploadError>::Err(err(Kind::Conflict,
                "This upload's install did not finish; restart Orcha to roll it back first"));
        }
        std::error_code ec;
        fs::remove_all(upload_dir(id), ec);
        if (ec) {
            return Result<void, PluginUploadError>::Err(err(Kind::Internal, "Could not remove the upload: " + ec.message()));
        }
        if (logger_) {
            logger_->warn("Plugin upload discarded: " + staged->name + " " + staged->version +
                          " (sha256 " + staged->sha256 + ") by " + user);
        }
        return Result<void, PluginUploadError>::Ok();
    }

    bool PluginInstaller::has_unfinished_install() const {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(staging_dir(), ec)) {
            if (fs::exists(entry.path() / kJournalFile)) return true;
        }
        return false;
    }

    Result<InstalledPlugin, PluginUploadError> PluginInstaller::install(const std::string& id,
                                                                        const std::string& user,
                                                                        bool allow_downgrade) {
        if (!enabled()) return InstallResult::Err(err(Kind::Disabled, disabled_reason_));
        std::unique_lock lock(*lifecycle_, kLockWait);
        if (!lock.owns_lock()) return InstallResult::Err(err(Kind::Conflict, kBusy));
        InstallLock process_lock(staging_dir() / kLockFile);
        if (!process_lock.held()) {
            return InstallResult::Err(err(Kind::Conflict, "Another Orcha process is installing a plugin; try again shortly"));
        }
        if (has_unfinished_install()) {
            return InstallResult::Err(err(Kind::Conflict,
                "A previous plugin install did not finish. Restart Orcha to roll it back, then try again."));
        }
        auto staged = read_staged(id);
        if (!staged) return InstallResult::Err(err(Kind::NotFound, "No staged upload " + id));

        // Check the files at rest again: they are only trusted as signed.
        const auto dir = upload_dir(id);
        std::string library, signature;
        std::optional<std::string> manifest;
        try {
            library = read_file(dir / staged->library_filename);
            signature = read_file(dir / kSignatureFile);
            if (staged->has_manifest) manifest = read_file(dir / kManifestFile);
        } catch (const std::exception& ex) {
            return InstallResult::Err(err(Kind::Internal, ex.what()));
        }
        const auto* signer = pkg::sha256_hex(library) == staged->sha256
            ? verify_signature(staged->library_filename, staged->sha256, manifest, signature) : nullptr;
        if (!signer || !pkg::inspect_library(library).loadable_here) {
            return InstallResult::Err(err(Kind::Invalid,
                "The staged files no longer match their signature, or its key is no longer trusted; "
                "discard this upload and upload it again"));
        }
        // Checked again: the key's scope may have changed since upload.
        if (auto denied = check_allowed(*signer, staged->name)) {
            if (logger_) logger_->warn("Plugin install refused: " + denied->message + " (by " + user + ")");
            return InstallResult::Err(std::move(*denied));
        }

        // Plan. Every install goes to a new directory, <uploads>/<name>-<id>:
        // a fresh path means dlopen maps the new library even while a running
        // workflow (or a library that cannot be unloaded) keeps the old image,
        // and a rollback reload gets the old code back. An uploaded current
        // version has its library, manifest and provenance moved aside
        // (nothing else in its directory is touched); a bundled one is only
        // overridden, its files left alone.
        const auto& plugin_dir = options_.upload_directory;
        const auto existing = find_existing(staged->name);
        // Against what is installed now, not at upload: a staged upload can
        // have become a downgrade since.
        const bool downgrade = existing && version_change(existing->meta.version, staged->version) == "downgrade";
        if (downgrade && !allow_downgrade) {
            if (logger_) {
                logger_->warn("Plugin install refused: " + staged->name + " " + staged->version +
                              " is older than the installed " + existing->meta.version +
                              " and the downgrade was not confirmed (by " + user + ")");
            }
            auto e = err(Kind::Conflict, staged->version + " is older than the installed " + existing->meta.version +
                                         "; confirm the downgrade to install it");
            e.requires_confirmation = "allow_downgrade";
            return InstallResult::Err(std::move(e));
        }
        const bool was_loaded = existing && existing->loaded;
        const bool overrides_bundled = existing && !existing->uploaded;
        const auto old_lib = existing ? existing->meta.library_path : fs::path{};
        const auto old_dir = old_lib.parent_path();
        const bool old_dedicated = existing && existing->uploaded && normal(old_dir) != normal(plugin_dir);
        const auto backup = dir / "previous";
        std::vector<std::pair<fs::path, fs::path>> restore;   // {backup, original}
        if (existing && existing->uploaded) {
            restore.emplace_back(backup / old_lib.filename(), old_lib);
            if (old_dedicated) {
                // Its manifest also hides any other library in that directory;
                // moving it would turn those into plugins, so refuse instead.
                std::error_code ec;
                const auto ext = IPluginDiscovery::get_library_extension();
                for (const auto& f : fs::directory_iterator(old_dir, ec)) {
                    if (f.path().extension() == ext && normal(f.path()) != normal(old_lib)) {
                        return InstallResult::Err(err(Kind::Conflict, old_dir.string() +
                            " holds other libraries besides " + staged->name + "'s; move them out first"));
                    }
                }
                const auto& old_manifest = existing->meta.manifest_path;
                if (!old_manifest.empty() && fs::exists(old_manifest)) {
                    restore.emplace_back(backup / kManifestFile, old_manifest);
                }
                if (const auto prov = old_dir / provenance_file(staged->name); fs::exists(prov)) {
                    restore.emplace_back(backup / prov.filename(), prov);
                }
            }
        }
        const auto target_dir = plugin_dir / (staged->name + "-" + id.substr(0, 8));
        const auto new_lib = target_dir / staged->library_filename;
        const auto new_manifest = target_dir / kManifestFile;
        if (fs::exists(target_dir)) {
            return InstallResult::Err(err(Kind::Conflict, target_dir.string() + " already exists"));
        }
        if (!strictly_within(target_dir, plugin_dir)) {
            return InstallResult::Err(err(Kind::Conflict, "Refusing to install outside plugins.upload.directory: " +
                                          target_dir.string()));
        }

        const bool was_denied = denylist_ && denylist_->contains(staged->name);
        const auto journal = dir / kJournalFile;
        const auto set_denied = [&](bool denied) {
            if (!denylist_) return;
            if (denied) denylist_->add(staged->name); else denylist_->remove(staged->name);
        };

        // Running workflows keep the commands they already resolved (and the
        // old image); new steps briefly see "unknown command" until the new
        // version is registered.
        if (was_loaded && !manager_->unload_plugin(staged->name)) {
            return InstallResult::Err(err(Kind::Internal, "Could not unload the current version"));
        }

        // Undo exactly what was done.
        bool journaled = false;
        const auto rollback = [&]() -> bool {
            std::error_code ec;
            fs::remove_all(target_dir, ec);
            for (const auto& [from, to] : restore) {
                if (fs::exists(from)) fs::rename(from, to);
            }
            set_denied(was_denied);
            if (journaled) fs::remove(journal, ec);
            fs::remove_all(backup, ec);
            if (!was_loaded) return true;
            return manager_->try_load_plugin(old_lib).is_ok();
        };

        std::string failure;
        try {
            Json restore_json = Json::array();
            for (const auto& [from, to] : restore) restore_json.push_back({from.string(), to.string()});
            const Json j = {{"name", staged->name}, {"was_denied", was_denied},
                            {"created_dir", target_dir.string()}, {"restore", restore_json}};
            write_durable(journal, j.dump(2));
            journaled = true;
            // While it loads, the plugin is denylisted: if loading kills the
            // process and the journal is lost, it is still not loaded at start.
            set_denied(true);

            if (!restore.empty()) fs::create_directories(backup);
            for (const auto& [to_backup, original] : restore) fs::rename(original, to_backup);
            fs::create_directory(target_dir);
            // .part names are invisible to discovery until complete.
            write_durable(fs::path(new_lib) += ".part", library);
            if (manifest) write_durable(fs::path(new_manifest) += ".part", *manifest);
            fs::rename(fs::path(new_lib) += ".part", new_lib);
            if (manifest) fs::rename(fs::path(new_manifest) += ".part", new_manifest);
            fsync_path(target_dir);

            if (auto loaded = manager_->try_load_plugin(new_lib); loaded.is_err()) failure = loaded.error();
        } catch (const std::exception& ex) {
            failure = std::string("installing the files failed: ") + ex.what();
        }

        if (!failure.empty()) {
            bool restored = false;
            try {
                restored = rollback();
            } catch (const std::exception& ex) {
                if (logger_) logger_->error("Plugin install rollback failed for " + staged->name + ": " + ex.what());
            }
            if (logger_) {
                logger_->warn("Plugin install failed: " + staged->name + " " + staged->version + " by " + user +
                              ": " + failure + (restored ? " (previous state restored)" : ""));
            }
            while (!failure.empty() && failure.back() == '.') failure.pop_back();
            PluginUploadError e = err(Kind::LoadFailed, std::string(existing ? "The new version" : "The plugin") +
                " did not load: " + failure +
                (existing ? (restored ? ". The previous version was restored."
                                      : ". The previous version could not be reloaded; check the server log.")
                          : "."));
            e.rolled_back = restored;
            return InstallResult::Err(std::move(e));
        }

        // Committed: enable it, record where it came from, then drop the journal.
        set_denied(false);
        try {
            const Json origin = {{"name", staged->name}, {"version", staged->version}, {"sha256", staged->sha256},
                                 {"signer", staged->signer}, {"uploaded_by", staged->uploaded_by},
                                 {"uploaded_at", staged->uploaded_at}, {"installed_by", user},
                                 {"installed_at", now_iso()}, {"library", staged->library_filename}};
            write_durable(target_dir / provenance_file(staged->name), origin.dump(2));
        } catch (const std::exception& ex) {
            if (logger_) logger_->warn("Could not record the provenance of " + staged->name + ": " + ex.what());
        }
        std::error_code ec;
        if (old_dedicated) fs::remove(old_dir, ec);   // only if now empty
        fs::remove(journal, ec);
        if (!ec) {
            try { fsync_path(dir); } catch (const std::exception&) {}
        } else if (logger_) {
            logger_->error("Could not remove the install journal " + journal.string() + " (" + ec.message() +
                           "): delete it, or the next start rolls this install back");
        }
        fs::remove_all(dir, ec);   // the upload and the backup of the previous version
        if (logger_) {
            logger_->warn("Plugin installed: " + staged->name + " " + staged->version + " (sha256 " +
                          staged->sha256 + ", key " + staged->signer + ")" +
                          (existing ? (overrides_bundled ? " overriding the bundled " : " replacing ") +
                                      existing->meta.version : std::string()) +
                          (downgrade ? " (confirmed downgrade)" : "") + " by " + user);
        }
        InstalledPlugin out{staged->name, staged->version, std::nullopt, overrides_bundled};
        if (existing) out.previous_version = existing->meta.version;
        return InstallResult::Ok(std::move(out));
    }

    void PluginInstaller::recover_interrupted() {
        std::lock_guard lock(*lifecycle_);   // start-up: nothing else runs yet
        std::error_code ec;
        if (!fs::is_directory(staging_dir(), ec)) return;
        InstallLock process_lock(staging_dir() / kLockFile);
        if (!process_lock.held()) {
            if (logger_) logger_->warn("Skipping plugin install recovery: another Orcha process holds the install lock");
            return;
        }
        for (const auto& entry : fs::directory_iterator(staging_dir(), ec)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with(".tmp-")) {
                fs::remove_all(entry.path(), ec);   // an upload that never finished
                continue;
            }
            const auto journal = entry.path() / kJournalFile;
            if (!fs::exists(journal)) continue;
            try {
                recover_one(entry.path(), parse_json_bounded(read_file(journal)));
            } catch (const std::exception& ex) {
                // Leave the journal: installs stay blocked until someone looks.
                // The plugin itself is on the denylist, so it is not loaded.
                if (logger_) {
                    logger_->error("Could not roll back the interrupted plugin install " + journal.string() +
                                   ": " + ex.what() + ". The plugin stays disabled; fix or delete the journal.");
                }
            }
        }
    }

    void PluginInstaller::recover_one(const fs::path& upload, const Json& j) {
        const auto& plugin_dir = options_.upload_directory;
        const auto inside = [&](const std::string& p) {
            if (!strictly_within(p, plugin_dir)) throw std::runtime_error("path outside plugins.upload.directory: " + p);
            return fs::path(p);
        };
        // Validate everything before touching anything.
        const auto name = j.at("name").get<std::string>();
        std::vector<std::pair<fs::path, fs::path>> restore;
        for (const auto& r : j.at("restore")) {
            restore.emplace_back(inside(r.at(0).get<std::string>()), inside(r.at(1).get<std::string>()));
        }
        const auto created_dir = inside(j.at("created_dir").get<std::string>());
        if (strictly_within(staging_dir(), created_dir) || normal(created_dir) == normal(staging_dir())) {
            throw std::runtime_error("created_dir contains the staging directory");
        }
        for (const auto& [from, to] : restore) {
            if (!strictly_within(from, upload)) throw std::runtime_error("backup outside the upload: " + from.string());
        }

        std::error_code ec;
        fs::remove_all(created_dir);
        for (const auto& [from, to] : restore) {
            if (fs::exists(from)) fs::rename(from, to);
        }
        if (denylist_ && !j.value("was_denied", false)) denylist_->remove(name);
        fs::remove_all(upload / "previous", ec);

        // Mark the upload, so the next attempt is not made blindly.
        if (auto staged = read_staged(upload.filename().string())) {
            staged->install_crashed_at = now_iso();
            write_durable(upload / kUploadFile, staged->to_json().dump(2));
        }
        fs::remove(upload / kJournalFile);
        fsync_path(upload);
        if (logger_) {
            logger_->warn("Rolled back an interrupted install of plugin " + name +
                          " (the server stopped while installing it)");
        }
    }

} // namespace Orcha::Core
