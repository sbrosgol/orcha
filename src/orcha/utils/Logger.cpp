//
// Logger.cpp - Concrete logging implementation
// Updated to implement ILogger interface
//

#include "Logger.hpp"
#include <filesystem>

namespace Orcha::Utils {

    Logger& Logger::instance() {
        static Logger logger;
        return logger;
    }

    Logger::Logger() {
        writer_running_ = true;
        writer_ = std::jthread([this](std::stop_token stop) { writer_loop(stop); });
    }

    Logger::~Logger() {
        shutdown();
    }

    void Logger::set_log_file(const std::string& filename) {
        std::lock_guard lock(output_mutex_);
        if (file_.is_open()) {
            file_.close();
        }
        if (filename.empty()) {
            return;
        }
        std::error_code ec;
        const auto dir = std::filesystem::path(filename).parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir, ec);
        }
        file_.open(filename, std::ios::app);
    }

    void Logger::set_console_output(bool enabled) {
        std::lock_guard lock(output_mutex_);
        console_ = enabled;
    }

    void Logger::log(LogLevel level,
                    const std::string& msg,
                    const LogContext& ctx,
                    const std::source_location& loc) {
        // Check minimum level
        if (level == LogLevel::OFF ||
            static_cast<int>(level) < static_cast<int>(min_level_.load())) {
            return;
        }

        Entry entry{level, format_log_entry(level, msg, ctx, loc)};
        {
            std::lock_guard lock(queue_mutex_);
            if (writer_running_) {
                queue_.push_back(std::move(entry));
                ++enqueued_;
                queue_cv_.notify_one();
                return;
            }
        }
        // After shutdown (e.g. during static destruction): write directly.
        write_entries(std::deque<Entry>{std::move(entry)});
    }

    std::string Logger::format_log_entry(LogLevel level,
                                         const std::string& msg,
                                         const LogContext& ctx,
                                         const std::source_location& loc) {
        std::ostringstream oss;

        // Timestamp
        oss << "[" << timestamp() << "]";

        // Level
        oss << "[" << to_string(level) << "]";

        // Component (if present)
        if (ctx.component.has_value()) {
            oss << "[" << ctx.component.value() << "]";
        }

        // Correlation ID (if present)
        if (ctx.correlation_id.has_value()) {
            oss << "[" << ctx.correlation_id.value() << "]";
        }

        // Message
        oss << " " << msg;

        // Tags (if any)
        if (!ctx.tags.empty()) {
            oss << " {";
            bool first = true;
            for (const auto& [key, value] : ctx.tags) {
                if (!first) oss << ", ";
                oss << key << "=" << value;
                first = false;
            }
            oss << "}";
        }

        // Source location for DEBUG and below
        if (level == LogLevel::DEBUG || level == LogLevel::TRACE) {
            oss << " [" << loc.file_name() << ":" << loc.line() << "]";
        }

        oss << '\n';
        return oss.str();
    }

    void Logger::writer_loop(std::stop_token stop) {
        std::unique_lock lock(queue_mutex_);
        for (;;) {
            // Wakes for new entries or for stop; the queue is drained before exit.
            queue_cv_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (queue_.empty()) {
                break;   // stop requested and nothing left
            }
            std::deque<Entry> batch;
            batch.swap(queue_);
            lock.unlock();

            write_entries(batch);

            lock.lock();
            written_ += batch.size();
            written_cv_.notify_all();
        }
    }

    void Logger::write_entries(const std::deque<Entry>& entries) {
        std::lock_guard lock(output_mutex_);
        for (const auto& entry : entries) {
            if (console_) {
                (entry.level >= LogLevel::WARNING ? std::cerr : std::cout) << entry.text;
            }
            if (file_.is_open()) {
                file_ << entry.text;
            }
        }
        if (file_.is_open()) {
            file_.flush();
        }
    }

    void Logger::flush() {
        std::unique_lock lock(queue_mutex_);
        const auto target = enqueued_;
        written_cv_.wait(lock, [this, target] { return written_ >= target || !writer_running_; });
    }

    void Logger::shutdown() {
        {
            // From here on log() writes synchronously, so nothing enqueued
            // after the writer's final drain can be lost.
            std::lock_guard lock(queue_mutex_);
            if (!writer_running_) {
                return;
            }
            writer_running_ = false;
        }
        if (writer_.joinable()) {
            writer_.request_stop();   // writer drains the queue, then exits
            writer_.join();
        }
        {
            std::lock_guard lock(queue_mutex_);
            written_cv_.notify_all();
        }
        std::lock_guard lock(output_mutex_);
        if (file_.is_open()) {
            file_.close();
        }
    }

    std::string Logger::timestamp() {
        using namespace std::chrono;
        const auto now = system_clock::now();
        auto t_c = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t_c);
#else
        localtime_r(&t_c, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%F %T")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

} // namespace Orcha::Utils
