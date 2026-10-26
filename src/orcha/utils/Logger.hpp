//
// Logger.hpp - Concrete logging implementation
// Updated to implement ILogger interface
//

#pragma once

#include "ILogger.hpp"
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <stop_token>
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
     * @brief Thread-safe logger with a background writer and file output.
     *
     * Implements ILogger interface and provides singleton access for convenience.
     *
     * log() formats on the caller's thread and enqueues; a single writer thread
     * drains the queue to the console and the log file. flush() blocks until
     * everything logged before the call has been written. After shutdown()
     * (or during static destruction) entries are written synchronously.
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
         * @brief Set output log file (empty disables file output).
         * @param filename Path to log file.
         */
        void set_log_file(const std::string& filename);

        /**
         * @brief Enable/disable console output (WARN and above go to stderr).
         */
        void set_console_output(bool enabled);

        // ILogger interface implementation
        void log(LogLevel level,
                const std::string& msg,
                const LogContext& ctx = {},
                const std::source_location& loc = std::source_location::current()) override;

        void set_level(LogLevel level) override { min_level_ = level; }
        [[nodiscard]] LogLevel get_level() const override { return min_level_; }

        /**
         * @brief Block until every entry logged before this call is written.
         */
        void flush() override;

        /**
         * @brief Graceful shutdown - drain the queue and stop the writer.
         */
        void shutdown();

    private:
        Logger();
        ~Logger() override;

        struct Entry {
            LogLevel level;
            std::string text;
        };

        void writer_loop(std::stop_token stop);
        void write_entries(const std::deque<Entry>& entries);

        static std::string timestamp();
        static std::string format_log_entry(LogLevel level,
                                           const std::string& msg,
                                           const LogContext& ctx,
                                           const std::source_location& loc);

        std::atomic<LogLevel> min_level_{LogLevel::INFO};

        // Queue state, guarded by queue_mutex_.
        std::mutex queue_mutex_;
        std::condition_variable_any queue_cv_;     // writer waits for entries
        std::condition_variable written_cv_;       // flush() waits for progress
        std::deque<Entry> queue_;
        std::uint64_t enqueued_ = 0;
        std::uint64_t written_ = 0;
        bool writer_running_ = false;

        // Output state, guarded by output_mutex_.
        std::mutex output_mutex_;
        std::ofstream file_;
        bool console_ = true;

        std::jthread writer_;   // last: stopped/joined before the state it uses
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
