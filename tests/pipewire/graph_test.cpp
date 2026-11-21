// Integration tests against a live PipeWire session. Opt-in: every test starts with the gate in
// pw_session.hpp and skips with a printed reason unless PWCAP_TEST_PIPEWIRE=1 and `pw-cli info 0`
// answers. Graph state read from pw-dump is the ground truth; the library never judges itself.

#include "goertzel.hpp"
#include "pw_session.hpp"

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using pwcap::test::Child;
using pwcap::test::GLink;
using pwcap::test::GNode;
using pwcap::test::GPort;
using pwcap::test::Graph;
using pwcap::test::TempDir;

constexpr std::string_view kStreamOutput = "Stream/Output/Audio";
constexpr std::string_view kSink = "Audio/Sink";

// One link from a tone's output port to a sink input port.
using LinkPair = std::pair<std::uint32_t, std::uint32_t>;

// `pwcap-test-<pid of this process>-<suffix>`: unique per run, so parallel runs keep apart.
[[nodiscard]] std::string tone_name(std::string_view suffix) {
    return "pwcap-test-" + std::to_string(::getpid()) + "-" + std::string{suffix};
}

// Writes a stereo tone WAV into `dir` and plays it with pw-play under the node name `name`.
[[nodiscard]] Child start_tone(const TempDir& dir, const std::string& name, double freq_hz,
                               double seconds) {
    const std::filesystem::path wav = dir.path() / (name + ".wav");
    pwcap::test::write_tone_wav(wav, freq_hz, seconds, 2, 0.1);
    return Child{
        std::vector<std::string>{"pw-play", "-P", "{ node.name=" + name + " }", wav.string()}};
}

[[nodiscard]] bool is_sink_input(const Graph& graph, const GLink& link) {
    const GNode* sink = graph.node_by_id(link.in_node);
    const GPort* port = graph.port_by_id(link.in_port);
    return sink != nullptr && sink->media_class == kSink && port != nullptr && !port->output;
}

// The output ports of `node_id`, as (channel, port id) in graph order.
[[nodiscard]] std::vector<const GPort*> output_ports(const Graph& graph, std::uint32_t node_id) {
    std::vector<const GPort*> outputs = graph.ports_of(node_id);
    std::erase_if(outputs, [](const GPort* port) { return !port->output; });
    return outputs;
}

// True when `name` is a Stream/Output/Audio node with exactly the output ports FL and FR, each
// linked to an input port of an Audio/Sink node.
[[nodiscard]] bool stereo_linked_to_sink(const Graph& graph, std::string_view name) {
    const GNode* node = graph.node_by_name(name);
    if (node == nullptr || node->media_class != kStreamOutput) {
        return false;
    }
    const std::vector<const GPort*> outputs = output_ports(graph, node->id);
    const auto has_channel = [&](std::string_view channel) {
        return std::ranges::count(outputs, channel, [](const GPort* port) -> std::string_view {
                   return port->channel;
               }) == 1;
    };
    if (outputs.size() != 2 || !has_channel("FL") || !has_channel("FR")) {
        return false;
    }
    return std::ranges::all_of(outputs, [&](const GPort* port) {
        const std::vector<const GLink*> out = graph.links_out_of(node->id);
        return std::ranges::any_of(out, [&](const GLink* link) {
            return link->out_port == port->id && is_sink_input(graph, *link);
        });
    });
}

// The (out port, in port) pairs by which `name` is linked to a sink input: its speaker links.
[[nodiscard]] std::vector<LinkPair> speaker_links(const Graph& graph, std::string_view name) {
    std::vector<LinkPair> pairs;
    const GNode* node = graph.node_by_name(name);
    if (node == nullptr) {
        return pairs;
    }
    const std::vector<const GLink*> out = graph.links_out_of(node->id);
    std::ranges::for_each(out, [&](const GLink* link) {
        if (is_sink_input(graph, *link)) {
            pairs.emplace_back(link->out_port, link->in_port);
        }
    });
    return pairs;
}

[[nodiscard]] bool has_link(const Graph& graph, const LinkPair& pair) {
    return std::ranges::any_of(graph.links, [&](const GLink& link) {
        return link.out_port == pair.first && link.in_port == pair.second;
    });
}

// Every baseline speaker link is still in the graph.
void expect_speaker_links_kept(const Graph& graph, const std::vector<LinkPair>& baseline,
                               std::string_view when) {
    std::ranges::for_each(baseline, [&](const LinkPair& pair) {
        EXPECT_TRUE(has_link(graph, pair)) << "speaker link lost " << when << ": out port "
                                           << pair.first << " -> in port " << pair.second;
    });
}

// The assertions of the graph test, all on the pw-dump graph. The level checks build on them
// (see goertzel.hpp).
//   - exactly one capture node (name starts with capture_prefix): Stream/Input/Audio, autoconnect
//     false (SAFE-03);
//   - its links are exactly {tone FL -> capture FL, tone FR -> capture FR}, matched by channel;
//   - no link from the distractor tone, from a node that is not Stream/Output/Audio, or from a
//     monitor port (no microphone, no sink monitor);
//   - the capture node has no output link;
//   - every baseline speaker link is still present (SAFE-02 during capture).
void expect_exact_capture_graph(const Graph& graph, std::string_view capture_prefix,
                                std::string_view tone_node_name, std::string_view distractor_name,
                                const std::vector<LinkPair>& baseline) {
    const std::vector<const GNode*> captures = graph.nodes_by_prefix(capture_prefix);
    ASSERT_EQ(captures.size(), 1U) << "expected exactly one node named " << capture_prefix << "*";
    const GNode& capture = *captures.front();
    EXPECT_EQ(capture.media_class, "Stream/Input/Audio");
    // Compared as an optional so a missing node.autoconnect fails too, not only a true one.
    EXPECT_EQ(capture.autoconnect, std::optional<bool>{false})
        << "the capture node must have node.autoconnect=false";

    const GNode* tone = graph.node_by_name(tone_node_name);
    const GNode* distractor = graph.node_by_name(distractor_name);
    ASSERT_NE(tone, nullptr);
    ASSERT_NE(distractor, nullptr);

    const std::vector<const GLink*> into = graph.links_into(capture.id);
    EXPECT_EQ(into.size(), 2U) << "the capture node must have exactly two links";

    std::vector<std::string> channel_pairs;
    std::ranges::for_each(into, [&](const GLink* link) {
        const GPort* out = graph.port_by_id(link->out_port);
        const GPort* in = graph.port_by_id(link->in_port);
        const GNode* source = graph.node_by_id(link->out_node);
        if (out == nullptr || in == nullptr || source == nullptr) {
            ADD_FAILURE() << "link " << link->id << " has an endpoint missing from the graph";
            return;
        }
        EXPECT_EQ(source->media_class, kStreamOutput)
            << "link " << link->id << " comes from " << source->name << " (" << source->media_class
            << "), not an application output stream";
        EXPECT_FALSE(out->monitor) << "link " << link->id << " comes from a monitor port";
        EXPECT_TRUE(out->output);
        EXPECT_FALSE(in->output);
        EXPECT_EQ(link->out_node, tone->id)
            << "link " << link->id << " does not come from the tone";
        EXPECT_NE(link->out_node, distractor->id) << "link from the distractor tone";
        EXPECT_EQ(in->node, capture.id);
        EXPECT_EQ(out->channel, in->channel) << "channels must pair FL->FL and FR->FR";
        channel_pairs.push_back(out->channel + "->" + in->channel);
    });
    std::ranges::sort(channel_pairs);
    EXPECT_EQ(channel_pairs, (std::vector<std::string>{"FL->FL", "FR->FR"}));

    EXPECT_TRUE(graph.links_out_of(capture.id).empty())
        << "the capture node must not feed anything";
    expect_speaker_links_kept(graph, baseline, "during capture");
}

// The last `capacity` bytes the Sink delivered, plus a running total. The Sink runs on the
// library's consumer thread, the test reads from its own, hence the mutex. Bounded, in memory only:
// no captured sample is ever written anywhere.
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

// The tone and the distractor were both seen on the speakers, and the tone's speaker links were
// recorded as the baseline.
struct OnSpeakers {
    Graph graph;
    std::vector<LinkPair> baseline;
};

[[nodiscard]] std::optional<OnSpeakers> wait_for_speakers(const std::string& tone,
                                                          const std::string& distractor) {
    OnSpeakers seen;
    const bool on_speakers = pwcap::test::wait_until(
        [&] {
            seen.graph = pwcap::test::read_graph();
            return stereo_linked_to_sink(seen.graph, tone) &&
                   stereo_linked_to_sink(seen.graph, distractor);
        },
        3000ms);
    if (!on_speakers) {
        return std::nullopt;
    }
    seen.baseline = speaker_links(seen.graph, tone);
    if (seen.baseline.empty()) {
        return std::nullopt;
    }
    return seen;
}

// Settle after the links appear, then judge a fresh dump: a stray extra link would show up a
// moment after the expected two, and the exactness check must see it.
[[nodiscard]] Graph settled_graph() {
    std::this_thread::sleep_for(300ms);
    return pwcap::test::read_graph();
}

} // namespace

// Proves the pw-dump oracle on its own, before any library assertion: a PID-tagged pw-play tone is
// found with FL/FR output ports, each linked to an Audio/Sink input port. Without this a broken
// oracle could make the graph test below pass for the wrong reason.
TEST(Harness, OracleSeesToneAndSink) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string name = tone_name("oracle440");
    const Child tone = start_tone(dir, name, 440.0, 10.0);

    Graph graph;
    const bool seen = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            return stereo_linked_to_sink(graph, name);
        },
        3000ms);
    ASSERT_TRUE(seen) << "the tone " << name << " never showed FL/FR output ports linked to an "
                      << kSink << " node in pw-dump";

    const GNode* node = graph.node_by_name(name);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->media_class, kStreamOutput);
    EXPECT_GT(node->serial, 0U);
}

// The graph test through the public API: Capture::open with a node_name match links
// exactly the 440 Hz tone's FL/FR output ports to the capture's FL/FR inputs, adds no other link,
// and leaves the tone on the speakers during capture and after teardown. pw-dump is the judge.
TEST(CaptureGraph, LinksExactlyTheToneAndKeepsSpeakers) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string tone = tone_name("tone440");
    const std::string distractor = tone_name("tone880");
    const Child tone_440 = start_tone(dir, tone, 440.0, 20.0);
    const Child tone_880 = start_tone(dir, distractor, 880.0, 20.0);

    Graph graph;
    const bool on_speakers = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            return stereo_linked_to_sink(graph, tone) && stereo_linked_to_sink(graph, distractor);
        },
        3000ms);
    ASSERT_TRUE(on_speakers) << "tone was never linked to a sink";
    const std::vector<LinkPair> baseline = speaker_links(graph, tone);
    ASSERT_FALSE(baseline.empty()) << "tone was never linked to a sink";

    const std::string capture_prefix = "pwcap-" + std::to_string(::getpid()) + "-";
    std::vector<std::uint32_t> capture_ports;
    // The last 4 s of the default 16 kHz mono S16LE output: 128000 bytes, in memory only.
    ByteTail tail{128000};
    {
        auto opened =
            pwcap::Capture::open(pwcap::Options{.include = {.node_name = tone}},
                                 [&tail](std::span<const std::byte> pcm) { tail.append(pcm); });
        ASSERT_TRUE(opened.has_value())
            << "Capture::open failed: " << (opened ? "" : pwcap::to_string(opened.error()));

        const bool linked_in_graph = pwcap::test::wait_until(
            [&] {
                graph = pwcap::test::read_graph();
                const std::vector<const GNode*> captures = graph.nodes_by_prefix(capture_prefix);
                return captures.size() == 1 && graph.links_into(captures.front()->id).size() >= 2;
            },
            3000ms);
        ASSERT_TRUE(linked_in_graph)
            << "pw-dump never showed two links into " << capture_prefix << "*";

        // Settle, then judge a fresh dump: a stray extra link would appear a moment after the
        // expected two, and the exactness check must see it.
        std::this_thread::sleep_for(300ms);
        graph = pwcap::test::read_graph();
        ASSERT_NO_FATAL_FAILURE(
            expect_exact_capture_graph(graph, capture_prefix, tone, distractor, baseline));

        // The library's own report is only a cross-check; the graph above is the ground truth.
        EXPECT_TRUE(opened->linked());

        const std::vector<const GNode*> captures = graph.nodes_by_prefix(capture_prefix);
        std::ranges::for_each(graph.ports_of(captures.front()->id),
                              [&](const GPort* port) { capture_ports.push_back(port->id); });

        // Level corroboration, only after the graph assertions above passed: 1.5 s of audio (at
        // 32000 bytes per second) must arrive, and its last second must be the 440 Hz tone and
        // not the 880 Hz distractor. The tone is 0.1 amplitude, so about 2317 RMS in int16 units.
        ASSERT_TRUE(pwcap::test::wait_until([&] { return tail.total() >= 48000; }, 4000ms))
            << "fewer than 1.5 s of audio arrived after linking; got " << tail.total() << " bytes";
        const std::vector<float> samples = pwcap::test::decode_s16le(tail.last(32000));
        ASSERT_EQ(samples.size(), 16000U);
        const double at_tone = pwcap::test::goertzel_power(samples, 440.0, 16000.0);
        const double at_distractor = pwcap::test::goertzel_power(samples, 880.0, 16000.0);
        EXPECT_GT(at_tone, 0.0);
        EXPECT_GE(at_tone, 1000.0 * at_distractor)
            << "P(440)=" << at_tone << " P(880)=" << at_distractor;
        EXPECT_GE(pwcap::test::rms(samples), 500.0);
    }

    // The Capture is destroyed. Its node must go, the tone must stay on the speakers, and no link
    // may keep a capture port.
    const bool node_gone = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            return graph.nodes_by_prefix(capture_prefix).empty();
        },
        2000ms);
    ASSERT_TRUE(node_gone) << "the capture node " << capture_prefix << "* outlived the Capture";
    EXPECT_FALSE(capture_ports.empty());
    expect_speaker_links_kept(graph, baseline, "after teardown");
    EXPECT_EQ(speaker_links(graph, tone).size(), baseline.size());
    EXPECT_TRUE(std::ranges::none_of(graph.links, [&](const GLink& link) {
        return std::ranges::contains(capture_ports, link.in_port) ||
               std::ranges::contains(capture_ports, link.out_port);
    })) << "a link still has a capture port after teardown";
}

// The 48 kHz stereo F32LE path (D-04 passthrough). Same graph assertions first; then the left
// channel of the last second carries the 440 Hz tone and not the 880 Hz distractor.
TEST(CaptureLevels, Passthrough48kStereoF32CarriesTheTone) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string tone = tone_name("levels440");
    const std::string distractor = tone_name("levels880");
    const Child tone_440 = start_tone(dir, tone, 440.0, 20.0);
    const Child tone_880 = start_tone(dir, distractor, 880.0, 20.0);

    const std::optional<OnSpeakers> before = wait_for_speakers(tone, distractor);
    if (!before) {
        FAIL() << "tone was never linked to a sink";
    }

    const std::string capture_prefix = "pwcap-" + std::to_string(::getpid()) + "-";
    // The last 4 s of 48 kHz stereo F32LE: 8 bytes a frame, 1536000 bytes.
    constexpr std::size_t kBytesPerSecond = std::size_t{48000} * 8;
    ByteTail tail{kBytesPerSecond * 4};
    auto opened =
        pwcap::Capture::open(pwcap::Options{.include = {.node_name = tone},
                                            .exclude = {},
                                            .sample_rate = 48000,
                                            .channels = 2,
                                            .format = pwcap::SampleFormat::F32LE,
                                            .reconnect = true},
                             [&tail](std::span<const std::byte> pcm) { tail.append(pcm); });
    ASSERT_TRUE(opened.has_value())
        << "Capture::open failed: " << (opened ? "" : pwcap::to_string(opened.error()));

    Graph graph;
    const bool linked_in_graph = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            const std::vector<const GNode*> captures = graph.nodes_by_prefix(capture_prefix);
            return captures.size() == 1 && graph.links_into(captures.front()->id).size() >= 2;
        },
        3000ms);
    ASSERT_TRUE(linked_in_graph) << "pw-dump never showed two links into " << capture_prefix << "*";
    graph = settled_graph();
    ASSERT_NO_FATAL_FAILURE(
        expect_exact_capture_graph(graph, capture_prefix, tone, distractor, before->baseline));

    // Levels, only now: 1.5 s of audio is 576000 bytes.
    ASSERT_TRUE(pwcap::test::wait_until([&] { return tail.total() >= 576000; }, 4000ms))
        << "fewer than 1.5 s of audio arrived after linking; got " << tail.total() << " bytes";
    const std::vector<std::byte> bytes = tail.last(kBytesPerSecond);
    ASSERT_EQ(bytes.size() % 8, 0U);
    EXPECT_EQ(tail.total() % 8, 0U);
    const std::vector<float> left = pwcap::test::decode_f32le_channel(bytes, 2, 0);
    ASSERT_EQ(left.size(), 48000U);
    const double at_tone = pwcap::test::goertzel_power(left, 440.0, 48000.0);
    const double at_distractor = pwcap::test::goertzel_power(left, 880.0, 48000.0);
    EXPECT_GT(at_tone, 0.0);
    EXPECT_GE(at_tone, 1000.0 * at_distractor)
        << "P(440)=" << at_tone << " P(880)=" << at_distractor;
    // A silent buffer would satisfy the ratio with 0 >= 0. The tone is 0.1 amplitude, 0.0707 RMS.
    EXPECT_GE(pwcap::test::rms(left), 0.02);
}
