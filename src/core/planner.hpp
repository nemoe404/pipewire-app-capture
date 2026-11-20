#ifndef PWCAP_CORE_PLANNER_HPP
#define PWCAP_CORE_PLANNER_HPP

// The link planner: a pure function from (snapshot, selection, capture inputs) to the links to add.

#include "core/graph.hpp"
#include "core/match.hpp"

#include <span>
#include <vector>

namespace pwcap::core {

struct LinkPlan {
    // Plain result aggregate: the shell and tests read and designated-initialise both lists, and
    // making them private would change the public signature that plan 02-05 reads in parallel.
    // The empty initialisers are kept so -Wmissing-designated-field-initializers stays quiet.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes,readability-redundant-member-init)
    std::vector<LinkInfo> to_add{};
    std::vector<LinkInfo> to_remove{};
    // NOLINTEND(misc-non-private-member-variables-in-classes,readability-redundant-member-init)

    [[nodiscard]] bool empty() const noexcept { return to_add.empty() && to_remove.empty(); }

    friend bool operator==(const LinkPlan&, const LinkPlan&) = default;
};

// Ids of the selected nodes, ascending. Only nodes of class Stream/Output/Audio are ever
// returned, whatever the Selection says (an allowlist applied before the Match).
[[nodiscard]] std::vector<NodeId> select_sources(const GraphSnapshot& snapshot,
                                                 const Selection& selection);

// The links to add and to remove so the capture inputs receive exactly the selected nodes' audio.
//
// Capture inputs. An id in capture_inputs is used only if it is found in the snapshot, is an Input
// port, is not a monitor port, and belongs to a Stream/Input/Audio node. Every other id is ignored
// (fails closed): it is never paired and never used to decide a removal.
//
// Source ports. Output, non-monitor ports with a non-empty channel, of the nodes select_sources
// returns (Stream/Output/Audio only). Microphones, Audio/Source nodes named default.audio.source,
// virtual sources, sinks and other recorders are never sources, even when the Match names them.
//
// Pairing. A source port and a capture input pair when their channel names are equal, or when
// either one is MONO. Nothing else pairs, so FC or LFE into an FL/FR capture get no link. The MONO
// rule is covered by unit tests on this pure function only; that the server mixes several links
// into one MONO input port is NOT verified (open item for the Phase 4 graph test).
//
// to_add: paired links absent from the snapshot. to_remove: links in the snapshot that end at a
// usable capture input and are not paired (stale, cross-channel, microphone, monitor, dangling).
// Links that end anywhere else, the application's link to the speakers above all, are never in
// the plan.
//
// Deterministic: both lists are sorted by (output port, input port) and de-duplicated, so the
// result does not depend on the order of the snapshot vectors, and the plan for the snapshot with
// the plan applied is empty.
[[nodiscard]] LinkPlan plan_links(const GraphSnapshot& snapshot, const Selection& selection,
                                  std::span<const PortId> capture_inputs);

} // namespace pwcap::core

#endif // PWCAP_CORE_PLANNER_HPP
