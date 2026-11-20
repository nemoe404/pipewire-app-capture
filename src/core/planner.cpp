#include "core/planner.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>

namespace pwcap::core {
namespace {

// Ids are recycled by the server, so nodes and ports are found by id, never indexed by it.
const NodeInfo* find_node(const GraphSnapshot& snapshot, NodeId id) {
    const auto found = std::ranges::find(snapshot.nodes, id, &NodeInfo::id);
    return found == snapshot.nodes.end() ? nullptr : &*found;
}

const PortInfo* find_port(const GraphSnapshot& snapshot, PortId id) {
    const auto found = std::ranges::find(snapshot.ports, id, &PortInfo::port_id);
    return found == snapshot.ports.end() ? nullptr : &*found;
}

// A capture input is usable only when it is an Input, non-monitor port of a Stream/Input/Audio
// node, all found in the snapshot. Anything else fails closed: it is neither paired nor used to
// decide which links to remove, so a speaker playback port passed by mistake can never cost the
// application its link to the speakers.
bool is_usable_capture_input(const GraphSnapshot& snapshot, const PortInfo& port) {
    if (port.direction != Direction::Input || port.monitor) {
        return false;
    }
    const NodeInfo* owner = find_node(snapshot, port.node_id);
    return owner != nullptr && owner->media_class == kStreamInputAudio;
}

// Output, non-monitor and channel-carrying: the only ports that may feed the capture.
bool is_source_port(const PortInfo& port) {
    return port.direction == Direction::Output && !port.monitor && !port.channel.empty();
}

// Equal channel names pair; a MONO port on either side pairs with everything on the other side.
// The pure planner only returns the pairs. That the server mixes several links into one MONO
// input port is not verified (open item for the Phase 4 graph test).
bool channels_pair(const PortInfo& source, const PortInfo& input) {
    return source.channel == input.channel || source.channel == kChannelMono ||
           input.channel == kChannelMono;
}

// Ascending by (output, input), without duplicates: independent of the snapshot vector order.
void sort_unique(std::vector<LinkInfo>& links) {
    std::ranges::sort(links);
    const auto duplicates = std::ranges::unique(links);
    links.erase(duplicates.begin(), duplicates.end());
}

} // namespace

std::vector<NodeId> select_sources(const GraphSnapshot& snapshot, const Selection& selection) {
    auto ids = snapshot.nodes | std::views::filter([&selection](const NodeInfo& node) {
                   return node.media_class == kStreamOutputAudio && selection.selects(node);
               }) |
               std::views::transform(&NodeInfo::id) | std::ranges::to<std::vector>();
    std::ranges::sort(ids);
    return ids;
}

LinkPlan plan_links(const GraphSnapshot& snapshot, const Selection& selection,
                    std::span<const PortId> capture_inputs) {
    const auto sources = select_sources(snapshot, selection);

    const auto source_ports =
        snapshot.ports | std::views::filter([&sources](const PortInfo& port) {
            return is_source_port(port) && std::ranges::binary_search(sources, port.node_id);
        }) |
        std::ranges::to<std::vector>();

    // Each requested id resolves to at most one port; ids that fail validation drop out here.
    const auto input_ports =
        capture_inputs |
        std::views::transform([&snapshot](PortId id) { return find_port(snapshot, id); }) |
        std::views::filter([&snapshot](const PortInfo* port) {
            return port != nullptr && is_usable_capture_input(snapshot, *port);
        }) |
        std::views::transform([](const PortInfo* port) { return *port; }) |
        std::ranges::to<std::vector>();

    auto desired =
        std::views::cartesian_product(source_ports, input_ports) |
        std::views::filter([](const auto& pair) {
            const auto& [source, input] = pair;
            return channels_pair(source, input);
        }) |
        std::views::transform([](const auto& pair) {
            const auto& [source, input] = pair;
            return LinkInfo{.output_port_id = source.port_id, .input_port_id = input.port_id};
        }) |
        std::ranges::to<std::vector>();
    sort_unique(desired);

    auto to_add = desired | std::views::filter([&snapshot](const LinkInfo& link) {
                      return !std::ranges::contains(snapshot.links, link);
                  }) |
                  std::ranges::to<std::vector>();

    // Only links that end at a usable capture input are judged; every other link, the speaker
    // links above all, is left alone.
    auto to_remove =
        snapshot.links | std::views::filter([&input_ports, &desired](const LinkInfo& link) {
            const bool into_capture =
                std::ranges::contains(input_ports, link.input_port_id, &PortInfo::port_id);
            return into_capture && !std::ranges::binary_search(desired, link);
        }) |
        std::ranges::to<std::vector>();
    sort_unique(to_remove);

    return LinkPlan{.to_add = std::move(to_add), .to_remove = std::move(to_remove)};
}

} // namespace pwcap::core
