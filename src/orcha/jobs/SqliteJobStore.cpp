//
// SqliteJobStore.cpp - SQLite-backed IJobStore implementation (Phase 2)
//

#include "SqliteJobStore.hpp"
#include "../core/JsonCompat.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <stdexcept>

namespace Orcha::Jobs {

    namespace {

        /**
         * @brief RAII wrapper around a prepared statement.
         */
        class Stmt {
        public:
            Stmt(sqlite3* db, const std::string& sql) {
                if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
                    throw std::runtime_error(std::string("sqlite prepare failed: ") +
                                             sqlite3_errmsg(db));
                }
            }
            ~Stmt() { if (stmt_) sqlite3_finalize(stmt_); }
            Stmt(const Stmt&) = delete;
            Stmt& operator=(const Stmt&) = delete;

            sqlite3_stmt* get() const { return stmt_; }

            void bind_text(int i, const std::string& v) {
                sqlite3_bind_text(stmt_, i, v.c_str(), -1, SQLITE_TRANSIENT);
            }
            void bind_int(int i, int v) { sqlite3_bind_int(stmt_, i, v); }
            void bind_null(int i) { sqlite3_bind_null(stmt_, i); }

            [[nodiscard]] std::string col_text(int i) const {
                const auto* p = sqlite3_column_text(stmt_, i);
                return p ? reinterpret_cast<const char*>(p) : std::string{};
            }
            [[nodiscard]] bool col_is_null(int i) const {
                return sqlite3_column_type(stmt_, i) == SQLITE_NULL;
            }

        private:
            sqlite3_stmt* stmt_ = nullptr;
        };

        JobDefinition read_job(const Stmt& s) {
            // columns: id,name,description,definition,schedule_cron,enabled,created_at,updated_at
            JobDefinition d;
            d.id = s.col_text(0);
            d.name = s.col_text(1);
            d.description = s.col_text(2);
            d.definition = Orcha::parse_json_or(s.col_text(3), Orcha::Json::object());
            if (!s.col_is_null(4)) d.schedule_cron = s.col_text(4);
            d.enabled = s.col_text(5) == "1";
            d.created_at = s.col_text(6);
            d.updated_at = s.col_text(7);
            return d;
        }

        /// Step outputs may contain invalid UTF-8 (e.g. binary shell output):
        /// store it with replacement characters instead of failing.
        std::string to_text(const Orcha::Json& j) {
            return j.dump(-1, ' ', false, Orcha::Json::error_handler_t::replace);
        }

        constexpr const char* kRunColumns =
            "id,job_id,trigger,status,started_at,finished_at,result,error,queued_at";

        RunRecord read_run(const Stmt& s) {
            // columns: kRunColumns
            RunRecord r;
            r.id = s.col_text(0);
            if (!s.col_is_null(1)) r.job_id = s.col_text(1);
            r.trigger = s.col_text(2);
            r.status = s.col_text(3);
            r.started_at = s.col_text(4);          // NULL -> "" (never started)
            if (!s.col_is_null(5)) r.finished_at = s.col_text(5);
            r.result = Orcha::parse_json_or(s.col_text(6), Orcha::Json(nullptr));
            r.error = s.col_text(7);
            r.queued_at = s.col_text(8);
            return r;
        }

    } // namespace

    SqliteJobStore::SqliteJobStore(const std::string& db_path,
                                   std::shared_ptr<Utils::ILogger> logger)
        : logger_(std::move(logger))
        , owner_(generate_id())
        , persistent_(db_path != ":memory:" && !db_path.empty()) {
        if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK) {
            const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
            if (db_) { sqlite3_close(db_); db_ = nullptr; }
            throw std::runtime_error("Failed to open job database '" + db_path + "': " + msg);
        }
        sqlite3_busy_timeout(db_, 5000);
        init_schema();
        if (logger_) logger_->info("Job store opened at " + db_path);
    }

    SqliteJobStore::~SqliteJobStore() {
        if (db_) sqlite3_close(db_);
    }

    void SqliteJobStore::init_schema() {
        const char* ddl =
            "PRAGMA journal_mode=WAL;"
            "CREATE TABLE IF NOT EXISTS jobs("
            "  id TEXT PRIMARY KEY, name TEXT UNIQUE NOT NULL, description TEXT,"
            "  definition TEXT NOT NULL, schedule_cron TEXT,"
            "  enabled INTEGER NOT NULL DEFAULT 1, created_at TEXT, updated_at TEXT);"
            "CREATE TABLE IF NOT EXISTS runs("
            "  id TEXT PRIMARY KEY, job_id TEXT, trigger TEXT NOT NULL, status TEXT NOT NULL,"
            "  started_at TEXT, finished_at TEXT, result TEXT, error TEXT);";
        char* err = nullptr;
        if (sqlite3_exec(db_, ddl, nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("Failed to initialise job schema: " + msg);
        }
        migrate_schema();
    }

    // Schema versions (PRAGMA user_version):
    //   0 - original runs table (ordered by started_at)
    //   1 - runs.queued_at (backfilled from started_at), runs.owner, indexes
    //       on queued_at; the started_at indexes are dropped.
    // Each step runs in one transaction, so a crash cannot leave it half done.
    void SqliteJobStore::migrate_schema() {
        int version = 0;
        {
            Stmt s(db_, "PRAGMA user_version");
            if (sqlite3_step(s.get()) == SQLITE_ROW) version = sqlite3_column_int(s.get(), 0);
        }
        if (version >= 1) {
            return;
        }

        auto has_column = [this](const char* name) {
            Stmt s(db_, "PRAGMA table_info(runs)");
            while (sqlite3_step(s.get()) == SQLITE_ROW) {
                if (s.col_text(1) == name) return true;
            }
            return false;
        };
        std::string ddl = "BEGIN IMMEDIATE;";
        if (!has_column("queued_at")) ddl += "ALTER TABLE runs ADD COLUMN queued_at TEXT;";
        if (!has_column("owner")) ddl += "ALTER TABLE runs ADD COLUMN owner TEXT;";
        ddl += "UPDATE runs SET queued_at=started_at WHERE queued_at IS NULL;"
               "DROP INDEX IF EXISTS idx_runs_job;"
               "DROP INDEX IF EXISTS idx_runs_started;"
               "CREATE INDEX IF NOT EXISTS idx_runs_queued ON runs(queued_at);"
               "CREATE INDEX IF NOT EXISTS idx_runs_job_queued ON runs(job_id, queued_at);"
               "CREATE INDEX IF NOT EXISTS idx_runs_status ON runs(status);"
               "PRAGMA user_version=1;"
               "COMMIT;";
        char* err = nullptr;
        if (sqlite3_exec(db_, ddl.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            throw std::runtime_error("Failed to migrate job schema: " + msg);
        }
        if (logger_) logger_->info("Job store schema at version 1");
    }

    void SqliteJobStore::set_max_runs(size_t max) {
        std::lock_guard lock(mutex_);
        max_runs_ = max;
    }

    // ---------------- Jobs ----------------

    std::vector<JobDefinition> SqliteJobStore::list_jobs() const {
        std::lock_guard lock(mutex_);
        std::vector<JobDefinition> out;
        Stmt s(db_,
            "SELECT id,name,description,definition,schedule_cron,enabled,created_at,updated_at "
            "FROM jobs ORDER BY name");
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            out.push_back(read_job(s));
        }
        return out;
    }

    std::optional<JobDefinition> SqliteJobStore::get_job(const std::string& id) const {
        std::lock_guard lock(mutex_);
        Stmt s(db_,
            "SELECT id,name,description,definition,schedule_cron,enabled,created_at,updated_at "
            "FROM jobs WHERE id=?1");
        s.bind_text(1, id);
        if (sqlite3_step(s.get()) == SQLITE_ROW) return read_job(s);
        return std::nullopt;
    }

    std::optional<JobDefinition> SqliteJobStore::get_job_by_name(const std::string& name) const {
        std::lock_guard lock(mutex_);
        Stmt s(db_,
            "SELECT id,name,description,definition,schedule_cron,enabled,created_at,updated_at "
            "FROM jobs WHERE name=?1");
        s.bind_text(1, name);
        if (sqlite3_step(s.get()) == SQLITE_ROW) return read_job(s);
        return std::nullopt;
    }

    bool SqliteJobStore::create_job(JobDefinition& job) {
        std::lock_guard lock(mutex_);
        job.id = generate_id();
        job.created_at = now_iso_utc();
        job.updated_at = job.created_at;
        try {
            Stmt s(db_,
                "INSERT INTO jobs(id,name,description,definition,schedule_cron,enabled,"
                "created_at,updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
            s.bind_text(1, job.id);
            s.bind_text(2, job.name);
            s.bind_text(3, job.description);
            s.bind_text(4, to_text(job.definition));
            if (job.schedule_cron) s.bind_text(5, *job.schedule_cron); else s.bind_null(5);
            s.bind_int(6, job.enabled ? 1 : 0);
            s.bind_text(7, job.created_at);
            s.bind_text(8, job.updated_at);
            if (sqlite3_step(s.get()) != SQLITE_DONE) {
                if (logger_) logger_->warn(std::string("create_job failed: ") + sqlite3_errmsg(db_));
                return false;
            }
            return true;
        } catch (const std::exception& ex) {
            if (logger_) logger_->error(std::string("create_job error: ") + ex.what());
            return false;
        }
    }

    bool SqliteJobStore::update_job(const JobDefinition& job) {
        std::lock_guard lock(mutex_);
        try {
            Stmt s(db_,
                "UPDATE jobs SET name=?2,description=?3,definition=?4,schedule_cron=?5,"
                "enabled=?6,updated_at=?7 WHERE id=?1");
            s.bind_text(1, job.id);
            s.bind_text(2, job.name);
            s.bind_text(3, job.description);
            s.bind_text(4, to_text(job.definition));
            if (job.schedule_cron) s.bind_text(5, *job.schedule_cron); else s.bind_null(5);
            s.bind_int(6, job.enabled ? 1 : 0);
            s.bind_text(7, now_iso_utc());
            if (sqlite3_step(s.get()) != SQLITE_DONE) return false;
            return sqlite3_changes(db_) > 0;
        } catch (const std::exception& ex) {
            if (logger_) logger_->error(std::string("update_job error: ") + ex.what());
            return false;
        }
    }

    bool SqliteJobStore::delete_job(const std::string& id) {
        std::lock_guard lock(mutex_);
        Stmt s(db_, "DELETE FROM jobs WHERE id=?1");
        s.bind_text(1, id);
        if (sqlite3_step(s.get()) != SQLITE_DONE) return false;
        return sqlite3_changes(db_) > 0;
    }

    // ---------------- Runs ----------------

    bool SqliteJobStore::insert_run(RunRecord& run) {
        std::lock_guard lock(mutex_);
        if (run.id.empty()) run.id = generate_id();
        if (run.queued_at.empty()) run.queued_at = now_iso_utc();
        try {
            Stmt s(db_,
                "INSERT INTO runs(id,job_id,trigger,status,started_at,finished_at,result,error,queued_at,owner) "
                "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
            s.bind_text(1, run.id);
            if (run.job_id) s.bind_text(2, *run.job_id); else s.bind_null(2);
            s.bind_text(3, run.trigger);
            s.bind_text(4, run.status);
            if (!run.started_at.empty()) s.bind_text(5, run.started_at); else s.bind_null(5);
            if (run.finished_at) s.bind_text(6, *run.finished_at); else s.bind_null(6);
            s.bind_text(7, to_text(run.result));
            s.bind_text(8, run.error);
            s.bind_text(9, run.queued_at);
            s.bind_text(10, owner_);
            if (sqlite3_step(s.get()) != SQLITE_DONE) {
                if (logger_) logger_->warn(std::string("insert_run failed: ") + sqlite3_errmsg(db_));
                return false;
            }
            // Retention: prune every ~10% of the limit (at most every 100 inserts).
            if (max_runs_ > 0 &&
                ++inserts_since_prune_ >= std::clamp<size_t>(max_runs_ / 10, 1, 100)) {
                inserts_since_prune_ = 0;
                (void)prune_runs_locked(max_runs_);
            }
            return true;
        } catch (const std::exception& ex) {
            if (logger_) logger_->error(std::string("insert_run error: ") + ex.what());
            return false;
        }
    }

    bool SqliteJobStore::update_run(const RunRecord& run) {
        std::lock_guard lock(mutex_);
        try {
            // Only live runs change: a final status is never overwritten.
            Stmt s(db_,
                "UPDATE runs SET status=?2,started_at=?3,finished_at=?4,result=?5,error=?6 "
                "WHERE id=?1 AND status IN (?7, ?8)");
            s.bind_text(1, run.id);
            s.bind_text(2, run.status);
            if (!run.started_at.empty()) s.bind_text(3, run.started_at); else s.bind_null(3);
            if (run.finished_at) s.bind_text(4, *run.finished_at); else s.bind_null(4);
            s.bind_text(5, to_text(run.result));
            s.bind_text(6, run.error);
            s.bind_text(7, run_status::Queued);
            s.bind_text(8, run_status::Running);
            if (sqlite3_step(s.get()) != SQLITE_DONE) {
                if (logger_) logger_->warn(std::string("update_run failed: ") + sqlite3_errmsg(db_));
                return false;
            }
            return sqlite3_changes(db_) > 0;
        } catch (const std::exception& ex) {
            if (logger_) logger_->error(std::string("update_run error: ") + ex.what());
            return false;
        }
    }

    size_t SqliteJobStore::mark_interrupted_runs(const std::string& reason) {
        std::lock_guard lock(mutex_);
        try {
            Stmt s(db_,
                "UPDATE runs SET status=?1, finished_at=?2, error=?3 "
                "WHERE status IN (?4, ?5) AND (owner IS NULL OR owner <> ?6)");
            s.bind_text(1, run_status::Interrupted);
            s.bind_text(2, now_iso_utc());
            s.bind_text(3, reason);
            s.bind_text(4, run_status::Queued);
            s.bind_text(5, run_status::Running);
            s.bind_text(6, owner_);
            if (sqlite3_step(s.get()) != SQLITE_DONE) return 0;
            return static_cast<size_t>(sqlite3_changes(db_));
        } catch (const std::exception& ex) {
            if (logger_) logger_->error(std::string("mark_interrupted_runs error: ") + ex.what());
            return 0;
        }
    }

    size_t SqliteJobStore::prune_runs(size_t keep) {
        std::lock_guard lock(mutex_);
        return prune_runs_locked(keep);
    }

    size_t SqliteJobStore::prune_runs_locked(size_t keep) {
        try {
            Stmt s(db_,
                "DELETE FROM runs WHERE id IN ("
                "  SELECT id FROM runs WHERE status NOT IN (?1, ?2)"
                "  ORDER BY queued_at DESC, rowid DESC LIMIT -1 OFFSET ?3)");
            s.bind_text(1, run_status::Queued);
            s.bind_text(2, run_status::Running);
            sqlite3_bind_int64(s.get(), 3, static_cast<sqlite3_int64>(keep));
            if (sqlite3_step(s.get()) != SQLITE_DONE) return 0;
            return static_cast<size_t>(sqlite3_changes(db_));
        } catch (const std::exception& ex) {
            if (logger_) logger_->error(std::string("prune_runs error: ") + ex.what());
            return 0;
        }
    }

    std::vector<RunRecord> SqliteJobStore::list_runs(
        const std::optional<std::string>& job_id, size_t limit) const {
        std::lock_guard lock(mutex_);
        std::vector<RunRecord> out;
        const std::string base = std::string("SELECT ") + kRunColumns + " FROM runs ";
        // Newest first by acceptance time; rowid breaks ties within a second.
        const std::string sql = job_id
            ? base + "WHERE job_id=?1 ORDER BY queued_at DESC, rowid DESC LIMIT ?2"
            : base + "ORDER BY queued_at DESC, rowid DESC LIMIT ?1";
        Stmt s(db_, sql);
        if (job_id) {
            s.bind_text(1, *job_id);
            s.bind_int(2, static_cast<int>(limit));
        } else {
            s.bind_int(1, static_cast<int>(limit));
        }
        while (sqlite3_step(s.get()) == SQLITE_ROW) {
            out.push_back(read_run(s));
        }
        return out;
    }

    std::optional<RunRecord> SqliteJobStore::get_run(const std::string& id) const {
        std::lock_guard lock(mutex_);
        Stmt s(db_, std::string("SELECT ") + kRunColumns + " FROM runs WHERE id=?1");
        s.bind_text(1, id);
        if (sqlite3_step(s.get()) == SQLITE_ROW) return read_run(s);
        return std::nullopt;
    }

} // namespace Orcha::Jobs
