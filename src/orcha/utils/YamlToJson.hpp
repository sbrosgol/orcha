#pragma once

#include <yaml-cpp/yaml.h>
#include <cpprest/json.h>
#include <string>

namespace Orcha::Utils {

    // Recursively convert a YAML::Node to web::json::value
    inline web::json::value yaml_to_json(const YAML::Node& node) {
        using namespace web;

        if (node.IsScalar()) {
            const auto& scalar = node.Scalar();
            // Try to parse int, then double, then bool, else fallback to string
            try { return {std::stoi(scalar)}; } catch(...) {}
            try { return {std::stod(scalar)}; } catch(...) {}
            if (scalar == "true") return json::value(true);
            if (scalar == "false") return json::value(false);
            return json::value::string(scalar);
        }

        if (node.IsSequence()) {
            json::value arr = json::value::array();
            int idx = 0;
            for (const auto& item : node) {
                arr[idx++] = yaml_to_json(item);
            }
            return arr;
        }

        if (node.IsMap()) {
            json::value obj = json::value::object();
            for (const auto& kv : node) {
                obj[utility::conversions::to_string_t(kv.first.as<std::string>())] = yaml_to_json(kv.second);
            }
            return obj;
        }

        return {};
    }

} // namespace Orcha::Utils
