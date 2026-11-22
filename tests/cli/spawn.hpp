#ifndef PWCAP_TESTS_CLI_SPAWN_HPP
#define PWCAP_TESTS_CLI_SPAWN_HPP

// Test-only process helper for the pwcap CLI tests. It starts a program with posix_spawn, sends it
// signals, waits for it with a bound, and kills and reaps it when its owner goes away. Standard
// output goes to a file or to a pipe that the test reads; standard error goes to a file. The
// environment is the parent's, with PIPEWIRE_REMOTE replaced when a test asks for it.
//
// Only std and POSIX are used, so tests under tests/cli can include this header without the live
// harness in tests/pipewire. Spawn failures throw: this is test code, and the public-API rules do
// not apply to it.

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace pwcap::test {

// Standard output goes to a pipe; the test reads it through Spawned::stdout_fd().
struct StdoutToPipe {};

// Standard output goes to this file, created or truncated.
using StdoutTarget = std::variant<std::filesystem::path, StdoutToPipe>;

namespace spawn_detail {

inline constexpr int kCreateFlags = O_WRONLY | O_CREAT | O_TRUNC;
inline constexpr mode_t kCreateMode = 0644;

// Owns one file descriptor. Private to this header, so it does not clash with the UniqueFd that
// pw_session.hpp defines in the same namespace.
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~Fd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

// The "NAME=value" strings of the parent's environment, with PIPEWIRE_REMOTE replaced when given.
[[nodiscard]] inline std::vector<std::string>
environment_with(const std::optional<std::string>& pipewire_remote) {
    std::vector<std::string> env;
    // A raw loop: environ is a null-terminated C array, and no range adaptor covers it.
    for (char** entry = environ; *entry != nullptr; ++entry) {
        env.emplace_back(*entry);
    }
    if (pipewire_remote) {
        std::erase_if(
            env, [](const std::string& entry) { return entry.starts_with("PIPEWIRE_REMOTE="); });
        env.push_back("PIPEWIRE_REMOTE=" + *pipewire_remote);
    }
    return env;
}

// The argv shape posix_spawnp wants: pointers into `storage`, then a null.
[[nodiscard]] inline std::vector<char*> pointers_of(std::vector<std::string>& storage) {
    std::vector<char*> pointers;
    pointers.reserve(storage.size() + 1);
    std::ranges::transform(
        storage, std::back_inserter(pointers),
        // cppcheck-suppress constParameterReference ; argv is char*, so data() must be non-const
        [](std::string& entry) { return entry.data(); });
    pointers.push_back(nullptr);
    return pointers;
}

// The exit status of a waited-for child: its code, or 128 plus the signal number when a signal
// killed it.
[[nodiscard]] inline int status_of(int raw) noexcept {
    if (WIFEXITED(raw)) {
        return WEXITSTATUS(raw);
    }
    if (WIFSIGNALED(raw)) {
        return 128 + WTERMSIG(raw);
    }
    return -1;
}

} // namespace spawn_detail

// A child process started with posix_spawnp. Move-only. The destructor sends SIGKILL to the child
// when it is still running and reaps it, so a failed test leaks no process.
class Spawned {
public:
    // Starts argv[0] with the whole argv as its arguments. `pipewire_remote`, when set, becomes
    // PIPEWIRE_REMOTE in the child's environment. Throws std::system_error when the program cannot
    // start.
    Spawned(const std::vector<std::string>& argv, const StdoutTarget& stdout_target,
            const std::filesystem::path& stderr_file,
            const std::optional<std::string>& pipewire_remote = std::nullopt) {
        if (argv.empty()) {
            throw std::invalid_argument("Spawned needs a program name");
        }
        std::vector<std::string> arguments_storage = argv;
        std::vector<char*> arguments = spawn_detail::pointers_of(arguments_storage);
        std::vector<std::string> env_storage = spawn_detail::environment_with(pipewire_remote);
        std::vector<char*> environment = spawn_detail::pointers_of(env_storage);

        spawn_detail::Fd write_end;
        if (std::holds_alternative<StdoutToPipe>(stdout_target)) {
            std::array<int, 2> ends{-1, -1};
            // O_CLOEXEC keeps the read end out of every later child. Otherwise a later spawn would
            // inherit it, and a closed reader would never produce EPIPE here.
            if (::pipe2(ends.data(), O_CLOEXEC) != 0) {
                throw std::system_error(errno, std::generic_category(), "Spawned: pipe2");
            }
            stdout_read_ = spawn_detail::Fd{ends[0]};
            write_end = spawn_detail::Fd{ends[1]};
        }

        // Only read when stdout goes to a file. The pipe case never uses the path.
        const std::string stdout_path =
            std::holds_alternative<StdoutToPipe>(stdout_target)
                ? std::string{}
                : std::get<std::filesystem::path>(stdout_target).string();
        const std::string stderr_path = stderr_file.string();

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        if (write_end.get() >= 0) {
            posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
        } else {
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_path.c_str(),
                                             spawn_detail::kCreateFlags, spawn_detail::kCreateMode);
        }
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_path.c_str(),
                                         spawn_detail::kCreateFlags, spawn_detail::kCreateMode);

        pid_t pid = 0;
        const int result = ::posix_spawnp(&pid, arguments.front(), &actions, nullptr,
                                          arguments.data(), environment.data());
        posix_spawn_file_actions_destroy(&actions);
        write_end.reset(); // the child holds its own copy of the write end now
        if (result != 0) {
            stdout_read_.reset();
            throw std::system_error(result, std::generic_category(),
                                    "Spawned: cannot start " + argv.front());
        }
        pid_ = pid;
    }

    Spawned(const Spawned&) = delete;
    Spawned& operator=(const Spawned&) = delete;
    Spawned(Spawned&& other) noexcept
        : pid_(std::exchange(other.pid_, -1)), reaped_(std::exchange(other.reaped_, true)),
          status_(std::exchange(other.status_, std::nullopt)),
          stdout_read_(std::move(other.stdout_read_)) {}
    Spawned& operator=(Spawned&& other) noexcept {
        if (this != &other) {
            kill_and_reap();
            pid_ = std::exchange(other.pid_, -1);
            reaped_ = std::exchange(other.reaped_, true);
            status_ = std::exchange(other.status_, std::nullopt);
            stdout_read_ = std::move(other.stdout_read_);
        }
        return *this;
    }
    ~Spawned() { kill_and_reap(); }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    // The read end of the stdout pipe, or -1 when stdout goes to a file.
    [[nodiscard]] int stdout_fd() const noexcept { return stdout_read_.get(); }

    // Closes the read end, so the child's next write fails with EPIPE.
    void close_stdout_pipe() noexcept { stdout_read_.reset(); }

    // Sends `signal` unless the child has been reaped. Returns false when it cannot be delivered.
    bool send(int signal) noexcept { return !reaped_ && pid_ > 0 && ::kill(pid_, signal) == 0; }

    // Waits up to `timeout` for the child to exit. Returns its exit status (see
    // spawn_detail::status_of), or nullopt when it is still running at the deadline.
    [[nodiscard]] std::optional<int> wait_for(std::chrono::milliseconds timeout) {
        if (reaped_) {
            return status_;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        // A raw loop: waitpid(WNOHANG) has no timeout, so the wait polls against a deadline. The
        // poll is 10 ms; a child that exits sooner is seen on the next poll.
        for (;;) {
            int raw = 0;
            const pid_t done = ::waitpid(pid_, &raw, WNOHANG);
            if (done == pid_) {
                reaped_ = true;
                status_ = spawn_detail::status_of(raw);
                return status_;
            }
            if (done < 0 && errno != EINTR) {
                throw std::system_error(errno, std::generic_category(), "Spawned: waitpid");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

private:
    // SIGKILL and reap, but only a child this object started and has not yet reaped. A reaped pid
    // may already belong to another process, so it is never signalled again.
    void kill_and_reap() noexcept {
        if (pid_ <= 0 || reaped_) {
            return;
        }
        ::kill(pid_, SIGKILL);
        int raw = 0;
        pid_t done = 0;
        // A raw loop: waitpid returns -1 with EINTR when a signal interrupts it, and must be
        // retried.
        do {
            done = ::waitpid(pid_, &raw, 0);
        } while (done < 0 && errno == EINTR);
        reaped_ = true;
        status_ = spawn_detail::status_of(raw);
    }

    pid_t pid_{-1};
    bool reaped_{false};
    std::optional<int> status_;
    spawn_detail::Fd stdout_read_;
};

} // namespace pwcap::test

#endif // PWCAP_TESTS_CLI_SPAWN_HPP
