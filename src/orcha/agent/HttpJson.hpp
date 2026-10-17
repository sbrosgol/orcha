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
     */
    [[nodiscard]] inline HttpResponse reply_json(int code, const Orcha::Json& body) {
        return HttpResponse(code, "application/json", body.dump());
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
