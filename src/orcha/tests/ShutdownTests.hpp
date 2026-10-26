//
// ShutdownTests.hpp - HTTP server lifecycle and background threads
//
// - HttpServer: request read timeout, stop() with idle connections (previously
//   blocked forever), stop() waiting for in-flight handlers, error handling.
// - PluginManager watcher / JobScheduler: stop() returns promptly.
//

#pragma once

#include "../agent/HttpServer.hpp"
#include "../core/CommandRegistry.hpp"
#include "../core/PluginManager.hpp"
#include "../jobs/JobScheduler.hpp"
#include "../jobs/JobService.hpp"
#include "../jobs/SqliteJobStore.hpp"

#include "Assertions.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace Orcha::Tests {

    namespace shutdown {

        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        inline long long ms_since(Clock::time_point start) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        }

#if !defined(_WIN32)
        /// Blocking client socket with a receive timeout (so a regression
        /// fails the test instead of hanging it).
        struct Client {
            int fd = -1;
            explicit Client(unsigned short port) {
                fd = ::socket(AF_INET, SOCK_STREAM, 0);
                timeval tv{5, 0};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                ORCHA_ASSERT(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
            }
            ~Client() { if (fd >= 0) ::close(fd); }

            void send(const std::string& data) const {
                ORCHA_ASSERT(::send(fd, data.data(), data.size(), 0) == static_cast<ssize_t>(data.size()));
            }

            /// Read until the server closes the connection (or the 5 s timeout).
            [[nodiscard]] std::string read_all() const {
                std::string out;
                char buf[4096];
                for (;;) {
                    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    out.append(buf, static_cast<size_t>(n));
                }
                return out;
            }
        };

        inline std::string get_request(const std::string& path) {
            return "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        }
#endif

        /// Run @p fn, failing the whole test binary (instead of hanging it)
        /// if it does not return within 10 s.
        template <typename Fn>
        void within_deadline(const char* what, Fn fn) {
            auto fut = std::async(std::launch::async, std::move(fn));
            if (fut.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
                std::cerr << "[FAIL] " << what << " did not return within 10s\n";
                std::_Exit(1);
            }
            fut.get();
        }

        inline std::filesystem::path temp_path(const std::string& name) {
            return std::filesystem::temp_directory_path() /
                   ("orcha-test-" + std::to_string(::getpid()) + "-" + name);
        }

        inline size_t count_lines(const std::filesystem::path& p, const std::string& needle) {
            std::ifstream in(p);
            size_t n = 0;
            for (std::string line; std::getline(in, line);) {
                if (line.find(needle) != std::string::npos) ++n;
            }
            return n;
        }

    } // namespace shutdown

    // ========== HttpServer ==========

    inline void test_http_server_serves_requests() {
#if !defined(_WIN32)
        using namespace shutdown;
        Agent::HttpServer server([](const Agent::HttpRequest& req) {
            if (req.path == "/boom") throw std::runtime_error("handler failed");
            return Agent::HttpResponse::text(Agent::status::OK, "path=" + req.path);
        });
        server.start("127.0.0.1", 0);
        ORCHA_ASSERT(server.port() != 0);

        {
            Client c(server.port());
            c.send(get_request("/hello%20world"));
            const auto resp = c.read_all();
            ORCHA_ASSERT(resp.rfind("HTTP/1.1 200", 0) == 0);
            ORCHA_ASSERT(resp.find("path=/hello world") != std::string::npos);
        }
        {
            Client c(server.port());
            c.send(get_request("/boom"));
            ORCHA_ASSERT(c.read_all().rfind("HTTP/1.1 500", 0) == 0);
        }
        server.stop();
        ORCHA_ASSERT_EQ(server.port(), 0);

        bool threw = false;
        try {
            Agent::HttpServer bad([](const Agent::HttpRequest&) { return Agent::HttpResponse{}; });
            bad.start("not-an-address", 0);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        ORCHA_ASSERT(threw);
        std::cout << "[PASS] test_http_server_serves_requests\n";
#else
        std::cout << "[SKIP] test_http_server_serves_requests\n";
#endif
    }

    inline void test_http_server_read_timeout_closes_idle_connection() {
#if !defined(_WIN32)
        using namespace shutdown;
        Agent::HttpServer::Options options;
        options.read_timeout = 200ms;
        Agent::HttpServer server([](const Agent::HttpRequest&) { return Agent::HttpResponse{}; },
                                 nullptr, options);
        server.start("127.0.0.1", 0);

        Client idle(server.port());   // connects, never sends a request
        const auto start = Clock::now();
        const auto resp = idle.read_all();
        // Closed by the server (previously the timeout was never applied and
        // the connection held a worker forever).
        ORCHA_ASSERT(resp.empty());
        ORCHA_ASSERT(ms_since(start) >= 150 && ms_since(start) < 2000);
        server.stop();
        std::cout << "[PASS] test_http_server_read_timeout_closes_idle_connection\n";
#else
        std::cout << "[SKIP] test_http_server_read_timeout_closes_idle_connection\n";
#endif
    }

    inline void test_http_server_stop_with_idle_connections() {
#if !defined(_WIN32)
        using namespace shutdown;
        Agent::HttpServer server([](const Agent::HttpRequest&) { return Agent::HttpResponse{}; },
                                 nullptr, 2);   // default 30 s read timeout
        server.start("127.0.0.1", 0);

        // More idle connections than workers: before the fix each one pinned
        // a worker in a blocking read and stop() never returned.
        std::vector<std::unique_ptr<Client>> idle;
        for (int i = 0; i < 4; ++i) idle.push_back(std::make_unique<Client>(server.port()));
        std::this_thread::sleep_for(50ms);

        const auto start = Clock::now();
        within_deadline("HttpServer::stop with idle connections", [&] { server.stop(); });
        ORCHA_ASSERT(ms_since(start) < 1000);
        for (auto& c : idle) ORCHA_ASSERT(c->read_all().empty()); // closed, no response
        std::cout << "[PASS] test_http_server_stop_with_idle_connections\n";
#else
        std::cout << "[SKIP] test_http_server_stop_with_idle_connections\n";
#endif
    }

    inline void test_http_server_stop_completes_in_flight_requests() {
#if !defined(_WIN32)
        using namespace shutdown;
        std::atomic<int> started{0};
        Agent::HttpServer server([&](const Agent::HttpRequest&) {
            ++started;
            std::this_thread::sleep_for(300ms);
            return Agent::HttpResponse::text(Agent::status::OK, "finished");
        }, nullptr, 2);
        server.start("127.0.0.1", 0);

        Client c(server.port());
        c.send(get_request("/slow"));
        auto response = std::async(std::launch::async, [&] { return c.read_all(); });
        while (started == 0) std::this_thread::sleep_for(5ms);

        const auto start = Clock::now();
        // Must wait for the handler and deliver its response.
        within_deadline("HttpServer::stop with an in-flight request", [&] { server.stop(); });
        const auto stop_ms = ms_since(start);

        const auto resp = response.get();
        ORCHA_ASSERT(resp.rfind("HTTP/1.1 200", 0) == 0);
        ORCHA_ASSERT(resp.find("finished") != std::string::npos);
        ORCHA_ASSERT(stop_ms >= 150 && stop_ms < 2000);
        std::cout << "[PASS] test_http_server_stop_completes_in_flight_requests\n";
#else
        std::cout << "[SKIP] test_http_server_stop_completes_in_flight_requests\n";
#endif
    }

    inline void test_http_server_concurrent_requests() {
#if !defined(_WIN32)
        using namespace shutdown;
        std::atomic<int> active{0};
        std::atomic<int> peak{0};
        Agent::HttpServer server([&](const Agent::HttpRequest& req) {
            const int now = ++active;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(100ms);
            --active;
            return Agent::HttpResponse::text(Agent::status::OK, req.path);
        }, nullptr, 3);
        server.start("127.0.0.1", 0);

        std::vector<std::future<std::string>> responses;
        for (int i = 0; i < 9; ++i) {
            responses.push_back(std::async(std::launch::async, [port = server.port(), i] {
                Client c(port);
                c.send(get_request("/r" + std::to_string(i)));
                return c.read_all();
            }));
        }
        for (int i = 0; i < 9; ++i) {
            const auto resp = responses[i].get();
            ORCHA_ASSERT(resp.find("/r" + std::to_string(i)) != std::string::npos);
        }
        ORCHA_ASSERT(peak.load() <= 3);   // bounded by the worker pool
        server.stop();
        std::cout << "[PASS] test_http_server_concurrent_requests\n";
#else
        std::cout << "[SKIP] test_http_server_concurrent_requests\n";
#endif
    }

    // ========== Background threads ==========

    inline void test_plugin_watcher_stops_promptly() {
        using namespace shutdown;
        auto logger = std::make_shared<Utils::NullLogger>();
        auto registry = std::make_shared<Core::CommandRegistry>();
        Core::PluginManager manager(registry, std::make_shared<Core::PluginDiscoveryService>(), logger);
        const auto dir = temp_path("watch");
        std::filesystem::create_directories(dir);

        // Concurrent start/stop from several threads must be safe (TSan).
        std::vector<std::thread> togglers;
        for (int i = 0; i < 4; ++i) {
            togglers.emplace_back([&, i] {
                for (int k = 0; k < 10; ++k) {
                    if ((i + k) % 2) manager.start_watching(dir, 10s);
                    else manager.stop_watching();
                }
            });
        }
        for (auto& t : togglers) t.join();

        manager.start_watching(dir, 10s);
        ORCHA_ASSERT(manager.is_watching());
        std::this_thread::sleep_for(50ms);
        const auto start = Clock::now();
        // Previously slept out the full interval.
        within_deadline("PluginManager::stop_watching", [&] { manager.stop_watching(); });
        ORCHA_ASSERT(ms_since(start) < 500);
        ORCHA_ASSERT_FALSE(manager.is_watching());
        std::filesystem::remove_all(dir);
        std::cout << "[PASS] test_plugin_watcher_stops_promptly\n";
    }

    inline void test_job_scheduler_stops_promptly() {
        using namespace shutdown;
        auto store = std::make_shared<Jobs::SqliteJobStore>(":memory:");
        auto service = std::make_shared<Jobs::JobService>(store, [] {
            return std::shared_ptr<Workflow::IWorkflowEngine>{};
        });
        Jobs::JobScheduler scheduler(service, nullptr, 30s);
        scheduler.start();
        std::this_thread::sleep_for(50ms);
        const auto start = Clock::now();
        scheduler.stop();
        ORCHA_ASSERT(ms_since(start) < 500);
        ORCHA_ASSERT_FALSE(scheduler.is_running());
        std::cout << "[PASS] test_job_scheduler_stops_promptly\n";
    }

    inline void run_shutdown_tests() {
        std::cout << "\n-- Shutdown & Threading --\n";
        test_http_server_serves_requests();
        test_http_server_read_timeout_closes_idle_connection();
        test_http_server_stop_with_idle_connections();
        test_http_server_stop_completes_in_flight_requests();
        test_http_server_concurrent_requests();
        test_plugin_watcher_stops_promptly();
        test_job_scheduler_stops_promptly();
    }

} // namespace Orcha::Tests
