//
// HttpServer.cpp - Boost.Beast implementation of the agent HTTP server.
//

#include "HttpServer.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace Orcha::Agent {

    namespace beast = boost::beast;
    namespace http  = boost::beast::http;
    namespace net   = boost::asio;
    using tcp = boost::asio::ip::tcp;

    namespace {

        std::string percent_decode(const std::string& in, bool plus_as_space = false) {
            std::string out;
            out.reserve(in.size());
            for (std::size_t i = 0; i < in.size(); ++i) {
                const char c = in[i];
                if (c == '%' && i + 2 < in.size()) {
                    auto hex = [](char h) -> int {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                        return -1;
                    };
                    const int hi = hex(in[i + 1]);
                    const int lo = hex(in[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                if (plus_as_space && c == '+') {
                    out.push_back(' ');
                } else {
                    out.push_back(c);
                }
            }
            return out;
        }

        void parse_query(const std::string& q, std::map<std::string, std::string>& out) {
            std::size_t start = 0;
            while (start < q.size()) {
                std::size_t amp = q.find('&', start);
                if (amp == std::string::npos) amp = q.size();
                const std::string pair = q.substr(start, amp - start);
                if (!pair.empty()) {
                    const std::size_t eq = pair.find('=');
                    if (eq == std::string::npos) {
                        out[percent_decode(pair, true)] = "";
                    } else {
                        out[percent_decode(pair.substr(0, eq), true)] =
                            percent_decode(pair.substr(eq + 1), true);
                    }
                }
                start = amp + 1;
            }
        }

        // Split a raw request target into a decoded, normalized path and parsed
        // query parameters. Collapses a leading `//` (which an authority-form
        // parser would otherwise misread) to a single slash — matching the
        // defense the old cpprest path carried.
        void parse_target(std::string target, std::string& path,
                          std::map<std::string, std::string>& query) {
            std::string q;
            if (const auto qp = target.find('?'); qp != std::string::npos) {
                q = target.substr(qp + 1);
                target = target.substr(0, qp);
            }
            if (target.size() >= 2 && target[0] == '/' && target[1] == '/') {
                std::size_t i = 0;
                while (i < target.size() && target[i] == '/') ++i;
                target = "/" + target.substr(i);
            }
            if (target.empty()) target = "/";
            path = percent_decode(target);
            parse_query(q, query);
        }

    } // namespace

    // ------------------------------------------------------------------------
    // Impl / HttpSession
    //
    // Threading model: the io_context is run by exactly ONE thread (io_thread),
    // so the acceptor, every session's stream and the session registry are
    // only touched from that thread and need no locking. Handlers run on the
    // worker pool and hand their response back by posting to the I/O thread.
    // ------------------------------------------------------------------------

    class HttpSession;

    struct HttpServer::Impl {
        net::io_context ioc;
        net::executor_work_guard<net::io_context::executor_type> work;
        tcp::acceptor acceptor;
        net::thread_pool workers;
        std::thread io_thread;

        const HttpServer::Handler& handler;
        const std::shared_ptr<Utils::ILogger>& logger;
        const HttpServer::Options& options;

        // I/O-thread only.
        bool stopping = false;
        std::unordered_map<HttpSession*, std::weak_ptr<HttpSession>> sessions;

        Impl(const HttpServer::Handler& h, const std::shared_ptr<Utils::ILogger>& l,
             const HttpServer::Options& o)
            : work(net::make_work_guard(ioc))
            , acceptor(ioc)
            , workers(o.threads ? o.threads : 1)
            , handler(h)
            , logger(l)
            , options(o) {}

        void accept();
    };

    class HttpSession : public std::enable_shared_from_this<HttpSession> {
    public:
        HttpSession(tcp::socket socket, HttpServer::Impl& impl)
            : stream_(std::move(socket)), impl_(impl) {}

        void start() {
            impl_.sessions.emplace(this, weak_from_this());
            parser_.body_limit(impl_.options.body_limit);
            stream_.expires_after(impl_.options.read_timeout);
            http::async_read(stream_, buffer_, parser_,
                [self = shared_from_this()](beast::error_code ec, std::size_t) {
                    self->on_read(ec);
                });
        }

        /// Called on stop: a connection still waiting for its request is closed.
        void abort_if_reading() {
            if (!dispatched_) {
                close();
            }
        }

    private:
        void on_read(beast::error_code ec) {
            if (ec || impl_.stopping) {
                close();
                return;
            }
            dispatched_ = true;

            auto req = parser_.release();
            HttpRequest hr;
            hr.method = std::string(req.method_string());
            parse_target(std::string(req.target()), hr.path, hr.query);
            for (const auto& field : req) {
                hr.headers.emplace_back(std::string(field.name_string()),
                                        std::string(field.value()));
            }
            hr.body = std::move(req.body());
            const unsigned version = req.version();

            // Run the (blocking) handler off the I/O thread.
            net::post(impl_.workers,
                [self = shared_from_this(), hr = std::move(hr), version]() {
                    HttpResponse hres = self->invoke(hr);
                    net::post(self->stream_.get_executor(),
                        [self, hres = std::move(hres), version]() mutable {
                            self->write(std::move(hres), version);
                        });
                });
        }

        HttpResponse invoke(const HttpRequest& hr) const {
            try {
                return impl_.handler(hr);
            } catch (const std::exception& ex) {
                if (impl_.logger) {
                    impl_.logger->error(
                        std::string("Unhandled error in request handler: ") + ex.what());
                }
            } catch (...) {
                if (impl_.logger) {
                    impl_.logger->error("Unhandled non-standard exception in request handler");
                }
            }
            return HttpResponse(status::InternalError, "application/json",
                                "{\"error\":\"Internal server error\"}");
        }

        void write(HttpResponse hres, unsigned version) {
            response_.result(static_cast<http::status>(hres.status));
            response_.version(version);
            response_.set(http::field::server, "Orcha");
            response_.set(http::field::content_type, hres.content_type);
            response_.keep_alive(false);
            response_.body() = std::move(hres.body);
            response_.prepare_payload();

            stream_.expires_after(impl_.options.write_timeout);
            http::async_write(stream_, response_,
                [self = shared_from_this()](beast::error_code, std::size_t) {
                    self->close();
                });
        }

        void close() {
            if (closed_) return;
            closed_ = true;
            beast::error_code ec;
            stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
            stream_.close();
            impl_.sessions.erase(this);
        }

        beast::tcp_stream stream_;
        HttpServer::Impl& impl_;
        beast::flat_buffer buffer_;
        http::request_parser<http::string_body> parser_;
        http::response<http::string_body> response_;
        bool dispatched_ = false;   // handed to a worker; do not abort on stop
        bool closed_ = false;
    };

    void HttpServer::Impl::accept() {
        acceptor.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (ec) {
                    if (stopping || !acceptor.is_open()) {
                        return;           // acceptor closed by stop()
                    }
                    if (logger) {
                        logger->warn("HTTP accept failed: " + ec.message());
                    }
                } else {
                    std::make_shared<HttpSession>(std::move(socket), *this)->start();
                }
                accept();
            });
    }

    // ------------------------------------------------------------------------
    // HttpServer
    // ------------------------------------------------------------------------

    HttpServer::HttpServer(Handler handler,
                           std::shared_ptr<Utils::ILogger> logger,
                           unsigned int threads)
        : HttpServer(std::move(handler), std::move(logger),
                     Options{.threads = threads ? threads : 1}) {}

    HttpServer::HttpServer(Handler handler,
                           std::shared_ptr<Utils::ILogger> logger,
                           Options options)
        : handler_(std::move(handler))
        , logger_(std::move(logger))
        , options_(options) {
        if (options_.threads == 0) {
            options_.threads = 1;
        }
    }

    HttpServer::~HttpServer() {
        stop();
    }

    void HttpServer::start(const std::string& host, unsigned short port) {
        if (running_.exchange(true)) {
            return; // already started
        }
        impl_ = std::make_unique<Impl>(handler_, logger_, options_);

        beast::error_code ec;
        std::string bind_host = host == "localhost" ? "127.0.0.1" : host;
        if (bind_host.size() > 2 && bind_host.front() == '[' && bind_host.back() == ']') {
            bind_host = bind_host.substr(1, bind_host.size() - 2); // "[::1]" -> "::1"
        }
        const auto address = net::ip::make_address(bind_host, ec);
        if (ec) {
            running_ = false;
            impl_.reset();
            throw std::runtime_error("Invalid server.host '" + host + "': " + ec.message());
        }
        const tcp::endpoint endpoint(address, port);
        impl_->acceptor.open(endpoint.protocol(), ec);
        if (!ec) impl_->acceptor.set_option(net::socket_base::reuse_address(true), ec);
        if (!ec) impl_->acceptor.bind(endpoint, ec);
        if (!ec) impl_->acceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            running_ = false;
            impl_.reset();
            throw std::runtime_error("Failed to bind HTTP server on " + host + ":" +
                                     std::to_string(port) + ": " + ec.message());
        }
        port_ = impl_->acceptor.local_endpoint().port();

        impl_->accept();
        impl_->io_thread = std::thread([impl = impl_.get()] { impl->ioc.run(); });
    }

    void HttpServer::stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (!impl_) {
            return;
        }

        // 1. On the I/O thread: stop accepting and drop connections that are
        //    still waiting for their request (idle / slow clients).
        net::post(impl_->ioc, [impl = impl_.get()] {
            impl->stopping = true;
            beast::error_code ec;
            impl->acceptor.close(ec);
            // Copy: closing a session erases it from the registry.
            auto sessions = impl->sessions;
            for (auto& [ptr, weak] : sessions) {
                if (auto session = weak.lock()) {
                    session->abort_if_reading();
                }
            }
        });

        // 2. Wait for in-flight handlers. Their responses are posted to the
        //    I/O thread, which keeps running meanwhile.
        impl_->workers.join();

        // 3. Let pending writes finish, bounded by the grace period.
        //    ioc.run() returns (stopped() becomes true) once no work remains.
        impl_->work.reset();
        const auto give_up_at = std::chrono::steady_clock::now() + options_.shutdown_grace;
        while (!impl_->ioc.stopped() && std::chrono::steady_clock::now() < give_up_at) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        impl_->ioc.stop();
        if (impl_->io_thread.joinable()) {
            impl_->io_thread.join();
        }
        impl_.reset();
        port_ = 0;
    }

} // namespace Orcha::Agent
