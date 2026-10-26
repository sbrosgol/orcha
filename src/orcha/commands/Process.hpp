//
// Process.hpp - Run a shell command as a cancellable child process.
//
// The command runs as `/bin/sh -c <command>` in its own process group, with
// stdout+stderr optionally captured through a pipe. While it runs, the caller's
// ExecutionControl is polled; once it asks to stop (cancelled or past its
// deadline) the whole group gets SIGTERM, then SIGKILL after a grace period, so
// child processes the shell started are terminated too. Header-only so each
// independently built plugin can include it.
//
// Windows has no process groups/posix_spawn; there the command runs through
// _popen/system and cannot be cancelled.
//

#pragma once

#include "../core/ExecutionControl.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

#if defined(_WIN32)
  #define ORCHA_POPEN  _popen
  #define ORCHA_PCLOSE _pclose
#else
  #include <cerrno>
  #include <csignal>
  #include <cstring>
  #include <fcntl.h>
  #include <poll.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <crt_externs.h>
  #else
    extern char** environ;
  #endif
#endif

namespace Orcha::Process {

    struct Result {
        int exit_code = -1;      ///< Exit status, 128+N if killed by signal N.
        std::string output;      ///< Merged stdout/stderr (when captured).
        bool timed_out = false;  ///< Killed because the deadline passed.
        bool cancelled = false;  ///< Killed because the run was cancelled.
    };

#if defined(_WIN32)

    inline Result run_shell(const std::string& command, bool capture,
                            const Core::ExecutionControl& /*control*/,
                            std::chrono::milliseconds /*grace*/ = std::chrono::seconds(2)) {
        Result result;
        if (capture) {
            FILE* raw = ORCHA_POPEN((command + " 2>&1").c_str(), "r");
            if (!raw) {
                throw std::runtime_error("Failed to launch process");
            }
            std::array<char, 4096> buf{};
            while (std::fgets(buf.data(), static_cast<int>(buf.size()), raw) != nullptr) {
                result.output.append(buf.data());
            }
            result.exit_code = ORCHA_PCLOSE(raw);
        } else {
            result.exit_code = std::system(command.c_str());
        }
        return result;
    }

#else

    namespace detail {

        inline char** environment() {
  #if defined(__APPLE__)
            return *_NSGetEnviron();
  #else
            return environ;
  #endif
        }

        inline int decode_status(int status) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return -1;
        }

        /// Owns a pipe's file descriptors.
        struct Pipe {
            int read_fd = -1;
            int write_fd = -1;
            ~Pipe() { close_read(); close_write(); }
            void close_read() { if (read_fd >= 0) { ::close(read_fd); read_fd = -1; } }
            void close_write() { if (write_fd >= 0) { ::close(write_fd); write_fd = -1; } }
        };

        /// Owns posix_spawn attribute/file-action objects.
        struct SpawnSetup {
            posix_spawnattr_t attr{};
            posix_spawn_file_actions_t actions{};
            SpawnSetup() {
                posix_spawnattr_init(&attr);
                posix_spawn_file_actions_init(&actions);
            }
            ~SpawnSetup() {
                posix_spawn_file_actions_destroy(&actions);
                posix_spawnattr_destroy(&attr);
            }
            SpawnSetup(const SpawnSetup&) = delete;
            SpawnSetup& operator=(const SpawnSetup&) = delete;
        };

        /// Read whatever is available on a non-blocking fd. Returns false on EOF.
        inline bool drain(int fd, std::string& out) {
            std::array<char, 4096> buf{};
            for (;;) {
                const ssize_t n = ::read(fd, buf.data(), buf.size());
                if (n > 0) { out.append(buf.data(), static_cast<size_t>(n)); continue; }
                if (n == 0) return false;                       // EOF
                if (errno == EINTR) continue;
                return true;                                    // EAGAIN: nothing more now
            }
        }

    } // namespace detail

    /**
     * @brief Run `/bin/sh -c command`, honouring @p control.
     * @param capture Merge stdout/stderr into Result::output (else inherited).
     * @param grace   Time between SIGTERM and SIGKILL when stopping.
     * @throws std::runtime_error if the process cannot be started.
     */
    inline Result run_shell(const std::string& command, bool capture,
                            const Core::ExecutionControl& control,
                            std::chrono::milliseconds grace = std::chrono::seconds(2)) {
        using namespace std::chrono_literals;

        detail::Pipe pipe_fds;
        detail::SpawnSetup setup;

        if (capture) {
            int fds[2];
            if (::pipe(fds) != 0) {
                throw std::runtime_error(std::string("pipe() failed: ") + std::strerror(errno));
            }
            pipe_fds.read_fd = fds[0];
            pipe_fds.write_fd = fds[1];
            posix_spawn_file_actions_adddup2(&setup.actions, pipe_fds.write_fd, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&setup.actions, pipe_fds.write_fd, STDERR_FILENO);
            posix_spawn_file_actions_addclose(&setup.actions, pipe_fds.read_fd);
            posix_spawn_file_actions_addclose(&setup.actions, pipe_fds.write_fd);
        }

        // Own process group (so the whole tree can be signalled), default
        // signal dispositions and an empty mask regardless of the host's.
        sigset_t defaults;
        sigemptyset(&defaults);
        for (int sig : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT}) sigaddset(&defaults, sig);
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        posix_spawnattr_setsigdefault(&setup.attr, &defaults);
        posix_spawnattr_setsigmask(&setup.attr, &empty_mask);
        posix_spawnattr_setpgroup(&setup.attr, 0);
        posix_spawnattr_setflags(&setup.attr,
            POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

        std::string shell = "/bin/sh";
        std::string dash_c = "-c";
        std::string cmd = command;
        char* argv[] = {shell.data(), dash_c.data(), cmd.data(), nullptr};

        pid_t pid = -1;
        const int rc = posix_spawn(&pid, "/bin/sh", &setup.actions, &setup.attr,
                                   argv, detail::environment());
        if (rc != 0) {
            throw std::runtime_error(std::string("Failed to launch process: ") + std::strerror(rc));
        }
        pipe_fds.close_write();
        if (capture) {
            ::fcntl(pipe_fds.read_fd, F_SETFL, ::fcntl(pipe_fds.read_fd, F_GETFL) | O_NONBLOCK);
        }

        Result result;
        int status = 0;
        bool exited = false;
        bool pipe_open = capture;

        while (!exited) {
            if (pipe_open) {
                pollfd pfd{pipe_fds.read_fd, POLLIN, 0};
                if (::poll(&pfd, 1, 50) > 0) {
                    pipe_open = detail::drain(pipe_fds.read_fd, result.output);
                }
            } else {
                std::this_thread::sleep_for(20ms);
            }

            const pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                exited = true;
                break;
            }

            if (control.should_stop()) {
                result.cancelled = control.stop_requested();
                result.timed_out = !result.cancelled;
                ::kill(-pid, SIGTERM);
                const auto kill_at = std::chrono::steady_clock::now() + grace;
                while (::waitpid(pid, &status, WNOHANG) == 0) {
                    if (std::chrono::steady_clock::now() >= kill_at) {
                        ::kill(-pid, SIGKILL);
                        ::waitpid(pid, &status, 0);
                        break;
                    }
                    std::this_thread::sleep_for(20ms);
                }
                ::kill(-pid, SIGKILL);   // stragglers in the group, if any
                exited = true;
            }
        }

        // Collect output still buffered in the pipe. Do not wait for EOF:
        // a background grandchild may hold the write end open indefinitely.
        if (pipe_open) {
            (void)detail::drain(pipe_fds.read_fd, result.output);
        }
        result.exit_code = detail::decode_status(status);
        return result;
    }

#endif

} // namespace Orcha::Process
