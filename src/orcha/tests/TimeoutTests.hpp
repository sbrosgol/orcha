//
// TimeoutTests.hpp - Step/workflow timeouts, cancellation and the plugin ABI check
//
// Engine behaviour is tested with in-process commands. The shell_exec and
// http_request plugins are loaded from the build tree to test that they stop
// promptly (killing the child process group / aborting the socket operation).
//

#pragma once

#include "../core/CommandRegistry.hpp"
#include "../core/ExecutionControl.hpp"
#include "../jobs/JobService.hpp"
#include "../jobs/RunJobCommand.hpp"
#include "../jobs/SqliteJobStore.hpp"
#include "../workflow/WorkflowEngine.hpp"

#include "Assertions.hpp"
#include <chrono>
#include <future>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Orcha::Tests {

    namespace timeouts {

        using Clock = std::chrono::steady_clock;
        using namespace std::chrono_literals;

        inline long long ms_since(Clock::time_point start) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        }

        /// Works in 10 ms slices for params.ms, stopping early when asked.
        class CooperativeCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "cooperative"; }
            Json execute(const Json& params) override { return execute_with_control(params, {}); }
            Json execute_with_control(const Json& params, const Core::ExecutionControl& control) override {
                const auto until = Clock::now() + std::chrono::milliseconds(params.value("ms", 0));
                while (Clock::now() < until) {
                    if (control.should_stop()) {
                        return Json{{"success", false}, {"error", "stopped"}};
                    }
                    std::this_thread::sleep_for(10ms);
                }
                return Json{{"done", true}};
            }
        };

        /// Sleeps for params.ms and ignores its ExecutionControl.
        class StubbornCommand : public Core::ICommand {
        public:
            [[nodiscard]] std::string name() const override { return "stubborn"; }
            Json execute(const Json& params) override {
                std::this_thread::sleep_for(std::chrono::milliseconds(params.value("ms", 0)));
                return Json{{"done", true}};
            }
        };

        inline std::shared_ptr<Core::CommandRegistry> make_registry() {
            auto registry = std::make_shared<Core::CommandRegistry>();
            (void)registry->register_command(std::make_shared<CooperativeCommand>());
            (void)registry->register_command(std::make_shared<StubbornCommand>());
            return registry;
        }

        inline Workflow::WorkflowStep step(const std::string& command, int ms,
                                           std::optional<int> timeout_ms = std::nullopt) {
            Workflow::WorkflowStep s{command, Json{{"ms", ms}}};
            s.timeout_ms = timeout_ms;
            return s;
        }

        inline Workflow::WorkflowEngine make_engine(std::shared_ptr<Core::CommandRegistry> registry,
                                                    Workflow::EngineOptions options = {}) {
            return Workflow::WorkflowEngine(std::move(registry),
                                            std::make_shared<Workflow::SyncStepExecutor>(),
                                            nullptr, std::move(options));
        }

        inline std::shared_ptr<Core::ICommand> load_plugin(Core::CommandRegistry& registry,
                                                           const char* path, const char* name) {
            ORCHA_ASSERT(registry.load_command_library(path));
            auto cmd = registry.get_command(name);
            ORCHA_ASSERT(cmd != nullptr);
            return cmd;
        }

#if !defined(_WIN32)
        /// A listening socket that never accepts: connects succeed (backlog),
        /// requests are never answered.
        struct SilentServer {
            int fd = -1;
            int port = 0;
            SilentServer() {
                fd = ::socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                ORCHA_ASSERT(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
                ORCHA_ASSERT(::listen(fd, 8) == 0);
                socklen_t len = sizeof(addr);
                ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
                port = ntohs(addr.sin_port);
            }
            ~SilentServer() { if (fd >= 0) ::close(fd); }
            [[nodiscard]] std::string url() const {
                return "http://127.0.0.1:" + std::to_string(port) + "/";
            }
        };

        /// Answers one request with "200 ok" and closes.
        struct OneShotServer {
            SilentServer listener;
            std::thread worker;
            OneShotServer() {
                worker = std::thread([fd = listener.fd] {
                    const int conn = ::accept(fd, nullptr, nullptr);
                    if (conn < 0) return;
                    char buf[4096];
                    (void)::recv(conn, buf, sizeof(buf), 0);
                    const std::string resp =
                        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                    (void)::send(conn, resp.data(), resp.size(), 0);
                    ::close(conn);
                });
            }
            ~OneShotServer() { if (worker.joinable()) worker.join(); }
            [[nodiscard]] std::string url() const { return listener.url(); }
        };
#endif

    } // namespace timeouts

    // ========== ExecutionControl ==========

    inline void test_execution_control_basics() {
        using namespace timeouts;
        Core::ExecutionControl unbounded;
        ORCHA_ASSERT_FALSE(unbounded.should_stop());
        ORCHA_ASSERT_FALSE(unbounded.remaining().has_value());

        std::stop_source source;
        Core::ExecutionControl past(source.get_token(), Clock::now() - 1ms);
        ORCHA_ASSERT(past.expired() && past.should_stop());
        ORCHA_ASSERT(past.remaining() == std::chrono::milliseconds::zero());

        Core::ExecutionControl future(source.get_token(), Clock::now() + 10s);
        ORCHA_ASSERT_FALSE(future.should_stop());
        source.request_stop();
        ORCHA_ASSERT(future.stop_requested() && future.should_stop());
        std::cout << "[PASS] test_execution_control_basics\n";
    }

    // ========== Step and workflow timeouts ==========

    inline void test_step_timeout_cooperative() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 5000, 100));
        def.steps.push_back(step("cooperative", 0));

        const auto start = Clock::now();
        auto result = engine.execute(def);
        const auto elapsed = ms_since(start);

        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Step timed out after 100 ms"));
        ORCHA_ASSERT(result.step_results[1].command_name.empty()); // never started
        ORCHA_ASSERT(elapsed >= 100 && elapsed < 1000);
        std::cout << "[PASS] test_step_timeout_cooperative\n";
    }

    inline void test_step_timeout_non_cooperative_fails_late() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("stubborn", 250, 50));

        auto result = engine.execute(def);
        ORCHA_ASSERT_FALSE(result.success);
        // Its {"done": true} output is not trusted: it returned past the deadline.
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Step timed out after 50 ms"));
        std::cout << "[PASS] test_step_timeout_non_cooperative_fails_late\n";
    }

    inline void test_step_within_timeout_succeeds() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 20, 2000));
        auto result = engine.execute(def);
        ORCHA_ASSERT(result.success);
        ORCHA_ASSERT(result.step_results[0].output.value("done", false));
        std::cout << "[PASS] test_step_within_timeout_succeeds\n";
    }

    inline void test_default_step_timeout_applies() {
        using namespace timeouts;
        Workflow::EngineOptions options;
        options.default_step_timeout = 80ms;
        auto engine = make_engine(make_registry(), options);

        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 5000));           // uses the default
        auto result = engine.execute(def);
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Step timed out after 80 ms"));

        Workflow::WorkflowDefinition overridden;
        overridden.steps.push_back(step("cooperative", 150, 1000)); // own limit wins
        ORCHA_ASSERT(engine.execute(overridden).success);
        std::cout << "[PASS] test_default_step_timeout_applies\n";
    }

    inline void test_workflow_timeout() {
        using namespace timeouts;
        auto engine = make_engine(make_registry());
        auto def = Workflow::WorkflowDefinition::from_json(Json{
            {"timeout_ms", 150},
            {"steps", Json::array({
                {{"command", "cooperative"}, {"params", {{"ms", 100}}}},
                {{"command", "cooperative"}, {"params", {{"ms", 5000}}}},
                {{"command", "cooperative"}, {"params", {{"ms", 0}}}}})}});

        const auto start = Clock::now();
        auto result = engine.execute(def);
        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT(result.step_results[0].success);
        ORCHA_ASSERT_EQ(result.step_results[1].error_message, std::string("Workflow timed out after 150 ms"));
        ORCHA_ASSERT(result.step_results[2].command_name.empty());
        ORCHA_ASSERT(ms_since(start) < 1000);
        std::cout << "[PASS] test_workflow_timeout\n";
    }

    // ========== Cancellation ==========

    inline void test_cancel_running_workflow() {
        using namespace timeouts;
        std::stop_source shutdown;
        Workflow::EngineOptions options;
        options.cancel = shutdown.get_token();
        auto engine = make_engine(make_registry(), options);

        Workflow::WorkflowDefinition def;
        def.steps.push_back(step("cooperative", 10000));
        def.steps.push_back(step("cooperative", 0));

        const auto start = Clock::now();
        auto running = std::async(std::launch::async, [&] { return engine.execute(def); });
        std::this_thread::sleep_for(100ms);
        shutdown.request_stop();
        auto result = running.get();

        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT_EQ(result.step_results[0].error_message, std::string("Cancelled: stopped"));
        ORCHA_ASSERT(result.step_results[1].command_name.empty());
        ORCHA_ASSERT(ms_since(start) < 1000);

        // Already cancelled: nothing runs at all.
        auto again = engine.execute(def);
        ORCHA_ASSERT_EQ(again.step_results[0].error_message, std::string("Cancelled"));
        std::cout << "[PASS] test_cancel_running_workflow\n";
    }

    inline void test_cancel_reaches_parallel_steps() {
        using namespace timeouts;
        std::stop_source shutdown;
        Workflow::EngineOptions options;
        options.cancel = shutdown.get_token();
        auto engine = make_engine(make_registry(), options);

        Workflow::WorkflowDefinition def;
        for (int i = 0; i < 3; ++i) {
            auto s = step("cooperative", 10000);
            s.parallel = true;
            def.steps.push_back(s);
        }

        const auto start = Clock::now();
        auto running = std::async(std::launch::async, [&] { return engine.execute(def); });
        std::this_thread::sleep_for(100ms);
        shutdown.request_stop();
        auto result = running.get();

        ORCHA_ASSERT_FALSE(result.success);
        for (const auto& r : result.step_results) {
            ORCHA_ASSERT_EQ(r.error_message, std::string("Cancelled: stopped"));
        }
        ORCHA_ASSERT(ms_since(start) < 1000);
        std::cout << "[PASS] test_cancel_reaches_parallel_steps\n";
    }

    inline void test_cancel_reaches_nested_run_job() {
        using namespace timeouts;
        auto registry = make_registry();
        auto store = std::make_shared<Jobs::SqliteJobStore>(":memory:");
        // Nested engines get NO cancel token of their own: cancellation must
        // reach them through the enclosing run's ExecutionContext.
        auto service = std::make_shared<Jobs::JobService>(store, [registry] {
            return std::make_shared<Workflow::WorkflowEngine>(registry);
        });
        ORCHA_ASSERT(registry->register_command(std::make_shared<Jobs::RunJobCommand>(service)));

        Jobs::JobDefinition inner;
        inner.name = "inner";
        inner.definition = Json{{"steps", Json::array({
            {{"command", "cooperative"}, {"params", {{"ms", 10000}}}}})}};
        ORCHA_ASSERT(store->create_job(inner));

        std::stop_source shutdown;
        Workflow::EngineOptions options;
        options.cancel = shutdown.get_token();
        auto outer = make_engine(registry, options);
        Workflow::WorkflowDefinition def;
        def.steps.push_back({"run_job", Json{{"job", "inner"}}});

        const auto start = Clock::now();
        auto running = std::async(std::launch::async, [&] { return outer.execute(def); });
        std::this_thread::sleep_for(100ms);
        shutdown.request_stop();
        auto result = running.get();

        ORCHA_ASSERT_FALSE(result.success);
        ORCHA_ASSERT(result.step_results[0].error_message.find("Cancelled") != std::string::npos);
        ORCHA_ASSERT(ms_since(start) < 1000);
        std::cout << "[PASS] test_cancel_reaches_nested_run_job\n";
    }

    // ========== Plugin ABI ==========

    inline void test_plugin_abi_version_checked() {
#if defined(ORCHA_TEST_LEGACY_PLUGIN_PATH) && defined(ORCHA_TEST_SLOW_PLUGIN_PATH)
        // Order matters under ASan on macOS: it does not unregister an unloaded
        // image's globals, so mapping a DIFFERENT instrumented library at the
        // refused plugin's old address yields a false global-buffer-overflow.
        // Load the versioned plugin first and refuse the legacy one last.
        Core::CommandRegistry registry;
        ORCHA_ASSERT(registry.load_command_library(ORCHA_TEST_SLOW_PLUGIN_PATH));
        ORCHA_ASSERT(registry.unregister_command("slow_test_plugin"));
        ORCHA_ASSERT_FALSE(registry.load_command_library(ORCHA_TEST_LEGACY_PLUGIN_PATH));
        ORCHA_ASSERT_FALSE(registry.has_command("slow_test_plugin"));
        std::cout << "[PASS] test_plugin_abi_version_checked\n";
#else
        std::cout << "[SKIP] test_plugin_abi_version_checked (no test plugins)\n";
#endif
    }

    // ========== shell_exec ==========

    inline void test_shell_exec_basic() {
#if defined(ORCHA_TEST_SHELLEXEC_PATH) && !defined(_WIN32)
        using namespace timeouts;
        Core::CommandRegistry registry;
        auto shell = load_plugin(registry, ORCHA_TEST_SHELLEXEC_PATH, "shell_exec");

        auto out = shell->execute_with_control(Json{{"cmd", "echo out; echo err >&2; exit 3"}}, {});
        ORCHA_ASSERT_EQ(out["exit_code"].get<int>(), 3);
        ORCHA_ASSERT_FALSE(out["success"].get<bool>());
        ORCHA_ASSERT(out["output"].get<std::string>().find("out") != std::string::npos);
        ORCHA_ASSERT(out["output"].get<std::string>().find("err") != std::string::npos);

        // A backgrounded grandchild keeps the pipe open; we must not wait for it.
        const auto start = Clock::now();
        auto bg = shell->execute_with_control(Json{{"cmd", "sleep 3 & echo done"}}, {});
        ORCHA_ASSERT(bg["success"].get<bool>());
        ORCHA_ASSERT(bg["output"].get<std::string>().find("done") != std::string::npos);
        ORCHA_ASSERT(ms_since(start) < 2000);
        std::cout << "[PASS] test_shell_exec_basic\n";
#else
        std::cout << "[SKIP] test_shell_exec_basic\n";
#endif
    }

    inline void test_shell_exec_timeout_kills_process_group() {
#if defined(ORCHA_TEST_SHELLEXEC_PATH) && !defined(_WIN32)
        using namespace timeouts;
        Core::CommandRegistry registry;
        auto shell = load_plugin(registry, ORCHA_TEST_SHELLEXEC_PATH, "shell_exec");

        const auto start = Clock::now();
        // The shell prints its background child's pid, then waits on it.
        auto out = shell->execute_with_control(
            Json{{"cmd", "sleep 30 & echo $!; wait"}},
            Core::ExecutionControl(std::stop_token{}, Clock::now() + 300ms));
        const auto elapsed = ms_since(start);

        ORCHA_ASSERT_FALSE(out["success"].get<bool>());
        ORCHA_ASSERT_EQ(out["error"].get<std::string>(), std::string("Timed out; process terminated"));
        ORCHA_ASSERT(elapsed >= 300 && elapsed < 3000);

        // The grandchild (sleep 30) was in the same process group: it is gone.
        const pid_t child = static_cast<pid_t>(std::stol(out["output"].get<std::string>()));
        bool gone = false;
        for (int i = 0; i < 50 && !gone; ++i) {
            gone = ::kill(child, 0) != 0;
            if (!gone) std::this_thread::sleep_for(20ms);
        }
        ORCHA_ASSERT(gone);
        std::cout << "[PASS] test_shell_exec_timeout_kills_process_group\n";
#else
        std::cout << "[SKIP] test_shell_exec_timeout_kills_process_group\n";
#endif
    }

    inline void test_shell_exec_cancellation() {
#if defined(ORCHA_TEST_SHELLEXEC_PATH) && !defined(_WIN32)
        using namespace timeouts;
        Core::CommandRegistry registry;
        auto shell = load_plugin(registry, ORCHA_TEST_SHELLEXEC_PATH, "shell_exec");

        std::stop_source stop;
        auto canceller = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(150ms);
            stop.request_stop();
        });
        const auto start = Clock::now();
        auto out = shell->execute_with_control(Json{{"cmd", "sleep 30"}},
                                               Core::ExecutionControl(stop.get_token(), std::nullopt));
        canceller.get();
        ORCHA_ASSERT_EQ(out["error"].get<std::string>(), std::string("Cancelled; process terminated"));
        ORCHA_ASSERT(ms_since(start) < 3000);
        std::cout << "[PASS] test_shell_exec_cancellation\n";
#else
        std::cout << "[SKIP] test_shell_exec_cancellation\n";
#endif
    }

    // ========== http_request ==========

    inline void test_http_request_timeouts_and_cancellation() {
#if defined(ORCHA_TEST_HTTPREQUEST_PATH) && !defined(_WIN32)
        using namespace timeouts;
        Core::CommandRegistry registry;
        auto http = load_plugin(registry, ORCHA_TEST_HTTPREQUEST_PATH, "http_request");

        // Sanity: the async client completes a normal exchange.
        {
            OneShotServer server;
            auto ok = http->execute_with_control(Json{{"url", server.url()}}, {});
            ORCHA_ASSERT(ok["success"].get<bool>());
            ORCHA_ASSERT_EQ(ok["body"].get<std::string>(), std::string("ok"));
        }

        SilentServer silent;

        // Per-operation inactivity timeout (timeout_ms param).
        auto start = Clock::now();
        auto stalled = http->execute_with_control(Json{{"url", silent.url()}, {"timeout_ms", 200}}, {});
        ORCHA_ASSERT_FALSE(stalled["success"].get<bool>());
        ORCHA_ASSERT(stalled["error"].get<std::string>().find("timed out after 200 ms") != std::string::npos);
        ORCHA_ASSERT(ms_since(start) < 2000);

        // Step deadline bounds the whole request.
        start = Clock::now();
        auto deadline = http->execute_with_control(
            Json{{"url", silent.url()}},
            Core::ExecutionControl(std::stop_token{}, Clock::now() + 250ms));
        ORCHA_ASSERT(deadline["error"].get<std::string>().find("HTTP request timed out") != std::string::npos);
        ORCHA_ASSERT(ms_since(start) < 2000);

        // Cancellation aborts it.
        std::stop_source stop;
        auto canceller = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(150ms);
            stop.request_stop();
        });
        start = Clock::now();
        auto cancelled = http->execute_with_control(
            Json{{"url", silent.url()}}, Core::ExecutionControl(stop.get_token(), std::nullopt));
        canceller.get();
        ORCHA_ASSERT(cancelled["error"].get<std::string>().find("cancelled") != std::string::npos);
        ORCHA_ASSERT(ms_since(start) < 2000);
        std::cout << "[PASS] test_http_request_timeouts_and_cancellation\n";
#else
        std::cout << "[SKIP] test_http_request_timeouts_and_cancellation\n";
#endif
    }

    inline void run_timeout_tests() {
        std::cout << "\n-- Timeouts & Cancellation --\n";
        test_execution_control_basics();
        test_step_timeout_cooperative();
        test_step_timeout_non_cooperative_fails_late();
        test_step_within_timeout_succeeds();
        test_default_step_timeout_applies();
        test_workflow_timeout();
        test_cancel_running_workflow();
        test_cancel_reaches_parallel_steps();
        test_cancel_reaches_nested_run_job();
        test_shell_exec_basic();
        test_shell_exec_timeout_kills_process_group();
        test_shell_exec_cancellation();
        test_http_request_timeouts_and_cancellation();
        test_plugin_abi_version_checked();   // last: see the ASan note inside
    }

} // namespace Orcha::Tests
