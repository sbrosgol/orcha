//
// IJobStore.hpp - Job/run persistence abstraction (Phase 2)
//
// A "job" is a saved, named workflow definition. A "run" is a single execution
// (of a job, or an ad-hoc POST /workflow request). The store persists both and
// the run history. The interface is backend-agnostic; SqliteJobStore is the
// default implementation.
//

#pragma once

#include "../core/Json.hpp"
#include <chrono>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Orcha::Jobs {

    /**
     * @brief Current UTC time as an ISO-8601 string (e.g. 2026-05-29T12:34:56Z).
     */
    [[nodiscard]] inline std::string now_iso_utc() {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return os.str();
    }

    /**
     * @brief Generate a random hex identifier (UUID-like, 32 hex chars).
     */
    [[nodiscard]] inline std::string generate_id() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<uint64_t> dist;
        const uint64_t a = dist(rng);
        const uint64_t b = dist(rng);
        std::ostringstream os;
        os << std::hex << std::setw(16) << std::setfill('0') << a
           << std::setw(16) << std::setfill('0') << b;
        return os.str();
    }

    /**
     * @struct JobDefinition
     * @brief A saved, named workflow definition.
     */
    struct JobDefinition {
        std::string id;
        std::string name;
        std::string description;
        Json definition = Json::object(); ///< { "steps": [...] }
        std::optional<std::string> schedule_cron;                 ///< Cron expr (Phase 3).
        bool enabled = true;
        std::string created_at;
        std::string updated_at;

        [[nodiscard]] Json to_json() const {
            Json o = Json::object();
            o["id"] = id;
            o["name"] = name;
            o["description"] = description;
            o["definition"] = definition;
            o["enabled"] = enabled;
            if (schedule_cron) {
                o["schedule_cron"] = *schedule_cron;
            } else {
                o["schedule_cron"] = nullptr;
            }
            o["created_at"] = created_at;
            o["updated_at"] = updated_at;
            return o;
        }

        /**
         * @brief Build from request JSON (id/timestamps are assigned by the store).
         */
        /**
         * @brief Read a job from an API body. Absent fields keep their defaults
         *        (enabled, no schedule); `description` and `schedule_cron` may
         *        also be null. Unknown fields are ignored.
         * @throws std::invalid_argument if a field has the wrong type. A
         *         mistyped field used to be skipped, so `"enabled": "false"`
         *         (or YAML `enabled: no`) left the job enabled and scheduled.
         */
        static JobDefinition from_json(const Json& j) {
            if (!j.is_object()) throw std::invalid_argument("a job must be an object");
            const auto field = [&](const char* key, const char* expected, auto&& ok) -> const Json* {
                if (!j.contains(key)) return nullptr;
                const Json& v = j.at(key);
                if (!ok(v)) {
                    auto got = v.dump(-1, ' ', false, Json::error_handler_t::replace);
                    if (got.size() > 40) got = got.substr(0, 37) + "...";
                    throw std::invalid_argument(std::string("'") + key + "' must be " + expected +
                                                " (got " + got + ")");
                }
                return v.is_null() ? nullptr : &v;
            };
            const auto string = [](const Json& v) { return v.is_string(); };
            const auto string_or_null = [](const Json& v) { return v.is_string() || v.is_null(); };

            JobDefinition d;
            if (const auto* v = field("name", "a string", string)) d.name = v->get<std::string>();
            if (const auto* v = field("description", "a string", string_or_null)) {
                d.description = v->get<std::string>();
            }
            if (const auto* v = field("definition", "an object with a 'steps' array",
                                      [](const Json& x) { return x.is_object(); })) {
                d.definition = *v;
            }
            if (const auto* v = field("enabled", "true or false",
                                      [](const Json& x) { return x.is_boolean(); })) {
                d.enabled = v->get<bool>();
            }
            if (const auto* v = field("schedule_cron", "a cron string such as \"0 3 * * *\"",
                                      string_or_null)) {
                d.schedule_cron = v->get<std::string>();
            }
            return d;
        }
    };

    /// Run lifecycle states (RunRecord::status).
    namespace run_status {
        inline constexpr const char* Queued      = "queued";      ///< accepted, waiting for a runner
        inline constexpr const char* Running     = "running";
        inline constexpr const char* Success     = "success";
        inline constexpr const char* Failed      = "failed";
        inline constexpr const char* Cancelled   = "cancelled";   ///< cancelled by a user or shutdown
        inline constexpr const char* Interrupted = "interrupted"; ///< server stopped before it finished
        inline constexpr const char* Skipped     = "skipped";     ///< scheduled run rejected (queue full)

        /// True once a run can no longer change.
        [[nodiscard]] inline bool is_terminal(const std::string& s) {
            return s != Queued && s != Running;
        }
    } // namespace run_status

    /**
     * @struct RunRecord
     * @brief A single workflow execution and its outcome.
     *
     * Lifecycle: queued -> running -> success | failed | cancelled, or
     * queued -> cancelled, or interrupted (server restart), or skipped
     * (a scheduled run that found the queue full).
     */
    struct RunRecord {
        std::string id;
        std::optional<std::string> job_id;     ///< Null for ad-hoc /workflow runs.
        std::string trigger;                    ///< "manual" | "api" | "schedule" | "nested"
        std::string status;                     ///< See run_status.
        std::string queued_at;                  ///< When the run was accepted.
        std::string started_at;                 ///< When it began executing; empty if it never did.
        std::optional<std::string> finished_at;
        Json result = Json(nullptr);
        std::string error;

        [[nodiscard]] Json to_json() const {
            Json o = Json::object();
            o["id"] = id;
            o["job_id"] = job_id ? Json(*job_id) : Json(nullptr);
            o["trigger"] = trigger;
            o["status"] = status;
            o["queued_at"] = queued_at.empty() ? Json(nullptr) : Json(queued_at);
            o["started_at"] = started_at.empty() ? Json(nullptr) : Json(started_at);
            o["finished_at"] = finished_at ? Json(*finished_at) : Json(nullptr);
            o["result"] = result;
            o["error"] = error;
            return o;
        }
    };

    /**
     * @interface IJobStore
     * @brief Persistence for job definitions and run history.
     */
    class IJobStore {
    public:
        virtual ~IJobStore() = default;

        // Jobs
        [[nodiscard]] virtual std::vector<JobDefinition> list_jobs() const = 0;
        [[nodiscard]] virtual std::optional<JobDefinition> get_job(const std::string& id) const = 0;
        [[nodiscard]] virtual std::optional<JobDefinition> get_job_by_name(
            const std::string& name) const = 0;
        /// Assigns id + created_at/updated_at on success. Returns false on conflict/error.
        [[nodiscard]] virtual bool create_job(JobDefinition& job) = 0;
        [[nodiscard]] virtual bool update_job(const JobDefinition& job) = 0;
        [[nodiscard]] virtual bool delete_job(const std::string& id) = 0;

        // Runs
        /// Assigns id and queued_at if empty. Returns false on error.
        [[nodiscard]] virtual bool insert_run(RunRecord& run) = 0;
        /// Updates status, started_at, finished_at, result and error of an
        /// existing run that is still queued/running (a final status never
        /// changes). Returns false if there was no such live run, or on error.
        [[nodiscard]] virtual bool update_run(const RunRecord& run) = 0;
        /// Marks runs that other (dead) processes left queued/running as
        /// interrupted; runs created through this store are not touched.
        /// Returns how many were updated.
        virtual size_t mark_interrupted_runs(const std::string& reason) = 0;
        /// Deletes the oldest finished runs beyond @p keep. Returns how many.
        virtual size_t prune_runs(size_t keep) = 0;
        /// False when runs and jobs do not survive a restart (in-memory store).
        [[nodiscard]] virtual bool persistent() const = 0;
        /// Most recent runs first; job_id == nullopt lists all runs.
        [[nodiscard]] virtual std::vector<RunRecord> list_runs(
            const std::optional<std::string>& job_id, size_t limit) const = 0;
        [[nodiscard]] virtual std::optional<RunRecord> get_run(const std::string& id) const = 0;
    };

} // namespace Orcha::Jobs
