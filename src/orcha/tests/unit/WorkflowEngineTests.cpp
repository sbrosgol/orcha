//
// WorkflowEngineTests.cpp - Unit tests for WorkflowEngine
// Created as part of test infrastructure
//
// Note: This file demonstrates test patterns using the mock infrastructure.
// Integrate with your preferred test framework (Google Test, Catch2, etc.)
//

#include "../TestFixtures.hpp"
#include <cassert>
#include <iostream>

namespace Orcha::Tests::Unit {

    /**
     * Test: Empty workflow executes successfully.
     */
    void test_empty_workflow() {
        WorkflowTestFixture fixture;
        fixture.SetUp();

        Workflow::WorkflowDefinition def;
        auto result = fixture.engine_->execute(def);

        ORCHA_ASSERT(result.success && "Empty workflow should succeed");
        ORCHA_ASSERT(result.step_results.empty() && "Should have no step results");

        std::cout << "  PASS: test_empty_workflow\n";
        fixture.TearDown();
    }

    /**
     * Test: Single command workflow executes.
     */
    void test_single_command_workflow() {
        WorkflowTestFixture fixture;
        fixture.SetUp();

        // Add echo command mock
        fixture.add_mock_command("echo", [](const web::json::value& params) {
            web::json::value result;
            result[U("echoed")] = params.has_field(U("message"))
                ? params.at(U("message"))
                : web::json::value::string(U(""));
            return result;
        });

        // Create workflow
        auto def = fixture.create_workflow({
            {"echo", [] {
                web::json::value p;
                p[U("message")] = web::json::value::string(U("Hello"));
                return p;
            }()}
        });

        auto result = fixture.engine_->execute(def);

        ORCHA_ASSERT(result.success && "Workflow should succeed");
        ORCHA_ASSERT(result.step_results.size() == 1 && "Should have one step result");
        ORCHA_ASSERT(result.step_results[0].success && "Step should succeed");

        std::cout << "  PASS: test_single_command_workflow\n";
        fixture.TearDown();
    }

    /**
     * Test: Command not found fails workflow.
     */
    void test_command_not_found() {
        WorkflowTestFixture fixture;
        fixture.SetUp();

        auto def = fixture.create_workflow({
            {"nonexistent", web::json::value::object()}
        });

        auto result = fixture.engine_->execute(def);

        ORCHA_ASSERT(!result.success && "Workflow should fail");
        ORCHA_ASSERT(!result.step_results.empty() && "Should have error result");
        ORCHA_ASSERT(result.step_results[0].error_message.find("not found") != std::string::npos);

        std::cout << "  PASS: test_command_not_found\n";
        fixture.TearDown();
    }

    /**
     * Test: Command failure stops workflow.
     */
    void test_command_failure_stops_workflow() {
        WorkflowTestFixture fixture;
        fixture.SetUp();

        fixture.add_failing_command("fail", "Intentional failure");
        fixture.add_mock_command("echo");

        auto def = fixture.create_workflow({
            {"fail", web::json::value::object()},
            {"echo", web::json::value::object()}
        });

        auto result = fixture.engine_->execute(def);

        ORCHA_ASSERT(!result.success && "Workflow should fail");
        ORCHA_ASSERT(result.step_results.size() == 1 && "Should stop after first step");

        std::cout << "  PASS: test_command_failure_stops_workflow\n";
        fixture.TearDown();
    }

    /**
     * Test: Multiple successful commands.
     */
    void test_multiple_commands() {
        WorkflowTestFixture fixture;
        fixture.SetUp();

        int call_count = 0;
        fixture.add_mock_command("step", [&call_count](const web::json::value&) {
            ++call_count;
            web::json::value r;
            r[U("count")] = web::json::value::number(call_count);
            return r;
        });

        auto def = fixture.create_workflow({
            {"step", web::json::value::object()},
            {"step", web::json::value::object()},
            {"step", web::json::value::object()}
        });

        auto result = fixture.engine_->execute(def);

        ORCHA_ASSERT(result.success && "Workflow should succeed");
        ORCHA_ASSERT(result.step_results.size() == 3 && "Should have three results");
        ORCHA_ASSERT(call_count == 3 && "Command should be called three times");

        std::cout << "  PASS: test_multiple_commands\n";
        fixture.TearDown();
    }

    /**
     * Test: Service locator registration and retrieval.
     */
    void test_service_locator() {
        ServiceLocatorTestFixture fixture;
        fixture.SetUp();

        auto registry = std::make_shared<Mocks::MockCommandRegistry>();
        fixture.services_->register_singleton<Core::ICommandRegistry>(registry);

        auto retrieved = fixture.services_->get<Core::ICommandRegistry>();
        ORCHA_ASSERT(retrieved.get() == registry.get() && "Should retrieve same instance");

        ORCHA_ASSERT(fixture.services_->has<Core::ICommandRegistry>());
        ORCHA_ASSERT(!fixture.services_->has<Utils::ILogger>());

        std::cout << "  PASS: test_service_locator\n";
        fixture.TearDown();
    }

    /**
     * Test: Mock logger captures entries.
     */
    void test_mock_logger() {
        auto logger = std::make_shared<Mocks::MockLogger>();

        logger->info("Test message");
        logger->error("Error message");
        logger->debug("Debug message");

        ORCHA_ASSERT(logger->entry_count() == 3);
        ORCHA_ASSERT(logger->has_message("Test message"));
        ORCHA_ASSERT(logger->has_message_at_level(Utils::LogLevel::ERROR, "Error"));
        ORCHA_ASSERT(logger->count_at_level(Utils::LogLevel::INFO) == 1);

        logger->clear();
        ORCHA_ASSERT(logger->entry_count() == 0);

        std::cout << "  PASS: test_mock_logger\n";
    }

    /**
     * Run all tests.
     */
    void run_all_tests() {
        std::cout << "Running WorkflowEngine tests...\n";

        test_empty_workflow();
        test_single_command_workflow();
        test_command_not_found();
        test_command_failure_stops_workflow();
        test_multiple_commands();
        test_service_locator();
        test_mock_logger();

        std::cout << "\nAll tests passed!\n";
    }

} // namespace Orcha::Tests::Unit

// Entry point for standalone test runner
#ifdef RUN_TESTS
int main() {
    Orcha::Tests::Unit::run_all_tests();
    return 0;
}
#endif
