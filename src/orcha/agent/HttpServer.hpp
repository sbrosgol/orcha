//
// HttpServer.hpp - Minimal multi-threaded HTTP server built on Boost.Beast.
//
// Replaces cpprestsdk's web::http::experimental::listener::http_listener. A
// dedicated thread runs the accept loop; each accepted connection is handled
// synchronously on a bounded thread pool. The caller supplies a single Handler
// that maps a parsed HttpRequest to an HttpResponse.
//

#pragma once

#include "Http.hpp"
#include "../utils/ILogger.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace Orcha::Agent {

    class HttpServer {
    public:
        using Handler = std::function<HttpResponse(const HttpRequest&)>;

        /**
         * @param handler  Request dispatcher (router + fallback). Must be thread-safe.
         * @param logger   Optional logger.
         * @param threads  Worker pool size for concurrent connections.
         */
        explicit HttpServer(Handler handler,
                            std::shared_ptr<Utils::ILogger> logger = nullptr,
                            unsigned int threads = 4);
        ~HttpServer();

        HttpServer(const HttpServer&) = delete;
        HttpServer& operator=(const HttpServer&) = delete;

        /// Bind to 0.0.0.0:@p port and start serving. Throws on bind failure.
        void start(unsigned short port);

        /// Stop accepting, drain workers, and release the socket.
        void stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        Handler handler_;
        std::shared_ptr<Utils::ILogger> logger_;
        unsigned int threads_;
        std::atomic<bool> running_{false};
    };

} // namespace Orcha::Agent
