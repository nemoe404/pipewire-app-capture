#ifndef PWCAP_TESTS_CORE_SNAPSHOT_GEN_HPP
#define PWCAP_TESTS_CORE_SNAPSHOT_GEN_HPP

// Seeded generator of adversarial GraphSnapshots for the planner property tests. Every random
// choice comes from the engine the caller passes in, so a seed reproduces a snapshot exactly.
//
// Ground truth is kept next to the snapshot: the role of every node and the sets of ports that a
// correct planner may link from and to. The oracle in the tests reads only these, never a planner
// predicate, so it cannot agree with the planner by sharing its mistakes.

#include "core/graph.hpp"
#include "core/match.hpp"
#include "core/planner.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pwcap::core::gen {

enum class Role : std::uint8_t {
    Target,
    Excluded,
    OtherApp,
    Microphone,
    DefaultSourceAlias,
    VirtualSource,
    Sink,
    OtherRecorder,
    Capture
};

struct Generated {
    GraphSnapshot snapshot;
    Selection selection;
    std::vector<PortId> capture_inputs;
    std::map<NodeId, Role> role;
    // Output, non-monitor, non-empty-channel ports of Target nodes: the only legal sources.
    std::set<PortId> eligible_sources;
    // Input, non-monitor ports of the Capture node: the only legal destinations.
    std::set<PortId> valid_capture_inputs;
    // The capture_inputs list carries an id the planner has to reject.
    bool adversarial_extra{false};
    // A Microphone or DefaultSourceAlias node shares app_name or node_name with the target.
    bool lookalike_source{false};
};

// A copy of the snapshot with to_remove erased and to_add appended.
inline GraphSnapshot apply_plan(GraphSnapshot snapshot, const LinkPlan& plan) {
    std::erase_if(snapshot.links, [&plan](const LinkInfo& link) {
        return std::ranges::contains(plan.to_remove, link);
    });
    std::ranges::copy(plan.to_add, std::back_inserter(snapshot.links));
    return snapshot;
}

namespace detail {

using Layout = std::vector<std::string>;

inline const Layout kMono{"MONO"};
inline const Layout kStereo{"FL", "FR"};
inline const Layout kSurround{"FL", "FR", "FC", "LFE", "RL", "RR"};

inline constexpr std::uint32_t kIdPoolSize = 400;

// The strings every look-alike node copies from the target.
struct Identity {
    std::string app_name;
    std::string binary;
    std::string node_name;
    std::string media_name;
};

class Builder {
public:
    explicit Builder(std::mt19937_64& rng) : rng_(&rng), pool_(kIdPoolSize) {
        std::ranges::iota(pool_, std::uint32_t{1});
        std::ranges::shuffle(pool_, *rng_);
    }

    Generated build() {
        token_ = 100 + below(900);
        identity_ = Identity{.app_name = "tgt" + std::to_string(token_) + "-app",
                             .binary = "tgt" + std::to_string(token_) + "-bin",
                             .node_name = "tgt" + std::to_string(token_) + "-node",
                             .media_name = "tgt" + std::to_string(token_) + "-media"};
        add_targets();
        add_excluded();
        add_other_apps();
        add_lookalike_sources();
        add_sinks();
        add_other_recorders();
        add_capture();
        add_capture_extras();
        add_links();
        return finish();
    }

private:
    [[nodiscard]] std::size_t below(std::size_t bound) {
        return static_cast<std::size_t>((*rng_)() % static_cast<std::uint64_t>(bound));
    }

    [[nodiscard]] bool chance(std::size_t percent) { return below(100) < percent; }

    [[nodiscard]] std::uint32_t next_id() {
        const auto id = pool_.back();
        pool_.pop_back();
        return id;
    }

    template <typename T> [[nodiscard]] const T& pick(const std::vector<T>& values) {
        return values.at(below(values.size()));
    }

    [[nodiscard]] const Layout& random_layout() {
        switch (below(3)) {
        case 0:
            return kMono;
        case 1:
            return kStereo;
        default:
            return kSurround;
        }
    }

    NodeId add_node(Role role, std::string node_name, std::string app_name, std::string binary,
                    std::string media_class, std::string media_name) {
        const NodeId id = next_id();
        snapshot_.nodes.push_back(NodeInfo{.id = id,
                                           .serial = id,
                                           .node_name = std::move(node_name),
                                           .app_name = std::move(app_name),
                                           .binary = std::move(binary),
                                           .media_class = std::move(media_class),
                                           .media_name = std::move(media_name)});
        role_.emplace(id, role);
        return id;
    }

    PortId add_port(NodeId node, Direction direction, const std::string& channel, bool monitor) {
        const PortId id = next_id();
        snapshot_.ports.push_back(PortInfo{.node_id = node,
                                           .port_id = id,
                                           .direction = direction,
                                           .channel = channel,
                                           .monitor = monitor});
        if (direction == Direction::Output) {
            all_outputs_.push_back(id);
        }
        return id;
    }

    // Output ports, one per channel of the layout. Returns their ids.
    std::vector<PortId> add_outputs(NodeId node, const Layout& layout) {
        std::vector<PortId> ids;
        std::ranges::for_each(layout, [&](const std::string& channel) {
            ids.push_back(add_port(node, Direction::Output, channel, false));
        });
        return ids;
    }

    // A node of class Stream/Output/Audio that carries the target's identity. `media_name` lets
    // an excluded node differ from the targets.
    NodeId add_stream_output(Role role, const std::string& app_name, const std::string& binary,
                             const std::string& node_name, const std::string& media_name) {
        return add_node(role, node_name, app_name, binary, std::string{kStreamOutputAudio},
                        media_name);
    }

    void add_targets() {
        const std::size_t count = 1 + below(3);
        for (std::size_t index = 0; index < count; ++index) { // one node per iteration
            const NodeId node =
                add_stream_output(Role::Target, identity_.app_name, identity_.binary,
                                  identity_.node_name, identity_.media_name);
            const auto ports = add_outputs(node, random_layout());
            eligible_.insert(ports.begin(), ports.end());
            speaker_sources_.insert(speaker_sources_.end(), ports.begin(), ports.end());
            if (chance(30)) {
                // Adversarial: a port without a channel must never be linked.
                add_port(node, Direction::Output, "", false);
            }
            if (chance(30)) {
                // Adversarial: a port with monitor = true on the selected stream must never be
                // linked.
                add_port(node, Direction::Output, pick(kStereo), true);
            }
        }
    }

    void add_excluded() {
        const std::size_t count = below(3);
        for (std::size_t index = 0; index < count; ++index) { // one node per iteration
            const std::string media =
                "tgt" + std::to_string(token_) + "-excluded" + std::to_string(index);
            const NodeId node = add_stream_output(Role::Excluded, identity_.app_name,
                                                  identity_.binary, identity_.node_name, media);
            const auto ports = add_outputs(node, random_layout());
            speaker_sources_.insert(speaker_sources_.end(), ports.begin(), ports.end());
            excluded_media_.push_back(media);
        }
    }

    void add_other_apps() {
        const std::size_t count = below(4);
        for (std::size_t index = 0; index < count; ++index) { // one node per iteration
            const std::string tag = "oth" + std::to_string(token_) + "-" + std::to_string(index);
            const NodeId node = add_stream_output(Role::OtherApp, tag + "-app", tag + "-bin",
                                                  tag + "-node", tag + "-media");
            const auto ports = add_outputs(node, random_layout());
            speaker_sources_.insert(speaker_sources_.end(), ports.begin(), ports.end());
        }
    }

    // Sources that copy the target's names exactly, so only the class rules can reject them.
    NodeId add_lookalike(Role role, const std::string& node_name, std::string media_class) {
        return add_node(role, node_name, identity_.app_name, identity_.binary,
                        std::move(media_class), identity_.media_name);
    }

    void add_lookalike_sources() {
        const auto add_stereo = [this](NodeId node) { add_outputs(node, kStereo); };
        if (chance(97)) {
            add_stereo(add_lookalike(Role::Microphone, identity_.node_name, "Audio/Source"));
            lookalike_source_ = true;
        }
        if (chance(50)) {
            add_stereo(
                add_lookalike(Role::DefaultSourceAlias, "default.audio.source", "Audio/Source"));
            lookalike_source_ = true;
        }
        if (chance(50)) {
            add_stereo(
                add_lookalike(Role::VirtualSource, identity_.node_name, "Audio/Source/Virtual"));
        }
    }

    void add_sinks() {
        const std::size_t count = 1 + below(2);
        for (std::size_t index = 0; index < count; ++index) { // one node per iteration
            const NodeId node = add_node(Role::Sink, identity_.node_name, identity_.app_name,
                                         identity_.binary, "Audio/Sink", identity_.media_name);
            std::ranges::for_each(kStereo, [&](const std::string& channel) {
                sink_playback_.push_back(add_port(node, Direction::Input, channel, false));
            });
            // Most monitor ports carry monitor=true; sometimes the flag is missing, as if the
            // shell had forgotten to set it. The node class must still keep it out.
            const bool forgot_flag = chance(25);
            std::ranges::for_each(kStereo, [&](const std::string& channel) {
                // monitor = true on every port, except the first one when the flag is forgotten
                const bool monitor = !(forgot_flag && channel == kStereo.front());
                add_port(node, Direction::Output, channel, monitor);
            });
        }
    }

    void add_other_recorders() {
        const std::size_t count = below(3);
        for (std::size_t index = 0; index < count; ++index) { // one node per iteration
            const NodeId node =
                add_node(Role::OtherRecorder, identity_.node_name, identity_.app_name,
                         identity_.binary, std::string{kStreamInputAudio}, identity_.media_name);
            std::ranges::for_each(kStereo, [&](const std::string& channel) {
                add_port(node, Direction::Input, channel, false);
                add_port(node, Direction::Output, channel, true); // monitor = true
            });
        }
    }

    void add_capture() {
        const NodeId node = add_node(Role::Capture, "pwcap-capture", "pwcap", "pwcap-bin",
                                     std::string{kStreamInputAudio}, "pwcap-capture");
        const Layout& layout = chance(50) ? kMono : kStereo;
        std::ranges::for_each(layout, [&](const std::string& channel) {
            const PortId port = add_port(node, Direction::Input, channel, false);
            valid_inputs_.insert(port);
            capture_inputs_.push_back(port);
        });
    }

    // Ids the planner has to reject: a speaker playback port, an output port and an unknown id.
    // Another recorder's input is not among them: telling two recorders apart is the shell's job.
    void add_capture_extras() {
        if (!chance(40)) {
            return;
        }
        adversarial_extra_ = true;
        const bool playback = chance(60);
        const bool output = chance(50);
        const bool unknown = chance(50);
        if (playback || !(output || unknown)) {
            capture_inputs_.push_back(pick(sink_playback_));
        }
        if (output) {
            capture_inputs_.push_back(pick(all_outputs_));
        }
        if (unknown) {
            capture_inputs_.push_back(next_id());
        }
    }

    void add_link(PortId output, PortId input) {
        snapshot_.links.push_back(LinkInfo{.output_port_id = output, .input_port_id = input});
    }

    void add_links() {
        // Speaker links, from every application stream to a sink playback port.
        std::ranges::for_each(speaker_sources_, [this](PortId source) {
            if (chance(70)) {
                add_link(source, pick(sink_playback_));
            }
        });
        // Links already in the capture: right and wrong channel, microphone and monitor sources.
        std::ranges::for_each(valid_inputs_, [this](PortId input) {
            const std::size_t count = below(4);
            for (std::size_t index = 0; index < count; ++index) { // `count` random sources
                add_link(pick(all_outputs_), input);
            }
        });
        if (chance(25)) {
            add_link(next_id(), pick(capture_inputs_)); // output port no longer in the snapshot
        }
        if (chance(15)) {
            add_link(pick(all_outputs_), next_id()); // input port no longer in the snapshot
        }
        if (chance(20) && !snapshot_.links.empty()) {
            snapshot_.links.push_back(pick(snapshot_.links)); // a duplicate entry
        }
    }

    Generated finish() {
        std::ranges::shuffle(snapshot_.nodes, *rng_);
        std::ranges::shuffle(snapshot_.ports, *rng_);
        std::ranges::shuffle(snapshot_.links, *rng_);
        std::ranges::shuffle(capture_inputs_, *rng_);
        return Generated{.snapshot = std::move(snapshot_),
                         .selection = make_selection(),
                         .capture_inputs = std::move(capture_inputs_),
                         .role = std::move(role_),
                         .eligible_sources = std::move(eligible_),
                         .valid_capture_inputs = std::move(valid_inputs_),
                         .adversarial_extra = adversarial_extra_,
                         .lookalike_source = lookalike_source_};
    }

    Selection make_selection() {
        const auto exact = [](std::string text) {
            return Pattern{.text = std::move(text), .kind = MatchKind::Exact};
        };
        const std::string token = std::to_string(token_);
        Match include;
        switch (below(4)) {
        case 0:
            include.app_name = exact(identity_.app_name);
            break;
        case 1:
            include.binary = Pattern{.text = "tgt" + token, .kind = MatchKind::Prefix};
            break;
        case 2:
            // Spans the target token, so "oth<k>-node" cannot hit it.
            include.node_name = Pattern{.text = "gt" + token + "-no", .kind = MatchKind::Substring};
            break;
        default:
            include.app_name = exact(identity_.app_name);
            include.media_name = exact(identity_.media_name);
            break;
        }
        std::vector<Match> exclude;
        std::ranges::for_each(excluded_media_, [&](const std::string& media) {
            exclude.push_back(Match{.media_name = exact(media)});
        });
        return Selection::make(std::move(include), std::move(exclude)).value();
    }

    std::mt19937_64* rng_; // non-owning: the caller's engine
    std::vector<std::uint32_t> pool_;
    std::size_t token_{0};
    Identity identity_;
    GraphSnapshot snapshot_;
    std::map<NodeId, Role> role_;
    std::set<PortId> eligible_;
    std::set<PortId> valid_inputs_;
    std::vector<PortId> capture_inputs_;
    std::vector<PortId> all_outputs_;
    std::vector<PortId> speaker_sources_;
    std::vector<PortId> sink_playback_;
    std::vector<std::string> excluded_media_;
    bool adversarial_extra_{false};
    bool lookalike_source_{false};
};

} // namespace detail

// One snapshot, with its selection, capture inputs and ground truth.
inline Generated generate(std::mt19937_64& rng) { return detail::Builder{rng}.build(); }

} // namespace pwcap::core::gen

#endif // PWCAP_TESTS_CORE_SNAPSHOT_GEN_HPP
