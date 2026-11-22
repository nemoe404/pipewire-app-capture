// API-02 on a live session: a full capture cycle and two error paths write nothing to standard
// output or standard error. The NoPipeWire path is proven silent by the subprocess probe
// (PIPEWIRE_REMOTE at a missing socket, D-05); this test covers what that probe cannot reach.
//
// Silence is judged at the file-descriptor level: fds 1 and 2 are redirected to temp files for the
// window. stdio buffers are flushed before the redirect and again before the restore, otherwise a
// buffered write could land outside the window and read as zero bytes. A control window that
// writes one byte to each C stream first proves the helper counts buffered writes. Inside the
// window nothing may use gtest, because its output would land in the redirected files.
//
// If a byte comes from libpipewire's own logging and not from pwcap code, the test fails on
// purpose and prints the text: that is a finding against API-02, not something to filter out.
// Opt-in, like every test under tests/pipewire.

#include "pw_session.hpp"

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using namespace std::chrono_literals;

namespace fs = std::filesystem;

// What one redirected stream received: its size, and its start for a failure message.
struct Received {
    std::uintmax_t bytes{0};
    std::string text;
};

struct Window {
    bool ok{false};
    Received out;
    Received err;
};

[[nodiscard]] Received read_back(const fs::path& file) {
    Received received;
    std::error_code error;
    const std::uintmax_t size = fs::file_size(file, error);
    if (error) {
        return received;
    }
    received.bytes = size;
    std::ifstream stream{file, std::ios::binary};
    std::string text(256, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    received.text = std::move(text);
    return received;
}

// Runs `action` with fds 1 and 2 redirected to two fresh files in `dir`. The same helper serves the
// control window and the real window.
[[nodiscard]] Window run_window(const fs::path& dir, const std::string& tag,
                                const std::function<void()>& action) {
    const fs::path out_file = dir / (tag + "-stdout.bin");
    const fs::path err_file = dir / (tag + "-stderr.bin");
    // open() is variadic in C; the third argument is the creation mode.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
    const int out_fd = ::open(out_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    const int err_fd = ::open(err_file.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    const int saved_out = ::dup(STDOUT_FILENO);
    const int saved_err = ::dup(STDERR_FILENO);
    if (out_fd < 0 || err_fd < 0 || saved_out < 0 || saved_err < 0) {
        return Window{};
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
    return Window{.ok = true, .out = read_back(out_file), .err = read_back(err_file)};
}

// Everything the real window observed, kept in locals and judged after the restore.
struct Cycle {
    bool opened{false};
    bool linked_seen{false};
    std::uint64_t bytes{0};
    // The error of an open that must fail, or nothing when it unexpectedly succeeded.
    std::optional<pwcap::Error> no_match;
    std::optional<pwcap::Error> invalid_options;
};

[[nodiscard]] std::optional<pwcap::Error> open_error(const pwcap::Options& options) {
    auto result = pwcap::Capture::open(options, [](std::span<const std::byte> /*pcm*/) {});
    if (result) {
        return std::nullopt;
    }
    return result.error();
}

// One full cycle: open on the tone, wait for the link and 1 s of S16 audio at 16 kHz (32000 bytes),
// destroy; then the two error paths. No assertion and no output of its own.
void run_cycle(const std::string& tone, Cycle& cycle) {
    std::atomic<std::uint64_t> bytes{0};
    {
        auto opened =
            pwcap::Capture::open(pwcap::Options{.include = {.node_name = tone}},
                                 [&bytes](std::span<const std::byte> pcm) {
                                     bytes.fetch_add(pcm.size(), std::memory_order_relaxed);
                                 });
        if (opened) {
            cycle.opened = true;
            static_cast<void>(pwcap::test::wait_until(
                [&] {
                    cycle.linked_seen = cycle.linked_seen || opened->linked();
                    return cycle.linked_seen && bytes.load(std::memory_order_relaxed) >= 32000;
                },
                5000ms));
        }
    }
    cycle.bytes = bytes.load(std::memory_order_relaxed);

    cycle.no_match = open_error(pwcap::Options{
        .include = {.node_name = "pwcap-quiet-test-no-such-node"}, .reconnect = false});
    cycle.invalid_options = open_error(pwcap::Options{
        .include = {.node_name = "pwcap-quiet-test-no-such-node"}, .sample_rate = 22050});
}

[[nodiscard]] std::string_view error_text(const std::optional<pwcap::Error>& error) {
    return error ? pwcap::to_string(*error) : "no error, the open succeeded";
}

[[nodiscard]] std::string describe(const Received& received) {
    return std::to_string(received.bytes) + " bytes, starting \"" + received.text + "\"";
}

} // namespace

TEST(Quiet, FullCaptureCycleWritesNoByteToFd1OrFd2) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const pwcap::test::TempDir dir;
    const std::string tone = pwcap::test::unique_tone_name("quiet440");
    const pwcap::test::Child child = pwcap::test::play_tone(dir, tone, 440.0, 30.0);
    const bool on_sink = pwcap::test::wait_until(
        [&] { return pwcap::test::stereo_on_sink(pwcap::test::read_graph(), tone); }, 3000ms);
    ASSERT_TRUE(on_sink) << "the tone was never linked to a sink";

    // Control window: one byte to each C stream must be counted, buffered or not. Under ctest,
    // standard output is a fully buffered pipe, so this proves the flush before the restore works.
    const Window control = run_window(dir.path(), "control", [] {
        static_cast<void>(std::fputc('x', stdout));
        static_cast<void>(std::fputc('y', stderr));
    });
    ASSERT_TRUE(control.ok) << "the control window could not be set up";
    ASSERT_EQ(control.out.bytes, 1U) << "the stdout redirect did not capture the control byte";
    ASSERT_EQ(control.err.bytes, 1U) << "the stderr redirect did not capture the control byte";

    Cycle cycle;
    const Window real = run_window(dir.path(), "real", [&] { run_cycle(tone, cycle); });
    ASSERT_TRUE(real.ok) << "the real window could not be set up";

    EXPECT_EQ(real.out.bytes, 0U) << "fd 1 received " << describe(real.out);
    EXPECT_EQ(real.err.bytes, 0U) << "fd 2 received " << describe(real.err);
    EXPECT_TRUE(cycle.opened) << "the capture did not open on the tone";
    EXPECT_TRUE(cycle.linked_seen) << "linked() was never true";
    EXPECT_GE(cycle.bytes, 32000U) << "less than 1 s of audio arrived";
    EXPECT_EQ(cycle.no_match, std::optional{pwcap::Error::NoMatchingSource})
        << "got " << error_text(cycle.no_match);
    EXPECT_EQ(cycle.invalid_options, std::optional{pwcap::Error::InvalidOptions})
        << "got " << error_text(cycle.invalid_options);
}
