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
        worker_ = std::thread(&Logger::logging_thread, this);
    }

    Logger::~Logger() {
        shutdown();
    }

    void Logger::set_log_file(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!filename.empty()) {
            std::filesystem::path p(filename);
            auto dir = p.parent_path();
            if (!dir.empty()) {
                std::filesystem::create_directories(dir);
            }
        }
        log_filename_ = filename;
        file_.open(filename, std::ios::app);
        use_file_ = file_.is_open();
    }

    void Logger::log(LogLevel level,
                    const std::string& msg,
                    const LogContext& ctx,
                    const std::source_location& loc) {
        // Check minimum level
        if (static_cast<int>(level) < static_cast<int>(min_level_.load())) {
            return;
        }

        std::string formatted = format_log_entry(level, msg, ctx, loc);
        enqueue(formatted);
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

    void Logger::enqueue(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            log_queue_.push(message);
        }
        cv_.notify_one();
    }

    void Logger::logging_thread() {
        while (running_ || !log_queue_.empty()) {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [&] { return !log_queue_.empty() || !running_; });

            while (!log_queue_.empty()) {
                const std::string message = log_queue_.front();
                log_queue_.pop();
                lock.unlock();

                // Output to appropriate streams based on level indicators
                if (message.find("[ERROR]") != std::string::npos ||
                    message.find("[WARN]") != std::string::npos ||
                    message.find("[FATAL]") != std::string::npos) {
                    std::cerr << message;
                } else {
                    std::cout << message;
                }

                if (use_file_ && file_.is_open()) {
                    file_ << message;
                    file_.flush();
                }
                lock.lock();
            }
        }
    }

    void Logger::flush() {
        // Wait for queue to empty
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.notify_one();
        // Note: This is a best-effort flush - for synchronous flush,
        // we'd need a more complex mechanism
    }

    void Logger::shutdown() {
        if (running_.exchange(false)) {
            cv_.notify_one();
            if (worker_.joinable()) {
                worker_.join();
            }
            if (file_.is_open()) {
                file_.close();
            }
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
