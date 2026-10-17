//
// JobRoute.hpp - Admin API for jobs and run history (Phase 2)
//
// Endpoints (all under the admin auth gate):
//   GET    /api/jobs                 list jobs
//   POST   /api/jobs                 create a job        (body: JobDefinition)
//   GET    /api/jobs/{id}            get a job
//   PUT    /api/jobs/{id}            update a job        (body: JobDefinition)
//   DELETE /api/jobs/{id}            delete a job
//   POST   /api/jobs/{id}/run        run a job now -> RunRecord
//   GET    /api/jobs/{id}/runs       run history for a job
//   GET    /api/runs                 recent runs (all jobs + ad-hoc)
//   GET    /api/runs/{id}            single run detail
//

#pragma once

#include "../IRouteHandler.hpp"
#include "../HttpJson.hpp"
#include "../../jobs/JobService.hpp"
#include "../../utils/ILogger.hpp"

#include <string>

namespace Orcha::Agent::Routes {

    class JobRoute : public IRouteHandler {
    public:
        JobRoute(std::shared_ptr<Jobs::JobService> service,
                 std::shared_ptr<Utils::ILogger> logger = nullptr)
            : service_(std::move(service))
            , logger_(std::move(logger)) {}

        [[nodiscard]] bool can_handle(
            const std::string& method, const std::string& path) const override {
            (void)method;
            return path == "/api/jobs" || path_starts_with(path, "/api/jobs/") ||
                   path == "/api/runs" || path_starts_with(path, "/api/runs/");
        }

        [[nodiscard]] HttpResponse handle(const HttpRequest& request) override {
            const std::string& method = request.method;
            const auto seg = split_path(request.path); // {"api","jobs",...} or {"api","runs",...}

            const std::string& root = seg[1];

            if (root == "runs") {
                if (seg.size() == 2 && method == "GET") return list_runs(request, std::nullopt);
                if (seg.size() == 3 && method == "GET") return get_run(seg[2]);
                return reply_error(status::MethodNotAllowed,
                                   "Unsupported /api/runs request");
            }

            // root == "jobs"
            if (seg.size() == 2) {
                if (method == "GET") return list_jobs();
                if (method == "POST") return create_job(request);
                return reply_error(status::MethodNotAllowed, "Use GET or POST /api/jobs");
            }
            if (seg.size() == 3) {
                if (method == "GET") return get_job(seg[2]);
                if (method == "PUT") return update_job(request, seg[2]);
                if (method == "DELETE") return delete_job(seg[2]);
                return reply_error(status::MethodNotAllowed,
                                   "Use GET, PUT or DELETE /api/jobs/{id}");
            }
            if (seg.size() == 4 && seg[3] == "run" && method == "POST") {
                return run_job(seg[2]);
            }
            if (seg.size() == 4 && seg[3] == "runs" && method == "GET") {
                return list_runs(request, seg[2]);
            }
            return reply_error(status::NotFound, "Unknown jobs endpoint");
        }

        [[nodiscard]] std::vector<RouteInfo> get_routes() const override {
            return {
                {.method = "GET",    .path = "/api/jobs",            .description = "List jobs"},
                {.method = "POST",   .path = "/api/jobs",            .description = "Create a job"},
                {.method = "GET",    .path = "/api/jobs/{id}",       .description = "Get a job"},
                {.method = "PUT",    .path = "/api/jobs/{id}",       .description = "Update a job"},
                {.method = "DELETE", .path = "/api/jobs/{id}",       .description = "Delete a job"},
                {.method = "POST",   .path = "/api/jobs/{id}/run",   .description = "Run a job now"},
                {.method = "GET",    .path = "/api/jobs/{id}/runs",  .description = "Job run history"},
                {.method = "GET",    .path = "/api/runs",            .description = "Recent runs"},
                {.method = "GET",    .path = "/api/runs/{id}",       .description = "Run detail"}
            };
        }

    private:
        // ---- Jobs ----

        HttpResponse list_jobs() {
            auto jobs = service_->store()->list_jobs();
            Orcha::Json arr = Orcha::Json::array();
            for (const auto& job : jobs) arr.push_back(job.to_json());
            Orcha::Json out = Orcha::Json::object();
            out["jobs"] = arr;
            out["count"] = static_cast<int>(jobs.size());
            return reply_json(status::OK, out);
        }

        HttpResponse get_job(const std::string& id) {
            if (auto job = service_->store()->get_job(id))
                return reply_json(status::OK, job->to_json());
            return reply_error(status::NotFound, "No such job: " + id);
        }

        HttpResponse create_job(const HttpRequest& request) {
            Jobs::JobDefinition job;
            try {
                job = Jobs::JobDefinition::from_json(Orcha::Json::parse(request.body));
            } catch (const std::exception& ex) {
                return reply_error(status::BadRequest,
                                   std::string("Invalid JSON body: ") + ex.what());
            }
            if (auto err = validate(job)) {
                return reply_error(status::BadRequest, *err);
            }
            if (service_->store()->get_job_by_name(job.name)) {
                return reply_error(status::Conflict,
                                   "A job named '" + job.name + "' already exists");
            }
            if (!service_->store()->create_job(job)) {
                return reply_error(status::InternalError, "Failed to create job");
            }
            if (logger_) logger_->info("Created job '" + job.name + "'");
            return reply_json(status::Created, job.to_json());
        }

        HttpResponse update_job(const HttpRequest& request, const std::string& id) {
            auto existing = service_->store()->get_job(id);
            if (!existing) {
                return reply_error(status::NotFound, "No such job: " + id);
            }
            Jobs::JobDefinition job;
            try {
                job = Jobs::JobDefinition::from_json(Orcha::Json::parse(request.body));
            } catch (const std::exception& ex) {
                return reply_error(status::BadRequest,
                                   std::string("Invalid JSON body: ") + ex.what());
            }
            if (auto err = validate(job)) {
                return reply_error(status::BadRequest, *err);
            }
            // Name uniqueness (allow keeping the same name on this job).
            if (auto byName = service_->store()->get_job_by_name(job.name);
                byName && byName->id != id) {
                return reply_error(status::Conflict,
                                   "A job named '" + job.name + "' already exists");
            }
            job.id = id;
            job.created_at = existing->created_at;
            if (!service_->store()->update_job(job)) {
                return reply_error(status::InternalError, "Failed to update job");
            }
            auto updated = service_->store()->get_job(id);
            return reply_json(status::OK,
                              updated ? updated->to_json() : job.to_json());
        }

        HttpResponse delete_job(const std::string& id) {
            if (service_->store()->delete_job(id)) {
                Orcha::Json out = Orcha::Json::object();
                out["success"] = true;
                return reply_json(status::OK, out);
            }
            return reply_error(status::NotFound, "No such job: " + id);
        }

        HttpResponse run_job(const std::string& id) {
            auto run = service_->run_job(id, "manual");
            if (!run) {
                return reply_error(status::NotFound, "No such job: " + id);
            }
            return reply_json(status::OK, run->to_json());
        }

        // ---- Runs ----

        HttpResponse list_runs(const HttpRequest& request,
                               std::optional<std::string> job_id) {
            const size_t limit = parse_limit(request, 50);
            auto runs = service_->store()->list_runs(job_id, limit);
            Orcha::Json arr = Orcha::Json::array();
            for (const auto& run : runs) arr.push_back(run.to_json());
            Orcha::Json out = Orcha::Json::object();
            out["runs"] = arr;
            out["count"] = static_cast<int>(runs.size());
            return reply_json(status::OK, out);
        }

        HttpResponse get_run(const std::string& id) {
            if (auto run = service_->store()->get_run(id))
                return reply_json(status::OK, run->to_json());
            return reply_error(status::NotFound, "No such run: " + id);
        }

        // ---- Helpers ----

        /// Returns an error message if the job is invalid, else nullopt.
        static std::optional<std::string> validate(const Jobs::JobDefinition& job) {
            if (job.name.empty()) return "Job 'name' is required";
            if (!job.definition.contains("steps") ||
                !job.definition.at("steps").is_array()) {
                return "Job 'definition' must contain a 'steps' array";
            }
            return std::nullopt;
        }

        static size_t parse_limit(const HttpRequest& request, size_t def) {
            if (auto v = request.query_param("limit")) {
                try {
                    long n = std::stol(*v);
                    if (n > 0 && n <= 1000) return static_cast<size_t>(n);
                } catch (...) { /* ignore */ }
            }
            return def;
        }

        std::shared_ptr<Jobs::JobService> service_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
