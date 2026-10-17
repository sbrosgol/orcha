//
// Json.hpp - Project-wide JSON type alias.
//
// Orcha standardizes on nlohmann/json. Code refers to the JSON type through the
// `Orcha::Json` alias rather than the concrete library so the public ICommand /
// workflow / jobs interfaces stay decoupled from the underlying implementation.
//

#pragma once

#include <nlohmann/json.hpp>

namespace Orcha {
    using Json = nlohmann::json;
}
