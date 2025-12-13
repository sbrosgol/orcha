//
// ILogger.hpp - Abstract interface for logging
// Created as part of architectural improvements
//

#pragma once

#include <string>
#include <map>
#include <optional>
#include <memory>
#include <source_location>

namespace Orcha::Utils {

    enum class LogLevel {
        TRACE,
        DEBUG,
        INFO,
        WARNING,
        ERROR,
        FATAL
    };

    /**
     * @struct LogContext
     * @brief Contextual information for log entries.
     */
    struct LogContext {
        std::optional<std::string> correlation_id;
        std::optional<std::string> component;
        std::map<std::string, std::string> tags;

        LogContext() = default;

        LogContext& with_correlation_id(std::string id) {
            correlation_id = std::move(id);
            return *this;
        }

        LogContext& with_component(std::string comp) {
            component = std::move(comp);
            return *this;
        }

        LogContext& with_tag(const std::string& key, const std::string& value) {
            tags[key] = value;
            return *this;
        }
    };

    /**
     * @interface ILogger
     * @brief Abstract interface for logging functionality.
     *
     * Enables dependency injection and mocking for testability.
     */
    class ILogger {
    public:
        virtual ~ILogger() = default;

        /**
         * @brief Log a message with level and optional context.
         */
        virtual void log(LogLevel level,
                        const std::string& msg,
                        const LogContext& ctx = {},
                        const std::source_location& loc = std::source_location::current()) = 0;

        // Convenience methods with context
        void trace(const std::string& msg, const LogContext& ctx = {},
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::TRACE, msg, ctx, loc);
        }

        void debug(const std::string& msg, const LogContext& ctx = {},
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::DEBUG, msg, ctx, loc);
        }

        void info(const std::string& msg, const LogContext& ctx = {},
                 const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::INFO, msg, ctx, loc);
        }

        void warn(const std::string& msg, const LogContext& ctx = {},
                 const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::WARNING, msg, ctx, loc);
        }

        void error(const std::string& msg, const LogContext& ctx = {},
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::ERROR, msg, ctx, loc);
        }

        void fatal(const std::string& msg, const LogContext& ctx = {},
                  const std::source_location& loc = std::source_location::current()) {
            log(LogLevel::FATAL, msg, ctx, loc);
        }

        /**
         * @brief Set minimum log level.
         */
        virtual void set_level(LogLevel level) = 0;

        /**
         * @brief Get current minimum log level.
         */
        [[nodiscard]] virtual LogLevel get_level() const = 0;

        /**
         * @brief Flush any buffered log entries.
         */
        virtual void flush() = 0;
    };

    /**
     * @class NullLogger
     * @brief No-op logger implementation for testing.
     */
    class NullLogger : public ILogger {
    public:
        void log(LogLevel, const std::string&, const LogContext&,
                const std::source_location&) override {}
        void set_level(LogLevel level) override { level_ = level; }
        [[nodiscard]] LogLevel get_level() const override { return level_; }
        void flush() override {}
    private:
        LogLevel level_ = LogLevel::INFO;
    };

    /**
     * @brief Convert LogLevel to string.
     */
    inline std::string to_string(LogLevel level) {
        switch (level) {
            case LogLevel::TRACE:   return "TRACE";
            case LogLevel::DEBUG:   return "DEBUG";
            case LogLevel::INFO:    return "INFO";
            case LogLevel::WARNING: return "WARN";
            case LogLevel::ERROR:   return "ERROR";
            case LogLevel::FATAL:   return "FATAL";
            default:                return "UNKNOWN";
        }
    }

} // namespace Orcha::Utils
