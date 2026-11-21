// S0 item 3 through the public API: an application restarts its stream (the node is killed and a
// new one with the same node.name appears, with a new object.serial and recycled port ids). With
// reconnect on, the capture links the new ports by channel; with reconnect off it does not follow.
// A pw-link monitor runs across the kill, the gap and the relink: no link from any other node
// (microphone, monitor, distractor) may reach the capture at any moment. Opt-in, like every test
// under tests/pipewire.

#include "goertzel.hpp"
#include "pw_session.hpp"

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using pwcap::test::Child;
using pwcap::test::DiagnosticLog;
using pwcap::test::Graph;
using pwcap::test::LinkGuard;
using pwcap::test::TempDir;

// Byte count only: no captured sample is stored anywhere.
[[nodiscard]] pwcap::Sink counting_sink(std::atomic<std::uint64_t>& bytes) {
    return [&bytes](std::span<const std::byte> pcm) {
        bytes.fetch_add(pcm.size(), std::memory_order_relaxed);
    };
}

// The last `capacity` bytes the capture delivered, and a running total. Memory only: no PCM is
// written anywhere.
class ByteTail {
public:
    explicit ByteTail(std::size_t capacity) : capacity_{capacity} {}

    void append(std::span<const std::byte> bytes) {
        const std::scoped_lock lock{mutex_};
        tail_.insert(tail_.end(), bytes.begin(), bytes.end());
        if (tail_.size() > capacity_) {
            tail_.erase(tail_.begin(),
                        tail_.begin() + static_cast<std::ptrdiff_t>(tail_.size() - capacity_));
        }
        total_ += bytes.size();
    }

    [[nodiscard]] std::uint64_t total() const {
        const std::scoped_lock lock{mutex_};
        return total_;
    }

    // At most the last `count` bytes.
    [[nodiscard]] std::vector<std::byte> last(std::size_t count) const {
        const std::scoped_lock lock{mutex_};
        const std::size_t take = std::min(count, tail_.size());
        return {tail_.end() - static_cast<std::ptrdiff_t>(take), tail_.end()};
    }

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<std::byte> tail_{};
    std::uint64_t total_{0};
};

[[nodiscard]] pwcap::Diagnostic collecting(DiagnosticLog& log) {
    return [&log](std::string_view message) { log.add(message); };
}

[[nodiscard]] std::string describe(const std::vector<std::string>& lines) {
    std::string text;
    std::ranges::for_each(lines, [&](const std::string& line) { text += "\n  " + line; });
    return text;
}

// The two tones of a restart test, on the speakers, and the speaker links of the distractor.
struct Tones {
    std::string restarted;
    std::string distractor;
    Child restarted_child;
    Child distractor_child;
    std::vector<pwcap::test::PortPair> distractor_speakers;
    // Both tones reached a sink.
    bool on_speakers{false};
};

[[nodiscard]] Tones start_tones(const TempDir& dir) {
    Tones tones{.restarted = pwcap::test::unique_tone_name("restart440"),
                .distractor = pwcap::test::unique_tone_name("distract880"),
                .restarted_child = {},
                .distractor_child = {},
                .distractor_speakers = {},
                .on_speakers = false};
    tones.restarted_child = pwcap::test::play_tone(dir, tones.restarted, 440.0, 30.0);
    tones.distractor_child = pwcap::test::play_tone(dir, tones.distractor, 880.0, 30.0);
    Graph graph;
    tones.on_speakers = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            return pwcap::test::stereo_on_sink(graph, tones.restarted) &&
                   pwcap::test::stereo_on_sink(graph, tones.distractor);
        },
        3000ms);
    if (!tones.on_speakers) {
        return tones;
    }
    tones.distractor_speakers = pwcap::test::sink_links_of(graph, tones.distractor);
    return tones;
}

[[nodiscard]] std::uint64_t serial_of(const Graph& graph, std::string_view name) {
    const pwcap::test::GNode* node = graph.node_by_name(name);
    return node == nullptr ? 0 : node->serial;
}

} // namespace

// Reconnect on: after the 440 Hz application is killed and relaunched under the same node.name,
// the capture is linked to the new node's ports by channel within 3000 ms, the new node is back on
// the speakers, and no other node ever linked to the capture, the gap included.
TEST(Restart, RelinksNewPortsAfterStreamRestart) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string prefix = pwcap::test::own_capture_prefix();
    Tones tones = start_tones(dir);
    ASSERT_TRUE(tones.on_speakers) << "the tones were never linked to a sink";

    DiagnosticLog log;
    // 4 s of 16 kHz S16 mono.
    ByteTail tail{128000};
    auto opened = pwcap::Capture::open(
        pwcap::Options{.include = {.node_name = tones.restarted}, .reconnect = true},
        [&tail](std::span<const std::byte> pcm) { tail.append(pcm); }, collecting(log));
    ASSERT_TRUE(opened.has_value())
        << "Capture::open failed: " << (opened ? "" : pwcap::to_string(opened.error()));

    Graph graph;
    std::optional<std::string> reason;
    const bool first_link = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            reason = pwcap::test::exact_capture_links(graph, prefix, tones.restarted);
            return !reason;
        },
        3000ms);
    ASSERT_TRUE(first_link) << "the capture never linked the tone: " << reason.value_or("");
    const std::uint64_t old_serial = serial_of(graph, tones.restarted);
    ASSERT_GT(old_serial, 0U);

    const LinkGuard guard{prefix, {tones.restarted + ":"}};
    ASSERT_TRUE(guard.wait_armed(3000ms)) << "the pw-link monitor printed nothing";
    const std::size_t adds_before = guard.capture_link_adds();

    // Restart as s0-item3 did: SIGTERM and wait, then relaunch under the same node.name.
    tones.restarted_child.terminate_and_wait();
    tones.restarted_child = pwcap::test::play_tone(dir, tones.restarted, 440.0, 30.0);
    const auto relaunched = std::chrono::steady_clock::now();

    std::optional<std::chrono::milliseconds> relink;
    std::string state;
    const bool relinked = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            const std::uint64_t serial = serial_of(graph, tones.restarted);
            if (serial == 0 || serial == old_serial) {
                state = "the relaunched node is not in the graph yet";
                return false;
            }
            reason = pwcap::test::exact_capture_links(graph, prefix, tones.restarted);
            if (reason) {
                state = *reason;
                return false;
            }
            if (!pwcap::test::stereo_on_sink(graph, tones.restarted)) {
                state = "the relaunched node is not on a sink";
                return false;
            }
            relink = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - relaunched);
            return true;
        },
        3000ms, 5ms);
    ASSERT_TRUE(relinked) << "no relink within 3000 ms: " << state;
    std::cout << "relink_ms=" << relink.value_or(0ms).count() << '\n';

    EXPECT_TRUE(opened->linked());
    EXPECT_GE(guard.capture_link_adds(), adds_before + 2)
        << "the capture did not get two new links after the relaunch";
    EXPECT_TRUE(guard.violations().empty())
        << "a link from another node reached the capture:" << describe(guard.violations());
    EXPECT_FALSE(guard.monitor_ended()) << "the pw-link monitor died; the guard was blind";
    std::ranges::for_each(tones.distractor_speakers, [&](const pwcap::test::PortPair& pair) {
        EXPECT_TRUE(pwcap::test::graph_has_link(graph, pair))
            << "the distractor lost a speaker link: " << pair.first << " -> " << pair.second;
    });

    // Audio half of ROADMAP criterion 2, judged only after the graph assertions above. One more
    // second of audio must arrive after the relink, and that second must be the 440 Hz tone and not
    // the 880 Hz distractor. The tone is 0.1 amplitude, about 2317 RMS in int16 units.
    const std::uint64_t bytes_at_relink = tail.total();
    ASSERT_TRUE(
        pwcap::test::wait_until([&] { return tail.total() >= bytes_at_relink + 32000; }, 4000ms))
        << "less than 1 s of audio arrived after the relink; got " << tail.total() - bytes_at_relink
        << " bytes";
    const std::vector<float> samples = pwcap::test::decode_s16le(tail.last(32000));
    ASSERT_EQ(samples.size(), 16000U);
    const double at_tone = pwcap::test::goertzel_power(samples, 440.0, 16000.0);
    const double at_distractor = pwcap::test::goertzel_power(samples, 880.0, 16000.0);
    EXPECT_GT(at_tone, 0.0);
    EXPECT_GE(at_tone, 1000.0 * at_distractor)
        << "P(440)=" << at_tone << " P(880)=" << at_distractor;
    EXPECT_GE(pwcap::test::rms(samples), 500.0) << "the audio did not resume after the relink";
}

// Reconnect off: the restarted stream is a different stream (new object.serial). The capture does
// not follow it, ends with zero links, and reports the end once with Stopped.
TEST(Restart, ReconnectOffDoesNotRelink) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string prefix = pwcap::test::own_capture_prefix();
    Tones tones = start_tones(dir);
    ASSERT_TRUE(tones.on_speakers) << "the tones were never linked to a sink";

    DiagnosticLog log;
    std::atomic<std::uint64_t> bytes{0};
    auto opened = pwcap::Capture::open(
        pwcap::Options{.include = {.node_name = tones.restarted}, .reconnect = false},
        counting_sink(bytes), collecting(log));
    ASSERT_TRUE(opened.has_value())
        << "Capture::open failed: " << (opened ? "" : pwcap::to_string(opened.error()));

    Graph graph;
    std::optional<std::string> reason;
    const bool first_link = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            reason = pwcap::test::exact_capture_links(graph, prefix, tones.restarted);
            return !reason;
        },
        3000ms);
    ASSERT_TRUE(first_link) << "the capture never linked the tone: " << reason.value_or("");
    const std::uint64_t old_serial = serial_of(graph, tones.restarted);
    ASSERT_GT(old_serial, 0U);
    EXPECT_FALSE(log.has_prefix("Stopped:")) << "Stopped before the stream ended";

    const LinkGuard guard{prefix, {tones.restarted + ":"}};
    ASSERT_TRUE(guard.wait_armed(3000ms)) << "the pw-link monitor printed nothing";
    const std::size_t adds_before = guard.capture_link_adds();

    tones.restarted_child.terminate_and_wait();
    tones.restarted_child = pwcap::test::play_tone(dir, tones.restarted, 440.0, 30.0);

    std::string state;
    const bool back_on_speakers = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            const std::uint64_t serial = serial_of(graph, tones.restarted);
            if (serial == 0 || serial == old_serial) {
                state = "the relaunched node is not in the graph yet";
                return false;
            }
            return pwcap::test::stereo_on_sink(graph, tones.restarted);
        },
        3000ms);
    ASSERT_TRUE(back_on_speakers) << "the relaunched tone never reached a sink: " << state;

    // Give a wrongly following reconciler time to link, then judge.
    std::this_thread::sleep_for(1500ms);
    graph = pwcap::test::read_graph();
    const std::vector<const pwcap::test::GNode*> captures = graph.nodes_by_prefix(prefix);
    ASSERT_EQ(captures.size(), 1U);
    EXPECT_TRUE(graph.links_into(captures.front()->id).empty())
        << "the capture was linked to the relaunched stream although reconnect is off";
    EXPECT_FALSE(opened->linked());
    EXPECT_TRUE(pwcap::test::wait_until([&] { return log.has_prefix("Stopped:"); }, 1000ms))
        << "no Stopped diagnostic; got:" << describe(log.all());
    EXPECT_EQ(log.count_prefix("Stopped:"), 1U);
    EXPECT_EQ(guard.capture_link_adds(), adds_before) << "a link was added after the stream ended";
    EXPECT_TRUE(guard.violations().empty())
        << "a link from another node reached the capture:" << describe(guard.violations());
    EXPECT_FALSE(guard.monitor_ended()) << "the pw-link monitor died; the guard was blind";
}
