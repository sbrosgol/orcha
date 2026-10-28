//
// YamlJson.hpp - Lossless conversion between YAML text and JSON values.
//
// Workflow and job definitions are stored and executed as JSON; YAML is an
// authoring format on top of it (CLI files, the admin job editor, the jobs
// API). Both directions share one scalar resolver, so a value converted
// JSON -> YAML -> JSON comes back identical:
//
//  * Plain scalars follow the YAML 1.2 core schema: null/~, true/false (and
//    the True/TRUE spellings the spec lists), decimal/0x/0o integers and
//    decimal floats. `.inf`/`.nan` have no JSON form and stay strings, as do
//    YAML 1.1 words such as `yes`/`on`.
//  * Quoted and block scalars are always strings ('5' is "5", not 5).
//  * Merge keys (`<<: *defaults`, or a list of them) are expanded; keys the
//    mapping sets itself win. Duplicate keys are an error.
//  * The emitter quotes any string the resolver would read as another type
//    and writes multi-line strings as literal blocks. Every emitted document
//    is parsed back and compared; if it differs, the emitter falls back to
//    double-quoted strings, which are JSON string literals and always safe.
//
// Comments, anchors and formatting are not preserved: input YAML becomes JSON.
// This header does not include yaml-cpp: the conversion is compiled once in
// YamlJson.cpp (target yaml_json), for Json and nlohmann::ordered_json.
//

#pragma once

#include "../core/Json.hpp"

#include <concepts>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace Orcha::Utils {

    /// Bounds for converting untrusted YAML. Aliases can expand a small
    /// document into a huge tree, so nodes are counted after expansion.
    struct YamlLimits {
        std::size_t max_bytes = 512 * 1024; ///< Input text (yaml-cpp needs ~100x this in memory).
        std::size_t max_depth = 64;         ///< Container nesting (as kMaxRequestJsonDepth).
        std::size_t max_nodes = 100'000;    ///< Values in the converted tree.
    };

    /// A YAML syntax or conversion error. line()/column() are 1-based; 0 if unknown.
    class YamlError : public std::runtime_error {
    public:
        YamlError(const std::string& message, int line, int column)
            : std::runtime_error(line > 0 ? "line " + std::to_string(line) + ", column " +
                                                std::to_string(column) + ": " + message
                                          : message)
            , message_(message), line_(line), column_(column) {}

        [[nodiscard]] const std::string& message() const noexcept { return message_; }
        [[nodiscard]] int line() const noexcept { return line_; }
        [[nodiscard]] int column() const noexcept { return column_; }

    private:
        std::string message_;
        int line_;
        int column_;
    };

    /// The JSON types the conversion is compiled for.
    template <class J>
    concept YamlJsonValue = std::same_as<J, Json> || std::same_as<J, nlohmann::ordered_json>;

    /**
     * @brief Parse YAML text holding a single document. Empty text is null.
     * @tparam J Json, or nlohmann::ordered_json to keep the document's key order.
     * @throws YamlError with the position of the first problem, or when
     *         @p limits are exceeded.
     */
    template <YamlJsonValue J = Json>
    [[nodiscard]] J parse_yaml(const std::string& text, const YamlLimits& limits = {});

    /**
     * @brief Write @p value as block-style YAML that parse_yaml() reads back
     *        as an equal value (key order included for ordered_json).
     * @throws std::runtime_error if no lossless form exists (not expected for
     *         values that came from JSON).
     */
    template <YamlJsonValue J = Json>
    [[nodiscard]] std::string json_to_yaml(const J& value);

    extern template Json parse_yaml<Json>(const std::string&, const YamlLimits&);
    extern template nlohmann::ordered_json parse_yaml<nlohmann::ordered_json>(const std::string&, const YamlLimits&);
    extern template std::string json_to_yaml<Json>(const Json&);
    extern template std::string json_to_yaml<nlohmann::ordered_json>(const nlohmann::ordered_json&);

} // namespace Orcha::Utils
