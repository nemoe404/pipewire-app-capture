#include "core/planner.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace pwcap::core {

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

    const auto source_ports = snapshot.ports | std::views::filter([&sources](const PortInfo& port) {
                                  return port.direction == Direction::Output &&
                                         std::ranges::binary_search(sources, port.node_id);
                              }) |
                              std::ranges::to<std::vector>();

    // Ids are recycled by the server, so ports are found by id, never indexed by it.
    const auto input_ports = snapshot.ports |
                             std::views::filter([capture_inputs](const PortInfo& port) {
                                 return port.direction == Direction::Input &&
                                        std::ranges::contains(capture_inputs, port.port_id);
                             }) |
                             std::ranges::to<std::vector>();

    auto to_add =
        std::views::cartesian_product(source_ports, input_ports) |
        std::views::filter([](const auto& pair) {
            const auto& [source, input] = pair;
            return !source.channel.empty() && source.channel == input.channel;
        }) |
        std::views::transform([](const auto& pair) {
            const auto& [source, input] = pair;
            return LinkInfo{.output_port_id = source.port_id, .input_port_id = input.port_id};
        }) |
        std::views::filter([&snapshot](const LinkInfo& link) {
            return !std::ranges::contains(snapshot.links, link);
        }) |
        std::ranges::to<std::vector>();

    std::ranges::sort(to_add);
    const auto duplicates = std::ranges::unique(to_add);
    to_add.erase(duplicates.begin(), duplicates.end());

    return LinkPlan{.to_add = std::move(to_add), .to_remove = {}};
}

} // namespace pwcap::core
