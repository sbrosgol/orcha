//
// HttpServer.cpp - Boost.Beast implementation of the agent HTTP server.
//

#include "HttpServer.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>

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

        // Read one request off @p socket, dispatch through @p handler, and write
        // the response. Connections are closed after a single exchange.
        void serve_connection(tcp::socket socket,
                              const HttpServer::Handler& handler,
                              const std::shared_ptr<Utils::ILogger>& logger) {
            beast::tcp_stream stream(std::move(socket));
            beast::flat_buffer buffer;
            http::request_parser<http::string_body> parser;
            parser.body_limit(static_cast<std::uint64_t>(64) * 1024 * 1024);

            beast::error_code ec;
            stream.expires_after(std::chrono::seconds(30));
            http::read(stream, buffer, parser, ec);
            if (ec) {
                stream.socket().shutdown(tcp::socket::shutdown_both, ec);
                return;
            }

            auto req = parser.release();

            HttpRequest hr;
            hr.method = std::string(req.method_string());
            parse_target(std::string(req.target()), hr.path, hr.query);
            for (const auto& field : req) {
                hr.headers.emplace_back(std::string(field.name_string()),
                                        std::string(field.value()));
            }
            hr.body = std::move(req.body());

            HttpResponse hres;
            try {
                hres = handler(hr);
            } catch (const std::exception& ex) {
                if (logger) {
                    logger->error(
                        std::string("Unhandled error in request handler: ") + ex.what());
                }
                hres = HttpResponse(status::InternalError, "application/json",
                                    "{\"error\":\"Internal server error\"}");
            }

            http::response<http::string_body> res(
                static_cast<http::status>(hres.status), req.version());
            res.set(http::field::server, "Orcha");
            res.set(http::field::content_type, hres.content_type);
            res.keep_alive(false);
            res.body() = std::move(hres.body);
            res.prepare_payload();

            stream.expires_after(std::chrono::seconds(30));
            http::write(stream, res, ec);
            stream.socket().shutdown(tcp::socket::shutdown_send, ec);
        }

    } // namespace

    struct HttpServer::Impl {
        net::io_context ioc;
        tcp::acceptor acceptor;
        net::thread_pool pool;
        std::thread accept_thread;

        explicit Impl(unsigned int threads)
            : acceptor(ioc), pool(threads ? threads : 1) {}
    };

    HttpServer::HttpServer(Handler handler,
                           std::shared_ptr<Utils::ILogger> logger,
                           unsigned int threads)
        : handler_(std::move(handler))
        , logger_(std::move(logger))
        , threads_(threads ? threads : 1) {}

    HttpServer::~HttpServer() {
        stop();
    }

    void HttpServer::start(unsigned short port) {
        if (running_.exchange(true)) {
            return; // already started
        }
        impl_ = std::make_unique<Impl>(threads_);

        beast::error_code ec;
        const tcp::endpoint endpoint(net::ip::make_address("0.0.0.0"), port);
        impl_->acceptor.open(endpoint.protocol(), ec);
        if (!ec) impl_->acceptor.set_option(net::socket_base::reuse_address(true), ec);
        if (!ec) impl_->acceptor.bind(endpoint, ec);
        if (!ec) impl_->acceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            running_ = false;
            impl_.reset();
            throw std::runtime_error("Failed to bind HTTP server on port " +
                                     std::to_string(port) + ": " + ec.message());
        }

        impl_->accept_thread = std::thread([this]() {
            while (running_.load()) {
                tcp::socket socket(impl_->ioc);
                beast::error_code accept_ec;
                impl_->acceptor.accept(socket, accept_ec);
                if (accept_ec) {
                    if (!running_.load()) break;
                    continue; // transient accept error
                }
                net::post(impl_->pool,
                          [handler = handler_, logger = logger_,
                           s = std::move(socket)]() mutable {
                              serve_connection(std::move(s), handler, logger);
                          });
            }
        });
    }

    void HttpServer::stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (impl_) {
            beast::error_code ec;
            impl_->acceptor.close(ec);
            if (impl_->accept_thread.joinable()) {
                impl_->accept_thread.join();
            }
            impl_->pool.join(); // drain in-flight connections
            impl_.reset();
        }
    }

} // namespace Orcha::Agent
