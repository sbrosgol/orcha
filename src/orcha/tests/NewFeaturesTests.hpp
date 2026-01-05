//
// NewFeaturesTests.hpp - Tests for new architecture features
//

#pragma once

#include "../config/ConfigValidator.hpp"
#include "../config/YamlConfiguration.hpp"
#include "../core/CircuitBreaker.hpp"
#include "../workflow/RollbackOrchestrator.hpp"
#include "../workflow/ParallelWorkflowExecutor.hpp"
#include "mocks/MockCommandRegistry.hpp"
#include "mocks/MockLogger.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

namespace Orcha::Tests {

    // ========== Configuration Validation Tests ==========

    inline void test_config_validation_valid() {
        std::string yaml = R"(
server:
  port: 8080
  host: "0.0.0.0"
  worker_threads: 4

plugins:
  directory: "commands"
  auto_reload: true
  scan_interval_ms: 5000

logging:
  level: "info"
  file: "orcha.log"
)";

        Config::YamlConfiguration config;
        config.load_from_string(yaml);

        Config::ConfigValidator validator;
        auto result = validator.validate(config);

        assert(result.valid && "Valid config should pass validation");
        assert(result.issues.empty() && "Should have no issues");

        std::cout << "[PASS] test_config_validation_valid\n";
    }

    inline void test_config_validation_invalid_port() {
        std::string yaml = R"(
server:
  port: 99999
)";

        Config::YamlConfiguration config;
        config.load_from_string(yaml);

        Config::ConfigValidator validator;
        auto result = validator.validate(config);

        assert(!result.valid && "Invalid port should fail validation");
        assert(!result.get_error_messages().empty() && "Should have error messages");

        std::cout << "[PASS] test_config_validation_invalid_port\n";
    }

    inline void test_config_validation_invalid_log_level() {
        std::string yaml = R"(
logging:
  level: "invalid_level"
)";

        Config::YamlConfiguration config;
        config.load_from_string(yaml);

        Config::ConfigValidator validator;
        auto result = validator.validate(config);

        assert(!result.valid && "Invalid log level should fail validation");

        std::cout << "[PASS] test_config_validation_invalid_log_level\n";
    }

    inline void test_config_validation_warnings() {
        std::string yaml = R"(
server:
  port: 80
  worker_threads: 100
)";

        Config::YamlConfiguration config;
        config.load_from_string(yaml);

        Config::ConfigValidator validator;
        auto result = validator.validate(config);

        // Port 80 requires root (warning), 100 threads is high (warning)
        assert(!result.get_warning_messages().empty() && "Should have warnings");

        std::cout << "[PASS] test_config_validation_warnings\n";
    }

    // ========== Circuit Breaker Tests ==========

    inline void test_circuit_breaker_closed_state() {
        Core::CircuitBreaker breaker("test", {5, 2, std::chrono::seconds{30}, std::chrono::seconds{60}});

        assert(breaker.state() == Core::CircuitState::Closed && "Initial state should be closed");
        assert(breaker.allow_request() && "Should allow requests when closed");

        breaker.record_success();
        assert(breaker.state() == Core::CircuitState::Closed && "Should stay closed after success");

        std::cout << "[PASS] test_circuit_breaker_closed_state\n";
    }

    inline void test_circuit_breaker_opens_on_failures() {
        Core::CircuitBreakerConfig config{3, 1, std::chrono::seconds{1}, std::chrono::seconds{60}};
        Core::CircuitBreaker breaker("test", config);

        // Record 3 failures (threshold)
        breaker.record_failure();
        breaker.record_failure();
        assert(breaker.state() == Core::CircuitState::Closed && "Should still be closed");

        breaker.record_failure();
        assert(breaker.state() == Core::CircuitState::Open && "Should be open after threshold");
        assert(!breaker.allow_request() && "Should reject requests when open");

        std::cout << "[PASS] test_circuit_breaker_opens_on_failures\n";
    }

    inline void test_circuit_breaker_half_open_transition() {
        Core::CircuitBreakerConfig config{2, 1, std::chrono::seconds{1}, std::chrono::seconds{60}};
        Core::CircuitBreaker breaker("test", config);

        // Trip the breaker
        breaker.record_failure();
        breaker.record_failure();
        assert(breaker.state() == Core::CircuitState::Open && "Should be open");

        // Wait for reset timeout
        std::this_thread::sleep_for(std::chrono::seconds{2});

        // Should transition to half-open and allow a request
        assert(breaker.allow_request() && "Should allow request after timeout");
        assert(breaker.state() == Core::CircuitState::HalfOpen && "Should be half-open");

        // Success should close the circuit
        breaker.record_success();
        assert(breaker.state() == Core::CircuitState::Closed && "Should close after success in half-open");

        std::cout << "[PASS] test_circuit_breaker_half_open_transition\n";
    }

    inline void test_circuit_breaker_registry() {
        Core::CircuitBreakerRegistry registry;

        auto& breaker1 = registry.get_or_create("cmd1");
        auto& breaker2 = registry.get_or_create("cmd2");

        assert(registry.exists("cmd1") && "Should exist after creation");
        assert(registry.exists("cmd2") && "Should exist after creation");
        assert(!registry.exists("cmd3") && "Should not exist");

        // Trip one breaker
        for (int i = 0; i < 5; ++i) breaker1.record_failure();

        assert(registry.open_circuit_count() == 1 && "Should have one open circuit");

        auto stats = registry.all_stats();
        assert(stats.size() == 2 && "Should have two breakers");

        std::cout << "[PASS] test_circuit_breaker_registry\n";
    }

    // ========== Rollback Orchestrator Tests ==========

    inline void test_rollback_records_steps() {
        auto registry = std::make_shared<Mocks::MockCommandRegistry>();
        auto logger = std::make_shared<Mocks::MockLogger>();

        Workflow::RollbackOrchestrator orchestrator(registry, logger);

        web::json::value params;
        params[U("key")] = web::json::value::string("value");
        web::json::value output;
        output[U("result")] = web::json::value::string("success");

        orchestrator.record_completed_step(0, "cmd1", params, output);
        orchestrator.record_completed_step(1, "cmd2", params, output);

        // No rollbackable steps since mock commands don't support rollback
        assert(!orchestrator.has_rollbackable_steps() && "Mock commands don't support rollback");

        std::cout << "[PASS] test_rollback_records_steps\n";
    }

    inline void test_rollback_mode_none() {
        auto registry = std::make_shared<Mocks::MockCommandRegistry>();
        auto logger = std::make_shared<Mocks::MockLogger>();

        Workflow::RollbackOrchestrator orchestrator(
            registry, logger, Workflow::RollbackMode::None);

        orchestrator.record_completed_step(0, "cmd1", {}, {});

        auto result = orchestrator.execute_rollback("test failure");

        assert(!result.initiated && "Rollback should not be initiated in None mode");

        std::cout << "[PASS] test_rollback_mode_none\n";
    }

    // ========== Parallel Workflow Executor Tests ==========

    inline void test_dependency_analyzer_no_deps() {
        Workflow::WorkflowDefinition def;

        Workflow::WorkflowStep step1;
        step1.command_name = "echo";
        step1.params = web::json::value::object();

        Workflow::WorkflowStep step2;
        step2.command_name = "echo";
        step2.params = web::json::value::object();

        def.steps = {step1, step2};

        auto deps = Workflow::WorkflowDependencyAnalyzer::analyze(def);

        assert(deps[0].depends_on.empty() && "Step 0 should have no deps");
        assert(deps[1].depends_on.empty() && "Step 1 should have no deps");

        std::cout << "[PASS] test_dependency_analyzer_no_deps\n";
    }

    inline void test_dependency_analyzer_with_refs() {
        Workflow::WorkflowDefinition def;

        Workflow::WorkflowStep step1;
        step1.command_name = "echo";
        step1.params = web::json::value::object();

        Workflow::WorkflowStep step2;
        step2.command_name = "echo";
        web::json::value params2;
        params2[U("message")] = web::json::value::string("Result: {{step1.output.value}}");
        step2.params = params2;

        def.steps = {step1, step2};

        auto deps = Workflow::WorkflowDependencyAnalyzer::analyze(def);

        assert(deps[0].depends_on.empty() && "Step 0 should have no deps");
        assert(deps[1].depends_on.count(0) == 1 && "Step 1 should depend on step 0");

        std::cout << "[PASS] test_dependency_analyzer_with_refs\n";
    }

    inline void test_execution_levels_parallel() {
        Workflow::WorkflowDefinition def;

        // Steps 0, 1, 2 are independent
        // Step 3 depends on all three

        Workflow::WorkflowStep step0, step1, step2, step3;
        step0.command_name = "echo";
        step0.params = web::json::value::object();

        step1.command_name = "echo";
        step1.params = web::json::value::object();

        step2.command_name = "echo";
        step2.params = web::json::value::object();

        web::json::value params3;
        params3[U("a")] = web::json::value::string("{{step1.output}}");
        params3[U("b")] = web::json::value::string("{{step2.output}}");
        params3[U("c")] = web::json::value::string("{{step3.output}}");
        step3.command_name = "aggregate";
        step3.params = params3;

        def.steps = {step0, step1, step2, step3};

        auto levels = Workflow::WorkflowDependencyAnalyzer::compute_execution_levels(def);

        assert(levels.size() == 2 && "Should have 2 execution levels");
        assert(levels[0].step_indices.size() == 3 && "Level 0 should have 3 parallel steps");
        assert(levels[1].step_indices.size() == 1 && "Level 1 should have 1 step");

        std::cout << "[PASS] test_execution_levels_parallel\n";
    }

    inline void test_has_parallelism() {
        // Sequential workflow (each step references previous)
        {
            Workflow::WorkflowDefinition def;

            Workflow::WorkflowStep step0, step1;
            step0.command_name = "echo";
            step0.params = web::json::value::object();

            web::json::value params1;
            params1[U("input")] = web::json::value::string("{{step1.output}}");
            step1.command_name = "process";
            step1.params = params1;

            def.steps = {step0, step1};

            assert(!Workflow::WorkflowDependencyAnalyzer::has_parallelism(def) &&
                   "Sequential workflow should have no parallelism");
        }

        // Parallel workflow (independent steps)
        {
            Workflow::WorkflowDefinition def;

            Workflow::WorkflowStep step0, step1;
            step0.command_name = "echo";
            step0.params = web::json::value::object();

            step1.command_name = "echo";
            step1.params = web::json::value::object();

            def.steps = {step0, step1};

            assert(Workflow::WorkflowDependencyAnalyzer::has_parallelism(def) &&
                   "Independent steps should have parallelism");
        }

        std::cout << "[PASS] test_has_parallelism\n";
    }

    // ========== Run All Tests ==========

    inline void run_new_features_tests() {
        std::cout << "\n=== Running New Features Tests ===\n\n";

        // Config Validation
        std::cout << "-- Configuration Validation --\n";
        test_config_validation_valid();
        test_config_validation_invalid_port();
        test_config_validation_invalid_log_level();
        test_config_validation_warnings();

        // Circuit Breaker
        std::cout << "\n-- Circuit Breaker --\n";
        test_circuit_breaker_closed_state();
        test_circuit_breaker_opens_on_failures();
        test_circuit_breaker_half_open_transition();
        test_circuit_breaker_registry();

        // Rollback Orchestrator
        std::cout << "\n-- Rollback Orchestrator --\n";
        test_rollback_records_steps();
        test_rollback_mode_none();

        // Parallel Workflow Executor
        std::cout << "\n-- Parallel Workflow Executor --\n";
        test_dependency_analyzer_no_deps();
        test_dependency_analyzer_with_refs();
        test_execution_levels_parallel();
        test_has_parallelism();

        std::cout << "\n=== All New Features Tests Passed ===\n";
    }

} // namespace Orcha::Tests
