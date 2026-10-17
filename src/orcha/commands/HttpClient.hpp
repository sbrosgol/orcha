//
// HttpClient.hpp - Minimal synchronous HTTP/HTTPS client built on Boost.Beast.
//
// Replaces cpprestsdk's web::http::client::http_client for the command plugins
// (HttpRequest + the downloaders). It performs a single blocking request,
// follows redirects (GitHub release assets 302 to a CDN), terminates TLS with
// OpenSSL, and returns the full response body. Header-only so each independently
// built plugin can include it without a shared link target.
//

#pragma once

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/url.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cctype>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Orcha::Http {

    namespace beast = boost::beast;
    namespace http  = boost::beast::http;
    namespace net   = boost::asio;
    namespace ssl   = boost::asio::ssl;
    namespace urls  = boost::urls;
    using tcp = boost::asio::ip::tcp;

    using HeaderList = std::vector<std::pair<std::string, std::string>>;

    struct Request {
        std::string method = "GET";
        std::string url;
        HeaderList headers;
        std::string body;
        std::chrono::milliseconds timeout{std::chrono::seconds(60)};
        int max_redirects = 10;
    };

    struct Response {
        int status = 0;
        std::string body;
        HeaderList headers;
    };

    namespace detail {

        /// Build a Beast request object from method/target/host and our extras.
        inline http::request<http::string_body> make_request(
            const Request& req, const urls::url_view& u) {
            const http::verb verb = http::string_to_verb(req.method);
            std::string target = u.encoded_path().empty()
                                     ? std::string("/")
                                     : std::string(u.encoded_path());
            if (u.has_query()) {
                target += "?";
                target += std::string(u.encoded_query());
            }

            http::request<http::string_body> hreq{
                verb == http::verb::unknown ? http::verb::get : verb, target, 11};
            hreq.set(http::field::host, std::string(u.host()));
            hreq.set(http::field::user_agent, "Orcha/" BOOST_BEAST_VERSION_STRING);
            hreq.set(http::field::accept, "*/*");
            hreq.set(http::field::connection, "close");

            bool has_user_agent = false;
            for (const auto& [k, v] : req.headers) {
                hreq.set(k, v);
                if (k == "User-Agent" || k == "user-agent") has_user_agent = true;
            }
            (void)has_user_agent;

            if (!req.body.empty()) {
                hreq.body() = req.body;
                hreq.prepare_payload();
            }
            return hreq;
        }

        /// Copy a parsed Beast response into our transport-agnostic Response.
        template <typename Body>
        inline Response to_response(http::response<Body>&& res) {
            Response out;
            out.status = static_cast<int>(res.result_int());
            out.body = std::move(res.body());
            for (const auto& field : res.base()) {
                out.headers.emplace_back(std::string(field.name_string()),
                                         std::string(field.value()));
            }
            return out;
        }

        /// Perform a single request to @p u (no redirect handling) and return
        /// the response. Throws on transport/TLS errors.
        inline Response perform_once(const Request& req, const urls::url_view& u) {
            const bool is_https = (u.scheme_id() == urls::scheme::https);
            const std::string host(u.host());
            std::string port = std::string(u.port());
            if (port.empty()) port = is_https ? "443" : "80";

            net::io_context ioc;
            tcp::resolver resolver(ioc);
            const auto results = resolver.resolve(host, port);

            // Read the (potentially large) body without Beast's 1 MiB default cap.
            http::response_parser<http::string_body> parser;
            parser.body_limit(boost::none);

            beast::flat_buffer buffer;
            auto hreq = make_request(req, u);

            if (is_https) {
                ssl::context ctx(ssl::context::tls_client);
                ctx.set_default_verify_paths();
                ctx.set_verify_mode(ssl::verify_peer);

                beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
                // SNI — many hosts (incl. CDNs) require it or reset the handshake.
                if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
                    throw beast::system_error(beast::error_code(
                        static_cast<int>(::ERR_get_error()),
                        net::error::get_ssl_category()));
                }
                stream.set_verify_callback(ssl::host_name_verification(host));

                beast::get_lowest_layer(stream).expires_after(req.timeout);
                beast::get_lowest_layer(stream).connect(results);
                stream.handshake(ssl::stream_base::client);

                beast::get_lowest_layer(stream).expires_after(req.timeout);
                http::write(stream, hreq);
                http::read(stream, buffer, parser);

                beast::error_code ec;
                stream.shutdown(ec); // best-effort; truncated/EOF is normal
                return to_response(parser.release());
            }

            beast::tcp_stream stream(ioc);
            stream.expires_after(req.timeout);
            stream.connect(results);
            http::write(stream, hreq);
            http::read(stream, buffer, parser);

            beast::error_code ec;
            stream.socket().shutdown(tcp::socket::shutdown_both, ec);
            return to_response(parser.release());
        }

        inline bool is_redirect(int status) {
            return status == 301 || status == 302 || status == 303 ||
                   status == 307 || status == 308;
        }

        inline std::string find_header(const HeaderList& headers,
                                       const std::string& name) {
            for (const auto& [k, v] : headers) {
                if (k.size() == name.size() &&
                    std::equal(k.begin(), k.end(), name.begin(),
                               [](char a, char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) ==
                                          std::tolower(static_cast<unsigned char>(b));
                               })) {
                    return v;
                }
            }
            return {};
        }

    } // namespace detail

    /**
     * @brief Execute @p req, following redirects up to @p req.max_redirects.
     * @throws std::runtime_error on malformed URLs and Boost.Beast exceptions on
     *         transport/TLS failures.
     */
    inline Response perform(const Request& req) {
        auto parsed = urls::parse_uri(req.url);
        if (!parsed) {
            throw std::runtime_error("Invalid URL: " + req.url);
        }
        urls::url current(*parsed);

        Request active = req;
        for (int hop = 0; ; ++hop) {
            Response res = detail::perform_once(active, current);

            if (!detail::is_redirect(res.status) || hop >= req.max_redirects) {
                return res;
            }
            const std::string location = detail::find_header(res.headers, "location");
            if (location.empty()) {
                return res;
            }

            // Resolve the (possibly relative) Location against the current URL.
            auto ref = urls::parse_uri_reference(location);
            if (!ref) {
                return res;
            }
            urls::url next;
            if (ref->has_scheme()) {
                next = urls::url(*ref);
            } else {
                urls::resolve(current, *ref, next);
            }
            current = next;

            // 303 (and, by common practice, 301/302 for non-GET) downgrade to GET.
            if (res.status == 303) {
                active.method = "GET";
                active.body.clear();
            }
        }
    }

    /**
     * @brief Convenience GET returning the response (follows redirects).
     */
    inline Response get(const std::string& url,
                        std::chrono::milliseconds timeout =
                            std::chrono::seconds(60)) {
        Request req;
        req.url = url;
        req.timeout = timeout;
        return perform(req);
    }

} // namespace Orcha::Http
