//
// HttpServer.hpp - Minimal multi-threaded HTTP server built on Boost.Beast.
//
// Replaces cpprestsdk's web::http::experimental::listener::http_listener.
// Network I/O (accept, read, write) is asynchronous on a single I/O thread, so
// read/write timeouts are enforced and an idle or slow client can neither pin
// a worker nor block shutdown. Each parsed request is handed to a bounded
// worker pool that runs the (blocking) Handler; the response is written back
// on the I/O thread. Connections are closed after a single exchange.
//

#pragma once

#include "Http.hpp"
#include "../utils/ILogger.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Orcha::Agent {

    class HttpSession;

    class HttpServer {
    public:
        using Handler = std::function<HttpResponse(const HttpRequest&)>;

        struct Options {
            /// Worker pool size: requests handled concurrently.
            unsigned int threads = 4;
            /// Time a client has to send a complete request.
            std::chrono::milliseconds read_timeout{std::chrono::seconds(30)};
            /// Time a client has to accept the complete response.
            std::chrono::milliseconds write_timeout{std::chrono::seconds(30)};
            /// After in-flight handlers finish, how long stop() lets pending
            /// response writes complete before closing their connections.
            std::chrono::milliseconds shutdown_grace{std::chrono::seconds(5)};
            /// Maximum request body size.
            std::uint64_t body_limit = 64ull * 1024 * 1024;
        };

        /**
         * @param handler  Request dispatcher (router + fallback). Must be thread-safe.
         * @param logger   Optional logger.
         * @param threads  Worker pool size for concurrent requests.
         */
        explicit HttpServer(Handler handler,
                            std::shared_ptr<Utils::ILogger> logger = nullptr,
                            unsigned int threads = 4);

        HttpServer(Handler handler, std::shared_ptr<Utils::ILogger> logger, Options options);
        ~HttpServer();

        HttpServer(const HttpServer&) = delete;
        HttpServer& operator=(const HttpServer&) = delete;

        /// Bind to @p host:@p port and start serving. Throws on bind failure
        /// or if @p host is not a valid IP address / "localhost". Port 0 binds
        /// an ephemeral port (see port()).
        void start(const std::string& host, unsigned short port);

        /**
         * @brief Stop serving.
         *
         * Stops accepting, aborts connections still waiting for a request,
         * waits for in-flight handlers (callers should cancel long-running work
         * first), lets their responses be written for up to
         * Options::shutdown_grace, then releases the socket. Idempotent.
         */
        void stop();

        /// The bound port (useful after binding port 0); 0 when not running.
        [[nodiscard]] unsigned short port() const { return port_.load(); }

    private:
        friend class HttpSession;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        Handler handler_;
        std::shared_ptr<Utils::ILogger> logger_;
        Options options_;
        std::atomic<bool> running_{false};
        std::atomic<unsigned short> port_{0};
    };

} // namespace Orcha::Agent
