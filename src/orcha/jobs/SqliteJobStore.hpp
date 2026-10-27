//
// SqliteJobStore.hpp - SQLite-backed IJobStore (Phase 2)
//

#pragma once

#include "IJobStore.hpp"
#include "../utils/ILogger.hpp"
#include <memory>
#include <mutex>
#include <string>

struct sqlite3; // forward declaration; sqlite3.h is included only in the .cpp

namespace Orcha::Jobs {

    /**
     * @class SqliteJobStore
     * @brief Stores job definitions and run history in a SQLite database.
     *
     * All access is serialized through a mutex; the store owns a single
     * connection. The schema is created on construction if absent.
     */
    class SqliteJobStore : public IJobStore {
    public:
        /**
         * @brief Open (or create) the database at @p db_path and ensure schema.
         * @throws std::runtime_error if the database cannot be opened/initialised.
         */
        explicit SqliteJobStore(const std::string& db_path,
                                std::shared_ptr<Utils::ILogger> logger = nullptr);

        /// Keep at most @p max finished runs (0 = unlimited); pruned
        /// periodically on insert.
        void set_max_runs(size_t max);
        ~SqliteJobStore() override;

        SqliteJobStore(const SqliteJobStore&) = delete;
        SqliteJobStore& operator=(const SqliteJobStore&) = delete;

        // IJobStore - jobs
        [[nodiscard]] std::vector<JobDefinition> list_jobs() const override;
        [[nodiscard]] std::optional<JobDefinition> get_job(const std::string& id) const override;
        [[nodiscard]] std::optional<JobDefinition> get_job_by_name(
            const std::string& name) const override;
        [[nodiscard]] bool create_job(JobDefinition& job) override;
        [[nodiscard]] bool update_job(const JobDefinition& job) override;
        [[nodiscard]] bool delete_job(const std::string& id) override;

        // IJobStore - runs
        [[nodiscard]] bool insert_run(RunRecord& run) override;
        [[nodiscard]] bool update_run(const RunRecord& run) override;
        size_t mark_interrupted_runs(const std::string& reason) override;
        size_t prune_runs(size_t keep) override;
        [[nodiscard]] bool persistent() const override { return persistent_; }
        [[nodiscard]] std::vector<RunRecord> list_runs(
            const std::optional<std::string>& job_id, size_t limit) const override;
        [[nodiscard]] std::optional<RunRecord> get_run(const std::string& id) const override;

    private:
        void init_schema();
        void migrate_schema();

        size_t prune_runs_locked(size_t keep);

        sqlite3* db_ = nullptr;
        std::shared_ptr<Utils::ILogger> logger_;
        mutable std::mutex mutex_;
        /// Identifies this process's runs (runs.owner), so startup recovery
        /// never touches runs another live process is executing.
        std::string owner_;
        bool persistent_ = true;
        size_t max_runs_ = 0;
        size_t inserts_since_prune_ = 0;
    };

} // namespace Orcha::Jobs
