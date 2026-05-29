//
// PluginAdminTests.hpp - Tests for the admin dashboard (Phase 1)
//
// Covers the pure security/logic surface: base64 decoding, constant-time
// comparison, Basic-auth header parsing, AdminConfig fail-closed behaviour,
// path helpers, and PluginAdminRoute path matching. End-to-end auth/route
// behaviour is exercised by scripts/admin_smoke.sh.
//

#pragma once

#include "../agent/AuthMiddleware.hpp"
#include "../agent/IRouteHandler.hpp"
#include "../agent/routes/PluginAdminRoute.hpp"
#include "../config/AdminConfig.hpp"

#include "Assertions.hpp"
#include <iostream>

namespace Orcha::Tests {

    // ========== base64 ==========

    inline void test_base64_decode_basic() {
        using Agent::detail::base64_decode;

        auto a = base64_decode("YWRtaW46c2VjcmV0"); // "admin:secret"
        ORCHA_ASSERT(a.has_value() && *a == "admin:secret");

        auto b = base64_decode("dXNlcjpwYXNz");      // "user:pass"
        ORCHA_ASSERT(b.has_value() && *b == "user:pass");

        auto empty = base64_decode("");
        ORCHA_ASSERT(empty.has_value() && empty->empty());

        std::cout << "[PASS] test_base64_decode_basic\n";
    }

    inline void test_base64_decode_invalid() {
        using Agent::detail::base64_decode;
        // '*' is not a valid base64 character.
        auto bad = base64_decode("not*base64");
        ORCHA_ASSERT(!bad.has_value());
        std::cout << "[PASS] test_base64_decode_invalid\n";
    }

    // ========== constant-time compare ==========

    inline void test_constant_time_equals() {
        using Agent::detail::constant_time_equals;
        ORCHA_ASSERT(constant_time_equals("secret", "secret"));
        ORCHA_ASSERT(!constant_time_equals("secret", "secreT"));
        ORCHA_ASSERT(!constant_time_equals("secret", "secret-longer"));
        ORCHA_ASSERT(constant_time_equals("", ""));
        std::cout << "[PASS] test_constant_time_equals\n";
    }

    // ========== Basic-auth header parsing ==========

    inline void test_parse_basic_auth() {
        using Agent::detail::parse_basic_auth;

        auto ok = parse_basic_auth("Basic YWRtaW46c2VjcmV0");
        ORCHA_ASSERT(ok.has_value());
        ORCHA_ASSERT(ok->user == "admin");
        ORCHA_ASSERT(ok->pass == "secret");

        // Password may itself contain ':' - only the first ':' splits.
        // "admin:a:b" -> base64
        auto colon = parse_basic_auth("Basic YWRtaW46YTpi"); // "admin:a:b"
        ORCHA_ASSERT(colon.has_value());
        ORCHA_ASSERT(colon->user == "admin");
        ORCHA_ASSERT(colon->pass == "a:b");

        // Wrong scheme.
        ORCHA_ASSERT(!parse_basic_auth("Bearer abc").has_value());
        // No colon in decoded payload ("admin").
        ORCHA_ASSERT(!parse_basic_auth("Basic YWRtaW4=").has_value());
        // Garbage.
        ORCHA_ASSERT(!parse_basic_auth("Basic ***").has_value());

        std::cout << "[PASS] test_parse_basic_auth\n";
    }

    // ========== AdminConfig fail-closed ==========

    inline void test_admin_config_serviceable() {
        Config::AdminConfig cfg;
        cfg.enabled = true;
        cfg.auth_required = true;
        cfg.password = "x";
        ORCHA_ASSERT(cfg.is_serviceable() && "enabled + auth + password should serve");

        cfg.password = "";
        ORCHA_ASSERT(!cfg.is_serviceable() && "auth required but no password -> fail closed");

        cfg.auth_required = false;
        ORCHA_ASSERT(cfg.is_serviceable() && "no auth required -> serves without password");

        cfg.enabled = false;
        ORCHA_ASSERT(!cfg.is_serviceable() && "disabled -> never serves");

        std::cout << "[PASS] test_admin_config_serviceable\n";
    }

    // ========== path helpers ==========

    inline void test_path_helpers() {
        using Agent::split_path;
        using Agent::path_starts_with;

        auto segs = split_path("/api/plugins/foo/reload");
        ORCHA_ASSERT(segs.size() == 4);
        ORCHA_ASSERT(segs[0] == "api" && segs[1] == "plugins" &&
               segs[2] == "foo" && segs[3] == "reload");

        ORCHA_ASSERT(split_path("/").empty());
        ORCHA_ASSERT(split_path("").empty());

        ORCHA_ASSERT(path_starts_with("/api/plugins", "/api/"));
        ORCHA_ASSERT(path_starts_with("/api/plugins/x", "/api/plugins"));
        ORCHA_ASSERT(!path_starts_with("/workflow", "/api/"));

        std::cout << "[PASS] test_path_helpers\n";
    }

    // ========== PluginAdminRoute path matching ==========

    inline void test_plugin_route_can_handle() {
        // Dependencies are not dereferenced by can_handle, so nulls are fine.
        Agent::Routes::PluginAdminRoute route(
            nullptr, nullptr, nullptr, "./commands",
            std::chrono::milliseconds(5000), nullptr);

        ORCHA_ASSERT(route.can_handle("GET", "/api/plugins"));
        ORCHA_ASSERT(route.can_handle("GET", "/api/plugins/foo"));
        ORCHA_ASSERT(route.can_handle("POST", "/api/plugins/foo/reload"));
        ORCHA_ASSERT(route.can_handle("GET", "/api/plugins/_watch"));

        ORCHA_ASSERT(!route.can_handle("GET", "/api/pluginsX"));   // prefix must be a boundary
        ORCHA_ASSERT(!route.can_handle("GET", "/workflow"));
        ORCHA_ASSERT(!route.can_handle("GET", "/commands"));

        std::cout << "[PASS] test_plugin_route_can_handle\n";
    }

    // ========== Runner ==========

    inline void run_plugin_admin_tests() {
        std::cout << "\n-- Admin Dashboard (Phase 1) --\n";
        test_base64_decode_basic();
        test_base64_decode_invalid();
        test_constant_time_equals();
        test_parse_basic_auth();
        test_admin_config_serviceable();
        test_path_helpers();
        test_plugin_route_can_handle();
    }

} // namespace Orcha::Tests
