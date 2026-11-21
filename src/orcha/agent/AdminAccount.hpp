//
// AdminAccount.hpp - Who may use the admin surface, and first-run setup.
//
// One object answers "is this admin login valid?" for both auth middlewares
// and the setup route. Credentials come from, in order:
//   1. admin.password (configuration / ORCHA_ADMIN_PASSWORD), as before;
//   2. the account first-run setup stored in admin.credentials_file
//      (username + salted PBKDF2-SHA256 hash, file mode 0600);
//   3. neither: first-run setup. The first person to submit the setup form
//      (POST /api/setup) creates the account. There is deliberately no setup
//      code (product decision): until it is claimed, anyone who can reach the
//      port can claim it, which is logged loudly on an exposed bind.
// On a non-loopback bind, auth_required=false and the placeholder password
// are ignored (AdminConfig::for_bind). An unreadable or corrupt account file
// disables the admin surface instead of reopening setup, so damaging the file
// can't be used to claim admin again. Delete it to set up again.
//
// OpenSSL stays in AdminAccount.cpp.
//

#pragma once

#include "../config/AdminConfig.hpp"
#include "../utils/ILogger.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Orcha::Agent {

    class AdminAccount {
    public:
        enum class Mode {
            Disabled,    ///< admin.enabled=false, or nothing usable (see notes())
            Open,        ///< auth_required=false on a loopback bind: no login
            Configured,  ///< admin.password
            Stored,      ///< the account created by first-run setup
            Setup        ///< no account yet: POST /api/setup creates it
        };

        struct SetupError {
            enum class Kind { Invalid, AlreadySet, Busy, Failed } kind;
            std::string message;
        };

        static constexpr std::size_t kMinPasswordLength = 12;
        static constexpr std::size_t kMaxPasswordLength = 1024;
        static constexpr std::size_t kMaxUsernameLength = 64;

        /**
         * @param config  Admin settings as configured.
         * @param exposed True on a non-loopback bind.
         * @param iterations PBKDF2 iterations for new accounts (tests lower it).
         */
        AdminAccount(Config::AdminConfig config, bool exposed,
                     std::shared_ptr<Utils::ILogger> logger = nullptr,
                     std::uint32_t iterations = kDefaultIterations);

        [[nodiscard]] Mode mode() const;
        [[nodiscard]] bool serves() const { return mode() != Mode::Disabled; }
        [[nodiscard]] bool requires_login() const { return mode() != Mode::Open; }
        [[nodiscard]] bool exposed() const { return exposed_; }
        /// The effective settings (after AdminConfig::for_bind).
        [[nodiscard]] const Config::AdminConfig& config() const { return config_; }
        /// Why settings were adjusted or the surface is disabled, for the log.
        /// Written only by the constructor (later problems go to the logger).
        [[nodiscard]] const std::vector<std::string>& notes() const { return notes_; }

        enum class Login {
            Ok,
            Denied,
            Busy   ///< another password check is hashing: retry shortly (429)
        };

        /**
         * @brief Check a login (Configured or Stored).
         *
         * A stored password is checked with PBKDF2, which is deliberately
         * slow, on the caller's HTTP worker. To keep a flood of wrong logins
         * from occupying every worker, at most one hash runs at a time: a
         * check of a *different* login returns Busy at once, while a check of
         * the login being hashed waits for that result (concurrent requests
         * of one client). Logins that already passed are recognised without
         * hashing (a per-process HMAC cache), as are recently failed identical
         * attempts (Denied).
         */
        [[nodiscard]] Login check(std::string_view user, std::string_view password) const;

        /// True if @p user / @p password is the admin login (waits instead of Busy).
        [[nodiscard]] bool verify(std::string_view user, std::string_view password) const;

        /**
         * @brief Create the admin account (first-run setup).
         *
         * Only in Mode::Setup. The file is created atomically and only if it
         * doesn't exist, so exactly one of several concurrent requests (or
         * processes sharing the file) succeeds; the others get AlreadySet.
         */
        [[nodiscard]] std::optional<SetupError> set_up(const std::string& user, const std::string& password);

        static constexpr std::uint32_t kDefaultIterations = 600'000;   // OWASP 2023, PBKDF2-HMAC-SHA256

    private:
        using Digest = std::array<unsigned char, 32>;

        struct Stored {
            std::string username;
            std::vector<unsigned char> salt;
            std::uint32_t iterations = 0;
            Digest hash{};
        };

        void load_stored(std::vector<std::string>* notes);
        [[nodiscard]] Login check(std::string_view user, std::string_view password, bool wait) const;

        Config::AdminConfig config_;
        bool exposed_;
        std::shared_ptr<Utils::ILogger> logger_;
        std::uint32_t iterations_;
        std::vector<std::string> notes_;

        mutable std::mutex mutex_;
        Mode mode_ = Mode::Disabled;
        Stored stored_;
        // Logins that already passed PBKDF2, keyed by a per-process HMAC, so
        // the dashboard's requests don't each pay ~0.3 s of hashing.
        Digest cache_key_{};
        mutable std::vector<Digest> verified_;
        mutable std::vector<Digest> rejected_;
        mutable std::optional<Digest> in_flight_;   ///< the login being hashed (one at a time)
        mutable std::condition_variable hashed_;    ///< in_flight_ finished
        std::mutex setup_;                          ///< one set_up() at a time
    };

    [[nodiscard]] const char* to_string(AdminAccount::Mode mode);

} // namespace Orcha::Agent
