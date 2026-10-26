//
// CircuitBreakerRoute.hpp - Admin API for per-command circuit breakers
//
// Endpoints (under the admin auth gate):
//   GET  /api/circuit-breakers                  state and counters of every breaker
//   POST /api/circuit-breakers/{command}/reset  close a breaker immediately
//
// Breakers exist for commands used by workflows with "circuit_breaker": true.
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../HttpJson.hpp"
#include "../../core/CircuitBreaker.hpp"
#include "../../utils/ILogger.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace Orcha::Agent::Routes {

    class CircuitBreakerRoute : public IRouteHandler {
    public:
        explicit CircuitBreakerRoute(std::shared_ptr<Core::CircuitBreakerRegistry> breakers,
                                     std::shared_ptr<Utils::ILogger> logger = nullptr)
            : breakers_(std::move(breakers))
            , logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method, const std::string& path) const override {
            (void)method;
            return path == kPrefix || path_starts_with(path, kPrefix + "/");
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            const auto seg = split_path(request.path); // {"api","circuit-breakers",...}

            if (seg.size() == 2) {
                if (request.method == "GET") return list();
                return reply_error(status::MethodNotAllowed, "Use GET /api/circuit-breakers");
            }
            if (seg.size() == 4 && seg[3] == "reset") {
                if (request.method == "POST") return reset(seg[2]);
                return reply_error(status::MethodNotAllowed,
                                   "Use POST /api/circuit-breakers/{command}/reset");
            }
            return reply_error(status::NotFound, "Unknown circuit-breakers endpoint");
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "GET",  .path = "/api/circuit-breakers",
                 .description = "List per-command circuit breakers"},
                {.method = "POST", .path = "/api/circuit-breakers/{command}/reset",
                 .description = "Close a circuit breaker"}
            };
        }

    private:
        inline static const std::string kPrefix = "/api/circuit-breakers";

        HttpResponse list() const {
            auto stats = breakers_->all_stats();
            std::ranges::sort(stats, {}, &Core::CircuitStats::name);

            Orcha::Json arr = Orcha::Json::array();
            for (const auto& st : stats) {
                Orcha::Json o = Orcha::Json::object();
                o["command"] = st.name;
                o["state"] = Core::to_string(st.state);
                o["consecutive_failures"] = st.consecutive_failures;
                o["total_calls"] = st.total_calls;
                o["failed_calls"] = st.failed_calls;
                o["rejected_calls"] = st.rejected_calls;
                o["last_failure"] = st.last_failure ? Orcha::Json(iso_utc(*st.last_failure))
                                                    : Orcha::Json(nullptr);
                arr.push_back(std::move(o));
            }
            Orcha::Json out = Orcha::Json::object();
            out["circuit_breakers"] = arr;
            out["count"] = arr.size();
            return reply_json(status::OK, out);
        }

        HttpResponse reset(const std::string& command) const {
            if (!breakers_->reset(command)) {
                return reply_error(status::NotFound, "No circuit breaker for command: " + command);
            }
            if (logger_) logger_->info("Circuit breaker for '" + command + "' reset via admin API");
            Orcha::Json out = Orcha::Json::object();
            out["command"] = command;
            out["state"] = "closed";
            return reply_json(status::OK, out);
        }

        static std::string iso_utc(std::chrono::system_clock::time_point tp) {
            const auto t = std::chrono::system_clock::to_time_t(tp);
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            std::ostringstream os;
            os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            return os.str();
        }

        std::shared_ptr<Core::CircuitBreakerRegistry> breakers_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
