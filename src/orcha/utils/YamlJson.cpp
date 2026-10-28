//
// YamlJson.cpp - YAML <-> JSON conversion (see YamlJson.hpp for the rules).
//
// The only translation unit that sees yaml-cpp for definitions: the header
// declares parse_yaml/json_to_yaml for Json and ordered_json, which are
// instantiated at the end of this file.
//

#include "YamlJson.hpp"

#include <yaml-cpp/yaml.h>
#include <yaml-cpp/depthguard.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Orcha::Utils {

    namespace {

        inline YamlError yaml_error(const std::string& message, const YAML::Mark& mark) {
            if (mark.is_null()) return {message, 0, 0};
            return {message, mark.line + 1, mark.column + 1};
        }

        inline bool all_digits(std::string_view s, int base) {
            if (s.empty()) return false;
            for (const char c : s) {
                const bool ok = base == 16 ? std::isxdigit(static_cast<unsigned char>(c)) != 0
                              : base == 8  ? (c >= '0' && c <= '7')
                                           : (c >= '0' && c <= '9');
                if (!ok) return false;
            }
            return true;
        }

        /// Core-schema float without sign: `.5`, `1`, `1.`, `1.5e-3`.
        inline bool is_decimal_float(std::string_view s) {
            std::size_t i = 0;
            const auto digits = [&] {
                const std::size_t start = i;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
                return i > start;
            };
            if (i < s.size() && s[i] == '.') {
                ++i;
                if (!digits()) return false;
            } else {
                if (!digits()) return false;
                if (i < s.size() && s[i] == '.') { ++i; digits(); }
            }
            if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                if (!digits()) return false;
            }
            return i == s.size();
        }

        /**
         * @brief Resolve a plain (unquoted) scalar by the YAML 1.2 core schema.
         * @return The typed value, or nullopt if it is a string.
         */
        template <class J>
        std::optional<J> resolve_plain_scalar(std::string_view s) {
            if (s.empty() || s == "~" || s == "null" || s == "Null" || s == "NULL") return J(nullptr);
            if (s == "true" || s == "True" || s == "TRUE") return J(true);
            if (s == "false" || s == "False" || s == "FALSE") return J(false);

            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o')) {
                const int base = s[1] == 'x' ? 16 : 8;
                const auto digits = s.substr(2);
                std::uint64_t v = 0;
                if (all_digits(digits, base) &&
                    std::from_chars(digits.data(), digits.data() + digits.size(), v, base).ec == std::errc{}) {
                    return J(v);
                }
                return std::nullopt;   // overflow or not a number: a string
            }

            const bool negative = s.front() == '-';
            const auto unsigned_part = (s.front() == '-' || s.front() == '+') ? s.substr(1) : s;
            if (all_digits(unsigned_part, 10)) {
                if (negative) {
                    std::int64_t v = 0;
                    if (std::from_chars(s.data(), s.data() + s.size(), v).ec == std::errc{}) return J(v);
                } else {
                    std::uint64_t v = 0;
                    const auto* end = unsigned_part.data() + unsigned_part.size();
                    if (std::from_chars(unsigned_part.data(), end, v).ec == std::errc{}) return J(v);
                }
                // Out of 64-bit range: continue as a float.
            }
            if (is_decimal_float(unsigned_part)) {
                // from_chars ignores the C locale (strtod would read "1.5" as 1
                // under a comma-decimal locale) and must consume the token.
                // It rejects a leading '+', which YAML allows.
                const auto text = s.front() == '+' ? s.substr(1) : s;
                double v = 0;
                const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
                if (ec == std::errc{} && end == text.data() + text.size() && std::isfinite(v)) return J(v);
            }
            return std::nullopt;
        }

        template <class J>
        J convert_node(const YAML::Node& node, const YamlLimits& limits,
                       std::size_t depth, std::size_t& nodes);

        template <class J>
        J convert_scalar(const YAML::Node& node) {
            const auto& tag = node.Tag();
            const auto& text = node.Scalar();
            // "?" is a plain scalar, "!" a quoted or block one (always a string).
            if (tag == "?") {
                if (auto v = resolve_plain_scalar<J>(text)) return *v;
                return J(text);
            }
            if (tag == "!" || tag == "tag:yaml.org,2002:str") return J(text);
            // Explicit core tags (!!int 5, !!bool true, ...) must match the value.
            constexpr std::string_view kCore = "tag:yaml.org,2002:";
            if (tag.starts_with(kCore)) {
                const auto type = std::string_view(tag).substr(kCore.size());
                if (auto v = resolve_plain_scalar<J>(text)) {
                    const bool fits = (type == "int" && v->is_number_integer()) ||
                                      (type == "float" && v->is_number()) ||
                                      (type == "bool" && v->is_boolean()) ||
                                      (type == "null" && v->is_null());
                    if (fits) return type == "float" ? J(v->template get<double>()) : *v;
                }
                throw yaml_error("'" + text + "' is not a valid !!" + std::string(type), node.Mark());
            }
            throw yaml_error("Unsupported tag '" + tag + "'", node.Mark());
        }

        // Appends without a lookup: ordered_json's operator[] scans linearly,
        // which makes large mappings quadratic. Callers rule out duplicates.
        template <class J>
        void append_member(J& obj, const std::string& key, J value) {
            auto& members = obj.template get_ref<typename J::object_t&>();
            if constexpr (requires { members.emplace_back(key, std::move(value)); }) {
                members.emplace_back(key, std::move(value));   // ordered_json
            } else {
                members.emplace(key, std::move(value));        // Json (std::map)
            }
        }

        inline bool is_merge_key(const YAML::Node& key) {
            return key.IsScalar() && key.Tag() == "?" && key.Scalar() == "<<";
        }

        template <class J>
        J convert_map(const YAML::Node& node, const YamlLimits& limits,
                      std::size_t depth, std::size_t& nodes) {
            // Keys set by the mapping itself; they win over merged ones.
            std::unordered_set<std::string> own;
            for (const auto& kv : node) {
                if (!kv.first.IsScalar()) {
                    throw yaml_error("Mapping keys must be strings", kv.first.Mark());
                }
                if (is_merge_key(kv.first)) continue;
                if (!own.insert(kv.first.Scalar()).second) {
                    throw yaml_error("Duplicate key '" + kv.first.Scalar() + "'", kv.first.Mark());
                }
            }
            J obj = J::object();
            std::unordered_set<std::string> merged;
            const auto merge = [&](const YAML::Node& source, const YAML::Mark& mark) {
                if (!source.IsMap()) {
                    throw yaml_error("A merge key (<<) needs a mapping or a list of mappings", mark);
                }
                // The source is converted on its own (its keys come back in order)
                // and sits at this mapping's level, not one below it.
                J from = convert_node<J>(source, limits, depth - 1, nodes);
                for (auto& [k, v] : from.items()) {
                    // Earlier sources win over later ones (YAML merge-key spec).
                    if (!own.contains(k) && merged.insert(k).second) append_member(obj, k, std::move(v));
                }
            };
            for (const auto& kv : node) {
                if (is_merge_key(kv.first)) {
                    if (kv.second.IsSequence()) {
                        for (const auto& source : kv.second) merge(source, kv.first.Mark());
                    } else {
                        merge(kv.second, kv.first.Mark());
                    }
                    continue;
                }
                append_member(obj, kv.first.Scalar(),
                              convert_node<J>(kv.second, limits, depth, nodes));
            }
            return obj;
        }

        template <class J>
        J convert_node(const YAML::Node& node, const YamlLimits& limits,
                       std::size_t depth, std::size_t& nodes) {
            if (++nodes > limits.max_nodes) {
                throw yaml_error("Document is too large (more than " +
                                 std::to_string(limits.max_nodes) +
                                 " values after expanding aliases)", node.Mark());
            }
            switch (node.Type()) {
                case YAML::NodeType::Scalar:
                    return convert_scalar<J>(node);
                case YAML::NodeType::Sequence:
                case YAML::NodeType::Map: {
                    if (depth + 1 > limits.max_depth) {
                        throw yaml_error("Nesting exceeds the maximum depth of " +
                                         std::to_string(limits.max_depth), node.Mark());
                    }
                    if (node.IsSequence()) {
                        J arr = J::array();
                        for (const auto& item : node) {
                            arr.push_back(convert_node<J>(item, limits, depth + 1, nodes));
                        }
                        return arr;
                    }
                    return convert_map<J>(node, limits, depth + 1, nodes);
                }
                case YAML::NodeType::Null:
                case YAML::NodeType::Undefined:
                    return J(nullptr);
            }
            return J(nullptr);
        }

        /// Strings YAML 1.1 tools (PyYAML, Ansible) would read as a boolean or a
        /// date. Quoted on output so copied YAML means the same there.
        inline bool looks_typed_in_yaml_1_1(std::string_view s) {
            static constexpr std::array<std::string_view, 18> kWords{
                "y", "Y", "yes", "Yes", "YES", "n", "N", "no", "No", "NO",
                "on", "On", "ON", "off", "Off", "OFF", "=", "<<"};
            if (std::ranges::find(kWords, s) != kWords.end()) return true;
            const auto digit = [&](std::size_t i) { return i < s.size() && s[i] >= '0' && s[i] <= '9'; };
            return digit(0) && digit(1) && digit(2) && digit(3) && s.size() > 4 && s[4] == '-';   // 2024-01-01...
        }

        /// A string that may be written unquoted and still read back as itself.
        inline bool is_plain_safe(std::string_view s) {
            if (s.empty() || looks_typed_in_yaml_1_1(s) || resolve_plain_scalar<Json>(s)) return false;
            if (std::string_view("-?:,[]{}#&*!|>'\"%@`").find(s.front()) != std::string_view::npos) return false;
            if (s.front() == ' ' || s.back() == ' ' || s.back() == ':') return false;
            if (s.find(": ") != std::string_view::npos || s.find(" #") != std::string_view::npos) return false;
            for (const unsigned char c : s) {
                if (c < 0x20 || c == 0x7f) return false;
            }
            return true;
        }

        /// A multi-line string a literal block (`|`) can hold without an
        /// indentation indicator.
        inline bool is_literal_safe(std::string_view s) {
            if (s.find('\n') == std::string_view::npos) return false;
            if (s.front() == ' ' || s.front() == '\t' || s.front() == '\n') return false;
            for (const unsigned char c : s) {
                if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7f) return false;
            }
            return true;
        }

        /// Block-style writer. Each entry ends with '\n'; nested blocks indent by 2.
        template <class J>
        class YamlWriter {
        public:
            explicit YamlWriter(bool literal_blocks) : literal_blocks_(literal_blocks) {}

            std::string document(const J& value) {
                if (is_block(value)) block(value, 0, false);
                else { out_ += scalar(value, 0); out_ += '\n'; }
                return std::move(out_);
            }

        private:
            static bool is_block(const J& v) {
                return (v.is_object() || v.is_array()) && !v.empty();
            }

            static std::string quoted(const std::string& s) {
                // A JSON string literal is a valid YAML double-quoted scalar.
                return J(s).dump(-1, ' ', false, J::error_handler_t::replace);
            }

            static std::string key(const std::string& k) {
                return is_plain_safe(k) ? k : quoted(k);
            }

            std::string scalar(const J& v, std::size_t indent) const {
                if (v.is_object()) return "{}";
                if (v.is_array()) return "[]";
                if (!v.is_string()) return v.dump();   // null, bool, number
                const auto& s = v.template get_ref<const std::string&>();
                if (is_plain_safe(s)) return s;
                if (literal_blocks_ && is_literal_safe(s)) return literal(s, indent);
                return quoted(s);
            }

            static std::string literal(std::string_view s, std::size_t indent) {
                std::size_t trailing = 0;
                while (trailing < s.size() && s[s.size() - 1 - trailing] == '\n') ++trailing;
                // Chomping: strip (no final newline), clip (one), keep (more).
                std::string out = trailing == 0 ? "|-" : trailing == 1 ? "|" : "|+";
                const auto body = s.substr(0, s.size() - (trailing > 0 ? 1 : 0));
                std::size_t start = 0;
                while (true) {
                    const auto nl = body.find('\n', start);
                    const auto line = body.substr(start, nl == std::string_view::npos ? nl : nl - start);
                    out += '\n';
                    if (!line.empty()) { out.append(indent, ' '); out += line; }
                    if (nl == std::string_view::npos) break;
                    start = nl + 1;
                }
                return out;
            }

            // Writes a non-empty container at column `col`. With `inline_first`
            // the first entry continues the current line (after "- ").
            void block(const J& v, std::size_t col, bool inline_first) {
                bool first = true;
                const auto lead = [&] {
                    if (!(first && inline_first)) out_.append(col, ' ');
                    first = false;
                };
                if (v.is_object()) {
                    for (const auto& [k, x] : v.items()) {
                        lead();
                        out_ += key(k);
                        out_ += ':';
                        child(x, col, true);
                    }
                } else {
                    for (const auto& x : v) {
                        lead();
                        out_ += '-';
                        child(x, col, false);
                    }
                }
            }

            void child(const J& x, std::size_t col, bool in_map) {
                if (!is_block(x)) {
                    out_ += ' ';
                    out_ += scalar(x, col + 2);
                    out_ += '\n';
                } else if (in_map) {
                    out_ += '\n';
                    block(x, col + 2, false);
                } else {
                    out_ += ' ';
                    block(x, col + 2, true);
                }
            }

            bool literal_blocks_;
            std::string out_;
        };

    /**
     * @brief Convert a parsed YAML node to JSON.
     * @tparam J Json, or nlohmann::ordered_json to keep the document's key order.
     * @throws YamlError on duplicate or non-scalar keys, or when @p limits are exceeded.
     */
    template <class J>
    J yaml_to_json(const YAML::Node& node, const YamlLimits& limits) {
        std::size_t nodes = 0;
        return convert_node<J>(node, limits, 0, nodes);
    }

    } // namespace

    template <YamlJsonValue J>
    J parse_yaml(const std::string& text, const YamlLimits& limits) {
        if (text.size() > limits.max_bytes) {
            throw YamlError("Document is too large (" + std::to_string(text.size()) +
                            " bytes; the limit is " + std::to_string(limits.max_bytes) + ")", 0, 0);
        }
        std::vector<YAML::Node> docs;
        try {
            docs = YAML::LoadAll(text);
        } catch (const YAML::DeepRecursion& ex) {
            throw yaml_error("Nesting is too deep", ex.mark);
        } catch (const YAML::Exception& ex) {
            throw yaml_error(ex.msg, ex.mark);
        }
        if (docs.size() > 1) {
            throw yaml_error("Expected one YAML document, found " +
                                     std::to_string(docs.size()) + " (separated by ---)",
                                     docs[1].Mark());
        }
        if (docs.empty()) return J(nullptr);
        return yaml_to_json<J>(docs.front(), limits);
    }

    template <YamlJsonValue J>
    std::string json_to_yaml(const J& value) {
        constexpr auto kMax = std::numeric_limits<std::size_t>::max();
        constexpr YamlLimits unbounded{kMax, kMax, kMax};
        for (const bool literal_blocks : {true, false}) {
            auto text = YamlWriter<J>(literal_blocks).document(value);
            try {
                if (parse_yaml<J>(text, unbounded) == value) return text;
            } catch (const YamlError&) {
                // Try the next, more conservative form.
            }
        }
        throw std::runtime_error("Value cannot be written as YAML without changing it");
    }

    template Json parse_yaml<Json>(const std::string&, const YamlLimits&);
    template nlohmann::ordered_json parse_yaml<nlohmann::ordered_json>(const std::string&, const YamlLimits&);
    template std::string json_to_yaml<Json>(const Json&);
    template std::string json_to_yaml<nlohmann::ordered_json>(const nlohmann::ordered_json&);

} // namespace Orcha::Utils
