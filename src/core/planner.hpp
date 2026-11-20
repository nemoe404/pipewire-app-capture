#pragma once

// The link planner: a pure function from (snapshot, selection, capture inputs) to the links to add.

#include "core/graph.hpp"
#include "core/match.hpp"

#include <span>
#include <vector>

namespace pwcap::core {

struct LinkPlan {
    std::vector<LinkInfo> to_add{};
    std::vector<LinkInfo> to_remove{};

    [[nodiscard]] bool empty() const noexcept { return to_add.empty() && to_remove.empty(); }

    friend bool operator==(const LinkPlan&, const LinkPlan&) = default;
};

// Ids of the selected nodes, ascending. Only nodes of class Stream/Output/Audio are ever
// returned, whatever the Selection says (an allowlist applied before the Match).
[[nodiscard]] std::vector<NodeId> select_sources(const GraphSnapshot& snapshot,
                                                 const Selection& selection);

// Links to add so the capture inputs receive the selected nodes' output ports, paired by equal
// non-empty channel. Deterministic: sorted, de-duplicated, and empty once applied.
[[nodiscard]] LinkPlan plan_links(const GraphSnapshot& snapshot, const Selection& selection,
                                  std::span<const PortId> capture_inputs);

}  // namespace pwcap::core
