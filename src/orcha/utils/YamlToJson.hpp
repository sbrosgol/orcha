#pragma once

#include <yaml-cpp/yaml.h>
#include "../core/Json.hpp"
#include <string>

namespace Orcha::Utils {

    // Recursively convert a YAML::Node to Orcha::Json
    inline Json yaml_to_json(const YAML::Node& node) {
        if (node.IsScalar()) {
            const auto& scalar = node.Scalar();
            // Try to parse int, then double, then bool, else fallback to string.
            // Use size-checked conversions so trailing garbage (e.g. "12ab")
            // falls through to the string branch instead of silently truncating.
            try {
                size_t pos = 0;
                long long v = std::stoll(scalar, &pos);
                if (pos == scalar.size()) return Json(v);
            } catch (...) {}
            try {
                size_t pos = 0;
                double v = std::stod(scalar, &pos);
                if (pos == scalar.size()) return Json(v);
            } catch (...) {}
            if (scalar == "true") return Json(true);
            if (scalar == "false") return Json(false);
            return Json(scalar);
        }

        if (node.IsSequence()) {
            Json arr = Json::array();
            for (const auto& item : node) {
                arr.push_back(yaml_to_json(item));
            }
            return arr;
        }

        if (node.IsMap()) {
            Json obj = Json::object();
            for (const auto& kv : node) {
                obj[kv.first.as<std::string>()] = yaml_to_json(kv.second);
            }
            return obj;
        }

        return Json(nullptr);
    }

} // namespace Orcha::Utils
