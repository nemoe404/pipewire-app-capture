// Unit tests of the pure conversion behind list_applications. No PipeWire is involved.

#include "shell/lister.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using pwcap::AppStream;
using pwcap::core::NodeInfo;
using pwcap::shell::to_app_streams;

// Builds a NodeInfo by assignment: designated initialisers would trip
// -Wmissing-designated-field-initializers.
[[nodiscard]] NodeInfo make_node(std::uint64_t serial, std::string app, std::string media_class,
                                 std::optional<std::int32_t> pid = std::nullopt) {
    NodeInfo node;
    node.id = static_cast<std::uint32_t>(serial);
    node.serial = serial;
    node.app_name = std::move(app);
    node.media_class = std::move(media_class);
    node.node_name = "node-" + std::to_string(serial);
    node.binary = "bin-" + node.app_name;
    node.media_name = "media-" + std::to_string(serial);
    node.pid = pid;
    return node;
}

} // namespace

TEST(ListConversion, KeepsOnlyOutputStreams) {
    const std::vector<NodeInfo> input{
        make_node(1, "player", "Stream/Output/Audio"),
        make_node(2, "recorder", "Stream/Input/Audio"),
        make_node(3, "speakers", "Audio/Sink"),
        make_node(4, "microphone", "Audio/Source"),
    };

    const std::vector<AppStream> result = to_app_streams(input);

    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result.front().app_name, "player");
    EXPECT_EQ(result.front().serial, 1U);
}

TEST(ListConversion, MapsEveryField) {
    const std::vector<NodeInfo> input{
        make_node(7, "browser", "Stream/Output/Audio", 4242),
        make_node(8, "tone", "Stream/Output/Audio", std::nullopt),
    };

    const std::vector<AppStream> result = to_app_streams(input);

    ASSERT_EQ(result.size(), 2U);
    const AppStream& with_pid = *std::ranges::find(result, std::uint64_t{7}, &AppStream::serial);
    EXPECT_EQ(with_pid.app_name, "browser");
    EXPECT_EQ(with_pid.binary, "bin-browser");
    EXPECT_EQ(with_pid.node_name, "node-7");
    EXPECT_EQ(with_pid.media_name, "media-7");
    EXPECT_EQ(with_pid.serial, 7U);
    EXPECT_EQ(with_pid.pid, std::optional<std::int32_t>{4242});

    const AppStream& without_pid = *std::ranges::find(result, std::uint64_t{8}, &AppStream::serial);
    EXPECT_EQ(without_pid.pid, std::nullopt);
}

TEST(ListConversion, SortsByAppThenSerial) {
    const std::vector<NodeInfo> input{
        make_node(9, "alpha", "Stream/Output/Audio"),
        make_node(5, "zulu", "Stream/Output/Audio"),
        make_node(3, "alpha", "Stream/Output/Audio"),
    };

    const std::vector<AppStream> result = to_app_streams(input);

    ASSERT_EQ(result.size(), 3U);
    EXPECT_EQ(result[0].app_name, "alpha");
    EXPECT_EQ(result[0].serial, 3U);
    EXPECT_EQ(result[1].app_name, "alpha");
    EXPECT_EQ(result[1].serial, 9U);
    EXPECT_EQ(result[2].app_name, "zulu");
    EXPECT_EQ(result[2].serial, 5U);
}

TEST(ListConversion, EmptyInEmptyOut) { EXPECT_TRUE(to_app_streams({}).empty()); }
