//
// YamlTests.hpp - YAML <-> JSON conversion (utils/YamlJson.hpp), YAML request
// bodies (agent/HttpBody.hpp), the definition conversion endpoint and YAML job
// bodies on the jobs API.
//

#pragma once

#include "../agent/HttpBody.hpp"
#include "../agent/routes/DefinitionRoute.hpp"
#include "../agent/routes/JobRoute.hpp"
#include "../jobs/RunQueue.hpp"
#include "../jobs/SqliteJobStore.hpp"
#include "../utils/YamlJson.hpp"

#include "Assertions.hpp"
#include <chrono>
#include <clocale>
#include <iostream>
#include <limits>
#include <string>

namespace Orcha::Tests {

    namespace yaml_tests {
        using OJson = nlohmann::ordered_json;

        inline Agent::HttpRequest request(const std::string& method, const std::string& path,
                                          const std::string& content_type, std::string body) {
            Agent::HttpRequest r;
            r.method = method;
            r.path = path;
            r.body = std::move(body);
            if (!content_type.empty()) r.headers.emplace_back("Content-Type", content_type);
            return r;
        }

        /// Runs @p fn, which must throw a YamlError; returns it.
        template <class Fn>
        Utils::YamlError expect_yaml_error(Fn&& fn) {
            try {
                fn();
            } catch (const Utils::YamlError& ex) {
                return ex;
            }
            throw std::runtime_error("expected a YamlError");
        }
    } // namespace yaml_tests

    // ========== Scalar resolution ==========

    inline void test_yaml_scalars_follow_core_schema() {
        using Utils::parse_yaml;
        const auto doc = parse_yaml(
            "int: 42\nneg: -7\nplus: +3\nhex: 0x1F\noct: 0o17\nfloat: 1.5\nexp: 1e3\ndot: .5\n"
            "t: true\nT: True\nf: FALSE\nn: null\ntilde: ~\nempty:\n"
            "yes: yes\non: on\ninf: .inf\nnan: .nan\nword: 12ab\nbig: 99999999999999999999\n"
            "q1: '5'\nq2: \"true\"\nq3: \"null\"\nstr: !!str 7\nblock: |\n  5\n");
        ORCHA_ASSERT(doc["int"] == 42 && doc["int"].is_number_integer());
        ORCHA_ASSERT(doc["neg"] == -7);
        ORCHA_ASSERT(doc["plus"] == 3);
        ORCHA_ASSERT(doc["hex"] == 31);
        ORCHA_ASSERT(doc["oct"] == 15);
        ORCHA_ASSERT(doc["float"] == 1.5);
        ORCHA_ASSERT(doc["exp"] == 1000.0 && doc["exp"].is_number_float());
        ORCHA_ASSERT(doc["dot"] == 0.5);
        ORCHA_ASSERT(doc["t"] == true && doc["T"] == true && doc["f"] == false);
        ORCHA_ASSERT(doc["n"].is_null() && doc["tilde"].is_null() && doc["empty"].is_null());
        // YAML 1.1 booleans, inf/nan (no JSON form) and near-numbers stay strings.
        ORCHA_ASSERT(doc["yes"] == "yes" && doc["on"] == "on");
        ORCHA_ASSERT(doc["inf"] == ".inf" && doc["nan"] == ".nan");
        ORCHA_ASSERT(doc["word"] == "12ab");
        ORCHA_ASSERT(doc["big"].is_number_float());
        // Quoted, explicitly tagged and block scalars are strings.
        ORCHA_ASSERT(doc["q1"] == "5" && doc["q2"] == "true" && doc["q3"] == "null");
        ORCHA_ASSERT(doc["str"] == "7");
        ORCHA_ASSERT(doc["block"] == "5\n");

        // Explicit core tags are honoured and checked; others are rejected.
        const auto tagged = parse_yaml("i: !!int 5\nf: !!float 5\nb: !!bool true\n");
        ORCHA_ASSERT(tagged["i"] == 5 && tagged["f"].is_number_float() && tagged["b"] == true);
        (void)yaml_tests::expect_yaml_error([] { (void)parse_yaml("x: !!bool yes\n"); });
        (void)yaml_tests::expect_yaml_error([] { (void)parse_yaml("x: !custom 5\n"); });

        // Floats do not depend on the C locale (strtod would give 1 under de_DE).
        if (const char* old = std::setlocale(LC_NUMERIC, nullptr); old) {
            const std::string saved = old;
            if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") || std::setlocale(LC_NUMERIC, "de_DE")) {
                const auto localized = parse_yaml("x: 1.5\n");
                std::setlocale(LC_NUMERIC, saved.c_str());
                ORCHA_ASSERT(localized["x"] == 1.5);
            }
        }
        std::cout << "[PASS] test_yaml_scalars_follow_core_schema\n";
    }

    // ========== Round trip ==========

    inline void test_yaml_round_trip_is_lossless() {
        using namespace yaml_tests;
        OJson tricky = OJson::array();
        for (const char* s : {"123", "-4", "1.5", "1e5", "0x1F", "true", "False", "null", "~", "",
                              ".inf", "yes", "{{steps.greet.output.echoed}}", "a: b", "a #b", "#c",
                              "- x", "-", "?", "*alias", "&anchor", "!tag", "%x", "@x", "`x", "key:",
                              " lead", "trail ", "http://example.com/a?b=c#d", "it's \"quoted\"",
                              "line1\nline2", "line1\nline2\n", "a\n\n", "\nstarts with newline",
                              "  indented\nnext", "tab\there", "ctrl\x01" "char", "crlf\r\nline",
                              "ünïcödé 日本語 ✓", "end\n  \n"}) {
            tricky.push_back(s);
        }
        OJson value = {
            {"name", "deploy"},
            {"timeout_ms", 30000},
            {"ratio", 1.0},
            {"tiny", 1.5e-7},
            {"huge", std::numeric_limits<std::uint64_t>::max()},
            {"negative", std::numeric_limits<std::int64_t>::min()},
            {"enabled", false},
            {"nothing", nullptr},
            {"empty_map", OJson::object()},
            {"empty_list", OJson::array()},
            {"nested", OJson::array({OJson::array({1, OJson::array({2, 3})}), OJson::object()})},
            {"strings", tricky},
            {"", "empty key"}, {"123", "numeric key"}, {"null", "null key"}, {"a: b", "colon key"},
            {"- k", "dash key"}, {"multi\nline key", 1},
            {"steps", OJson::array({
                OJson{{"name", "greet"}, {"command", "echo"},
                      {"params", {{"message", "Hello"}, {"tags", OJson::array({"a", "b"})}}}},
                OJson{{"command", "shell_exec"}, {"params", {{"command", "echo one\necho two"}}}}})}
        };
        const auto text = Utils::json_to_yaml(value);
        const auto back = Utils::parse_yaml<OJson>(text);
        ORCHA_ASSERT(back == value && "YAML round trip must be lossless, key order included");
        ORCHA_ASSERT(back.dump() == value.dump());

        // Every string survives on its own too (exercises the top-level scalar path).
        for (const auto& s : tricky) {
            ORCHA_ASSERT(Utils::parse_yaml<OJson>(Utils::json_to_yaml(s)) == s);
        }
        std::cout << "[PASS] test_yaml_round_trip_is_lossless\n";
    }

    inline void test_yaml_emitter_output_is_readable() {
        using namespace yaml_tests;
        const OJson def = {
            {"rollback", "reverse"},
            {"steps", OJson::array({
                OJson{{"name", "greet"}, {"command", "echo"},
                      {"params", {{"message", "Hello, {{steps.x.output}}"}, {"count", 3}}}},
                OJson{{"command", "shell_exec"},
                      {"params", {{"command", "echo one\necho two"}, {"env", OJson::object()}}}}})}
        };
        const std::string expected =
            "rollback: reverse\n"
            "steps:\n"
            "  - name: greet\n"
            "    command: echo\n"
            "    params:\n"
            "      message: Hello, {{steps.x.output}}\n"
            "      count: 3\n"
            "  - command: shell_exec\n"
            "    params:\n"
            "      command: |-\n"
            "        echo one\n"
            "        echo two\n"
            "      env: {}\n";
        const auto text = Utils::json_to_yaml(def);
        if (text != expected) std::cerr << "Got:\n" << text;
        ORCHA_ASSERT(text == expected);
        // Placeholders at the start of a value need quotes ('{' opens a flow map).
        ORCHA_ASSERT(Utils::json_to_yaml(OJson{{"m", "{{step1.output}}"}}) ==
                     "m: \"{{step1.output}}\"\n");
        ORCHA_ASSERT(Utils::json_to_yaml(OJson{{"port", "8080"}}) == "port: \"8080\"\n");
        // Quoted for YAML 1.1 readers too (booleans, dates).
        ORCHA_ASSERT(Utils::json_to_yaml(OJson{{"a", "yes"}, {"b", "Off"}, {"c", "2024-01-01"}}) ==
                     "a: \"yes\"\nb: \"Off\"\nc: \"2024-01-01\"\n");
        std::cout << "[PASS] test_yaml_emitter_output_is_readable\n";
    }

    // ========== Errors and limits ==========

    inline void test_yaml_errors_carry_positions() {
        using namespace yaml_tests;
        auto syntax = expect_yaml_error([] { (void)Utils::parse_yaml("steps:\n  - a\n  b: [1, 2\n"); });
        ORCHA_ASSERT(syntax.line() > 0 && syntax.column() > 0);

        auto dup = expect_yaml_error([] { (void)Utils::parse_yaml("name: a\nsteps: []\nname: b\n"); });
        ORCHA_ASSERT(dup.line() == 3 && dup.column() == 1);
        ORCHA_ASSERT(dup.message().find("Duplicate key 'name'") != std::string::npos);

        auto docs = expect_yaml_error([] { (void)Utils::parse_yaml("a: 1\n---\nb: 2\n"); });
        ORCHA_ASSERT(docs.message().find("Expected one YAML document") != std::string::npos);

        auto key = expect_yaml_error([] { (void)Utils::parse_yaml("? [a, b]\n: c\n"); });
        ORCHA_ASSERT(key.message().find("Mapping keys must be strings") != std::string::npos);

        ORCHA_ASSERT(Utils::parse_yaml("").is_null());
        ORCHA_ASSERT(Utils::parse_yaml("# only a comment\n").is_null());
        std::cout << "[PASS] test_yaml_errors_carry_positions\n";
    }

    inline void test_yaml_limits_bound_untrusted_input() {
        using namespace yaml_tests;
        // Nesting: 64 levels are fine, 65 are not (as for JSON bodies).
        const auto nested = [](int n) { return std::string(n, '[') + std::string(n, ']'); };
        ORCHA_ASSERT(Utils::parse_yaml(nested(64)).is_array());
        auto deep = expect_yaml_error([&] { (void)Utils::parse_yaml(nested(65)); });
        ORCHA_ASSERT(deep.message().find("maximum depth of 64") != std::string::npos);
        // Past yaml-cpp's own recursion guard: a clean error, not a crash.
        (void)expect_yaml_error([&] { (void)Utils::parse_yaml(nested(5000)); });

        // "Billion laughs": aliases expand exponentially; the node cap stops it.
        std::string bomb = "a: &a [x, x, x, x, x, x, x, x, x, x]\n";
        for (char c = 'b'; c <= 'j'; ++c) {
            const char prev = static_cast<char>(c - 1);
            bomb += std::string(1, c) + ": &" + c + " [*" + prev + ", *" + prev + ", *" + prev +
                    ", *" + prev + ", *" + prev + ", *" + prev + ", *" + prev + ", *" + prev +
                    ", *" + prev + ", *" + prev + "]\n";
        }
        auto big = expect_yaml_error([&] { (void)Utils::parse_yaml(bomb); });
        ORCHA_ASSERT(big.message().find("too large") != std::string::npos);
        std::cout << "[PASS] test_yaml_limits_bound_untrusted_input\n";
    }

    inline void test_yaml_merge_keys() {
        using namespace yaml_tests;
        const auto doc = Utils::parse_yaml<OJson>(
            "base: &base {command: echo, timeout_ms: 100, params: {message: hi}}\n"
            "extra: &extra {timeout_ms: 999, parallel: true}\n"
            "one:\n  <<: *base\n  timeout_ms: 5\n"
            "two:\n  <<: [*extra, *base]\n"
            "literal:\n  \"<<\": kept\n");
        // The mapping's own keys win; merged keys keep the source's order.
        ORCHA_ASSERT(doc["one"].dump() == R"({"command":"echo","params":{"message":"hi"},"timeout_ms":5})");
        // Earlier sources win over later ones.
        ORCHA_ASSERT(doc["two"]["timeout_ms"] == 999 && doc["two"]["parallel"] == true &&
                     doc["two"]["command"] == "echo");
        // A quoted "<<" is an ordinary key, and the emitter quotes it.
        ORCHA_ASSERT(doc["literal"]["<<"] == "kept");
        const OJson literal = {{"<<", "kept"}};
        ORCHA_ASSERT(Utils::json_to_yaml(literal) == "\"<<\": kept\n");
        ORCHA_ASSERT(Utils::parse_yaml<OJson>(Utils::json_to_yaml(literal)) == literal);

        // A merged mapping counts at its own level: 63 sequences + a map is
        // within the 64-level limit with or without the merge.
        const std::string open(63, '['), close(63, ']');
        ORCHA_ASSERT(Utils::parse_yaml(open + "{x: 1}" + close).is_array());
        ORCHA_ASSERT(Utils::parse_yaml(open + "{<<: {x: 1}}" + close).is_array());

        auto bad = expect_yaml_error([] { (void)Utils::parse_yaml("a:\n  <<: 5\n"); });
        ORCHA_ASSERT(bad.message().find("merge key") != std::string::npos);
        std::cout << "[PASS] test_yaml_merge_keys\n";
    }

    inline void test_yaml_large_inputs_are_bounded_and_linear() {
        using namespace yaml_tests;
        // Size: refused before yaml-cpp builds its (~100x larger) node tree.
        const std::string big(Utils::YamlLimits{}.max_bytes + 1, '#');
        auto too_big = expect_yaml_error([&] { (void)Utils::parse_yaml(big); });
        ORCHA_ASSERT(too_big.message().find("too large") != std::string::npos);

        // A wide mapping keeps its order and converts in linear time
        // (ordered_json's operator[] made this quadratic: ~3 s for 50k keys).
        std::string wide;
        for (int i = 0; i < 50'000; ++i) wide += "k" + std::to_string(i) + ": " + std::to_string(i) + "\n";
        Utils::YamlLimits roomy;
        roomy.max_bytes = wide.size();
        const auto start = std::chrono::steady_clock::now();
        const auto doc = Utils::parse_yaml<OJson>(wide, roomy);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        ORCHA_ASSERT(doc.size() == 50'000 && doc.begin().key() == "k0" && doc["k49999"] == 49999);
        ORCHA_ASSERT(elapsed < std::chrono::seconds(1));

        // JSON into ordered_json: keys per object are capped.
        ORCHA_ASSERT(parse_json_bounded<OJson>(R"({"a":1,"b":{"c":2,"d":3}})", 64, 2).size() == 2);
        bool threw = false;
        try { (void)parse_json_bounded<OJson>(R"({"a":1,"b":2,"c":3})", 64, 2); }
        catch (const std::exception&) { threw = true; }
        ORCHA_ASSERT(threw);
        std::cout << "[PASS] test_yaml_large_inputs_are_bounded_and_linear\n";
    }

    // ========== HTTP ==========

    inline void test_yaml_media_types() {
        using Agent::is_yaml_media_type;
        ORCHA_ASSERT(is_yaml_media_type("application/yaml"));
        ORCHA_ASSERT(is_yaml_media_type("Application/YAML; charset=utf-8"));
        ORCHA_ASSERT(is_yaml_media_type("application/x-yaml"));
        ORCHA_ASSERT(is_yaml_media_type("text/yaml"));
        ORCHA_ASSERT(is_yaml_media_type(" text/x-yaml "));
        ORCHA_ASSERT(!is_yaml_media_type("application/json"));
        ORCHA_ASSERT(!is_yaml_media_type("text/plain"));
        ORCHA_ASSERT(!is_yaml_media_type(""));
        std::cout << "[PASS] test_yaml_media_types\n";
    }

    inline void test_definition_convert_route() {
        using namespace yaml_tests;
        Agent::Routes::DefinitionRoute route;
        const std::string path = "/api/definitions/convert";
        ORCHA_ASSERT(route.can_handle("POST", path));
        ORCHA_ASSERT(!route.can_handle("POST", "/api/definitions"));

        // YAML -> JSON keeps the author's key order.
        auto from_yaml = route.handle(request("POST", path, "application/yaml",
            "steps:\n  - name: greet\n    command: echo\n    params: {message: '42'}\nrollback: reverse\n"));
        ORCHA_ASSERT_EQ(from_yaml.status, Agent::status::OK);
        ORCHA_ASSERT(from_yaml.body.find(R"("definition":{"steps":[{"name":"greet","command":"echo","params":{"message":"42"}}],"rollback":"reverse"})") != std::string::npos);

        // JSON -> YAML.
        auto from_json = route.handle(request("POST", path, "application/json; charset=utf-8",
            R"({"steps":[{"name":"a","command":"echo","params":{"n":"7"}}]})"));
        ORCHA_ASSERT_EQ(from_json.status, Agent::status::OK);
        const auto body = Json::parse(from_json.body);
        // "n" is quoted too: YAML 1.1 readers take a bare n for false.
        ORCHA_ASSERT(body["yaml"] == "steps:\n  - name: a\n    command: echo\n    params:\n      \"n\": \"7\"\n");

        // Syntax errors report where.
        auto bad = route.handle(request("POST", path, "text/yaml", "steps:\n  - [unclosed\n"));
        ORCHA_ASSERT_EQ(bad.status, Agent::status::BadRequest);
        const auto err = Json::parse(bad.body);
        ORCHA_ASSERT(err.contains("line") && err.contains("column"));
        ORCHA_ASSERT(err["error"].get<std::string>().rfind("Invalid YAML body: ", 0) == 0);

        ORCHA_ASSERT_EQ(route.handle(request("POST", path, "application/yaml", "- a\n- b\n")).status,
                        Agent::status::BadRequest);   // not a mapping
        ORCHA_ASSERT_EQ(route.handle(request("POST", path, "text/plain", "steps: []")).status,
                        Agent::status::UnsupportedMediaType);
        ORCHA_ASSERT_EQ(route.handle(request("GET", path, "", "")).status,
                        Agent::status::MethodNotAllowed);
        ORCHA_ASSERT_EQ(route.handle(request("POST", path, "Application/JSON", R"({"steps":[]})")).status,
                        Agent::status::OK);

        // Size caps apply to both formats here (ordered JSON is costly to build).
        const std::string huge(Agent::kMaxYamlBodyBytes + 1, ' ');
        ORCHA_ASSERT_EQ(route.handle(request("POST", path, "application/yaml", huge)).status,
                        Agent::status::PayloadTooLarge);
        ORCHA_ASSERT_EQ(route.handle(request("POST", path, "application/json", huge)).status,
                        Agent::status::PayloadTooLarge);
        std::string many = R"({"steps":[],"params":{)";
        for (int i = 0; i <= 10'000; ++i) many += (i ? "," : "") + std::string("\"k") + std::to_string(i) + "\":0";
        many += "}}";
        ORCHA_ASSERT_EQ(route.handle(request("POST", path, "application/json", many)).status,
                        Agent::status::BadRequest);
        std::cout << "[PASS] test_definition_convert_route\n";
    }

    inline void test_job_api_accepts_yaml_bodies() {
        using namespace yaml_tests;
        auto store = std::make_shared<Jobs::SqliteJobStore>(":memory:");
        auto service = std::make_shared<Jobs::JobService>(
            store, [] { return std::shared_ptr<Workflow::IWorkflowEngine>{}; });
        auto runs = std::make_shared<Jobs::RunQueue>(service, Jobs::RunQueue::Options{1, 8});
        Agent::Routes::JobRoute route(service, runs);

        auto created = route.handle(request("POST", "/api/jobs", "application/yaml",
            "name: nightly\n"
            "schedule_cron: 0 3 * * *\n"
            "enabled: true\n"
            "definition:\n"
            "  rollback: best_effort\n"
            "  steps:\n"
            "    - name: greet\n"
            "      command: echo\n"
            "      params:\n"
            "        message: |-\n"
            "          line one\n"
            "          line two\n"));
        ORCHA_ASSERT_EQ(created.status, Agent::status::Created);
        auto job = store->get_job_by_name("nightly");
        ORCHA_ASSERT(job.has_value());
        ORCHA_ASSERT(job->schedule_cron == "0 3 * * *");
        ORCHA_ASSERT(job->definition["rollback"] == "best_effort");
        ORCHA_ASSERT(job->definition["steps"][0]["params"]["message"] == "line one\nline two");

        // Update with YAML; a bad document is a 400 with its position, not a 500.
        auto updated = route.handle(request("PUT", "/api/jobs/" + job->id, "application/yaml",
            "name: nightly\ndefinition:\n  steps: []\nenabled: false\n"));
        ORCHA_ASSERT_EQ(updated.status, Agent::status::OK);
        ORCHA_ASSERT(store->get_job(job->id)->enabled == false);

        auto bad = route.handle(request("PUT", "/api/jobs/" + job->id, "application/yaml",
            "name: nightly\nname: again\n"));
        ORCHA_ASSERT_EQ(bad.status, Agent::status::BadRequest);
        ORCHA_ASSERT(Json::parse(bad.body)["line"] == 2);

        // YAML job fields are typed strictly: `enabled: no` is the string "no",
        // which JobDefinition::from_json would skip, leaving the job enabled.
        auto yes_no = route.handle(request("POST", "/api/jobs", "application/yaml",
            "name: wordy\nenabled: no\ndefinition:\n  steps: []\n"));
        ORCHA_ASSERT_EQ(yes_no.status, Agent::status::BadRequest);
        ORCHA_ASSERT(Json::parse(yes_no.body)["error"].get<std::string>().find("'enabled' must be true or false") == 0);
        ORCHA_ASSERT(!store->get_job_by_name("wordy").has_value());
        auto numeric_name = route.handle(request("POST", "/api/jobs", "application/yaml",
            "name: 2024\ndefinition:\n  steps: []\n"));
        ORCHA_ASSERT_EQ(numeric_name.status, Agent::status::BadRequest);
        ORCHA_ASSERT(Json::parse(numeric_name.body)["error"] == "'name' must be a string");

        // JSON bodies are unchanged (no or JSON Content-Type).
        auto json_job = route.handle(request("POST", "/api/jobs", "",
            R"({"name":"plain","definition":{"steps":[]}})"));
        ORCHA_ASSERT_EQ(json_job.status, Agent::status::Created);
        std::cout << "[PASS] test_job_api_accepts_yaml_bodies\n";
    }

    inline void run_yaml_tests() {
        test_yaml_scalars_follow_core_schema();
        test_yaml_round_trip_is_lossless();
        test_yaml_emitter_output_is_readable();
        test_yaml_errors_carry_positions();
        test_yaml_limits_bound_untrusted_input();
        test_yaml_merge_keys();
        test_yaml_large_inputs_are_bounded_and_linear();
        test_yaml_media_types();
        test_definition_convert_route();
        test_job_api_accepts_yaml_bodies();
    }

} // namespace Orcha::Tests
