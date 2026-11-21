// The pwcap executable: the imperative shell of the CLI. It owns the POSIX calls (signal handlers,
// writes to fd 1 and fd 2) and the lifetime of the Capture. Parsing and classification live in
// cli/args and cli/status, which are pure.

#include "cli/args.hpp"
#include "cli/render.hpp"
#include "cli/status.hpp"
#include "pwcap/capture.hpp"
#include "pwcap/list.hpp"

#include <sys/types.h>

#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace cli = pwcap::cli;

constexpr auto kPollInterval = std::chrono::milliseconds{50};

// SA_RESETHAND makes a second SIGINT or SIGTERM end the process at once, so a hung teardown can
// still be killed. SA_RESTART keeps the other system calls going.
constexpr int kStopFlags = static_cast<int>(SA_RESETHAND | SA_RESTART);

constexpr std::string_view kUsageLine = "usage: pwcap list [--json] | pwcap capture [OPTIONS]\n";

constexpr std::string_view kHelpText =
    "usage: pwcap list [--json]\n"
    "       pwcap capture --app NAME\n"
    "       pwcap --help\n"
    "\n"
    "list prints the applications that have an audio output stream: a header line, then one\n"
    "tab-separated row per stream. --json prints one JSON array instead.\n"
    "\n"
    "capture writes the audio of application NAME to stdout as raw PCM: S16LE, 16 kHz, mono.\n"
    "Diagnostics go to stderr. Stop the capture with SIGINT or SIGTERM.\n";

// Set by the stop signal handler, which may run on any thread. A lock-free atomic is the only state
// a signal handler may touch. The handler does not allocate, lock or write.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_stop_requested{false}; // a signal handler can reach only file-scope state
static_assert(std::atomic<bool>::is_always_lock_free);

// State shared by the library thread (Sink and Diagnostic) and the main thread. Declared before the
// Capture, so it outlives the Capture's destructor, which still calls the Sink while it drains.
struct Shared {
    std::atomic<bool> downstream_closed{false}; // the reader closed the pipe (EPIPE)
    std::atomic<int> write_errno{0};            // a write error other than EPIPE; 0 means none
    std::atomic<std::uint8_t> fatal{0}; // 0 means none, else the Error's underlying value + 1
};
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

extern "C" void on_stop_signal(int /*signo*/) noexcept { g_stop_requested.store(true); }

[[nodiscard]] bool install_handler(int signo, void (*handler)(int), int flags) noexcept {
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_flags = flags;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): sigaction exposes its handler
    // through a union
    action.sa_handler = handler;
    return ::sigaction(signo, &action, nullptr) == 0;
}

// The handlers are installed before the Capture opens, so a signal during open is already a stop.
// SIG_IGN for SIGPIPE turns a closed reader into EPIPE, which is a clean stop.
[[nodiscard]] bool install_signal_handling() noexcept {
    return install_handler(SIGINT, on_stop_signal, kStopFlags) &&
           install_handler(SIGTERM, on_stop_signal, kStopFlags) &&
           install_handler(SIGPIPE, SIG_IGN, 0);
}

// Writes all of text to fd and returns 0, or the errno that stopped the write. Never throws:
// std::println would throw when the fd is closed, and on the library thread that would end the
// process instead of stopping cleanly.
[[nodiscard]] int write_checked(int fd, std::string_view text) noexcept {
    // A raw loop: write(2) may accept part of the text or be interrupted, and no algorithm retries
    // that.
    while (!text.empty()) {
        const ssize_t written = ::write(fd, text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (written == 0) {
            return EIO;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return 0;
}

// Writes all of text to fd. A failed diagnostic write has nowhere to be reported, so it is dropped.
void write_text(int fd, std::string_view text) noexcept {
    static_cast<void>(write_checked(fd, text));
}

// Writes one PCM block to fd 1. A closed reader is recorded as a clean stop, any other failure in
// write_errno. Once either is set, the rest of the stream is dropped.
void write_pcm(Shared& shared, std::span<const std::byte> pcm) noexcept {
    if (shared.downstream_closed.load() || shared.write_errno.load() != 0) {
        return;
    }
    // A raw loop: write(2) may accept part of the block and the rest must be retried.
    while (!pcm.empty()) {
        const ssize_t written = ::write(STDOUT_FILENO, pcm.data(), pcm.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EPIPE) {
                shared.downstream_closed.store(true);
            } else {
                shared.write_errno.store(errno);
            }
            return;
        }
        if (written == 0) {
            shared.write_errno.store(EIO);
            return;
        }
        pcm = pcm.subspan(static_cast<std::size_t>(written));
    }
}

// True once the capture must end for a reason other than a fatal Error: a signal, a closed reader
// or a stdout error.
[[nodiscard]] bool stop_requested(const Shared& shared) noexcept {
    return g_stop_requested.load() || shared.downstream_closed.load() ||
           shared.write_errno.load() != 0;
}

[[nodiscard]] bool should_end(const Shared& shared) noexcept {
    return stop_requested(shared) || shared.fatal.load() != 0;
}

// Prints the message verbatim and latches a fatal Error. A fatal Error reported during a stop is
// teardown noise (for example a lost connection while the stream is destroyed), so it does not
// change the exit status.
void report(Shared& shared, bool quiet, std::string_view message) noexcept {
    if (!quiet) {
        write_text(STDERR_FILENO, message);
        write_text(STDERR_FILENO, "\n");
    }
    const std::optional<pwcap::Error> error = cli::error_prefix(message);
    if (error && cli::is_fatal(*error) && !stop_requested(shared)) {
        shared.fatal.store(static_cast<std::uint8_t>(std::to_underlying(*error) + 1U));
    }
}

[[nodiscard]] pwcap::Sink make_sink(Shared& shared) {
    return [&shared](std::span<const std::byte> pcm) noexcept { write_pcm(shared, pcm); };
}

[[nodiscard]] pwcap::Diagnostic make_diagnostic(Shared& shared, bool quiet) {
    return [&shared, quiet](std::string_view message) noexcept { report(shared, quiet, message); };
}

// Exit status in order of precedence: a stdout write error, then a fatal Error, then success.
[[nodiscard]] int exit_status(const Shared& shared) {
    if (const int err = shared.write_errno.load(); err != 0) {
        write_text(STDERR_FILENO,
                   std::format("pwcap: stdout: {}\n",
                               std::error_code(err, std::generic_category()).message()));
        return cli::kExitError;
    }
    if (const std::uint8_t fatal = shared.fatal.load(); fatal != 0) {
        const auto error = static_cast<pwcap::Error>(fatal - 1U);
        write_text(STDERR_FILENO, std::format("pwcap: {}\n", pwcap::to_string(error)));
        return cli::kExitError;
    }
    return cli::kExitOk;
}

[[nodiscard]] int run_capture(const cli::CaptureCommand& command) {
    if (!install_signal_handling()) {
        write_text(STDERR_FILENO, "pwcap: cannot install signal handlers\n");
        return cli::kExitError;
    }

    Shared shared;
    auto opened = pwcap::Capture::open(command.options, make_sink(shared),
                                       make_diagnostic(shared, command.quiet));
    if (!opened) {
        write_text(STDERR_FILENO, std::format("pwcap: {}\n", pwcap::to_string(opened.error())));
        return cli::kExitError;
    }

    std::optional<pwcap::Capture> capture{std::move(opened).value()};
    // A raw loop: the wait is on flags set by a signal handler and by the library thread. A signal
    // handler cannot notify a condition variable, so the wait polls.
    while (!should_end(shared)) {
        std::this_thread::sleep_for(kPollInterval);
    }
    // The destructor drains the ring into the Sink before it returns, so every captured byte
    // reaches fd 1 before the exit status is decided.
    capture.reset();
    return exit_status(shared);
}

// pwcap list: no signal handler, so a closed reader ends the process by SIGPIPE, as `ls | head`
// does. The listing is written with a checked write, so a stdout error is reported and exits 1.
[[nodiscard]] int run_list(const cli::ListCommand& command) {
    const std::expected<std::vector<pwcap::AppStream>, pwcap::Error> listed =
        pwcap::list_applications();
    if (!listed) {
        write_text(STDERR_FILENO, std::format("pwcap: {}\n", pwcap::to_string(listed.error())));
        return cli::kExitError;
    }
    const std::string text = command.json ? cli::render_json(*listed) : cli::render_text(*listed);
    if (const int err = write_checked(STDOUT_FILENO, text); err != 0) {
        write_text(STDERR_FILENO,
                   std::format("pwcap: stdout: {}\n",
                               std::error_code(err, std::generic_category()).message()));
        return cli::kExitError;
    }
    return cli::kExitOk;
}

void report_usage(const cli::UsageFailure& failure, std::span<const std::string_view> args) {
    write_text(STDERR_FILENO, std::format("pwcap: {}\n", cli::describe(failure.error)));
    if (failure.index < args.size()) {
        write_text(STDERR_FILENO, std::format("pwcap: at argument '{}'\n", args[failure.index]));
    }
    write_text(STDERR_FILENO, kUsageLine);
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> raw{argv, static_cast<std::size_t>(argc)};
    const auto tokens = std::ranges::to<std::vector<std::string_view>>(
        raw | std::views::drop(1) |
        std::views::transform([](char* text) { return std::string_view{text}; }));

    const auto parsed = cli::parse_command_line(tokens);
    if (!parsed) {
        report_usage(parsed.error(), tokens);
        return cli::kExitUsage;
    }
    if (std::holds_alternative<cli::HelpCommand>(*parsed)) {
        write_text(STDOUT_FILENO, kHelpText);
        return cli::kExitOk;
    }
    if (const auto* list = std::get_if<cli::ListCommand>(&*parsed)) {
        return run_list(*list);
    }
    return run_capture(std::get<cli::CaptureCommand>(*parsed));
}
