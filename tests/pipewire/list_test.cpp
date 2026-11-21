// Integration tests of list_applications against a live PipeWire session. Opt-in: every test starts
// with the gate in pw_session.hpp and skips with a printed reason unless PWCAP_TEST_PIPEWIRE=1 and
// `pw-cli info 0` answers. The graph read from pw-dump is the judge. Other processes share this
// session, so every check selects its own PID-unique nodes and never asserts a total count.

#include "pw_session.hpp"
#include "pwcap/capture.hpp"
#include "pwcap/list.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using pwcap::AppStream;
using pwcap::test::Child;
using pwcap::test::GNode;
using pwcap::test::Graph;
using pwcap::test::TempDir;

constexpr std::string_view kStreamOutputClass = "Stream/Output/Audio";

// Starts pw-play on a stereo tone with node.name, application.name and
// application.process.binary set to the given values, so the node carries all three.
[[nodiscard]] Child start_named_tone(const TempDir& dir, const std::string& node,
                                     const std::string& app, double freq_hz, double seconds) {
    const std::filesystem::path wav = dir.path() / (node + ".wav");
    pwcap::test::write_tone_wav(wav, freq_hz, seconds, 2, 0.1);
    const std::string props = "{ node.name=" + node + " application.name=" + app +
                              " application.process.binary=" + app + " }";
    return Child{std::vector<std::string>{"pw-play", "-P", props, wav.string()}};
}

// Calls list_applications. A failed call is a test failure, and the list is then empty.
[[nodiscard]] std::vector<AppStream> must_list() {
    auto listed = pwcap::list_applications();
    EXPECT_TRUE(listed.has_value())
        << "list_applications failed: "
        << (listed ? std::string_view{} : pwcap::to_string(listed.error()));
    return listed ? std::move(*listed) : std::vector<AppStream>{};
}

[[nodiscard]] const AppStream* find_by_node(const std::vector<AppStream>& list,
                                            std::string_view node_name) {
    const auto found = std::ranges::find(list, node_name, &AppStream::node_name);
    return found == list.end() ? nullptr : &*found;
}

// Polls list_applications until the node is listed, or `timeout` passes.
[[nodiscard]] bool wait_for_listed(std::string_view node_name, std::chrono::milliseconds timeout) {
    return pwcap::test::wait_until([&] { return find_by_node(must_list(), node_name) != nullptr; },
                                   timeout);
}

[[nodiscard]] bool sorted_by_app_then_serial(const std::vector<AppStream>& list) {
    return std::ranges::is_sorted(list, std::ranges::less{}, [](const AppStream& stream) {
        return std::tie(stream.app_name, stream.serial);
    });
}

} // namespace

// A playing tone shows up with the four Match fields, its serial and its pid, and the list is
// sorted.
TEST(ListApplications, ShowsAPlayingToneWithItsProperties) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string node = pwcap::test::unique_tone_name("list-node");
    const std::string app = pwcap::test::unique_tone_name("list-app");
    const Child tone = start_named_tone(dir, node, app, 440.0, 20.0);

    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(wait_for_listed(node, 3000ms)) << "the tone never appeared in list_applications";
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const std::vector<AppStream> list = must_list();
    const AppStream* found = find_by_node(list, node);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->app_name, app);
    EXPECT_EQ(found->binary, app);
    EXPECT_EQ(found->node_name, node);
    EXPECT_TRUE(found->media_name.ends_with(".wav")) << "media_name was: " << found->media_name;

    const Graph graph = pwcap::test::read_graph();
    const GNode* in_graph = graph.node_by_name(node);
    ASSERT_NE(in_graph, nullptr) << "pw-dump does not show the tone";
    EXPECT_EQ(found->serial, in_graph->serial);
    EXPECT_TRUE(sorted_by_app_then_serial(list)) << "list_applications is not sorted";

    // Evidence for the summary: the observed fields and the time the first matching call took.
    std::cout << "[  EVIDENCE ] tone fields: app_name=" << found->app_name
              << " binary=" << found->binary << " node_name=" << found->node_name
              << " media_name=" << found->media_name << " serial=" << found->serial
              << " pid=" << (found->pid ? std::to_string(*found->pid) : std::string{"none"})
              << " first_listed_after_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << '\n';
}

// Every listed serial that is still in the graph belongs to a Stream/Output/Audio node.
TEST(ListApplications, ListsOnlyOutputStreams) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string node = pwcap::test::unique_tone_name("only-output");
    const Child tone = start_named_tone(dir, node, node, 330.0, 20.0);
    ASSERT_TRUE(wait_for_listed(node, 3000ms));

    const std::vector<AppStream> list = must_list();
    const Graph graph = pwcap::test::read_graph();

    const auto node_of = [&graph](const AppStream& stream) -> const GNode* {
        const auto found = std::ranges::find(graph.nodes, stream.serial, &GNode::serial);
        return found == graph.nodes.end() ? nullptr : &*found;
    };
    // An entry whose node already left the graph is churn, not a failure; it is only counted.
    const auto churned = std::ranges::count_if(
        list, [&](const AppStream& stream) { return node_of(stream) == nullptr; });
    const auto wrong_class = std::ranges::count_if(list, [&](const AppStream& stream) {
        const GNode* found = node_of(stream);
        return found != nullptr && found->media_class != kStreamOutputClass;
    });
    EXPECT_EQ(wrong_class, 0) << "entries that are not Stream/Output/Audio nodes (churned: "
                              << churned << " of " << list.size() << ")";
}

// The capture node this process opens is never listed; the tone is.
TEST(ListApplications, OwnCaptureIsNotListed) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string node = pwcap::test::unique_tone_name("own-capture");
    const Child tone = start_named_tone(dir, node, node, 550.0, 20.0);
    ASSERT_TRUE(wait_for_listed(node, 3000ms));

    auto opened = pwcap::Capture::open(pwcap::Options{.include = {.node_name = node}},
                                       [](std::span<const std::byte> /*pcm*/) {});
    ASSERT_TRUE(opened.has_value())
        << "Capture::open failed: "
        << (opened ? std::string_view{} : pwcap::to_string(opened.error()));
    ASSERT_TRUE(pwcap::test::wait_until([&] { return opened->linked(); }, 3000ms))
        << "the capture never linked to the tone";

    const std::vector<AppStream> list = must_list();
    const std::string prefix = pwcap::test::own_capture_prefix();
    const auto own = std::ranges::count_if(
        list, [&prefix](const AppStream& stream) { return stream.node_name.starts_with(prefix); });
    EXPECT_EQ(own, 0) << "the capture's own node was listed";
    EXPECT_NE(find_by_node(list, node), nullptr) << "the tone is missing while captured";
}

// A stream that has ended disappears from a fresh list within 2 s.
TEST(ListApplications, EndedStreamDisappears) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string node = pwcap::test::unique_tone_name("ended");
    Child tone = start_named_tone(dir, node, node, 220.0, 20.0);
    ASSERT_TRUE(wait_for_listed(node, 3000ms));

    tone.terminate_and_wait();
    const bool gone =
        pwcap::test::wait_until([&] { return find_by_node(must_list(), node) == nullptr; }, 2000ms);
    EXPECT_TRUE(gone) << "the ended stream was still listed after 2 s";
}
