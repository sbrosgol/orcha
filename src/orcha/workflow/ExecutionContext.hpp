//
// ExecutionContext.hpp - Per-run context that follows a workflow across threads.
//
// Workflow execution is synchronous per thread, but engines hand steps to other
// threads (`parallel: true` steps, DAG levels). State that must follow the
// logical run rather than the OS thread -- currently the stack of job ids used
// by `run_job` for cycle/depth detection -- lives here. Engines capture() it
// before spawning a task and the task installs it with a Scope, so a sub-job
// started on a worker thread still sees its ancestors.
//

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Orcha::Workflow {

    class ExecutionContext {
    public:
        /// Snapshot of the calling thread's context, for handing to another thread.
        struct Snapshot {
            std::vector<std::string> job_stack;
        };

        [[nodiscard]] static Snapshot capture() { return {current().job_stack}; }

        /// Job ids of the run_job chain currently executing on this thread.
        [[nodiscard]] static const std::vector<std::string>& job_stack() {
            return current().job_stack;
        }

        /**
         * @brief RAII: install a snapshot on this thread, restore the previous
         *        context on destruction.
         */
        class Scope {
        public:
            explicit Scope(Snapshot snapshot)
                : saved_(std::exchange(current(), std::move(snapshot))) {}
            ~Scope() { current() = std::move(saved_); }
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

        private:
            Snapshot saved_;
        };

        /**
         * @brief RAII: push a job id for the duration of a nested run.
         */
        class JobFrame {
        public:
            explicit JobFrame(std::string job_id) {
                current().job_stack.push_back(std::move(job_id));
            }
            ~JobFrame() { current().job_stack.pop_back(); }
            JobFrame(const JobFrame&) = delete;
            JobFrame& operator=(const JobFrame&) = delete;
        };

    private:
        static Snapshot& current() {
            thread_local Snapshot ctx;
            return ctx;
        }
    };

} // namespace Orcha::Workflow
