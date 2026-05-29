//
// CronExpr.hpp - Minimal 5-field cron expression parser/matcher (Phase 3).
//
// Fields:  minute(0-59) hour(0-23) day-of-month(1-31) month(1-12) day-of-week(0-6, Sun=0)
// Per field: '*', 'a', 'a-b', 'a-b/n', '*/n', and comma-separated lists thereof.
// day-of-week also accepts 7 as Sunday.
//
// day-of-month / day-of-week follow the classic rule: if BOTH are restricted
// (not '*'), a tick matches when EITHER matches; otherwise both are ANDed.
//
// All matching is done in UTC (the scheduler ticks in UTC).
//

#pragma once

#include <array>
#include <bitset>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace Orcha::Jobs {

    class CronExpr {
    public:
        /// Parse a 5-field cron string. Returns nullopt on malformed input.
        [[nodiscard]] static std::optional<CronExpr> parse(const std::string& expr) {
            std::vector<std::string> fields;
            std::istringstream is(expr);
            std::string tok;
            while (is >> tok) fields.push_back(tok);
            if (fields.size() != 5) return std::nullopt;

            CronExpr c;
            if (!parse_field(fields[0], 0, 59, c.minute_)) return std::nullopt;
            if (!parse_field(fields[1], 0, 23, c.hour_))   return std::nullopt;
            if (!parse_field(fields[2], 1, 31, c.dom_))    return std::nullopt;
            if (!parse_field(fields[3], 1, 12, c.month_))  return std::nullopt;
            if (!parse_field(fields[4], 0, 7,  c.dow_, /*dow=*/true)) return std::nullopt;
            c.dom_star_ = (fields[2] == "*");
            c.dow_star_ = (fields[4] == "*");
            return c;
        }

        /// True if the given UTC broken-down time matches this expression.
        [[nodiscard]] bool matches(const std::tm& t) const {
            if (!minute_.test(t.tm_min)) return false;
            if (!hour_.test(t.tm_hour)) return false;
            if (!month_.test(t.tm_mon + 1)) return false; // tm_mon is 0-based

            const bool dom_hit = dom_.test(t.tm_mday);
            const bool dow_hit = dow_.test(t.tm_wday); // tm_wday: 0=Sun..6=Sat
            const bool day_ok = (!dom_star_ && !dow_star_)
                ? (dom_hit || dow_hit)   // both restricted -> OR
                : (dom_hit && dow_hit);  // at least one '*' -> AND
            return day_ok;
        }

    private:
        // Bitsets sized to the max index+1 used by any field (dow uses 0..7).
        std::bitset<60> minute_;
        std::bitset<24> hour_;
        std::bitset<32> dom_;
        std::bitset<13> month_;
        std::bitset<8>  dow_;
        bool dom_star_ = true;
        bool dow_star_ = true;

        template <size_t N>
        static bool set_bit(std::bitset<N>& bs, int v, bool dow) {
            if (dow && v == 7) v = 0;            // Sunday alias
            if (v < 0 || v >= static_cast<int>(N)) return false;
            bs.set(static_cast<size_t>(v));
            return true;
        }

        /// Parse one field (comma list of *, a, a-b, a-b/n, */n) into a bitset.
        template <size_t N>
        static bool parse_field(const std::string& field, int lo, int hi,
                                std::bitset<N>& out, bool dow = false) {
            std::istringstream is(field);
            std::string part;
            while (std::getline(is, part, ',')) {
                if (part.empty()) return false;

                int step = 1;
                std::string range = part;
                if (auto slash = part.find('/'); slash != std::string::npos) {
                    range = part.substr(0, slash);
                    try { step = std::stoi(part.substr(slash + 1)); }
                    catch (...) { return false; }
                    if (step <= 0) return false;
                }

                int start, end;
                if (range == "*") {
                    start = lo; end = hi;
                } else if (auto dash = range.find('-'); dash != std::string::npos) {
                    try { start = std::stoi(range.substr(0, dash));
                          end   = std::stoi(range.substr(dash + 1)); }
                    catch (...) { return false; }
                } else {
                    try { start = end = std::stoi(range); }
                    catch (...) { return false; }
                }
                if (start > end) return false;
                if (start < lo || end > hi) return false;

                for (int v = start; v <= end; v += step) {
                    if (!set_bit(out, v, dow)) return false;
                }
            }
            return true;
        }
    };

} // namespace Orcha::Jobs
