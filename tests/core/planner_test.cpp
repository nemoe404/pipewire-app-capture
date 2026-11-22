#include "core/graph.hpp"
#include "core/match.hpp"
#include "core/planner.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace pwcap::core {
namespace {

constexpr auto kSinkName = "alsa_output.pci-0000_00_1f.3.analog-stereo";
constexpr auto kSourceName = "alsa_input.pci-0000_00_1f.3.analog-stereo";

NodeInfo make_node(NodeId id, Serial serial, std::string node_name, std::string app_name,
                   std::string media_class) {
    return NodeInfo{.id = id,
                    .serial = serial,
                    .node_name = std::move(node_name),
                    .app_name = std::move(app_name),
                    .media_class = std::move(media_class)};
}

PortInfo make_port(NodeId node, PortId port, Direction direction, std::string channel,
                   bool monitor = false) {
    return PortInfo{.node_id = node,
                    .port_id = port,
                    .direction = direction,
                    .channel = std::move(channel),
                    .monitor = monitor};
}

// Two pw-play streams, the sink, a microphone and the recorder.
GraphSnapshot pw_play_graph() {
    GraphSnapshot graph;
    graph.nodes = {
        make_node(126, 1814, "pwprobe-tone440", "pw-play", std::string{kStreamOutputAudio}),
        make_node(133, 1815, "pwprobe-tone880", "pw-play", std::string{kStreamOutputAudio}),
        make_node(40, 40, kSinkName, "", "Audio/Sink"),
        make_node(50, 50, kSourceName, "", "Audio/Source"),
        make_node(104, 1827, "pwprobe-rec-serial", "pw-cat", std::string{kStreamInputAudio}),
    };
    graph.ports = {
        make_port(126, 124, Direction::Output, "FL"),
        make_port(126, 127, Direction::Output, "FR"),
        make_port(133, 135, Direction::Output, "FL"),
        make_port(133, 107, Direction::Output, "FR"),
        make_port(40, 41, Direction::Input, "FL"),
        make_port(40, 42, Direction::Input, "FR"),
        make_port(40, 43, Direction::Output, "FL", true),
        make_port(40, 44, Direction::Output, "FR", true),
        make_port(50, 51, Direction::Output, "FL"),
        make_port(50, 52, Direction::Output, "FR"),
        make_port(104, 138, Direction::Input, "FL"),
        make_port(104, 130, Direction::Input, "FR"),
    };
    graph.links = {
        LinkInfo{.output_port_id = 124, .input_port_id = 41},
        LinkInfo{.output_port_id = 127, .input_port_id = 42},
        LinkInfo{.output_port_id = 135, .input_port_id = 41},
        LinkInfo{.output_port_id = 107, .input_port_id = 42},
    };
    return graph;
}

// A copy of the snapshot with to_remove erased and to_add appended.
GraphSnapshot apply(GraphSnapshot snapshot, const LinkPlan& plan) {
    std::erase_if(snapshot.links, [&plan](const LinkInfo& link) {
        return std::ranges::contains(plan.to_remove, link);
    });
    std::ranges::copy(plan.to_add, std::back_inserter(snapshot.links));
    return snapshot;
}

Selection node_name_selection(std::string name) {
    auto selection = Selection::make(
        Match{.node_name = Pattern{.text = std::move(name), .kind = MatchKind::Exact}});
    EXPECT_TRUE(selection.has_value());
    return *std::move(selection);
}

constexpr std::array<PortId, 2> kCaptureInputs{138, 130};

TEST(PlannerTracer, LinksOnlyTheSelectedStreamByChannel) {
    const auto snapshot = pw_play_graph();
    const auto plan = plan_links(snapshot, node_name_selection("pwprobe-tone440"), kCaptureInputs);

    const std::vector<LinkInfo> expected{
        LinkInfo{.output_port_id = 124, .input_port_id = 138},
        LinkInfo{.output_port_id = 127, .input_port_id = 130},
    };
    EXPECT_EQ(plan.to_add, expected);
    EXPECT_TRUE(plan.to_remove.empty());
}

TEST(PlannerTracer, SamePlanTwiceAndEmptyAfterApply) {
    const auto snapshot = pw_play_graph();
    const auto selection = node_name_selection("pwprobe-tone440");

    const auto first = plan_links(snapshot, selection, kCaptureInputs);
    const auto second = plan_links(snapshot, selection, kCaptureInputs);
    EXPECT_EQ(first, second);
    EXPECT_FALSE(first.empty());

    const auto applied = apply(snapshot, first);
    EXPECT_TRUE(plan_links(applied, selection, kCaptureInputs).empty());
}

TEST(PlannerTracer, MicrophoneNameSelectsNothing) {
    const auto snapshot = pw_play_graph();
    const auto selection = node_name_selection(kSourceName);

    EXPECT_TRUE(select_sources(snapshot, selection).empty());
    EXPECT_TRUE(plan_links(snapshot, selection, kCaptureInputs).empty());
}

TEST(PlannerTracer, WildcardIncludeIsRejected) {
    const auto selection = Selection::make(Match{});

    ASSERT_FALSE(selection.has_value());
    EXPECT_EQ(selection.error(), SelectionError::InvalidMatch);
}

} // namespace
} // namespace pwcap::core
