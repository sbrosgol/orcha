//
// RunResponses.hpp - HTTP representations shared by the run endpoints
// (/workflow/runs/* for API clients, /api/runs/* for the admin API).
//

#pragma once

#include "../HttpJson.hpp"
#include "../../jobs/RunQueue.hpp"

#include <string>

namespace Orcha::Agent::Routes {

    /// A run resource with links: `{...RunRecord, "links": {"self", "cancel"}}`.
    [[nodiscard]] inline Orcha::Json run_resource(const Jobs::RunRecord& run, const std::string& base) {
        Orcha::Json o = run.to_json();
        const std::string self = base + "/" + run.id;
        o["links"] = Orcha::Json{{"self", self}, {"cancel", self + "/cancel"}};
        return o;
    }

    /// 202 Accepted for a run that is not finished yet, with its Location.
    [[nodiscard]] inline HttpResponse reply_run_accepted(const Jobs::RunRecord& run, const std::string& base) {
        return reply_json(status::Accepted, run_resource(run, base))
            .with_header("Location", base + "/" + run.id);
    }

    /// 200 for a finished (or currently known) run.
    [[nodiscard]] inline HttpResponse reply_run(const Jobs::RunRecord& run, const std::string& base) {
        return reply_json(status::OK, run_resource(run, base));
    }

    /// Map a rejected submission to 429 (queue full) / 503 (shutting down) / 500.
    [[nodiscard]] inline HttpResponse reply_submit_error(Jobs::RunQueue::SubmitError error,
                                                         std::chrono::seconds retry_after) {
        using E = Jobs::RunQueue::SubmitError;
        switch (error) {
            case E::QueueFull: {
                Orcha::Json body{{"error", "The run queue is full; retry later"},
                                 {"retry_after_seconds", retry_after.count()}};
                return reply_json(status::TooManyRequests, body)
                    .with_header("Retry-After", std::to_string(retry_after.count()));
            }
            case E::ShuttingDown:
                return reply_error(status::ServiceUnavailable, "The server is shutting down")
                    .with_header("Retry-After", "30");
            case E::StoreError:
                break;
        }
        return reply_error(status::InternalError, "The run could not be recorded");
    }

    /// Response for POST <base>/{id}/cancel.
    [[nodiscard]] inline HttpResponse reply_cancel(Jobs::RunQueue& runs, const std::string& run_id,
                                                   const std::string& base) {
        using C = Jobs::RunQueue::CancelOutcome;
        const auto outcome = runs.cancel(run_id);
        auto run = runs.get(run_id);
        switch (outcome) {
            case C::NotFound:
                return reply_error(status::NotFound, "No such run: " + run_id);
            case C::AlreadyFinished: {
                Orcha::Json body{{"error", "The run has already finished"}};
                if (run) body["run"] = run_resource(*run, base);
                return reply_json(status::Conflict, body);
            }
            case C::Cancelled:
                return run ? reply_run(*run, base) : reply_error(status::NotFound, "No such run: " + run_id);
            case C::Cancelling:
                // Cooperative: the run stops at its next cancellation point.
                return run ? reply_run_accepted(*run, base)
                           : reply_error(status::NotFound, "No such run: " + run_id);
            case C::NotCancellable: {
                Orcha::Json body{{"error", "nested run: cancel the run that started it"}};
                if (run) body["run"] = run_resource(*run, base);
                return reply_json(status::Conflict, body);
            }
        }
        return reply_error(status::InternalError, "Unexpected cancel outcome");
    }

} // namespace Orcha::Agent::Routes
