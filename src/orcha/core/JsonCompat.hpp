//
// JsonCompat.hpp - Small helpers for JSON idioms that don't map 1:1 from the
// previous cpprestsdk API. Most cpprest patterns translate directly to
// nlohmann/json (see core/Json.hpp); these cover the few that benefit from a
// shared helper.
//

#pragma once

#include "Json.hpp"
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace Orcha {

    /**
     * @brief Parse @p s as JSON, returning @p fallback on empty input or any
     *        parse error. Replaces the old `web::json::value::parse` + try/catch
     *        used around persisted/untrusted strings.
     */
    inline Json parse_json_or(const std::string& s, Json fallback = Json(nullptr)) {
        if (s.empty()) {
            return fallback;
        }
        try {
            return Json::parse(s);
        } catch (...) {
            return fallback;
        }
    }

    /// Maximum nesting accepted from untrusted JSON (HTTP request bodies).
    inline constexpr std::size_t kMaxRequestJsonDepth = 64;

    /**
     * @brief Thrown by parse_json_bounded when input nests too deeply.
     */
    class JsonDepthError : public std::runtime_error {
    public:
        explicit JsonDepthError(std::size_t limit)
            : std::runtime_error("JSON nesting exceeds the maximum depth of " +
                                 std::to_string(limit)) {}
    };

    /**
     * @brief Parse untrusted JSON, rejecting documents nested deeper than
     *        @p max_depth.
     *
     * Parsing itself is iterative, but copying, serializing and resolving a
     * Json value are recursive; an unbounded document (e.g. 5000 nested
     * arrays) overflows a worker thread's stack. The limit is enforced while
     * parsing, so an oversized document is never materialised.
     *
     * @p max_object_keys bounds each object's member count. Set it when
     * parsing into nlohmann::ordered_json, whose member lookup is linear (an
     * object with n keys costs O(n^2) to build).
     *
     * @tparam J Json, or nlohmann::ordered_json to keep the document's key order.
     * @throws JsonDepthError if a limit is exceeded.
     * @throws Json::parse_error on malformed input.
     */
    template <class J = Json>
    [[nodiscard]] J parse_json_bounded(const std::string& s,
                                       std::size_t max_depth = kMaxRequestJsonDepth,
                                       std::size_t max_object_keys = std::numeric_limits<std::size_t>::max()) {
        std::vector<std::size_t> keys;   // members seen in each open object
        // The callback's depth is 0-based (a top-level array is at depth 0),
        // so the nesting level is depth + 1.
        return J::parse(s, [&](int depth, typename J::parse_event_t event, J&) {
            if (static_cast<std::size_t>(depth) + 1 > max_depth) {
                throw JsonDepthError(max_depth);
            }
            using E = typename J::parse_event_t;
            if (event == E::object_start) keys.push_back(0);
            else if (event == E::object_end && !keys.empty()) keys.pop_back();
            else if (event == E::key && !keys.empty() && ++keys.back() > max_object_keys) {
                throw std::runtime_error("JSON object has more than " +
                                         std::to_string(max_object_keys) + " keys");
            }
            return true;
        });
    }

} // namespace Orcha
