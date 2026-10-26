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
#include "../workflow/ExecutionContext.hpp"

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

        Orcha::Json execute(const Orcha::Json& params) override {
            auto svc = service_.lock();
            if (!svc) {
                throw std::runtime_error("run_job: job service is unavailable");
            }
            if (!params.contains("job") || !params.at("job").is_string()) {
                throw std::runtime_error("run_job: required string parameter 'job' is missing");
            }
            const std::string ref = params.at("job").get<std::string>();

            // Resolve by id first, then by name.
            auto job = svc->store()->get_job(ref);
            if (!job) job = svc->store()->get_job_by_name(ref);
            if (!job) {
                throw std::runtime_error("run_job: no job named or with id '" + ref + "'");
            }

            // Recursion / cycle guard. The stack lives in the workflow
            // ExecutionContext, which engines propagate to the threads they
            // spawn, so `parallel: true` steps cannot bypass it.
            const auto& stack = Workflow::ExecutionContext::job_stack();
            if (std::find(stack.begin(), stack.end(), job->id) != stack.end()) {
                throw std::runtime_error(
                    "run_job: cycle detected involving job '" + job->name + "'");
            }
            if (stack.size() >= kMaxDepth) {
                throw std::runtime_error(
                    "run_job: maximum nesting depth (" + std::to_string(kMaxDepth) + ") exceeded");
            }

            std::optional<RunRecord> run;
            {
                Workflow::ExecutionContext::JobFrame frame(job->id);
                run = svc->run_job(job->id, "nested");
            }

            if (!run) {
                throw std::runtime_error("run_job: job '" + ref + "' could not be run");
            }
            if (run->status != "success") {
                throw std::runtime_error(
                    "run_job: sub-job '" + job->name + "' failed: " + run->error);
            }

            Orcha::Json out = Orcha::Json::object();
            out["job"] = job->name;
            out["run_id"] = run->id;
            out["status"] = run->status;
            out["result"] = run->result;
            // Convenience: surface the sub-job's last step output as "last".
            if (run->result.is_array() && run->result.size() > 0) {
                const auto& arr = run->result;
                const auto& last_step = arr.at(arr.size() - 1);
                if (last_step.contains("output")) {
                    out["last"] = last_step.at("output");
                }
            }
            return out;
        }

    private:
        static constexpr size_t kMaxDepth = 16;
        std::weak_ptr<JobService> service_;
    };

} // namespace Orcha::Jobs
