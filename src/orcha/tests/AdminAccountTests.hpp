//
// AdminAccountTests.hpp - Admin login sources, exposed-bind rules and
// first-run setup (agent/AdminAccount.*, agent/routes/SetupRoute.hpp).
//

#pragma once

#include "../agent/AdminAccount.hpp"
#include "../agent/AuthMiddleware.hpp"
#include "../agent/routes/SetupRoute.hpp"
#include "../config/ConfigValidator.hpp"
#include "../config/YamlConfiguration.hpp"
#include "Assertions.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

namespace Orcha::Tests {

    namespace admin_tests {
        namespace fs = std::filesystem;
        using Mode = Agent::AdminAccount::Mode;
        constexpr std::uint32_t kFastIterations = 1000;   // the real default is 600k
        constexpr const char* kGoodPassword = "correct horse battery";

        /// A fresh directory, removed at scope exit.
        struct TempDir {
            fs::path path;
            TempDir() {
                std::random_device rd;
                path = fs::temp_directory_path() / ("orcha-admin-" + std::to_string(rd()) + std::to_string(rd()));
                fs::create_directories(path);
            }
            ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
            [[nodiscard]] std::string file() const { return (path / "orcha-admin.json").string(); }
        };

        inline Config::AdminConfig config(std::string credentials_file, std::string password = "",
                                          bool auth_required = true) {
            Config::AdminConfig c;
            c.credentials_file = std::move(credentials_file);
            c.password = std::move(password);
            c.auth_required = auth_required;
            return c;
        }

        inline std::shared_ptr<Agent::AdminAccount> account(const Config::AdminConfig& c, bool exposed) {
            return std::make_shared<Agent::AdminAccount>(c, exposed, nullptr, kFastIterations);
        }

        inline std::string basic(const std::string& user, const std::string& pass) {
            static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const std::string in = user + ":" + pass;
            std::string out;
            int val = 0, bits = -6;
            for (unsigned char c : in) {
                val = (val << 8) + c;
                bits += 8;
                while (bits >= 0) { out.push_back(kAlphabet[(val >> bits) & 0x3F]); bits -= 6; }
            }
            if (bits > -6) out.push_back(kAlphabet[((val << 8) >> (bits + 8)) & 0x3F]);
            while (out.size() % 4) out.push_back('=');
            return "Basic " + out;
        }

        inline Agent::HttpRequest request(std::string method, std::string path,
                                          std::optional<std::string> authorization = std::nullopt,
                                          std::string body = "") {
            Agent::HttpRequest r;
            r.method = std::move(method);
            r.path = std::move(path);
            if (authorization) r.headers.emplace_back("Authorization", *authorization);
            if (!body.empty()) r.headers.emplace_back("Content-Type", "application/json");
            r.body = std::move(body);
            return r;
        }
    } // namespace admin_tests

    // Where the login comes from, and the exposed-bind rules. Regression: on a
    // non-loopback bind, auth_required=false served /api/* (and so shell_exec
    // jobs) without any login, and the placeholder password was accepted.
    inline void test_admin_account_modes() {
        using namespace admin_tests;
        TempDir dir;
        // Loopback: configuration as written.
        ORCHA_ASSERT(account(config(dir.file(), "", false), false)->mode() == Mode::Open);
        ORCHA_ASSERT(account(config(dir.file(), "change-me"), false)->mode() == Mode::Configured);
        ORCHA_ASSERT(account(config(dir.file(), "s3cret-enough!"), false)->mode() == Mode::Configured);
        ORCHA_ASSERT(account(config(dir.file()), false)->mode() == Mode::Setup);
        // Exposed: auth off and the placeholder are ignored -> setup.
        auto open = account(config(dir.file(), "", false), true);
        ORCHA_ASSERT(open->mode() == Mode::Setup && open->config().auth_required);
        ORCHA_ASSERT(!open->notes().empty() && open->notes()[0].find("auth_required") != std::string::npos);
        auto placeholder = account(config(dir.file(), "change-me"), true);
        ORCHA_ASSERT(placeholder->mode() == Mode::Setup);
        ORCHA_ASSERT(!placeholder->verify("admin", "change-me"));
        ORCHA_ASSERT(account(config(dir.file(), "s3cret-enough!"), true)->mode() == Mode::Configured);
        // A real password always wins over a stored account.
        ORCHA_ASSERT(!account(config(dir.file()), false)->set_up("first", kGoodPassword));
        auto configured = account(config(dir.file(), "s3cret-enough!"), true);
        ORCHA_ASSERT(configured->mode() == Mode::Configured);
        ORCHA_ASSERT(configured->verify("admin", "s3cret-enough!") && !configured->verify("first", kGoodPassword));
        // Off, and nowhere to store an account.
        auto off = config(dir.file());
        off.enabled = false;
        ORCHA_ASSERT(account(off, false)->mode() == Mode::Disabled);
        ORCHA_ASSERT(account(config(""), true)->mode() == Mode::Disabled);
        // The validator agrees: "uploads ignored" only where auth can be off.
        const auto upload_warning = [](const char* host) {
            Config::YamlConfiguration c;
            c.load_from_string(std::string("server:\n  host: \"") + host + "\"\nadmin:\n  auth_required: false\n"
                               "plugins:\n  upload:\n    enabled: true\n");
            const auto r = Config::ConfigValidator{}.validate(c);
            return std::ranges::any_of(r.issues, [](const auto& i) { return i.message.find("Ignored: uploads") != std::string::npos; });
        };
        ORCHA_ASSERT(upload_warning("127.0.0.1") && !upload_warning("0.0.0.0"));
        // The plugin installer uses the same effective setting.
        ORCHA_ASSERT(!config("", "", false).for_bind(false).auth_required);
        ORCHA_ASSERT(config("", "", false).for_bind(true).auth_required);
        std::cout << "[PASS] test_admin_account_modes\n";
    }

    inline void test_admin_first_run_setup() {
        using namespace admin_tests;
        using Kind = Agent::AdminAccount::SetupError::Kind;
        TempDir dir;
        auto acct = account(config(dir.file()), true);
        ORCHA_ASSERT(acct->mode() == Mode::Setup && !acct->verify("admin", ""));

        // Invalid input leaves setup open.
        ORCHA_ASSERT(acct->set_up("", kGoodPassword)->kind == Kind::Invalid);
        ORCHA_ASSERT(acct->set_up("a:b", kGoodPassword)->kind == Kind::Invalid);   // Basic auth splits at ':'
        ORCHA_ASSERT(acct->set_up(std::string(65, 'a'), kGoodPassword)->kind == Kind::Invalid);
        ORCHA_ASSERT(acct->set_up("ops", "short")->kind == Kind::Invalid);
        ORCHA_ASSERT(acct->set_up("ops", std::string("twelve chars\0x", 14))->kind == Kind::Invalid);
        ORCHA_ASSERT(acct->mode() == Mode::Setup && !fs::exists(dir.file()));

        ORCHA_ASSERT(!acct->set_up("ops@example", kGoodPassword));
        ORCHA_ASSERT(acct->mode() == Mode::Stored);
        // The creator's login is known at once (no re-hash, no 429 for the
        // dashboard's first requests).
        ORCHA_ASSERT(acct->check("ops@example", kGoodPassword) == Agent::AdminAccount::Login::Ok);
        ORCHA_ASSERT(acct->verify("ops@example", kGoodPassword));
        ORCHA_ASSERT(acct->verify("ops@example", kGoodPassword));   // cached path
        ORCHA_ASSERT(!acct->verify("ops@example", "correct horse batterY"));
        ORCHA_ASSERT(!acct->verify("admin", kGoodPassword));
        ORCHA_ASSERT(acct->set_up("other", kGoodPassword)->kind == Kind::AlreadySet);

        // Stored privately, hashed, never the password itself; no temp files left.
        struct stat st{};
        ORCHA_ASSERT(::stat(dir.file().c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
        std::ifstream in(dir.file());
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        ORCHA_ASSERT(text.find("pbkdf2-sha256") != std::string::npos);
        ORCHA_ASSERT(text.find(kGoodPassword) == std::string::npos);
        ORCHA_ASSERT(std::distance(fs::directory_iterator(dir.path), fs::directory_iterator()) == 1);

        // A restart reads it back: login works, setup stays closed.
        auto restarted = account(config(dir.file()), true);
        ORCHA_ASSERT(restarted->mode() == Mode::Stored);
        ORCHA_ASSERT(restarted->verify("ops@example", kGoodPassword) && !restarted->verify("ops@example", "nope nope nope"));
        ORCHA_ASSERT(restarted->set_up("ops@example", "another password")->kind == Kind::AlreadySet);
        std::cout << "[PASS] test_admin_first_run_setup\n";
    }

    // A damaged account file must not reopen setup (that would let whoever can
    // corrupt it claim admin again): the surface stays off until it is fixed.
    inline void test_admin_corrupt_account_file_disables() {
        using namespace admin_tests;
        TempDir dir;
        for (const char* bytes : {"", "{not json", R"({"version":1})",
                                  R"({"version":1,"kdf":"pbkdf2-sha256","username":"a","iterations":1,"salt":"zz","hash":"00"})"}) {
            { std::ofstream(dir.file(), std::ios::trunc) << bytes; }
            auto acct = account(config(dir.file()), true);
            ORCHA_ASSERT(acct->mode() == Mode::Disabled);
            ORCHA_ASSERT(!acct->notes().empty() && acct->notes().back().find("Delete the file") != std::string::npos);
            ORCHA_ASSERT(acct->set_up("ops", kGoodPassword).has_value());
        }
        std::cout << "[PASS] test_admin_corrupt_account_file_disables\n";
    }

    // Only the server's own private regular file is trusted; anything else
    // disables admin instead of being followed, trusted, or reopening setup.
    // Regression: a dangling symlink kept setup open while every claim got 409.
    inline void test_admin_account_file_must_be_private() {
        using namespace admin_tests;
        TempDir dir;
        const auto disabled_because = [&](const char* why) {
            auto acct = account(config(dir.file()), true);
            return acct->mode() == Mode::Disabled && !acct->notes().empty() &&
                   acct->notes().back().find(why) != std::string::npos;
        };
        fs::create_symlink(dir.path / "nowhere.json", dir.file());          // dangling
        ORCHA_ASSERT(disabled_because("not a regular file"));
        fs::remove(dir.file());

        TempDir other;
        ORCHA_ASSERT(!account(config(other.file()), true)->set_up("ops", kGoodPassword));
        fs::create_symlink(other.file(), dir.file());                        // to a valid account
        ORCHA_ASSERT(disabled_because("not a regular file"));
        fs::remove(dir.file());

        fs::copy_file(other.file(), dir.file());
        fs::permissions(dir.file(), fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_write);
        ORCHA_ASSERT(disabled_because("writable by group or others"));
        fs::permissions(dir.file(), fs::perms::owner_read | fs::perms::owner_write);
        ORCHA_ASSERT(account(config(dir.file()), true)->mode() == Mode::Stored);

        fs::create_directory(dir.path / "adir");
        ORCHA_ASSERT(account(config((dir.path / "adir").string()), true)->mode() == Mode::Disabled);
        std::cout << "[PASS] test_admin_account_file_must_be_private\n";
    }

    // Exactly one claim wins: concurrent requests on one server, and two
    // servers (processes) sharing the file.
    inline void test_admin_setup_is_claimed_once() {
        using namespace admin_tests;
        TempDir dir;
        auto acct = account(config(dir.file()), true);
        std::atomic<int> won{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&, i] {
                if (!acct->set_up("user" + std::to_string(i), kGoodPassword)) ++won;
            });
        }
        for (auto& t : threads) t.join();
        ORCHA_ASSERT(won == 1 && acct->mode() == Mode::Stored);

        TempDir shared;
        auto a = account(config(shared.file()), true);
        auto b = account(config(shared.file()), true);   // started before either claimed
        ORCHA_ASSERT(!a->set_up("first", kGoodPassword));
        auto late = b->set_up("second", kGoodPassword);
        ORCHA_ASSERT(late && late->kind == Agent::AdminAccount::SetupError::Kind::AlreadySet);
        ORCHA_ASSERT(b->mode() == Mode::Stored && b->verify("first", kGoodPassword) && !b->verify("second", kGoodPassword));
        std::cout << "[PASS] test_admin_setup_is_claimed_once\n";
    }

    inline void test_admin_auth_follows_setup() {
        using namespace admin_tests;
        TempDir dir;
        auto acct = account(config(dir.file()), true);
        auto admin_mw = Agent::make_basic_auth(acct, {"/api/"});
        Config::ServerConfig server;
        server.host = "0.0.0.0";
        server.api_token = "tok";
        auto api_mw = Agent::make_api_auth(server, acct, {"/workflow"});

        // Before setup: /api/* refused with a hint, /api/setup open, no admin login for /workflow.
        auto refused = admin_mw(request("GET", "/api/jobs", basic("admin", "change-me")));
        ORCHA_ASSERT(refused && refused->status == 401 && refused->body.find("setup_required") != std::string::npos);
        ORCHA_ASSERT(!admin_mw(request("GET", "/api/setup")));
        ORCHA_ASSERT(!admin_mw(request("POST", "/api/setup")));
        ORCHA_ASSERT(admin_mw(request("GET", "/api/setup-other"))->status == 401);
        ORCHA_ASSERT(api_mw(request("POST", "/workflow", basic("ops", kGoodPassword)))->status == 401);
        ORCHA_ASSERT(!api_mw(request("POST", "/workflow", std::string("Bearer tok"))));

        ORCHA_ASSERT(!acct->set_up("ops", kGoodPassword));
        ORCHA_ASSERT(!admin_mw(request("GET", "/api/jobs", basic("ops", kGoodPassword))));
        ORCHA_ASSERT(admin_mw(request("GET", "/api/jobs", basic("ops", "wrong password")))->status == 401);
        ORCHA_ASSERT(admin_mw(request("GET", "/api/jobs"))->status == 401);
        ORCHA_ASSERT(!api_mw(request("POST", "/workflow", basic("ops", kGoodPassword))));

        // Open only on loopback.
        TempDir other;
        auto loop_open = Agent::make_basic_auth(account(config(other.file(), "", false), false), {"/api/"});
        ORCHA_ASSERT(!loop_open(request("GET", "/api/jobs")));
        auto exposed_open = Agent::make_basic_auth(account(config(other.file(), "", false), true), {"/api/"});
        ORCHA_ASSERT(exposed_open(request("GET", "/api/jobs"))->status == 401);
        std::cout << "[PASS] test_admin_auth_follows_setup\n";
    }

    // Wrong logins can't occupy every HTTP worker: PBKDF2 runs one at a time
    // (a check that would wait answers Busy -> 429 at once), known-good and
    // recently failed identical logins skip hashing.
    inline void test_admin_password_checks_are_bounded() {
        using namespace admin_tests;
        using Login = Agent::AdminAccount::Login;
        using clock = std::chrono::steady_clock;
        TempDir dir;
        auto slow = std::make_shared<Agent::AdminAccount>(config(dir.file()), true, nullptr, 3'000'000);
        ORCHA_ASSERT(!slow->set_up("ops", kGoodPassword));
        ORCHA_ASSERT(slow->check("ops", kGoodPassword) == Login::Ok);   // now cached

        // Several distinct wrong logins at once: one hashes, the rest are Busy
        // at once. Meanwhile the signed-in admin is answered from the cache.
        std::atomic<int> ok{0}, busy_count{0}, denied{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> flood;
        for (int i = 0; i < 6; ++i) {
            flood.emplace_back([&, i] {
                while (!go) std::this_thread::yield();
                switch (slow->check("ops", "wrong password #" + std::to_string(i))) {
                    case Login::Ok: ++ok; break;
                    case Login::Busy: ++busy_count; break;
                    case Login::Denied: ++denied; break;
                }
            });
        }
        go = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto t1 = clock::now();
        ORCHA_ASSERT(slow->check("ops", kGoodPassword) == Login::Ok);
        ORCHA_ASSERT(clock::now() - t1 < std::chrono::milliseconds(20));
        for (auto& t : flood) t.join();
        ORCHA_ASSERT(ok == 0 && denied >= 1 && busy_count >= 1 && denied + busy_count == 6);

        // One client's parallel requests (the dashboard after a restart): the
        // same login waits for the hash in flight instead of getting 429.
        // Regression: all but one of them failed right after setup or a restart.
        auto restarted = std::make_shared<Agent::AdminAccount>(config(dir.file()), true, nullptr);
        std::atomic<int> same_ok{0}, same_other{0};
        std::atomic<bool> fire{false};
        std::vector<std::thread> tabs;
        for (int i = 0; i < 5; ++i) {
            tabs.emplace_back([&] {
                while (!fire) std::this_thread::yield();
                (restarted->check("ops", kGoodPassword) == Login::Ok ? same_ok : same_other)++;
            });
        }
        fire = true;
        for (auto& t : tabs) t.join();
        ORCHA_ASSERT(same_ok == 5 && same_other == 0);

        // Repeating a failed login is answered from memory.
        const std::string failed = "wrong password #" + std::to_string(0);
        (void)slow->check("ops", failed);   // hashed (or Busy) once...
        while (slow->check("ops", failed) == Login::Busy) {}
        const auto t3 = clock::now();
        ORCHA_ASSERT(slow->check("ops", failed) == Login::Denied);   // ...then from memory
        ORCHA_ASSERT(clock::now() - t3 < std::chrono::milliseconds(20));
        // verify() waits instead of failing when busy.
        ORCHA_ASSERT(!slow->verify("ops", "wrong password #3") && slow->verify("ops", kGoodPassword));

        // Middleware: Busy is a 429 with Retry-After, not a 401.
        auto mw = Agent::make_basic_auth(slow, {"/api/"});
        std::atomic<bool> start{false};
        std::vector<std::optional<Agent::HttpResponse>> replies(6);
        std::vector<std::thread> clients;
        for (int i = 0; i < 6; ++i) {
            clients.emplace_back([&, i] {
                while (!start) std::this_thread::yield();
                replies[i] = mw(request("GET", "/api/jobs", basic("ops", "another wrong #" + std::to_string(i))));
            });
        }
        start = true;
        for (auto& t : clients) t.join();
        int n429 = 0, n401 = 0;
        for (const auto& r : replies) {
            ORCHA_ASSERT(r.has_value());
            if (r->status == 429) {
                ++n429;
                ORCHA_ASSERT(r->headers.size() == 1 && r->headers[0].first == "Retry-After");
            } else if (r->status == 401) {
                ++n401;
            }
        }
        ORCHA_ASSERT(n429 >= 1 && n401 >= 1 && n429 + n401 == 6);
        std::cout << "[PASS] test_admin_password_checks_are_bounded\n";
    }

    inline void test_setup_route() {
        using namespace admin_tests;
        TempDir dir;
        auto acct = account(config(dir.file()), true);
        Agent::Routes::SetupRoute route(acct);
        ORCHA_ASSERT(route.can_handle("GET", "/api/setup") && !route.can_handle("GET", "/api/setup/x"));
        ORCHA_ASSERT(route.handle(request("GET", "/api/setup")).body.find("\"setup_required\":true") != std::string::npos);

        auto no_type = request("POST", "/api/setup");
        no_type.body = R"({"username":"ops","password":"correct horse battery"})";
        ORCHA_ASSERT(route.handle(no_type).status == 415);
        ORCHA_ASSERT(route.handle(request("POST", "/api/setup", std::nullopt, "{oops")).status == 400);
        ORCHA_ASSERT(route.handle(request("POST", "/api/setup", std::nullopt, R"({"username":"ops"})")).status == 400);
        ORCHA_ASSERT(route.handle(request("POST", "/api/setup", std::nullopt, R"({"username":"ops","password":5})")).status == 400);
        auto weak = route.handle(request("POST", "/api/setup", std::nullopt, R"({"username":"ops","password":"short"})"));
        ORCHA_ASSERT(weak.status == 400 && weak.body.find("at least 12") != std::string::npos);
        ORCHA_ASSERT(route.handle(request("DELETE", "/api/setup")).status == 405);

        auto made = route.handle(request("POST", "/api/setup", std::nullopt,
                                         R"({"username":"ops","password":"correct horse battery"})"));
        ORCHA_ASSERT(made.status == 201 && made.body.find("\"username\":\"ops\"") != std::string::npos);
        ORCHA_ASSERT(route.handle(request("GET", "/api/setup")).body.find("\"setup_required\":false") != std::string::npos);
        ORCHA_ASSERT(route.handle(request("POST", "/api/setup", std::nullopt,
                                          R"({"username":"evil","password":"correct horse battery"})")).status == 409);
        ORCHA_ASSERT(acct->verify("ops", kGoodPassword) && !acct->verify("evil", kGoodPassword));
        std::cout << "[PASS] test_setup_route\n";
    }

    inline void run_admin_account_tests() {
        std::cout << "\n-- Admin account & first-run setup --\n";
        test_admin_account_modes();
        test_admin_first_run_setup();
        test_admin_corrupt_account_file_disables();
        test_admin_account_file_must_be_private();
        test_admin_setup_is_claimed_once();
        test_admin_auth_follows_setup();
        test_admin_password_checks_are_bounded();
        test_setup_route();
    }

} // namespace Orcha::Tests
