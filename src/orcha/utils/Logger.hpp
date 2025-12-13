//
// Logger.hpp - Concrete logging implementation
// Updated to implement ILogger interface
//

#pragma once

#include "ILogger.hpp"
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <atomic>
#include <fstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace Orcha::Utils {

    /**
     * @class Logger
     * @brief Thread-safe logger with background worker and file output.
     *
     * Implements ILogger interface and provides singleton access for convenience.
     */
    class Logger : public ILogger {
    public:
        /**
         * @brief Get singleton instance.
         */
        static Logger& instance();

        // Singleton - prevent copy/move
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        // Legacy static convenience methods (for backward compatibility)
        static void info(const std::string& msg) { instance().log(LogLevel::INFO, msg); }
        static void warn(const std::string& msg) { instance().log(LogLevel::WARNING, msg); }
        static void error(const std::string& msg) { instance().log(LogLevel::ERROR, msg); }
        static void debug(const std::string& msg) { instance().log(LogLevel::DEBUG, msg); }

        /**
         * @brief Set output log file.
         * @param filename Path to log file.
         */
        void set_log_file(const std::string& filename);

        // ILogger interface implementation
        void log(LogLevel level,
                const std::string& msg,
                const LogContext& ctx = {},
                const std::source_location& loc = std::source_location::current()) override;

        void set_level(LogLevel level) override { min_level_ = level; }
        [[nodiscard]] LogLevel get_level() const override { return min_level_; }
        void flush() override;

        /**
         * @brief Graceful shutdown - flush and stop worker.
         */
        void shutdown();

    private:
        Logger();
        ~Logger() override;

        void logging_thread();
        void enqueue(const std::string& message);

        static std::string timestamp();
        static std::string format_log_entry(LogLevel level,
                                           const std::string& msg,
                                           const LogContext& ctx,
                                           const std::source_location& loc);

        std::mutex mtx_;
        std::condition_variable cv_;
        std::queue<std::string> log_queue_;
        std::thread worker_;
        std::atomic<bool> running_{true};
        std::atomic<LogLevel> min_level_{LogLevel::INFO};

        std::ofstream file_;
        std::string log_filename_;
        bool use_file_ = false;
    };

    /**
     * @class ScopedLogger
     * @brief RAII logger that can be injected into components.
     *
     * Wraps an ILogger reference with component context.
     */
    class ScopedLogger {
    public:
        ScopedLogger(std::shared_ptr<ILogger> logger, std::string component)
            : logger_(std::move(logger))
            , ctx_(LogContext().with_component(std::move(component))) {}

        void trace(const std::string& msg,
                  const std::source_location& loc = std::source_location::current()) {
            logger_->log(LogLevel::TRACE, msg, ctx_, loc);
        }

        void debug(const std::string& msg,
                  const std::source_location& loc = std::source_location::current()) {
            logger_->log(LogLevel::DEBUG, msg, ctx_, loc);
        }

        void info(const std::string& msg,
                 const std::source_location& loc = std::source_location::current()) {
            logger_->log(LogLevel::INFO, msg, ctx_, loc);
        }

        void warn(const std::string& msg,
                 const std::source_location& loc = std::source_location::current()) {
            logger_->log(LogLevel::WARNING, msg, ctx_, loc);
        }

        void error(const std::string& msg,
                  const std::source_location& loc = std::source_location::current()) {
            logger_->log(LogLevel::ERROR, msg, ctx_, loc);
        }

        void with_correlation_id(const std::string& id) {
            ctx_.with_correlation_id(id);
        }

    private:
        std::shared_ptr<ILogger> logger_;
        LogContext ctx_;
    };

} // namespace Orcha::Utils
