//
// HttpJson.hpp - Helpers for building JSON HTTP responses.
//
// All JSON flows as Orcha::Json (nlohmann). These helpers serialize it via
// `.dump()` and tag the response application/json, so route handlers can return
// a ready-made HttpResponse without repeating the boilerplate.
//

#pragma once

#include "Http.hpp"
#include "../core/Json.hpp"

#include <string>

namespace Orcha::Agent {

    /**
     * @brief Build an application/json response with @p body and status @p code.
     *
     * @p body is an Orcha::Json, or an nlohmann::ordered_json when the reply
     * must keep the key order the client sent (definition conversion).
     */
    template <class J = Orcha::Json>
    [[nodiscard]] HttpResponse reply_json(int code, const J& body) {
        // Step outputs may contain invalid UTF-8: replace it rather than throw.
        return HttpResponse(code, "application/json",
                            body.dump(-1, ' ', false, J::error_handler_t::replace));
    }

    /**
     * @brief Build a `{ "error": <message> }` response with the given status.
     */
    [[nodiscard]] inline HttpResponse reply_error(int code, const std::string& message) {
        Orcha::Json body = Orcha::Json::object();
        body["error"] = message;
        return reply_json(code, body);
    }

} // namespace Orcha::Agent
