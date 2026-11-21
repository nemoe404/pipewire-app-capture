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
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <unistd.h>

#ifndef PWCAP_CLI_EXE
#error "PWCAP_CLI_EXE must name the built pwcap executable"
#endif

namespace {

namespace test = pwcap::test;

constexpr double kToneSeconds = 30.0;
constexpr std::chrono::seconds kNodeWait{5};
constexpr std::chrono::seconds kLinkWait{5};
constexpr std::chrono::seconds kLinkWaitShort{3};
constexpr std::chrono::seconds kExitWait{5};
constexpr std::chrono::seconds kSelfExitWait{3};
constexpr std::chrono::seconds kListWait{10};
constexpr std::size_t kReaderBytes = 32000;
constexpr std::uintmax_t kWaitGrowthBytes = 16000;

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

// The whole content of a file, as text. A file that cannot be read yields "".
std::string read_text(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// The bytes of a file, as std::byte for the PCM decoders.
std::vector<std::byte> read_file_bytes(const std::filesystem::path& path) {
    const std::string text = read_text(path);
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(bytes),
                           [](char byte) { return static_cast<std::byte>(byte); });
    return bytes;
}

// The size of a file, or 0 when it does not exist yet.
std::uintmax_t file_size_of(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

// The lines of `text` that are not empty.
std::vector<std::string> nonempty_lines(const std::string& text) {
    auto lines = std::ranges::to<std::vector<std::string>>(
        std::views::split(text, '\n') |
        std::views::transform([](auto&& line) { return std::string(line.begin(), line.end()); }));
    std::erase_if(lines, [](const std::string& line) { return line.empty(); });
    return lines;
}

bool contains_byte(const std::vector<std::byte>& bytes, std::byte value) {
    return std::ranges::find(bytes, value) != bytes.end();
}

// The number of capture nodes of the CLI child whose prefix is `prefix`, read from a fresh graph.
std::size_t capture_count(const std::string& prefix) {
    return test::read_graph().nodes_by_prefix(prefix).size();
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

TEST(CliCapture, SigtermStopsWithExitZero) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("term-a");
    const test::Child tone = start_app_tone(dir, app, 440.0);
    ASSERT_TRUE(await_node(app, kNodeWait)) << "tone " << app << " never reached the graph";

    const std::filesystem::path pcm = dir.path() / "term.pcm";
    test::Spawned cli{cli_argv({"capture", "--app", app}), pcm, dir.path() / "term.err"};
    const std::optional<std::string> links = await_exact_links(cli.pid(), app, kLinkWait);
    if (links) {
        FAIL() << *links;
    }

    std::this_thread::sleep_for(std::chrono::seconds{1});
    ASSERT_TRUE(cli.send(SIGTERM));
    const std::optional<int> status = cli.wait_for(kExitWait);
    ASSERT_TRUE(status.has_value()) << "pwcap did not exit after SIGTERM";
    EXPECT_EQ(status.value_or(-1), 0);
    const std::uintmax_t size = file_size_of(pcm);
    EXPECT_GT(size, std::uintmax_t{0});
    EXPECT_EQ(size % 2, std::uintmax_t{0});
}

TEST(CliCapture, ReaderCloseExitsZero) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("pipe-a");
    const test::Child tone = start_app_tone(dir, app, 440.0);
    ASSERT_TRUE(await_node(app, kNodeWait)) << "tone " << app << " never reached the graph";

    test::Spawned cli{cli_argv({"capture", "--app", app}), test::StdoutToPipe{},
                      dir.path() / "pipe.err"};
    const std::optional<std::string> links = await_exact_links(cli.pid(), app, kLinkWait);
    if (links) {
        FAIL() << *links;
    }

    // Read 32000 bytes, then close the read end. The CLI's next write fails with EPIPE, which is
    // a clean stop.
    std::array<char, 4096> buffer{};
    std::size_t total = 0;
    // A raw loop: read(2) returns what the pipe holds, which may be less than asked, so it repeats
    // until the byte count is reached.
    while (total < kReaderBytes) {
        const ssize_t got = ::read(cli.stdout_fd(), buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) {
            continue;
        }
        ASSERT_GT(got, 0) << "stdout closed before " << kReaderBytes << " bytes";
        total += static_cast<std::size_t>(got);
    }
    cli.close_stdout_pipe();

    const auto closed_at = std::chrono::steady_clock::now();
    const std::optional<int> status = cli.wait_for(kSelfExitWait);
    ASSERT_TRUE(status.has_value()) << "pwcap did not exit within 3 s of the reader closing";
    EXPECT_EQ(status.value_or(-1), 0);
    RecordProperty("reader_close_exit_ms",
                   std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - closed_at)
                                      .count()));
}

TEST(CliCapture, WaitsWithNoMatchingSourceThenLinks) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("wait-a");
    const std::filesystem::path pcm = dir.path() / "wait.pcm";
    const std::filesystem::path err = dir.path() / "wait.err";
    test::Spawned cli{cli_argv({"capture", "--app", app}), pcm, err};
    const std::string prefix = cli_capture_prefix(cli.pid());

    // Nothing plays yet: within 2 s the CLI reports the wait and its capture node exists.
    const bool waiting = test::wait_until(
        [&] {
            return read_text(err).find("NoMatchingSource:") != std::string::npos &&
                   capture_count(prefix) == 1;
        },
        std::chrono::seconds{2});
    ASSERT_TRUE(waiting) << "no NoMatchingSource wait with one capture node within 2 s";
    EXPECT_EQ(file_size_of(pcm), std::uintmax_t{0}) << "stdout had bytes before any source";

    // The capture node has no link into it while it waits.
    const test::Graph before = test::read_graph();
    const std::vector<const test::GNode*> captures = before.nodes_by_prefix(prefix);
    ASSERT_EQ(captures.size(), std::size_t{1});
    EXPECT_TRUE(before.links_into(captures.front()->id).empty())
        << "the waiting capture node already has a link";

    const test::Child tone = start_app_tone(dir, app, 440.0);
    const auto started = std::chrono::steady_clock::now();
    const std::optional<std::string> links = await_exact_links(cli.pid(), app, kLinkWaitShort);
    if (links) {
        FAIL() << *links;
    }
    RecordProperty("link_after_tone_ms",
                   std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count()));

    ASSERT_TRUE(test::wait_until([&] { return file_size_of(pcm) > kWaitGrowthBytes; },
                                 std::chrono::seconds{5}))
        << "stdout did not grow past " << kWaitGrowthBytes << " bytes";
    ASSERT_TRUE(cli.send(SIGINT));
    const std::optional<int> status = cli.wait_for(kExitWait);
    ASSERT_TRUE(status.has_value()) << "pwcap did not exit after SIGINT";
    EXPECT_EQ(status.value_or(-1), 0);
}

TEST(CliCapture, NoReconnectStoppedExitsNonZero) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("nr-a");
    test::Child tone = start_app_tone(dir, app, 440.0);
    ASSERT_TRUE(await_node(app, kNodeWait)) << "tone " << app << " never reached the graph";

    const std::filesystem::path err = dir.path() / "nr.err";
    test::Spawned cli{cli_argv({"capture", "--app", app, "--no-reconnect"}), dir.path() / "nr.pcm",
                      err};
    const std::optional<std::string> links = await_exact_links(cli.pid(), app, kLinkWait);
    if (links) {
        FAIL() << *links;
    }

    // The selected stream ends: the tone is stopped, then pwcap must end on its own.
    tone.terminate_and_wait();
    const auto ended_at = std::chrono::steady_clock::now();
    const std::optional<int> status = cli.wait_for(kSelfExitWait);
    ASSERT_TRUE(status.has_value()) << "pwcap did not exit on its own within 3 s";
    EXPECT_EQ(status.value_or(-1), 1);
    RecordProperty("self_exit_ms",
                   std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - ended_at)
                                      .count()));

    const std::vector<std::string> lines = nonempty_lines(read_text(err));
    ASSERT_FALSE(lines.empty()) << "pwcap printed nothing on stderr";
    EXPECT_TRUE(std::ranges::any_of(lines, [](const std::string& line) {
        return line.starts_with("Stopped:");
    })) << "no Stopped: line on stderr";
    EXPECT_EQ(lines.back(), "pwcap: Stopped");
}

TEST(CliCapture, F32StereoByteRate) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("f32-a");
    const test::Child tone = start_app_tone(dir, app, 440.0);
    ASSERT_TRUE(await_node(app, kNodeWait)) << "tone " << app << " never reached the graph";

    const std::filesystem::path pcm = dir.path() / "f32.pcm";
    test::Spawned cli{cli_argv({"capture", "--app", app, "--rate", "48000", "--channels", "2",
                                "--format", "f32le"}),
                      pcm, dir.path() / "f32.err"};
    const std::optional<std::string> links = await_exact_links(cli.pid(), app, kLinkWait);
    if (links) {
        FAIL() << *links;
    }

    std::this_thread::sleep_for(std::chrono::seconds{2});
    ASSERT_TRUE(cli.send(SIGINT));
    const std::optional<int> status = cli.wait_for(kExitWait);
    ASSERT_TRUE(status.has_value()) << "pwcap did not exit after SIGINT";
    EXPECT_EQ(status.value_or(-1), 0);
    const std::uintmax_t size = file_size_of(pcm);
    EXPECT_EQ(size % 8, std::uintmax_t{0}) << "f32 stereo frames are 8 bytes";
    EXPECT_GE(size, std::uintmax_t{48000} * 8);
}

TEST(CliList, JsonShowsTheToneAndOnlyOutputStreams) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("list-a");
    const test::Child tone = start_app_tone(dir, app, 440.0);
    ASSERT_TRUE(await_node(app, kNodeWait)) << "tone " << app << " never reached the graph";

    const std::filesystem::path json_file = dir.path() / "list.json";
    test::Spawned list{cli_argv({"list", "--json"}), json_file, dir.path() / "list.err"};
    const std::optional<int> status = list.wait_for(kListWait);
    ASSERT_TRUE(status.has_value()) << "pwcap list --json did not exit within 10 s";
    EXPECT_EQ(status.value_or(-1), 0);

    const nlohmann::json doc = nlohmann::json::parse(read_text(json_file), nullptr, false);
    ASSERT_FALSE(doc.is_discarded()) << "list --json is not JSON";
    ASSERT_TRUE(doc.is_array());
    const auto entry = std::ranges::find_if(doc, [&](const nlohmann::json& element) {
        return element.value("node_name", std::string{}) == app;
    });
    ASSERT_TRUE(entry != doc.end()) << "the tone is not in the listing";
    EXPECT_EQ(entry->value("app_name", std::string{}), app);
    EXPECT_EQ(entry->value("binary", std::string{}), app);

    // Every listed serial still in the graph must be an output stream. A stream that has already
    // gone is churn: it is counted and reported, not failed.
    const test::Graph graph = test::read_graph();
    std::size_t churned = 0;
    std::ranges::for_each(doc, [&](const nlohmann::json& element) {
        const std::uint64_t serial = element.value("serial", std::uint64_t{0});
        const auto found = std::ranges::find(graph.nodes, serial, &test::GNode::serial);
        if (found == graph.nodes.end()) {
            ++churned;
            return;
        }
        EXPECT_EQ(found->media_class, std::string{test::kStreamOutputClass})
            << "listed serial " << serial << " is not an output stream";
    });
    RecordProperty("churned_serials", std::to_string(churned));
}

TEST(CliList, TextHasHeaderAndTone) {
    if (const auto reason = test::skip_reason()) {
        GTEST_SKIP() << *reason;
    }
    const test::TempDir dir;
    const std::string app = test::unique_tone_name("text-a");
    const test::Child tone = start_app_tone(dir, app, 440.0);
    ASSERT_TRUE(await_node(app, kNodeWait)) << "tone " << app << " never reached the graph";

    const std::filesystem::path text_file = dir.path() / "list.txt";
    test::Spawned list{cli_argv({"list"}), text_file, dir.path() / "list-text.err"};
    const std::optional<int> status = list.wait_for(kListWait);
    ASSERT_TRUE(status.has_value()) << "pwcap list did not exit within 10 s";
    EXPECT_EQ(status.value_or(-1), 0);

    const std::vector<std::string> lines = nonempty_lines(read_text(text_file));
    ASSERT_FALSE(lines.empty()) << "pwcap list printed nothing";
    EXPECT_EQ(lines.front(), "SERIAL\tPID\tAPP\tBINARY\tNODE\tMEDIA");
    const std::string row = "\t" + app + "\t" + app + "\t" + app + "\t";
    EXPECT_TRUE(std::ranges::any_of(lines, [&](const std::string& line) {
        return line.contains(row);
    })) << "no row for the tone";
}
