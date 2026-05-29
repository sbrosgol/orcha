//
// RunJobCommand.hpp - Built-in command that runs another job as a sub-workflow
// (Phase 2 follow-up: job-to-job linking).
//
// Usage inside a job definition:
//   { "command": "run_job", "params": { "job": "<name-or-id>" } }
//
// The referenced job runs synchronously; its result is returned as this step's
// output, so later steps can reference it via {{stepN.output.status}},
// {{stepN.output.last.<field>}}, etc. A failed sub-job fails this step.
//
// Holds a weak_ptr to JobService: the registry owns this command and JobService
// (via its engine factory) owns the registry, so a strong ref would form a cycle.
//

#pragma once

#include "../core/ICommand.hpp"
#include "JobService.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Orcha::Jobs {

    class RunJobCommand : public Core::ICommand {
    public:
        explicit RunJobCommand(std::weak_ptr<JobService> service)
            : service_(std::move(service)) {}

        [[nodiscard]] std::string name() const override { return "run_job"; }

        [[nodiscard]] Core::CommandMetadata metadata() const override {
            Core::CommandMetadata meta;
            meta.name = "run_job";
            meta.description = "Run another saved job as a sub-workflow and return its result";
            meta.tags = {"jobs", "control-flow"};
            Core::CommandParameter p;
            p.name = "job";
            p.type = "string";
            p.required = true;
            p.description = "Name or id of the job to run";
            meta.parameters.push_back(p);
            return meta;
        }

        web::json::value execute(const web::json::value& params) override {
            auto svc = service_.lock();
            if (!svc) {
                throw std::runtime_error("run_job: job service is unavailable");
            }
            if (!params.has_field(U("job")) || !params.at(U("job")).is_string()) {
                throw std::runtime_error("run_job: required string parameter 'job' is missing");
            }
            const std::string ref =
                utility::conversions::to_utf8string(params.at(U("job")).as_string());

            // Resolve by id first, then by name.
            auto job = svc->store()->get_job(ref);
            if (!job) job = svc->store()->get_job_by_name(ref);
            if (!job) {
                throw std::runtime_error("run_job: no job named or with id '" + ref + "'");
            }

            // Recursion / cycle guard (synchronous, thread-local call stack).
            if (std::find(s_stack.begin(), s_stack.end(), job->id) != s_stack.end()) {
                throw std::runtime_error(
                    "run_job: cycle detected involving job '" + job->name + "'");
            }
            if (s_stack.size() >= kMaxDepth) {
                throw std::runtime_error(
                    "run_job: maximum nesting depth (" + std::to_string(kMaxDepth) + ") exceeded");
            }

            s_stack.push_back(job->id);
            std::optional<RunRecord> run;
            try {
                run = svc->run_job(job->id, "nested");
            } catch (...) {
                s_stack.pop_back();
                throw;
            }
            s_stack.pop_back();

            if (!run) {
                throw std::runtime_error("run_job: job '" + ref + "' could not be run");
            }
            if (run->status != "success") {
                throw std::runtime_error(
                    "run_job: sub-job '" + job->name + "' failed: " + run->error);
            }

            web::json::value out = web::json::value::object();
            out[U("job")] = web::json::value::string(
                utility::conversions::to_string_t(job->name));
            out[U("run_id")] = web::json::value::string(
                utility::conversions::to_string_t(run->id));
            out[U("status")] = web::json::value::string(
                utility::conversions::to_string_t(run->status));
            out[U("result")] = run->result;
            // Convenience: surface the sub-job's last step output as "last".
            if (run->result.is_array() && run->result.as_array().size() > 0) {
                const auto& arr = run->result.as_array();
                const auto& last_step = arr.at(arr.size() - 1);
                if (last_step.has_field(U("output"))) {
                    out[U("last")] = last_step.at(U("output"));
                }
            }
            return out;
        }

    private:
        static constexpr size_t kMaxDepth = 16;
        // Per-thread call stack of job ids currently executing, for cycle/depth detection.
        inline static thread_local std::vector<std::string> s_stack;

        std::weak_ptr<JobService> service_;
    };

} // namespace Orcha::Jobs
