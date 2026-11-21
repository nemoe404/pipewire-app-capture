// Reconciler behaviour against a live PipeWire session: waiting for an application that is not
// playing yet, refusing to open without a source when reconnect is off, and removing a link that
// somebody else put on the capture input. Opt-in: every test starts with the gate in
// pw_session.hpp. Graph state from pw-dump and the continuous pw-link monitor judge; the library
// never judges itself.

#include "pw_session.hpp"

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using pwcap::test::DiagnosticLog;
using pwcap::test::Graph;
using pwcap::test::LinkGuard;
using pwcap::test::TempDir;

// Byte count only: no captured sample is stored anywhere.
[[nodiscard]] pwcap::Sink counting_sink(std::atomic<std::uint64_t>& bytes) {
    return [&bytes](std::span<const std::byte> pcm) {
        bytes.fetch_add(pcm.size(), std::memory_order_relaxed);
    };
}

[[nodiscard]] pwcap::Diagnostic collecting(DiagnosticLog& log) {
    return [&log](std::string_view message) { log.add(message); };
}

[[nodiscard]] std::string describe(const std::vector<std::string>& lines) {
    std::string text;
    std::ranges::for_each(lines, [&](const std::string& line) { text += "\n  " + line; });
    return text;
}

} // namespace

// A capture opened before its application plays waits with zero links and reports
// NoMatchingSource; when the application starts, it links exactly the application's FL and FR. A
// pw-link monitor runs from before open until the end, so a link of any other origin, the
// microphone included, cannot slip between two pw-dump snapshots.
TEST(Reconcile, WaitsWithZeroLinksThenLinksWhenAppStarts) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string late = pwcap::test::unique_tone_name("late440");
    const std::string prefix = pwcap::test::own_capture_prefix();

    const LinkGuard guard{prefix, {late + ":"}};
    ASSERT_TRUE(guard.wait_armed(3000ms)) << "the pw-link monitor printed nothing";

    DiagnosticLog log;
    std::atomic<std::uint64_t> bytes{0};
    auto opened = pwcap::Capture::open(pwcap::Options{.include = {.node_name = late}},
                                       counting_sink(bytes), collecting(log));
    ASSERT_TRUE(opened.has_value())
        << "Capture::open failed: " << (opened ? "" : pwcap::to_string(opened.error()));

    EXPECT_TRUE(
        pwcap::test::wait_until([&] { return log.has_prefix("NoMatchingSource:"); }, 1000ms))
        << "no NoMatchingSource diagnostic; got:" << describe(log.all());
    EXPECT_FALSE(opened->linked());

    // Watch the wait: nothing may link to the capture while no application matches.
    std::this_thread::sleep_for(1500ms);
    Graph graph = pwcap::test::read_graph();
    const std::vector<const pwcap::test::GNode*> captures = graph.nodes_by_prefix(prefix);
    ASSERT_EQ(captures.size(), 1U) << "expected exactly one node named " << prefix << "*";
    EXPECT_TRUE(graph.links_into(captures.front()->id).empty())
        << "the waiting capture has links into it";
    EXPECT_TRUE(graph.links_out_of(captures.front()->id).empty())
        << "the waiting capture has links out of it";
    EXPECT_EQ(guard.capture_link_adds(), 0U) << "a link was added to the capture while it waited";
    EXPECT_FALSE(opened->linked());
    EXPECT_EQ(log.count_prefix("NoMatchingSource:"), 1U)
        << "NoMatchingSource is posted once per transition, not on every reconcile";

    // The application starts.
    const pwcap::test::Child tone = pwcap::test::play_tone(dir, late, 440.0, 20.0);
    std::optional<std::string> reason;
    const bool linked = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            reason = pwcap::test::exact_capture_links(graph, prefix, late);
            return !reason && opened->linked();
        },
        3000ms);
    ASSERT_TRUE(linked) << "the capture did not link the application within 3000 ms: "
                        << reason.value_or("pw-dump is right, linked() is false");

    // A link added a moment after the two expected ones would show in a fresh dump or the guard.
    std::this_thread::sleep_for(300ms);
    graph = pwcap::test::read_graph();
    EXPECT_FALSE(pwcap::test::exact_capture_links(graph, prefix, late))
        << pwcap::test::exact_capture_links(graph, prefix, late).value_or("");
    EXPECT_EQ(guard.capture_link_adds(), 2U);
    EXPECT_TRUE(guard.violations().empty())
        << "links from another source reached the capture:" << describe(guard.violations());
    EXPECT_FALSE(guard.monitor_ended()) << "the pw-link monitor died; the guard was blind";
    EXPECT_TRUE(pwcap::test::wait_until(
        [&] { return log.has_prefix("pwcap: matching stream found"); }, 1000ms));
}

// With reconnect off and nothing to capture, open fails and leaves no capture node behind.
TEST(Reconcile, ReconnectOffWithoutSourceFailsOpen) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const std::string prefix = pwcap::test::own_capture_prefix();
    std::atomic<std::uint64_t> bytes{0};
    const auto opened = pwcap::Capture::open(
        pwcap::Options{.include = {.node_name = pwcap::test::unique_tone_name("nobody-plays-this")},
                       .reconnect = false},
        counting_sink(bytes));
    ASSERT_FALSE(opened.has_value()) << "open succeeded without a matching source";
    EXPECT_TRUE(opened.error() == pwcap::Error::NoMatchingSource)
        << "open failed with " << pwcap::to_string(opened.error());

    Graph graph;
    const bool gone = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            return !graph.nodes.empty() && graph.nodes_by_prefix(prefix).empty();
        },
        2000ms);
    EXPECT_TRUE(gone) << "a node named " << prefix << "* is still in the graph (or pw-dump failed)";
}

// A link from another application into the capture's input is removed by the reconciler, and the
// capture again has exactly the selected application's two links. Both applications keep their
// links to the speakers.
TEST(Reconcile, RemovesForeignLinkToCaptureInput) {
    if (const auto why = pwcap::test::skip_reason()) {
        GTEST_SKIP() << "pipewire integration test skipped: " << *why;
    }

    const TempDir dir;
    const std::string wanted = pwcap::test::unique_tone_name("tone440");
    const std::string foreign = pwcap::test::unique_tone_name("tone880");
    const std::string prefix = pwcap::test::own_capture_prefix();
    const pwcap::test::Child tone_440 = pwcap::test::play_tone(dir, wanted, 440.0, 25.0);
    const pwcap::test::Child tone_880 = pwcap::test::play_tone(dir, foreign, 880.0, 25.0);

    Graph graph;
    const bool on_speakers = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            return pwcap::test::stereo_on_sink(graph, wanted) &&
                   pwcap::test::stereo_on_sink(graph, foreign);
        },
        3000ms);
    ASSERT_TRUE(on_speakers) << "a tone was never linked to a sink";
    const std::vector<pwcap::test::PortPair> baseline_wanted =
        pwcap::test::sink_links_of(graph, wanted);
    const std::vector<pwcap::test::PortPair> baseline_foreign =
        pwcap::test::sink_links_of(graph, foreign);

    // The guard is only a witness here: it proves the injected link existed, however briefly.
    const LinkGuard witness{prefix, {wanted + ":"}};
    ASSERT_TRUE(witness.wait_armed(3000ms));

    DiagnosticLog log;
    std::atomic<std::uint64_t> bytes{0};
    auto opened = pwcap::Capture::open(pwcap::Options{.include = {.node_name = wanted}},
                                       counting_sink(bytes), collecting(log));
    ASSERT_TRUE(opened.has_value())
        << "Capture::open failed: " << (opened ? "" : pwcap::to_string(opened.error()));

    std::optional<std::string> reason;
    const bool exact = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            reason = pwcap::test::exact_capture_links(graph, prefix, wanted);
            return !reason;
        },
        3000ms);
    ASSERT_TRUE(exact) << "the capture never had exactly the wanted tone's links: "
                       << reason.value_or("");

    const std::vector<const pwcap::test::GNode*> captures = graph.nodes_by_prefix(prefix);
    ASSERT_EQ(captures.size(), 1U);
    const std::string capture_name = captures.front()->name;
    const std::uint32_t capture_id = captures.front()->id;

    const auto injected = std::chrono::steady_clock::now();
    const pwcap::test::CommandResult linked =
        pwcap::test::run_command({"pw-link", foreign + ":output_FL", capture_name + ":input_FL"});
    ASSERT_EQ(linked.status, 0) << "pw-link could not inject " << foreign << ":output_FL -> "
                                << capture_name << ":input_FL";

    const auto from_foreign = [&](const Graph& current) {
        const pwcap::test::GNode* node = current.node_by_name(foreign);
        return node != nullptr && std::ranges::any_of(current.links_into(capture_id),
                                                      [&](const pwcap::test::GLink* link) {
                                                          return link->out_node == node->id;
                                                      });
    };
    std::optional<std::chrono::milliseconds> removed_after;
    const bool removed = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            // Only a dump taken after the monitor saw the injected link can show its removal.
            if (witness.violations().empty() || from_foreign(graph)) {
                return false;
            }
            removed_after = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - injected);
            return true;
        },
        2000ms, 5ms);
    ASSERT_FALSE(witness.violations().empty())
        << "the pw-link monitor never saw the injected link; the test did not inject";
    ASSERT_TRUE(removed) << "the foreign link into " << capture_name
                         << " was not removed in 2000 ms";
    std::cout << "foreign_link_removed_ms=" << removed_after.value_or(0ms).count() << '\n';

    // The capture is back to the wanted application's two links, and the speakers are untouched.
    const bool restored = pwcap::test::wait_until(
        [&] {
            graph = pwcap::test::read_graph();
            reason = pwcap::test::exact_capture_links(graph, prefix, wanted);
            return !reason;
        },
        1000ms);
    EXPECT_TRUE(restored) << reason.value_or("");
    std::ranges::for_each(baseline_wanted, [&](const pwcap::test::PortPair& pair) {
        EXPECT_TRUE(pwcap::test::graph_has_link(graph, pair))
            << "speaker link of " << wanted << " lost: " << pair.first << " -> " << pair.second;
    });
    std::ranges::for_each(baseline_foreign, [&](const pwcap::test::PortPair& pair) {
        EXPECT_TRUE(pwcap::test::graph_has_link(graph, pair))
            << "speaker link of " << foreign << " lost: " << pair.first << " -> " << pair.second;
    });
    EXPECT_TRUE(opened->linked());
}
