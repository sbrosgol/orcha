//
// MockLogger.hpp - Mock logger for testing
// Created as part of test infrastructure
//

#pragma once

#include "../../utils/ILogger.hpp"
#include <vector>
#include <string>
#include <mutex>

namespace Orcha::Tests::Mocks {

    /**
     * @struct LogEntry
     * @brief Captured log entry for verification.
     */
    struct LogEntry {
        Utils::LogLevel level;
        std::string message;
        Utils::LogContext context;
        std::source_location location;
    };

    /**
     * @class MockLogger
     * @brief Mock logger that captures log entries for testing.
     */
    class MockLogger : public Utils::ILogger {
    public:
        void log(Utils::LogLevel level,
                const std::string& msg,
                const Utils::LogContext& ctx = {},
                const std::source_location& loc = std::source_location::current()) override {
            std::lock_guard lock(mutex_);
            entries_.push_back({level, msg, ctx, loc});
        }

        void set_level(Utils::LogLevel level) override {
            level_ = level;
        }

        [[nodiscard]] Utils::LogLevel get_level() const override {
            return level_;
        }

        void flush() override {
            // No-op for mock
        }

        // ========== Mock Verification ==========

        /**
         * @brief Get all captured log entries.
         */
        [[nodiscard]] std::vector<LogEntry> get_entries() const {
            std::lock_guard lock(mutex_);
            return entries_;
        }

        /**
         * @brief Get entries at a specific level.
         */
        [[nodiscard]] std::vector<LogEntry> get_entries_at_level(Utils::LogLevel level) const {
            std::lock_guard lock(mutex_);
            std::vector<LogEntry> result;
            for (const auto& entry : entries_) {
                if (entry.level == level) {
                    result.push_back(entry);
                }
            }
            return result;
        }

        /**
         * @brief Check if any entry contains the given message.
         */
        [[nodiscard]] bool has_message(const std::string& substring) const {
            std::lock_guard lock(mutex_);
            for (const auto& entry : entries_) {
                if (entry.message.find(substring) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Check if any entry at level contains message.
         */
        [[nodiscard]] bool has_message_at_level(Utils::LogLevel level,
                                                const std::string& substring) const {
            std::lock_guard lock(mutex_);
            for (const auto& entry : entries_) {
                if (entry.level == level &&
                    entry.message.find(substring) != std::string::npos) {
                    return true;
                }
            }
            return false;
        }

        /**
         * @brief Get the count of entries at a specific level.
         */
        [[nodiscard]] size_t count_at_level(Utils::LogLevel level) const {
            std::lock_guard lock(mutex_);
            size_t count = 0;
            for (const auto& entry : entries_) {
                if (entry.level == level) {
                    ++count;
                }
            }
            return count;
        }

        /**
         * @brief Clear all captured entries.
         */
        void clear() {
            std::lock_guard lock(mutex_);
            entries_.clear();
        }

        /**
         * @brief Get total entry count.
         */
        [[nodiscard]] size_t entry_count() const {
            std::lock_guard lock(mutex_);
            return entries_.size();
        }

    private:
        mutable std::mutex mutex_;
        std::vector<LogEntry> entries_;
        Utils::LogLevel level_ = Utils::LogLevel::DEBUG;
    };

} // namespace Orcha::Tests::Mocks
