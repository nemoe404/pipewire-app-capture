#pragma once

// Pure value types describing a snapshot of the audio graph. No I/O and no audio-server headers:
// the shell builds a GraphSnapshot from the registry, the core only reads it.

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pwcap::core {

using NodeId = std::uint32_t;
using PortId = std::uint32_t;
using Serial = std::uint64_t;

enum class Direction : std::uint8_t { Input, Output };

inline constexpr std::string_view kStreamOutputAudio = "Stream/Output/Audio";
inline constexpr std::string_view kStreamInputAudio = "Stream/Input/Audio";
inline constexpr std::string_view kChannelMono = "MONO";

struct NodeInfo {
    NodeId id{};
    Serial serial{};
    std::string node_name{};
    std::string app_name{};
    std::string binary{};
    std::string media_class{};
    std::string media_name{};
    // Optional: application.process.id is absent for some streams (pw-play, for one).
    std::optional<std::int32_t> pid{};

    friend bool operator==(const NodeInfo&, const NodeInfo&) = default;
};

struct PortInfo {
    NodeId node_id{};
    PortId port_id{};
    Direction direction{Direction::Input};
    // Raw audio.channel value (FL, FR, MONO, ...), empty when the port has none.
    std::string channel{};
    // Mirrors port.monitor.
    bool monitor{false};

    friend bool operator==(const PortInfo&, const PortInfo&) = default;
};

struct LinkInfo {
    PortId output_port_id{};
    PortId input_port_id{};

    friend auto operator<=>(const LinkInfo&, const LinkInfo&) = default;
    friend bool operator==(const LinkInfo&, const LinkInfo&) = default;
};

struct GraphSnapshot {
    std::vector<NodeInfo> nodes{};
    std::vector<PortInfo> ports{};
    std::vector<LinkInfo> links{};

    friend bool operator==(const GraphSnapshot&, const GraphSnapshot&) = default;
};

}  // namespace pwcap::core
