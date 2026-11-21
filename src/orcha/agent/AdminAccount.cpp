//
// AdminAccount.cpp - see AdminAccount.hpp.
//

#include "AdminAccount.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace Orcha::Agent {

    namespace {

        constexpr int kFileVersion = 1;
        constexpr std::size_t kSaltBytes = 16;
        constexpr std::size_t kMaxCachedLogins = 8;
        constexpr std::uintmax_t kMaxFileBytes = 64 * 1024;
        constexpr std::int64_t kMaxIterations = 10'000'000;

        std::string to_hex(const unsigned char* data, std::size_t n) {
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string out(n * 2, '0');
            for (std::size_t i = 0; i < n; ++i) {
                out[2 * i] = kDigits[data[i] >> 4];
                out[2 * i + 1] = kDigits[data[i] & 0xF];
            }
            return out;
        }

        std::optional<std::vector<unsigned char>> from_hex(std::string_view s) {
            if (s.size() % 2 != 0) return std::nullopt;
            const auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            std::vector<unsigned char> out(s.size() / 2);
            for (std::size_t i = 0; i < out.size(); ++i) {
                const int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
                if (hi < 0 || lo < 0) return std::nullopt;
                out[i] = static_cast<unsigned char>(hi << 4 | lo);
            }
            return out;
        }

        bool pbkdf2(std::string_view password, const std::vector<unsigned char>& salt,
                    std::uint32_t iterations, std::array<unsigned char, 32>& out) {
            return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                                     salt.data(), static_cast<int>(salt.size()),
                                     static_cast<int>(iterations), EVP_sha256(),
                                     static_cast<int>(out.size()), out.data()) == 1;
        }

        /// HMAC-SHA256(key, user || 0 || password): identifies a login without storing it.
        std::array<unsigned char, 32> login_digest(const std::array<unsigned char, 32>& key,
                                                   std::string_view user, std::string_view password) {
            std::string message;
            message.reserve(user.size() + 1 + password.size());
            message.append(user).push_back('\0');
            message.append(password);
            std::array<unsigned char, 32> out{};
            unsigned int len = 0;
            HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                 reinterpret_cast<const unsigned char*>(message.data()), message.size(), out.data(), &len);
            OPENSSL_cleanse(message.data(), message.size());
            return out;
        }

        bool same(std::string_view a, std::string_view b) {
            return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
        }

        std::optional<std::string> invalid_username(const std::string& user) {
            if (user.empty() || user.size() > AdminAccount::kMaxUsernameLength) {
                return "The username must be 1 to " + std::to_string(AdminAccount::kMaxUsernameLength) +
                       " characters";
            }
            // Basic auth splits user:password at the first ':'.
            const bool ok = std::ranges::all_of(user, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '_' || c == '-' || c == '@';
            });
            if (!ok) return "The username may contain only letters, digits and . _ - @";
            return std::nullopt;
        }

        std::optional<std::string> invalid_password(const std::string& password) {
            if (password.size() < AdminAccount::kMinPasswordLength) {
                return "The password must be at least " + std::to_string(AdminAccount::kMinPasswordLength) +
                       " characters";
            }
            if (password.size() > AdminAccount::kMaxPasswordLength) return "The password is too long";
            if (password.find('\0') != std::string::npos) return "The password may not contain NUL";
            if (password == Config::AdminConfig::kPlaceholderPassword) return "Choose a real password";
            return std::nullopt;
        }

        void fsync_fd(int fd, const std::string& what) {
            if (::fsync(fd) != 0) throw std::system_error(errno, std::generic_category(), "fsync " + what);
        }

        void fsync_dir(const fs::path& dir) {
            const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) throw std::system_error(errno, std::generic_category(), "open " + dir.string());
            const int rc = ::fsync(fd);
            ::close(fd);
            if (rc != 0) throw std::system_error(errno, std::generic_category(), "fsync " + dir.string());
        }

        /// Create @p path with @p bytes (mode 0600) only if it doesn't exist:
        /// write a private temp file, fsync it, then link() it into place,
        /// which fails with EEXIST if another writer got there first.
        /// Returns false on EEXIST; throws on other failures.
        bool create_exclusive(const fs::path& path, const std::string& bytes) {
            const auto dir = path.has_parent_path() ? path.parent_path() : fs::path(".");
            std::error_code ec;
            fs::create_directories(dir, ec);
            unsigned char rnd[8];
            if (RAND_bytes(rnd, sizeof rnd) != 1) throw std::runtime_error("no randomness for a temp name");
            const fs::path tmp = dir / (path.filename().string() + ".tmp-" + to_hex(rnd, sizeof rnd));
            const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0) throw std::system_error(errno, std::generic_category(), "create " + tmp.string());
            struct Unlink {
                const fs::path& p;
                ~Unlink() { std::error_code e; fs::remove(p, e); }
            } cleanup{tmp};
            try {
                std::size_t done = 0;
                while (done < bytes.size()) {
                    const auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        throw std::system_error(errno, std::generic_category(), "write " + tmp.string());
                    }
                    done += static_cast<std::size_t>(n);
                }
                fsync_fd(fd, tmp.string());
            } catch (...) {
                ::close(fd);
                throw;
            }
            ::close(fd);
            if (::link(tmp.c_str(), path.c_str()) != 0) {
                if (errno == EEXIST) return false;
                throw std::system_error(errno, std::generic_category(), "create " + path.string());
            }
            // The file is complete and fsynced; it exists now whatever happens.
            // A failed directory fsync only weakens durability across a power
            // loss, so it must not turn a created account into an error.
            try { fsync_dir(dir); } catch (...) {}
            return true;
        }

    } // namespace

    const char* to_string(AdminAccount::Mode mode) {
        switch (mode) {
            case AdminAccount::Mode::Disabled:   return "disabled";
            case AdminAccount::Mode::Open:       return "open";
            case AdminAccount::Mode::Configured: return "configured";
            case AdminAccount::Mode::Stored:     return "stored";
            case AdminAccount::Mode::Setup:      return "setup";
        }
        return "disabled";
    }

    AdminAccount::AdminAccount(Config::AdminConfig config, bool exposed,
                               std::shared_ptr<Utils::ILogger> logger, std::uint32_t iterations)
        : exposed_(exposed), logger_(std::move(logger)),
          iterations_(std::max<std::uint32_t>(1, iterations)) {
        // In the body: notes_ is declared after config_, so it can't be filled
        // from config_'s initializer.
        config_ = config.for_bind(exposed, &notes_);
        if (RAND_bytes(cache_key_.data(), static_cast<int>(cache_key_.size())) != 1) {
            throw std::runtime_error("OpenSSL has no randomness: cannot set up admin authentication");
        }
        if (!config_.enabled) {
            mode_ = Mode::Disabled;
        } else if (!config_.auth_required) {
            mode_ = Mode::Open;
        } else if (!config_.password.empty()) {
            mode_ = Mode::Configured;
        } else if (config_.credentials_file.empty()) {
            mode_ = Mode::Disabled;
            notes_.push_back("no admin.password and no admin.credentials_file: the admin surface is off");
        } else {
            load_stored(&notes_);
        }
    }

    void AdminAccount::load_stored(std::vector<std::string>* notes) {
        // notes_ is only written by the constructor (notes() reads it without
        // a lock); later calls report through the logger instead.
        const auto note = [&](const std::string& text) {
            if (notes) notes->push_back(text);
            else if (logger_) logger_->error("Admin dashboard: " + text);
        };
        const fs::path path = config_.credentials_file;
        std::error_code ec;
        // symlink_status: a link at the path is never followed (a dangling one
        // would otherwise look absent here while link() reports it exists).
        const auto status = fs::symlink_status(path, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            mode_ = Mode::Disabled;
            note("cannot read the admin account file " + path.string() + ": " + ec.message() +
                 "; the admin surface is off (fix its permissions, or delete it to set up again)");
            return;
        }
        if (status.type() == fs::file_type::not_found) {
            mode_ = Mode::Setup;
            return;
        }
        const auto fail = [&](const std::string& why) {
            mode_ = Mode::Disabled;
            note("the admin account file " + path.string() + " is unusable (" + why +
                 "); the admin surface is off. Delete the file to set up a new account.");
        };
        if (status.type() != fs::file_type::regular) return fail("not a regular file");
        // Whoever can write it decides the admin login: only the server's user.
        struct stat st{};
        if (::lstat(path.c_str(), &st) != 0) return fail(std::strerror(errno));
        if (st.st_uid != ::geteuid()) return fail("not owned by the user running Orcha");
        if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return fail("writable by group or others; chmod 600 it");
        const auto size = fs::file_size(path, ec);
        if (ec || size > kMaxFileBytes) return fail(ec ? ec.message() : "too large");
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail("cannot open it");
        try {
            const auto j = nlohmann::json::parse(in);
            if (j.value("version", 0) != kFileVersion) return fail("unknown version");
            if (j.value("kdf", std::string()) != "pbkdf2-sha256") return fail("unknown kdf");
            Stored s;
            s.username = j.at("username").get<std::string>();
            const auto iterations = j.at("iterations").get<std::int64_t>();
            // Bounded: every first login hashes with it while holding the one
            // hashing slot (a huge value would stall logins for minutes).
            if (iterations < 1 || iterations > kMaxIterations) return fail("bad iterations");
            s.iterations = static_cast<std::uint32_t>(iterations);
            auto salt = from_hex(j.at("salt").get<std::string>());
            auto hash = from_hex(j.at("hash").get<std::string>());
            if (!salt || salt->size() < 8 || !hash || hash->size() != s.hash.size()) return fail("bad salt or hash");
            if (invalid_username(s.username)) return fail("bad username");
            s.salt = std::move(*salt);
            std::ranges::copy(*hash, s.hash.begin());
            stored_ = std::move(s);
            mode_ = Mode::Stored;
        } catch (const std::exception& ex) {
            fail(ex.what());
        }
    }

    AdminAccount::Mode AdminAccount::mode() const {
        std::lock_guard lock(mutex_);
        return mode_;
    }

    namespace {
        bool known(const std::vector<std::array<unsigned char, 32>>& list, const std::array<unsigned char, 32>& digest) {
            return std::ranges::any_of(list, [&](const auto& d) {
                return CRYPTO_memcmp(d.data(), digest.data(), d.size()) == 0; });
        }
        void remember(std::vector<std::array<unsigned char, 32>>& list, const std::array<unsigned char, 32>& digest) {
            if (list.size() >= kMaxCachedLogins) list.erase(list.begin());
            list.push_back(digest);
        }
    } // namespace

    AdminAccount::Login AdminAccount::check(std::string_view user, std::string_view password) const {
        return check(user, password, /*wait=*/false);
    }

    AdminAccount::Login AdminAccount::check(std::string_view user, std::string_view password, bool wait) const {
        Stored stored;
        Digest digest{};
        {
            std::unique_lock lock(mutex_);
            if (mode_ == Mode::Configured) {
                const bool user_ok = same(user, config_.username);
                return (same(password, config_.password) & user_ok) ? Login::Ok : Login::Denied;
            }
            if (mode_ != Mode::Stored) return Login::Denied;
            digest = login_digest(cache_key_, user, password);
            for (;;) {
                if (known(verified_, digest)) return Login::Ok;
                if (known(rejected_, digest)) return Login::Denied;
                if (!in_flight_) break;
                // The same login is being hashed (a dashboard sends several
                // requests at once): wait for that answer. A different one is
                // Busy at once, so a flood of guesses holds one worker, not all.
                const bool same_login = CRYPTO_memcmp(in_flight_->data(), digest.data(), digest.size()) == 0;
                if (!same_login && !wait) return Login::Busy;
                hashed_.wait(lock);
            }
            if (mode_ != Mode::Stored) return Login::Denied;
            in_flight_ = digest;
            stored = stored_;
        }
        // PBKDF2 outside mutex_: mode() and cached logins never wait for it.
        Digest computed{};
        const bool hashed = pbkdf2(password, stored.salt, stored.iterations, computed);
        const bool user_ok = same(user, stored.username);
        const bool ok = hashed && ((CRYPTO_memcmp(computed.data(), stored.hash.data(), computed.size()) == 0) & user_ok);
        {
            std::lock_guard lock(mutex_);
            remember(ok ? verified_ : rejected_, digest);
            in_flight_.reset();
        }
        hashed_.notify_all();
        return ok ? Login::Ok : Login::Denied;
    }

    bool AdminAccount::verify(std::string_view user, std::string_view password) const {
        return check(user, password, /*wait=*/true) == Login::Ok;
    }

    std::optional<AdminAccount::SetupError> AdminAccount::set_up(const std::string& user,
                                                                 const std::string& password) {
        using Kind = SetupError::Kind;
        if (mode() != Mode::Setup) return SetupError{Kind::AlreadySet, "The admin account is already set up"};
        if (auto why = invalid_username(user)) return SetupError{Kind::Invalid, *why};
        if (auto why = invalid_password(password)) return SetupError{Kind::Invalid, *why};

        // One setup at a time; it hashes and fsyncs without holding mutex_, so
        // requests keep being answered meanwhile. create_exclusive() decides
        // the winner (also across processes sharing the file).
        std::unique_lock setting_up(setup_, std::try_to_lock);
        if (!setting_up.owns_lock()) return SetupError{Kind::Busy, "Setup is already in progress; try again shortly"};
        if (mode() != Mode::Setup) return SetupError{Kind::AlreadySet, "The admin account is already set up"};

        Stored s;
        s.username = user;
        s.iterations = iterations_;
        s.salt.resize(kSaltBytes);
        if (RAND_bytes(s.salt.data(), static_cast<int>(s.salt.size())) != 1 ||
            !pbkdf2(password, s.salt, s.iterations, s.hash)) {
            return SetupError{Kind::Failed, "Could not hash the password"};
        }
        nlohmann::json j = {
            {"version", kFileVersion},
            {"username", s.username},
            {"kdf", "pbkdf2-sha256"},
            {"iterations", s.iterations},
            {"salt", to_hex(s.salt.data(), s.salt.size())},
            {"hash", to_hex(s.hash.data(), s.hash.size())},
        };
        bool created = false;
        try {
            created = create_exclusive(config_.credentials_file, j.dump(2) + "\n");
        } catch (const std::exception& ex) {
            if (logger_) logger_->error(std::string("First-run setup could not save the admin account: ") + ex.what());
            return SetupError{Kind::Failed, "Could not save the admin account (see the server log)"};
        }
        std::lock_guard lock(mutex_);
        if (!created) {
            // Another process sharing the file claimed it first: use theirs.
            load_stored(nullptr);
            return SetupError{Kind::AlreadySet, "The admin account is already set up"};
        }
        stored_ = std::move(s);
        mode_ = Mode::Stored;
        verified_.clear();
        rejected_.clear();
        // The creator signs in right away (the dashboard then sends several
        // requests at once): don't make them wait for, or race, a re-hash.
        remember(verified_, login_digest(cache_key_, user, password));
        if (logger_) {
            logger_->warn("Admin account '" + user + "' created by first-run setup (stored in " +
                          config_.credentials_file + ")");
        }
        return std::nullopt;
    }

} // namespace Orcha::Agent
