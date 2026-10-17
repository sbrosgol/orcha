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
        static JobDefinition from_json(const Json& j) {
            JobDefinition d;
            if (j.contains("name")) {
                d.name = j.at("name").get<std::string>();
            }
            if (j.contains("description") && j.at("description").is_string()) {
                d.description = j.at("description").get<std::string>();
            }
            if (j.contains("definition")) {
                d.definition = j.at("definition");
            }
            if (j.contains("enabled") && j.at("enabled").is_boolean()) {
                d.enabled = j.at("enabled").get<bool>();
            }
            if (j.contains("schedule_cron") && j.at("schedule_cron").is_string()) {
                d.schedule_cron = j.at("schedule_cron").get<std::string>();
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
        Json result = Json(nullptr);
        std::string error;

        [[nodiscard]] Json to_json() const {
            Json o = Json::object();
            o["id"] = id;
            o["job_id"] = job_id ? Json(*job_id) : Json(nullptr);
            o["trigger"] = trigger;
            o["status"] = status;
            o["started_at"] = started_at;
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
        /// Assigns id if empty. Returns false on error.
        [[nodiscard]] virtual bool insert_run(RunRecord& run) = 0;
        /// Most recent runs first; job_id == nullopt lists all runs.
        [[nodiscard]] virtual std::vector<RunRecord> list_runs(
            const std::optional<std::string>& job_id, size_t limit) const = 0;
        [[nodiscard]] virtual std::optional<RunRecord> get_run(const std::string& id) const = 0;
    };

} // namespace Orcha::Jobs
