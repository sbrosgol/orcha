//
// IJobStore.hpp - Job/run persistence abstraction (Phase 2)
//
// A "job" is a saved, named workflow definition. A "run" is a single execution
// (of a job, or an ad-hoc POST /workflow request). The store persists both and
// the run history. The interface is backend-agnostic; SqliteJobStore is the
// default implementation.
//

#pragma once

#include <cpprest/json.h>
#include <chrono>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
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
        web::json::value definition = web::json::value::object(); ///< { "steps": [...] }
        std::optional<std::string> schedule_cron;                 ///< Cron expr (Phase 3).
        bool enabled = true;
        std::string created_at;
        std::string updated_at;

        [[nodiscard]] web::json::value to_json() const {
            web::json::value o = web::json::value::object();
            o[U("id")] = web::json::value::string(utility::conversions::to_string_t(id));
            o[U("name")] = web::json::value::string(utility::conversions::to_string_t(name));
            o[U("description")] = web::json::value::string(
                utility::conversions::to_string_t(description));
            o[U("definition")] = definition;
            o[U("enabled")] = web::json::value::boolean(enabled);
            if (schedule_cron) {
                o[U("schedule_cron")] = web::json::value::string(
                    utility::conversions::to_string_t(*schedule_cron));
            } else {
                o[U("schedule_cron")] = web::json::value::null();
            }
            o[U("created_at")] = web::json::value::string(
                utility::conversions::to_string_t(created_at));
            o[U("updated_at")] = web::json::value::string(
                utility::conversions::to_string_t(updated_at));
            return o;
        }

        /**
         * @brief Build from request JSON (id/timestamps are assigned by the store).
         */
        static JobDefinition from_json(const web::json::value& j) {
            JobDefinition d;
            if (j.has_field(U("name"))) {
                d.name = utility::conversions::to_utf8string(j.at(U("name")).as_string());
            }
            if (j.has_field(U("description")) && j.at(U("description")).is_string()) {
                d.description = utility::conversions::to_utf8string(
                    j.at(U("description")).as_string());
            }
            if (j.has_field(U("definition"))) {
                d.definition = j.at(U("definition"));
            }
            if (j.has_field(U("enabled")) && j.at(U("enabled")).is_boolean()) {
                d.enabled = j.at(U("enabled")).as_bool();
            }
            if (j.has_field(U("schedule_cron")) && j.at(U("schedule_cron")).is_string()) {
                d.schedule_cron = utility::conversions::to_utf8string(
                    j.at(U("schedule_cron")).as_string());
            }
            return d;
        }
    };

    /**
     * @struct RunRecord
     * @brief A single workflow execution and its outcome.
     */
    struct RunRecord {
        std::string id;
        std::optional<std::string> job_id;     ///< Null for ad-hoc /workflow runs.
        std::string trigger;                    ///< "manual" | "api" | "schedule"
        std::string status;                     ///< "success" | "failed"
        std::string started_at;
        std::optional<std::string> finished_at;
        web::json::value result = web::json::value::null();
        std::string error;

        [[nodiscard]] web::json::value to_json() const {
            web::json::value o = web::json::value::object();
            o[U("id")] = web::json::value::string(utility::conversions::to_string_t(id));
            o[U("job_id")] = job_id
                ? web::json::value::string(utility::conversions::to_string_t(*job_id))
                : web::json::value::null();
            o[U("trigger")] = web::json::value::string(utility::conversions::to_string_t(trigger));
            o[U("status")] = web::json::value::string(utility::conversions::to_string_t(status));
            o[U("started_at")] = web::json::value::string(
                utility::conversions::to_string_t(started_at));
            o[U("finished_at")] = finished_at
                ? web::json::value::string(utility::conversions::to_string_t(*finished_at))
                : web::json::value::null();
            o[U("result")] = result;
            o[U("error")] = web::json::value::string(utility::conversions::to_string_t(error));
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
        /// Assigns id if empty. Returns false on error.
        [[nodiscard]] virtual bool insert_run(RunRecord& run) = 0;
        /// Most recent runs first; job_id == nullopt lists all runs.
        [[nodiscard]] virtual std::vector<RunRecord> list_runs(
            const std::optional<std::string>& job_id, size_t limit) const = 0;
        [[nodiscard]] virtual std::optional<RunRecord> get_run(const std::string& id) const = 0;
    };

} // namespace Orcha::Jobs
