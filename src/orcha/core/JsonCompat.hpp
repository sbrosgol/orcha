//
// JsonCompat.hpp - Small helpers for JSON idioms that don't map 1:1 from the
// previous cpprestsdk API. Most cpprest patterns translate directly to
// nlohmann/json (see core/Json.hpp); these cover the few that benefit from a
// shared helper.
//

#pragma once

#include "Json.hpp"
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

} // namespace Orcha
