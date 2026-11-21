// Integration tests against a live PipeWire session. Opt-in: every test starts with the gate in
// pw_session.hpp and skips with a printed reason unless PWCAP_TEST_PIPEWIRE=1 and `pw-cli info 0`
// answers. Graph state read from pw-dump is the ground truth; the library never judges itself.

#include "pw_session.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
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
