#include "core/graph.hpp"
#include "core/match.hpp"
#include "core/planner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace pwcap::core {
namespace {

constexpr auto kOut = Direction::Output;
constexpr auto kIn = Direction::Input;

// Small fluent builder so each test reads as the graph it describes.
class GraphBuilder {
public:
    GraphBuilder& node(NodeId id, std::string node_name, std::string app_name,
                       std::string media_class, std::string media_name = {}) {
        snapshot_.nodes.push_back(NodeInfo{.id = id,
                                           .serial = id,
                                           .node_name = std::move(node_name),
                                           .app_name = std::move(app_name),
                                           .media_class = std::move(media_class),
                                           .media_name = std::move(media_name)});
        return *this;
    }

    GraphBuilder& port(NodeId node_id, PortId port_id, Direction direction, std::string channel,
                       bool monitor = false) {
        snapshot_.ports.push_back(PortInfo{.node_id = node_id,
                                           .port_id = port_id,
                                           .direction = direction,
                                           .channel = std::move(channel),
                                           .monitor = monitor});
        return *this;
    }

    GraphBuilder& link(PortId output, PortId input) {
        snapshot_.links.push_back(LinkInfo{.output_port_id = output, .input_port_id = input});
        return *this;
    }

    [[nodiscard]] const GraphSnapshot& snapshot() const noexcept { return snapshot_; }

private:
    GraphSnapshot snapshot_;
};

LinkInfo link(PortId output, PortId input) {
    return LinkInfo{.output_port_id = output, .input_port_id = input};
}

std::vector<LinkInfo> links(std::initializer_list<LinkInfo> list) {
    return std::vector<LinkInfo>{list};
}

Selection selection_of(Match include, std::vector<Match> exclude = {}) {
    auto selection = Selection::make(std::move(include), std::move(exclude));
    EXPECT_TRUE(selection.has_value());
    return *std::move(selection);
}

Match app(std::string name) {
    return Match{.app_name = Pattern{.text = std::move(name), .kind = MatchKind::Exact}};
}

const std::string kStreamOut{kStreamOutputAudio};
const std::string kStreamIn{kStreamInputAudio};

// Node 10 "player" (selected by app name) with the given source ports, plus capture node 20 with
// the given input channels. Ports 11.. are outputs, 21.. are inputs.
GraphBuilder player_and_capture(std::initializer_list<const char*> source_channels,
                                std::initializer_list<const char*> capture_channels) {
    GraphBuilder graph;
    graph.node(10, "player-node", "player", kStreamOut);
    graph.node(20, "recorder", "pw-cat", kStreamIn);
    PortId next = 11;
    for (const char* channel : source_channels) { // ids are handed out in listing order
        graph.port(10, next++, kOut, channel);
    }
    next = 21;
    for (const char* channel : capture_channels) { // same, for the capture inputs
        graph.port(20, next++, kIn, channel);
    }
    return graph;
}

TEST(PlannerRules, MonoSourceFeedsEveryCaptureInput) {
    const auto graph = player_and_capture({"MONO"}, {"FL", "FR"});
    const std::array<PortId, 2> inputs{21, 22};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21), link(11, 22)}));
    EXPECT_TRUE(plan.to_remove.empty());
}

TEST(PlannerRules, StereoSourceIntoMonoCapture) {
    const auto graph = player_and_capture({"FL", "FR"}, {"MONO"});
    const std::array<PortId, 1> inputs{21};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21), link(12, 21)}));
    EXPECT_TRUE(plan.to_remove.empty());
}

TEST(PlannerRules, SurroundIntoStereoLinksOnlyMatchingChannels) {
    const auto graph = player_and_capture({"FL", "FR", "FC", "LFE", "RL", "RR"}, {"FL", "FR"});
    const std::array<PortId, 2> inputs{21, 22};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21), link(12, 22)}));
}

TEST(PlannerRules, SurroundIntoMonoLinksEveryChannel) {
    const auto graph = player_and_capture({"FL", "FR", "FC", "LFE", "RL", "RR"}, {"MONO"});
    const std::array<PortId, 1> inputs{21};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21), link(12, 21), link(13, 21), link(14, 21),
                                  link(15, 21), link(16, 21)}));
}

TEST(PlannerRules, EmptyChannelNeverLinked) {
    // Port 11 has no channel, port 12 is FL. Neither the MONO nor the FL input takes port 11.
    const auto graph = player_and_capture({"", "FL"}, {"MONO"});
    const std::array<PortId, 1> inputs{21};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_add, links({link(12, 21)}));

    const auto stereo = player_and_capture({"", "FL"}, {"FL", "FR"});
    const std::array<PortId, 2> stereo_inputs{21, 22};
    const auto stereo_plan =
        plan_links(stereo.snapshot(), selection_of(app("player")), stereo_inputs);
    EXPECT_EQ(stereo_plan.to_add, links({link(12, 21)}));
}

TEST(PlannerRules, MonitorPortOnSelectedStreamNeverLinked) {
    auto graph = player_and_capture({"FL"}, {"FL"});
    graph.port(10, 13, kOut, "FL", true);
    const std::array<PortId, 1> inputs{21};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21)}));
}

TEST(PlannerRules, TwoStreamsOfOneAppBothLinked) {
    // Brave publishes one stream node per tab (s0-item4): both belong to the application.
    GraphBuilder graph;
    graph.node(10, "tab-a", "Brave", kStreamOut);
    graph.node(30, "tab-b", "Brave", kStreamOut);
    graph.node(20, "recorder", "pw-cat", kStreamIn);
    graph.port(10, 11, kOut, "FL").port(10, 12, kOut, "FR");
    graph.port(30, 31, kOut, "FL").port(30, 32, kOut, "FR");
    graph.port(20, 21, kIn, "FL").port(20, 22, kIn, "FR");
    const std::array<PortId, 2> inputs{21, 22};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("Brave")), inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21), link(12, 22), link(31, 21), link(32, 22)}));
}

TEST(PlannerRules, ExcludedTabNotLinked) {
    // Firefox tab nodes (s0-item4): the exclude on tab 880 leaves only tab 440 linked.
    GraphBuilder graph;
    graph.node(10, "tab-440", "Firefox", kStreamOut, "pwspike-tab-440");
    graph.node(30, "tab-880", "Firefox", kStreamOut, "pwspike-tab-880");
    graph.node(20, "recorder", "pw-cat", kStreamIn);
    graph.port(10, 11, kOut, "FL").port(10, 12, kOut, "FR");
    graph.port(30, 31, kOut, "FL").port(30, 32, kOut, "FR");
    graph.port(20, 21, kIn, "FL").port(20, 22, kIn, "FR");
    const std::array<PortId, 2> inputs{21, 22};
    const auto selection = selection_of(
        app("Firefox"),
        {Match{.media_name = Pattern{.text = "pwspike-tab-880", .kind = MatchKind::Exact}}});
    const auto plan = plan_links(graph.snapshot(), selection, inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21), link(12, 22)}));
}

// A graph with one selected stereo player, a stereo capture and a set of foreign sources: a
// microphone (41), a sink with a playback port (51) and a monitor output (52), and a stream of
// another application (61).
GraphBuilder foreign_graph() {
    auto graph = player_and_capture({"FL", "FR"}, {"FL", "FR"});
    graph.node(40, "alsa_input.mic", "", "Audio/Source");
    graph.port(40, 41, kOut, "FL");
    graph.node(50, "alsa_output.speakers", "", "Audio/Sink");
    graph.port(50, 51, kIn, "FL").port(50, 52, kOut, "FL", true);
    graph.node(60, "other-node", "other", kStreamOut);
    graph.port(60, 61, kOut, "FL");
    return graph;
}

TEST(PlannerRules, StaleAndForeignLinksIntoCaptureAreRemoved) {
    auto graph = foreign_graph();
    graph.link(41, 21); // microphone
    graph.link(52, 21); // sink monitor
    graph.link(61, 22); // a stream that is not selected
    graph.link(11, 22); // cross-channel: FL source into the FR input
    graph.link(11, 21); // correct
    graph.link(12, 22); // correct
    const std::array<PortId, 2> inputs{21, 22};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_remove, links({link(11, 22), link(41, 21), link(52, 21), link(61, 22)}));
    EXPECT_TRUE(plan.to_add.empty());
}

TEST(PlannerRules, SpeakerLinksAreNeverRemoved) {
    GraphBuilder graph;
    graph.node(10, "player-node", "player", kStreamOut);
    graph.node(20, "recorder", "pw-cat", kStreamIn);
    graph.node(50, "alsa_output.speakers", "", "Audio/Sink");
    graph.port(10, 11, kOut, "FL").port(10, 12, kOut, "FR");
    graph.port(20, 21, kIn, "FL").port(20, 22, kIn, "FR");
    graph.port(50, 51, kIn, "FL").port(50, 52, kIn, "FR");
    graph.link(11, 51).link(12, 52);
    // The shell mistakenly passes the speaker playback ports along with the capture inputs.
    const std::array<PortId, 4> inputs{21, 22, 51, 52};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_TRUE(plan.to_remove.empty());
    EXPECT_EQ(plan.to_add, links({link(11, 21), link(12, 22)}));

    const std::array<PortId, 2> only_speakers{51, 52};
    const auto none = plan_links(graph.snapshot(), selection_of(app("player")), only_speakers);
    EXPECT_TRUE(none.empty());
}

TEST(PlannerRules, InvalidCaptureInputsAreIgnored) {
    auto graph = player_and_capture({"FL"}, {"FL"});
    graph.port(20, 23, kIn, "FL", true); // monitor-flagged input of the capture node
    graph.node(40, "alsa_input.mic", "", "Audio/Source");
    graph.port(40, 41, kOut, "FL");
    graph.node(50, "alsa_output.speakers", "", "Audio/Sink");
    graph.port(50, 51, kIn, "FL");
    graph.link(41, 23).link(41, 51);
    // 999 is not in the snapshot, 11 is an output, 23 is a monitor, 51 is owned by a sink.
    const std::array<PortId, 5> inputs{999, 11, 23, 51, 21};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_add, links({link(11, 21)}));
    EXPECT_TRUE(plan.to_remove.empty());
}

TEST(PlannerRules, DanglingReferencesAreSafe) {
    auto graph = player_and_capture({"FL"}, {"FL"});
    graph.port(999, 70, kOut, "FL"); // port whose node is not in the snapshot
    graph.link(88, 21);              // output port 88 does not exist
    graph.link(88, 21);              // duplicated
    graph.link(11, 21);              // correct, and duplicated
    graph.link(11, 21);
    const std::array<PortId, 1> inputs{21};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(plan.to_remove, links({link(88, 21)}));
    EXPECT_TRUE(plan.to_add.empty());

    GraphBuilder missing_link = player_and_capture({"FL"}, {"FL"});
    missing_link.port(999, 70, kOut, "FL");
    const auto added = plan_links(missing_link.snapshot(), selection_of(app("player")), inputs);
    EXPECT_EQ(added.to_add, links({link(11, 21)}));
}

TEST(PlannerRules, RestartRegressionRecycledIds) {
    // s0-item3 case 3A after the respawn: node 104 came back with serial 1993 (it was 1968), and
    // port 138, which used to be FR, is now FL. The recorder inputs are 133 FL and 129 FR.
    GraphBuilder graph;
    graph.node(104, "pwspike-tone440", "pw-play", kStreamOut);
    graph.node(90, "pwspike-rec", "pw-cat", kStreamIn);
    graph.node(31, "alsa_output.pci-0000_00_1f.3.analog-stereo", "", "Audio/Sink");
    graph.port(104, 138, kOut, "FL").port(104, 108, kOut, "FR");
    graph.port(90, 133, kIn, "FL").port(90, 129, kIn, "FR");
    graph.port(31, 40, kIn, "FL").port(31, 47, kIn, "FR");
    graph.link(138, 40).link(108, 47);
    const auto selection = selection_of(
        Match{.node_name = Pattern{.text = "pwspike-tone440", .kind = MatchKind::Exact}});
    const std::array<PortId, 2> inputs{133, 129};

    const auto fresh = plan_links(graph.snapshot(), selection, inputs);
    EXPECT_EQ(fresh.to_add, links({link(108, 129), link(138, 133)}));
    EXPECT_TRUE(fresh.to_remove.empty());

    // The link of the previous incarnation (138 was FR then) is stale now.
    graph.link(138, 129);
    const auto stale = plan_links(graph.snapshot(), selection, inputs);
    EXPECT_EQ(stale.to_add, links({link(108, 129), link(138, 133)}));
    EXPECT_EQ(stale.to_remove, links({link(138, 129)}));
}

TEST(PlannerRules, OrderOfSnapshotVectorsDoesNotMatter) {
    auto graph = foreign_graph();
    graph.link(41, 21).link(52, 21).link(61, 22).link(11, 22).link(11, 21);
    const std::array<PortId, 2> inputs{21, 22};
    const auto selection = selection_of(app("player"));
    const auto forward = plan_links(graph.snapshot(), selection, inputs);

    GraphSnapshot reversed = graph.snapshot();
    std::ranges::reverse(reversed.nodes);
    std::ranges::reverse(reversed.ports);
    std::ranges::reverse(reversed.links);
    EXPECT_EQ(plan_links(reversed, selection, inputs), forward);
    EXPECT_FALSE(forward.empty());
}

TEST(PlannerRules, NoSelectedSourceRemovesForeignLinks) {
    auto graph = foreign_graph();
    graph.link(41, 21);
    const std::array<PortId, 2> inputs{21, 22};
    const auto plan = plan_links(graph.snapshot(), selection_of(app("nobody")), inputs);
    EXPECT_TRUE(plan.to_add.empty());
    EXPECT_EQ(plan.to_remove, links({link(41, 21)}));
}

} // namespace
} // namespace pwcap::core
