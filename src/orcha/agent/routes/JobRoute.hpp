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

        void handle(web::http::http_request request) override {
            const auto method = utility::conversions::to_utf8string(request.method());
            const auto path = utility::conversions::to_utf8string(request.request_uri().path());
            const auto seg = split_path(path); // {"api","jobs",...} or {"api","runs",...}

            const std::string& root = seg[1];

            if (root == "runs") {
                if (seg.size() == 2 && method == "GET") return list_runs(request, std::nullopt);
                if (seg.size() == 3 && method == "GET") return get_run(request, seg[2]);
                return reply_error(request, web::http::status_codes::MethodNotAllowed,
                                   "Unsupported /api/runs request");
            }

            // root == "jobs"
            if (seg.size() == 2) {
                if (method == "GET") return list_jobs(request);
                if (method == "POST") return create_job(request);
                return reply_error(request, web::http::status_codes::MethodNotAllowed,
                                   "Use GET or POST /api/jobs");
            }
            if (seg.size() == 3) {
                if (method == "GET") return get_job(request, seg[2]);
                if (method == "PUT") return update_job(request, seg[2]);
                if (method == "DELETE") return delete_job(request, seg[2]);
                return reply_error(request, web::http::status_codes::MethodNotAllowed,
                                   "Use GET, PUT or DELETE /api/jobs/{id}");
            }
            if (seg.size() == 4 && seg[3] == "run" && method == "POST") {
                return run_job(request, seg[2]);
            }
            if (seg.size() == 4 && seg[3] == "runs" && method == "GET") {
                return list_runs(request, seg[2]);
            }
            reply_error(request, web::http::status_codes::NotFound, "Unknown jobs endpoint");
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

        void list_jobs(web::http::http_request request) {
            auto service = service_;
            pplx::create_task([=]() {
                auto jobs = service->store()->list_jobs();
                web::json::value arr = web::json::value::array(jobs.size());
                for (size_t i = 0; i < jobs.size(); ++i) arr[i] = jobs[i].to_json();
                web::json::value out = web::json::value::object();
                out[U("jobs")] = arr;
                out[U("count")] = web::json::value::number(static_cast<int>(jobs.size()));
                reply_json(request, web::http::status_codes::OK, out);
            });
        }

        void get_job(web::http::http_request request, std::string id) {
            auto service = service_;
            pplx::create_task([=]() {
                if (auto job = service->store()->get_job(id))
                    reply_json(request, web::http::status_codes::OK, job->to_json());
                else
                    reply_error(request, web::http::status_codes::NotFound, "No such job: " + id);
            });
        }

        void create_job(web::http::http_request request) {
            auto service = service_;
            auto logger = logger_;
            request.extract_json().then([=](pplx::task<web::json::value> bt) {
                Jobs::JobDefinition job;
                try {
                    job = Jobs::JobDefinition::from_json(bt.get());
                } catch (const std::exception& ex) {
                    return reply_error(request, web::http::status_codes::BadRequest,
                                       std::string("Invalid JSON body: ") + ex.what());
                }
                if (auto err = validate(job)) {
                    return reply_error(request, web::http::status_codes::BadRequest, *err);
                }
                if (service->store()->get_job_by_name(job.name)) {
                    return reply_error(request, web::http::status_codes::Conflict,
                                       "A job named '" + job.name + "' already exists");
                }
                if (!service->store()->create_job(job)) {
                    return reply_error(request, web::http::status_codes::InternalError,
                                       "Failed to create job");
                }
                if (logger) logger->info("Created job '" + job.name + "'");
                reply_json(request, web::http::status_codes::Created, job.to_json());
            });
        }

        void update_job(web::http::http_request request, std::string id) {
            auto service = service_;
            request.extract_json().then([=](pplx::task<web::json::value> bt) {
                auto existing = service->store()->get_job(id);
                if (!existing) {
                    return reply_error(request, web::http::status_codes::NotFound,
                                       "No such job: " + id);
                }
                Jobs::JobDefinition job;
                try {
                    job = Jobs::JobDefinition::from_json(bt.get());
                } catch (const std::exception& ex) {
                    return reply_error(request, web::http::status_codes::BadRequest,
                                       std::string("Invalid JSON body: ") + ex.what());
                }
                if (auto err = validate(job)) {
                    return reply_error(request, web::http::status_codes::BadRequest, *err);
                }
                // Name uniqueness (allow keeping the same name on this job).
                if (auto byName = service->store()->get_job_by_name(job.name);
                    byName && byName->id != id) {
                    return reply_error(request, web::http::status_codes::Conflict,
                                       "A job named '" + job.name + "' already exists");
                }
                job.id = id;
                job.created_at = existing->created_at;
                if (!service->store()->update_job(job)) {
                    return reply_error(request, web::http::status_codes::InternalError,
                                       "Failed to update job");
                }
                auto updated = service->store()->get_job(id);
                reply_json(request, web::http::status_codes::OK,
                           updated ? updated->to_json() : job.to_json());
            });
        }

        void delete_job(web::http::http_request request, std::string id) {
            auto service = service_;
            pplx::create_task([=]() {
                if (service->store()->delete_job(id)) {
                    web::json::value out = web::json::value::object();
                    out[U("success")] = web::json::value::boolean(true);
                    reply_json(request, web::http::status_codes::OK, out);
                } else {
                    reply_error(request, web::http::status_codes::NotFound, "No such job: " + id);
                }
            });
        }

        void run_job(web::http::http_request request, std::string id) {
            auto service = service_;
            pplx::create_task([=]() {
                auto run = service->run_job(id, "manual");
                if (!run) {
                    return reply_error(request, web::http::status_codes::NotFound,
                                       "No such job: " + id);
                }
                reply_json(request, web::http::status_codes::OK, run->to_json());
            });
        }

        // ---- Runs ----

        void list_runs(web::http::http_request request, std::optional<std::string> job_id) {
            auto service = service_;
            const size_t limit = parse_limit(request, 50);
            pplx::create_task([=]() {
                auto runs = service->store()->list_runs(job_id, limit);
                web::json::value arr = web::json::value::array(runs.size());
                for (size_t i = 0; i < runs.size(); ++i) arr[i] = runs[i].to_json();
                web::json::value out = web::json::value::object();
                out[U("runs")] = arr;
                out[U("count")] = web::json::value::number(static_cast<int>(runs.size()));
                reply_json(request, web::http::status_codes::OK, out);
            });
        }

        void get_run(web::http::http_request request, std::string id) {
            auto service = service_;
            pplx::create_task([=]() {
                if (auto run = service->store()->get_run(id))
                    reply_json(request, web::http::status_codes::OK, run->to_json());
                else
                    reply_error(request, web::http::status_codes::NotFound, "No such run: " + id);
            });
        }

        // ---- Helpers ----

        /// Returns an error message if the job is invalid, else nullopt.
        static std::optional<std::string> validate(const Jobs::JobDefinition& job) {
            if (job.name.empty()) return "Job 'name' is required";
            if (!job.definition.has_field(U("steps")) ||
                !job.definition.at(U("steps")).is_array()) {
                return "Job 'definition' must contain a 'steps' array";
            }
            return std::nullopt;
        }

        static size_t parse_limit(const web::http::http_request& request, size_t def) {
            auto q = web::uri::split_query(request.request_uri().query());
            auto it = q.find(U("limit"));
            if (it != q.end()) {
                try {
                    long v = std::stol(utility::conversions::to_utf8string(it->second));
                    if (v > 0 && v <= 1000) return static_cast<size_t>(v);
                } catch (...) { /* ignore */ }
            }
            return def;
        }

        static void reply_json(web::http::http_request request,
                               web::http::status_code code,
                               const web::json::value& body) {
            web::http::http_response resp(code);
            resp.headers().add(web::http::header_names::content_type, U("application/json"));
            resp.set_body(body);
            request.reply(resp);
        }

        static void reply_error(web::http::http_request request,
                                web::http::status_code code,
                                const std::string& message) {
            web::json::value body = web::json::value::object();
            body[U("error")] = web::json::value::string(
                utility::conversions::to_string_t(message));
            reply_json(request, code, body);
        }

        std::shared_ptr<Jobs::JobService> service_;
        std::shared_ptr<Utils::ILogger> logger_;
    };

} // namespace Orcha::Agent::Routes
