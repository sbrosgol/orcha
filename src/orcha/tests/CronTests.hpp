//
// CronTests.hpp - Tests for the CronExpr parser/matcher (Phase 3)
//

#pragma once

#include "Assertions.hpp"
#include "../jobs/CronExpr.hpp"
#include <ctime>
#include <iostream>

namespace Orcha::Tests {

    // Build a UTC broken-down time (with tm_wday filled in) from components.
    inline std::tm cron_utc(int y, int mo, int d, int h, int mi) {
        std::tm t{};
        t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
        t.tm_hour = h; t.tm_min = mi; t.tm_sec = 0;
        time_t e = timegm(&t);
        std::tm out{};
        gmtime_r(&e, &out);
        return out;
    }

    inline void test_cron_parse_invalid() {
        ORCHA_ASSERT(!Jobs::CronExpr::parse("* * * *"));        // 4 fields
        ORCHA_ASSERT(!Jobs::CronExpr::parse("* * * * * *"));    // 6 fields
        ORCHA_ASSERT(!Jobs::CronExpr::parse("60 * * * *"));     // minute out of range
        ORCHA_ASSERT(!Jobs::CronExpr::parse("* 24 * * *"));     // hour out of range
        ORCHA_ASSERT(!Jobs::CronExpr::parse("x * * * *"));      // not a number
        ORCHA_ASSERT(!Jobs::CronExpr::parse("*/0 * * * *"));    // zero step
        ORCHA_ASSERT(!Jobs::CronExpr::parse(""));               // empty
        ORCHA_ASSERT(Jobs::CronExpr::parse("* * * * *").has_value());
        std::cout << "[PASS] test_cron_parse_invalid\n";
    }

    inline void test_cron_every_minute() {
        auto c = Jobs::CronExpr::parse("* * * * *").value();
        ORCHA_ASSERT(c.matches(cron_utc(2024, 1, 1, 0, 0)));
        ORCHA_ASSERT(c.matches(cron_utc(2024, 7, 4, 13, 37)));
        std::cout << "[PASS] test_cron_every_minute\n";
    }

    inline void test_cron_step_and_list() {
        auto c = Jobs::CronExpr::parse("*/15 * * * *").value();
        ORCHA_ASSERT(c.matches(cron_utc(2024, 1, 1, 0, 0)));
        ORCHA_ASSERT(c.matches(cron_utc(2024, 1, 1, 0, 15)));
        ORCHA_ASSERT(c.matches(cron_utc(2024, 1, 1, 0, 45)));
        ORCHA_ASSERT(!c.matches(cron_utc(2024, 1, 1, 0, 7)));

        auto r = Jobs::CronExpr::parse("1-10/3 * * * *").value(); // 1,4,7,10
        ORCHA_ASSERT(r.matches(cron_utc(2024, 1, 1, 0, 4)));
        ORCHA_ASSERT(r.matches(cron_utc(2024, 1, 1, 0, 10)));
        ORCHA_ASSERT(!r.matches(cron_utc(2024, 1, 1, 0, 5)));

        auto l = Jobs::CronExpr::parse("0,30 * * * *").value();
        ORCHA_ASSERT(l.matches(cron_utc(2024, 1, 1, 0, 30)));
        ORCHA_ASSERT(!l.matches(cron_utc(2024, 1, 1, 0, 31)));
        std::cout << "[PASS] test_cron_step_and_list\n";
    }

    inline void test_cron_weekday_range() {
        // 09:30 on weekdays (Mon-Fri). 2024-01-01 is a Monday; 2024-01-06 a Saturday.
        auto c = Jobs::CronExpr::parse("30 9 * * 1-5").value();
        ORCHA_ASSERT(c.matches(cron_utc(2024, 1, 1, 9, 30)));   // Mon
        ORCHA_ASSERT(!c.matches(cron_utc(2024, 1, 6, 9, 30)));  // Sat
        ORCHA_ASSERT(!c.matches(cron_utc(2024, 1, 1, 9, 31)));  // wrong minute
        std::cout << "[PASS] test_cron_weekday_range\n";
    }

    inline void test_cron_dom_dow_or_rule() {
        // Both day-of-month(13) and day-of-week(Fri=5) restricted -> OR.
        auto c = Jobs::CronExpr::parse("0 12 13 * 5").value();
        ORCHA_ASSERT(c.matches(cron_utc(2024, 1, 13, 12, 0))); // 13th (Sat) -> dom hit
        ORCHA_ASSERT(c.matches(cron_utc(2024, 1, 12, 12, 0))); // Fri, not 13th -> dow hit
        ORCHA_ASSERT(!c.matches(cron_utc(2024, 1, 14, 12, 0))); // Sun, not 13th -> neither

        // Sunday via both 0 and 7.
        ORCHA_ASSERT(Jobs::CronExpr::parse("0 0 * * 0").value().matches(cron_utc(2024, 1, 7, 0, 0)));
        ORCHA_ASSERT(Jobs::CronExpr::parse("0 0 * * 7").value().matches(cron_utc(2024, 1, 7, 0, 0)));
        std::cout << "[PASS] test_cron_dom_dow_or_rule\n";
    }

    inline void test_cron_midnight_daily() {
        auto c = Jobs::CronExpr::parse("0 0 * * *").value();
        ORCHA_ASSERT(c.matches(cron_utc(2024, 3, 15, 0, 0)));
        ORCHA_ASSERT(!c.matches(cron_utc(2024, 3, 15, 0, 1)));
        ORCHA_ASSERT(!c.matches(cron_utc(2024, 3, 15, 1, 0)));
        std::cout << "[PASS] test_cron_midnight_daily\n";
    }

    inline void run_cron_tests() {
        std::cout << "\n-- Cron (Phase 3) --\n";
        test_cron_parse_invalid();
        test_cron_every_minute();
        test_cron_step_and_list();
        test_cron_weekday_range();
        test_cron_dom_dow_or_rule();
        test_cron_midnight_daily();
    }

} // namespace Orcha::Tests
