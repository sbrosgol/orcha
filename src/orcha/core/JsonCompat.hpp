//
// JsonCompat.hpp - Small helpers for JSON idioms that don't map 1:1 from the
// previous cpprestsdk API. Most cpprest patterns translate directly to
// nlohmann/json (see core/Json.hpp); these cover the few that benefit from a
// shared helper.
//

#pragma once

#include "Json.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>

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
     * @throws JsonDepthError if the limit is exceeded.
     * @throws Json::parse_error on malformed input.
     */
    [[nodiscard]] inline Json parse_json_bounded(const std::string& s,
                                                 std::size_t max_depth = kMaxRequestJsonDepth) {
        // The callback's depth is 0-based (a top-level array is at depth 0),
        // so the nesting level is depth + 1.
        return Json::parse(s, [max_depth](int depth, Json::parse_event_t, Json&) {
            if (static_cast<std::size_t>(depth) + 1 > max_depth) {
                throw JsonDepthError(max_depth);
            }
            return true;
        });
    }

} // namespace Orcha
