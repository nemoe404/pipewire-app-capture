// Opt-in live tests of the spawned pwcap binary. Graph first: every capture case reads pw-dump and
// checks the exact links of its own pwcap child before any byte count or Goertzel check. Every case
// skips with a printed reason unless PWCAP_TEST_PIPEWIRE=1 and `pw-cli info 0` answers. Tones carry
// PID-unique names and every assertion is scoped to its own child, so a parallel live run on the
// same session does not interfere.

#include "../cli/spawn.hpp"
#include "goertzel.hpp"
#include "pw_session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef PWCAP_CLI_EXE
#error "PWCAP_CLI_EXE must name the built pwcap executable"
#endif

namespace {

namespace test = pwcap::test;

constexpr double kToneSeconds = 30.0;
constexpr std::chrono::seconds kNodeWait{5};
constexpr std::chrono::seconds kLinkWait{5};
constexpr std::chrono::seconds kExitWait{5};

// The arguments with the pwcap executable in front.
std::vector<std::string> cli_argv(std::vector<std::string> args) {
    args.insert(args.begin(), std::string{PWCAP_CLI_EXE});
    return args;
}

// The prefix of the one capture node that the CLI child `pid` creates: pwcap-<pid>-.
std::string cli_capture_prefix(pid_t pid) { return "pwcap-" + std::to_string(pid) + "-"; }

// Writes a 30 s stereo tone and plays it with pw-play. The node carries the application name, the
// binary name and the node name `name`, so `pwcap list` shows it as one application.
test::Child start_app_tone(const test::TempDir& dir, const std::string& name, double freq_hz) {
    const std::filesystem::path wav = dir.path() / (name + ".wav");
    test::write_tone_wav(wav, freq_hz, kToneSeconds, 2, 0.1);
    const std::string props = "{ node.name=" + name + " application.name=" + name +
                              " application.process.binary=" + name + " }";
    return test::Child{std::vector<std::string>{"pw-play", "-P", props, wav.string()}};
}

// True once a node named `name` is in the graph, within `timeout`.
bool await_node(const std::string& name, std::chrono::milliseconds timeout) {
    return test::wait_until([&] { return test::read_graph().node_by_name(name) != nullptr; },
                            timeout);
}

// Polls the graph until the capture of CLI child `pid` holds exactly the FL and FR links from the
// application `source`. Returns nullopt on success, or the reason from the last graph read.
std::optional<std::string> await_exact_links(pid_t pid, const std::string& source,
                                             std::chrono::milliseconds timeout) {
    const std::string prefix = cli_capture_prefix(pid);
    std::optional<std::string> reason = std::string{"no graph read"};
    const bool linked = test::wait_until(
        [&] {
            reason = test::exact_capture_links(test::read_graph(), prefix, source);
            return !reason;
        },
        timeout);
    return linked ? std::nullopt : reason;
}

// The bytes of a file. A file that cannot be read yields no bytes.
std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(bytes),
                           [](char byte) { return static_cast<std::byte>(byte); });
    return bytes;
}

bool contains_byte(const std::vector<std::byte>& bytes, std::byte value) {
    return std::ranges::find(bytes, value) != bytes.end();
}

} // namespace

TEST(CliCapture, LinksOnlyTheSelectedAppAndStopsOnSigint) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("cap-a");
    const std::string other = test::unique_tone_name("cap-b");
    const test::Child tone_a = start_app_tone(dir, app, 440.0);
    const test::Child tone_b = start_app_tone(dir, other, 880.0);
    ASSERT_TRUE(await_node(app, kNodeWait)) << "tone " << app << " never reached the graph";
    ASSERT_TRUE(await_node(other, kNodeWait)) << "tone " << other << " never reached the graph";

    const std::filesystem::path pcm = dir.path() / "capture.pcm";
    const std::filesystem::path err = dir.path() / "capture.err";
    test::Spawned cli{cli_argv({"capture", "--app", app}), pcm, err};

    // Graph first: the links into the capture are judged before any level is read.
    const std::optional<std::string> links = await_exact_links(cli.pid(), app, kLinkWait);
    if (links) {
        FAIL() << *links;
    }
    ASSERT_TRUE(test::stereo_on_sink(test::read_graph(), app))
        << "the application is not stereo on the sink";

    // Capture for a while, then stop. This sleep spans audio; it does not wait for readiness.
    std::this_thread::sleep_for(std::chrono::seconds{2});
    ASSERT_TRUE(cli.send(SIGINT));
    const std::optional<int> status = cli.wait_for(kExitWait);
    ASSERT_TRUE(status.has_value()) << "pwcap did not exit after SIGINT";
    EXPECT_EQ(status.value_or(-1), 0);

    const std::vector<std::byte> bytes = read_file_bytes(pcm);
    ASSERT_FALSE(bytes.empty()) << "no PCM reached stdout";
    EXPECT_EQ(bytes.size() % 2, std::size_t{0});
    const std::vector<float> samples = test::decode_s16le(bytes);
    const double at_440 = test::goertzel_power(samples, 440.0, 16000.0);
    const double at_880 = test::goertzel_power(samples, 880.0, 16000.0);
    EXPECT_GE(at_440, 1000.0 * at_880) << "440 Hz power " << at_440 << ", 880 Hz power " << at_880;
    EXPECT_FALSE(contains_byte(read_file_bytes(err), std::byte{0}));
}
