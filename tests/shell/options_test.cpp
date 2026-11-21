// Output-format validation at Capture::open (API-03, D-01, D-04). No PipeWire session is used:
// PIPEWIRE_REMOTE points to a socket that does not exist, so a call that gets past validation ends
// in NoPipeWire without ever contacting the live daemon. A call that returns InvalidOptions
// therefore proves the validation ran before any PipeWire connection was attempted.
//
// Silence is checked in process: fds 1 and 2 are redirected to temp files for the window of the
// call. stdio buffers are flushed before the redirect and again before the restore, otherwise a
// buffered write could land outside the window and read as zero bytes. A control window with one
// byte per stream proves the redirect really counts.

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace {

namespace fs = std::filesystem;

// A scratch directory removed on scope exit.
class ScratchDir {
public:
    ScratchDir() {
        std::string pattern = (fs::temp_directory_path() / "pwcap-options-XXXXXX").string();
        if (mkdtemp(pattern.data()) != nullptr) {
            path_ = pattern;
        }
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ScratchDir(ScratchDir&&) = delete;
    ScratchDir& operator=(ScratchDir&&) = delete;
    ~ScratchDir() {
        std::error_code ignored;
        if (!path_.empty()) {
            fs::remove_all(path_, ignored);
        }
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    // NOLINTNEXTLINE(readability-redundant-member-init)
    fs::path path_{};
};

// Sets PIPEWIRE_REMOTE for the scope and puts the old value back afterwards.
class RemoteGuard {
public:
    explicit RemoteGuard(const fs::path& socket) {
        if (const char* old = std::getenv("PIPEWIRE_REMOTE"); old != nullptr) {
            old_ = old;
            had_old_ = true;
        }
        ::setenv("PIPEWIRE_REMOTE", socket.c_str(), 1);
    }
    RemoteGuard(const RemoteGuard&) = delete;
    RemoteGuard& operator=(const RemoteGuard&) = delete;
    RemoteGuard(RemoteGuard&&) = delete;
    RemoteGuard& operator=(RemoteGuard&&) = delete;
    ~RemoteGuard() {
        if (had_old_) {
            ::setenv("PIPEWIRE_REMOTE", old_.c_str(), 1);
        } else {
            ::unsetenv("PIPEWIRE_REMOTE");
        }
    }

private:
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::string old_{};
    bool had_old_{false};
};

struct Written {
    std::uintmax_t out{};
    std::uintmax_t err{};
};

[[nodiscard]] std::uintmax_t size_of(const fs::path& file) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(file, error);
    return error ? std::uintmax_t{0} : size;
}

// Runs `action` with fds 1 and 2 redirected to files in `dir`; returns the bytes written to each.
// Nothing inside the window may use gtest: its output would land in the redirected files.
[[nodiscard]] Written run_silenced(const fs::path& dir, const std::function<void()>& action) {
    const fs::path out_file = dir / "stdout.bin";
    const fs::path err_file = dir / "stderr.bin";
    // open() is variadic in C; the third argument is the creation mode.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
    const int out_fd = ::open(out_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    const int err_fd = ::open(err_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    const int saved_out = ::dup(STDOUT_FILENO);
    const int saved_err = ::dup(STDERR_FILENO);
    if (out_fd < 0 || err_fd < 0 || saved_out < 0 || saved_err < 0) {
        return Written{.out = ~std::uintmax_t{0}, .err = ~std::uintmax_t{0}};
    }

    (void)std::fflush(nullptr);
    (void)::dup2(out_fd, STDOUT_FILENO);
    (void)::dup2(err_fd, STDERR_FILENO);
    action();
    (void)std::fflush(nullptr);
    (void)::dup2(saved_out, STDOUT_FILENO);
    (void)::dup2(saved_err, STDERR_FILENO);

    ::close(out_fd);
    ::close(err_fd);
    ::close(saved_out);
    ::close(saved_err);
    return Written{.out = size_of(out_file), .err = size_of(err_file)};
}

[[nodiscard]] pwcap::Options options_for(std::uint32_t rate, std::uint16_t channels) {
    return pwcap::Options{.include = {.node_name = "options-test-never-exists"},
                          .exclude = {},
                          .sample_rate = rate,
                          .channels = channels,
                          .format = pwcap::SampleFormat::S16LE,
                          .reconnect = true};
}

[[nodiscard]] pwcap::Sink discard_sink() {
    return [](std::span<const std::byte> /*pcm*/) {};
}

// Opens with the given format and reports the error, or nothing when open unexpectedly succeeds.
struct OpenOutcome {
    bool opened{false};
    pwcap::Error error{pwcap::Error::NoPipeWire};
};

[[nodiscard]] OpenOutcome try_open(std::uint32_t rate, std::uint16_t channels) {
    auto result = pwcap::Capture::open(options_for(rate, channels), discard_sink());
    if (result) {
        return OpenOutcome{.opened = true, .error = pwcap::Error::NoPipeWire};
    }
    return OpenOutcome{.opened = false, .error = result.error()};
}

TEST(Options, UnsupportedFormatIsInvalidOptions) {
    const ScratchDir dir;
    ASSERT_FALSE(dir.path().empty());
    const RemoteGuard remote{dir.path() / "no-such-pipewire-socket"};

    // The control window: one byte on each stream must be counted, or the check below proves
    // nothing.
    const Written control = run_silenced(dir.path(), [] {
        (void)std::fputc('x', stdout);
        (void)std::fputc('y', stderr);
    });
    ASSERT_EQ(control.out, 1U) << "the stdout redirect did not capture the control byte";
    ASSERT_EQ(control.err, 1U) << "the stderr redirect did not capture the control byte";

    struct Case {
        std::uint32_t rate;
        std::uint16_t channels;
    };
    constexpr std::array<Case, 5> cases{{
        {.rate = 16000, .channels = 2},
        {.rate = 44100, .channels = 1},
        {.rate = 96000, .channels = 2},
        {.rate = 48000, .channels = 3},
        {.rate = 8000, .channels = 1},
    }};
    std::array<OpenOutcome, cases.size()> outcomes{};
    const Written written = run_silenced(dir.path(), [&] {
        std::ranges::transform(cases, outcomes.begin(), [](const Case& tried) {
            return try_open(tried.rate, tried.channels);
        });
    });

    std::ranges::for_each(std::views::iota(std::size_t{0}, cases.size()), [&](std::size_t index) {
        const Case& tried = cases.at(index);
        const OpenOutcome& outcome = outcomes.at(index);
        EXPECT_FALSE(outcome.opened) << tried.rate << "/" << tried.channels;
        EXPECT_EQ(outcome.error, pwcap::Error::InvalidOptions)
            << tried.rate << "/" << tried.channels << " returned "
            << pwcap::to_string(outcome.error);
    });
    EXPECT_EQ(written.out, 0U) << "bytes reached fd 1";
    EXPECT_EQ(written.err, 0U) << "bytes reached fd 2";
}

TEST(Options, SupportedFormatsPassValidation) {
    const ScratchDir dir;
    ASSERT_FALSE(dir.path().empty());
    const RemoteGuard remote{dir.path() / "no-such-pipewire-socket"};

    struct Case {
        std::uint32_t rate;
        std::uint16_t channels;
    };
    constexpr std::array<Case, 3> cases{{
        {.rate = 16000, .channels = 1},
        {.rate = 48000, .channels = 2},
        {.rate = 48000, .channels = 1},
    }};
    for (const Case& tried : cases) {
        const OpenOutcome outcome = try_open(tried.rate, tried.channels);
        EXPECT_FALSE(outcome.opened) << tried.rate << "/" << tried.channels;
        EXPECT_EQ(outcome.error, pwcap::Error::NoPipeWire)
            << tried.rate << "/" << tried.channels << " returned "
            << pwcap::to_string(outcome.error);
    }
}

// Regression test for a hang found in plan 04-06: with no daemon, `open` started the loop thread,
// failed to connect and stopped the thread again at once, and in libpipewire 1.6.2 the stop request
// was lost about once in a few hundred opens, so the destructor joined forever. One open is not
// enough to see it; the count is high enough to hit the old code in most runs. If this test ever
// times out, `pw_thread_loop_stop` is being called too soon after `pw_thread_loop_start`.
TEST(Options, RepeatedOpenWithoutDaemonNeverHangs) {
    const ScratchDir dir;
    ASSERT_FALSE(dir.path().empty());
    const RemoteGuard remote{dir.path() / "no-such-pipewire-socket"};

    constexpr std::size_t kOpens = 2000;
    const auto no_pipewire = std::ranges::count_if(
        std::views::iota(std::size_t{0}, kOpens), [](std::size_t /*attempt*/) {
            const OpenOutcome outcome = try_open(16000, 1);
            return !outcome.opened && outcome.error == pwcap::Error::NoPipeWire;
        });
    EXPECT_EQ(static_cast<std::size_t>(no_pipewire), kOpens);
}

} // namespace
