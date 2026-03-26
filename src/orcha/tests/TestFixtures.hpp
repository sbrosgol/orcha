//
// TestFixtures.hpp - Test fixture base classes
// Created as part of test infrastructure
//

#pragma once

#include "mocks/MockCommandRegistry.hpp"
#include "mocks/MockLogger.hpp"
#include "../workflow/WorkflowEngine.hpp"
#include "../core/ServiceLocator.hpp"
#include <memory>
#include <stdexcept>

#define ORCHA_ASSERT(expr) \
    do { if (!(expr)) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        " Assertion failed: " #expr); } while(0)

#define ORCHA_ASSERT_EQ(a, b) \
    do { if ((a) != (b)) throw std::runtime_error( \
        std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
        " Expected " #a " == " #b); } while(0)

#define ORCHA_ASSERT_TRUE(expr) ORCHA_ASSERT(expr)
#define ORCHA_ASSERT_FALSE(expr) ORCHA_ASSERT(!(expr))

namespace Orcha::Tests {

    /**
     * @class WorkflowTestFixture
     * @brief Base fixture for workflow-related tests.
     */
    class WorkflowTestFixture {
    public:
        virtual void SetUp() {
            registry_ = std::make_shared<Mocks::MockCommandRegistry>();
            logger_ = std::make_shared<Mocks::MockLogger>();
            executor_ = std::make_shared<Workflow::SyncStepExecutor>();
            engine_ = std::make_unique<Workflow::WorkflowEngine>(
                registry_, executor_, logger_);
        }

        virtual void TearDown() {
            engine_.reset();
            registry_->clear();
            logger_->clear();
        }

    protected:
        /**
         * @brief Add a mock command that returns specific output.
         */
        void add_mock_command(const std::string& name,
                             std::function<web::json::value(const web::json::value&)> fn = nullptr) {
            registry_->add_command(std::make_shared<Mocks::MockCommand>(name, fn));
        }

        /**
         * @brief Add a command that always fails.
         */
        void add_failing_command(const std::string& name,
                                const std::string& error = "Command failed") {
            registry_->add_command(std::make_shared<Mocks::FailingCommand>(name, error));
        }

        /**
         * @brief Create a simple workflow definition.
         */
        static Workflow::WorkflowDefinition create_workflow(
            std::initializer_list<std::pair<std::string, web::json::value>> steps) {

            Workflow::WorkflowDefinition def;
            for (const auto& [cmd_name, params] : steps) {
                Workflow::WorkflowStep step;
                step.command_name = cmd_name;
                step.params = params;
                def.steps.push_back(step);
            }
            return def;
        }

        std::shared_ptr<Mocks::MockCommandRegistry> registry_;
        std::shared_ptr<Mocks::MockLogger> logger_;
        std::shared_ptr<Workflow::IStepExecutor> executor_;
        std::unique_ptr<Workflow::WorkflowEngine> engine_;
    };

    /**
     * @class ServiceLocatorTestFixture
     * @brief Base fixture for DI container tests.
     */
    class ServiceLocatorTestFixture {
    public:
        virtual void SetUp() {
            services_ = std::make_unique<Core::ServiceLocator>();
        }

        virtual void TearDown() {
            services_->clear();
        }

    protected:
        std::unique_ptr<Core::ServiceLocator> services_;
    };

    /**
     * @class CommandTestFixture
     * @brief Base fixture for individual command tests.
     */
    class CommandTestFixture {
    public:
        virtual void SetUp() {
            logger_ = std::make_shared<Mocks::MockLogger>();
        }

        virtual void TearDown() {
            logger_->clear();
        }

    protected:
        /**
         * @brief Execute a command and return result.
         */
        web::json::value execute_command(Core::ICommand& cmd,
                                        const web::json::value& params) {
            return cmd.execute(params);
        }

        /**
         * @brief Validate command parameters.
         */
        Core::Result<void, Core::ValidationError> validate_params(
            Core::ICommand& cmd,
            const web::json::value& params) {
            return cmd.validate(params);
        }

        /**
         * @brief Create JSON params from key-value pairs.
         */
        static web::json::value make_params(
            std::initializer_list<std::pair<std::string, web::json::value>> pairs) {

            web::json::value params = web::json::value::object();
            for (const auto& [key, value] : pairs) {
                params[key] = value;
            }
            return params;
        }

        std::shared_ptr<Mocks::MockLogger> logger_;
    };

    /**
     * @class ConfigurationTestFixture
     * @brief Base fixture for configuration tests.
     */
    class ConfigurationTestFixture {
    public:
        virtual void SetUp() {
            // Create empty in-memory config
        }

        virtual void TearDown() {
            // Cleanup
        }

    protected:
        /**
         * @brief Create config from YAML string.
         */
        static std::unique_ptr<Config::YamlConfiguration> create_config(
            const std::string& yaml_content) {
            auto config = std::make_unique<Config::YamlConfiguration>();
            config->load_from_string(yaml_content);
            return config;
        }
    };

    // ========== Test Assertion Helpers ==========

    /**
     * @brief Assert that workflow succeeded.
     */
    inline void assert_workflow_success(const Workflow::WorkflowResult& result) {
        if (!result.success) {
            throw std::runtime_error("Workflow failed: " + result.error_message);
        }
    }

    /**
     * @brief Assert that workflow failed.
     */
    inline void assert_workflow_failed(const Workflow::WorkflowResult& result) {
        if (result.success) {
            throw std::runtime_error("Expected workflow to fail but it succeeded");
        }
    }

    /**
     * @brief Assert step count in result.
     */
    inline void assert_step_count(const Workflow::WorkflowResult& result, size_t expected) {
        if (result.step_results.size() != expected) {
            throw std::runtime_error(
                "Expected " + std::to_string(expected) + " steps but got " +
                std::to_string(result.step_results.size()));
        }
    }

} // namespace Orcha::Tests
