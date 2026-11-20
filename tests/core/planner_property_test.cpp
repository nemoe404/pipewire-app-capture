#include "core/graph.hpp"
#include "core/match.hpp"
#include "core/planner.hpp"
#include "snapshot_gen.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace pwcap::core {
namespace {

using gen::Generated;
using gen::Role;

// 16 seeds x 1000 snapshots. A case is seeded from (seed, iteration) alone, so a failure
// reproduces without replaying the cases before it.
constexpr std::array<std::uint64_t, 16> kSeeds{
    0x9E3779B97F4A7C15ULL, 0xBF58476D1CE4E5B9ULL, 0x94D049BB133111EBULL, 0xD6E8FEB86659FD93ULL,
    0xA0761D6478BD642FULL, 0xE7037ED1A0B428DBULL, 0x8EBC6AF09C88C6E3ULL, 0x589965CC75374CC3ULL,
    0x1D8E4E27C47D124FULL, 0xEB44ACCAB455D165ULL, 0x2545F4914F6CDD1DULL, 0x5851F42D4C957F2DULL,
    0x14057B7EF767814FULL, 0x2127599BF4325C37ULL, 0x880355F21E6D1965ULL, 0xC2B2AE3D27D4EB4FULL};
constexpr std::uint64_t kIterations = 1000;
constexpr std::uint64_t kSeedStride = 1'000'003;

struct CaseContext {
    std::uint64_t seed;
    std::uint64_t iteration;
};

std::mt19937_64 case_rng(const CaseContext& context) {
    // Deterministic seed by design, for reproducible failures.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
    return std::mt19937_64{(context.seed * kSeedStride) + context.iteration};
}

std::mt19937_64 shuffle_rng(const CaseContext& context) {
    // Deterministic seed by design, for reproducible failures.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
    return std::mt19937_64{~((context.seed * kSeedStride) + context.iteration)};
}

// Runs `check(context, generated, plan)` over every generated snapshot and stops at the first
// failing case, so one broken property prints one seed and iteration instead of thousands.
template <typename Check> void for_each_case(const Check& check) {
    // Raw loops: each case needs its own seed and iteration for the failure trace, and the
    // nested pair has no single range to hand to an algorithm.
    for (const std::uint64_t seed : kSeeds) {
        for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
            const CaseContext context{.seed = seed, .iteration = iteration};
            SCOPED_TRACE("seed=" + std::to_string(seed) +
                         " iteration=" + std::to_string(iteration));
            auto rng = case_rng(context);
            const Generated generated = gen::generate(rng);
            const LinkPlan plan =
                plan_links(generated.snapshot, generated.selection, generated.capture_inputs);
            check(context, generated, plan);
            if (::testing::Test::HasFailure()) {
                return;
            }
        }
    }
}

// Ground-truth lookups. They read the raw snapshot and the generator's role map only.
const PortInfo* port_of(const Generated& generated, PortId id) {
    const auto found = std::ranges::find(generated.snapshot.ports, id, &PortInfo::port_id);
    return found == generated.snapshot.ports.end() ? nullptr : &*found;
}

std::optional<Role> role_of(const Generated& generated, NodeId node) {
    const auto found = generated.role.find(node);
    if (found == generated.role.end()) {
        return std::nullopt;
    }
    return found->second;
}

// Test-side restatement of the channel rule: equal names, or MONO on either side.
bool rule_pairs(const PortInfo& source, const PortInfo& input) {
    return source.channel == input.channel || source.channel == "MONO" || input.channel == "MONO";
}

TEST(PlannerProperty, SafeSourcesOnly) {
    for_each_case([](const CaseContext&, const Generated& generated, const LinkPlan& plan) {
        for (const LinkInfo& link : plan.to_add) { // every planned link is checked
            EXPECT_TRUE(generated.eligible_sources.contains(link.output_port_id))
                << "output port " << link.output_port_id << " is not an eligible source";
        }
    });
}

TEST(PlannerProperty, NoMicrophoneNoMonitor) {
    for_each_case([](const CaseContext&, const Generated& generated, const LinkPlan& plan) {
        for (const LinkInfo& link : plan.to_add) { // every planned link is checked
            const PortInfo* port = port_of(generated, link.output_port_id);
            ASSERT_NE(port, nullptr)
                << "planned output port " << link.output_port_id << " is not in the snapshot";
            EXPECT_FALSE(port->monitor) << "port " << port->port_id << " is a monitor port";
            // Microphone, DefaultSourceAlias, VirtualSource, Sink, OtherRecorder, Capture,
            // Excluded and OtherApp are all anything but Target.
            EXPECT_EQ(role_of(generated, port->node_id), std::optional<Role>{Role::Target})
                << "port " << port->port_id << " does not belong to a target stream";
        }
    });
}

TEST(PlannerProperty, OnlyCaptureInputs) {
    for_each_case([](const CaseContext&, const Generated& generated, const LinkPlan& plan) {
        for (const LinkInfo& link : plan.to_add) { // every planned link is checked
            EXPECT_TRUE(generated.valid_capture_inputs.contains(link.input_port_id))
                << "input port " << link.input_port_id << " is not a valid capture input";
        }
    });
}

TEST(PlannerProperty, NeverTouchesOtherLinks) {
    for_each_case([](const CaseContext&, const Generated& generated, const LinkPlan& plan) {
        for (const LinkInfo& link : plan.to_remove) { // every removal is checked
            EXPECT_TRUE(generated.valid_capture_inputs.contains(link.input_port_id))
                << "link " << link.output_port_id << "->" << link.input_port_id
                << " does not end at a valid capture input";
            EXPECT_TRUE(std::ranges::contains(generated.snapshot.links, link))
                << "link " << link.output_port_id << "->" << link.input_port_id
                << " is not in the snapshot";
        }
    });
}

TEST(PlannerProperty, ChannelRuleAndComplete) {
    for_each_case([](const CaseContext&, const Generated& generated, const LinkPlan& plan) {
        for (const LinkInfo& link : plan.to_add) { // every planned link obeys the rule
            const PortInfo* source = port_of(generated, link.output_port_id);
            const PortInfo* input = port_of(generated, link.input_port_id);
            ASSERT_NE(source, nullptr);
            ASSERT_NE(input, nullptr);
            EXPECT_TRUE(rule_pairs(*source, *input))
                << source->channel << " must not be linked to " << input->channel;
        }

        const GraphSnapshot applied = gen::apply_plan(generated.snapshot, plan);
        // Complete: every legal (source, capture input) pair is linked afterwards.
        for (const PortId source_id : generated.eligible_sources) { // all legal pairs
            for (const PortId input_id : generated.valid_capture_inputs) {
                const PortInfo* source = port_of(generated, source_id);
                const PortInfo* input = port_of(generated, input_id);
                ASSERT_NE(source, nullptr);
                ASSERT_NE(input, nullptr);
                if (rule_pairs(*source, *input)) {
                    const LinkInfo wanted{.output_port_id = source_id, .input_port_id = input_id};
                    EXPECT_TRUE(std::ranges::contains(applied.links, wanted))
                        << "missing link " << source_id << "->" << input_id;
                }
            }
        }
        // Clean: whatever ends at a valid capture input comes from a legal, paired source.
        for (const LinkInfo& link : applied.links) { // every remaining link is checked
            if (!generated.valid_capture_inputs.contains(link.input_port_id)) {
                continue;
            }
            ASSERT_TRUE(generated.eligible_sources.contains(link.output_port_id))
                << "foreign link " << link.output_port_id << "->" << link.input_port_id
                << " left in the capture";
            const PortInfo* source = port_of(generated, link.output_port_id);
            const PortInfo* input = port_of(generated, link.input_port_id);
            ASSERT_NE(source, nullptr);
            ASSERT_NE(input, nullptr);
            EXPECT_TRUE(rule_pairs(*source, *input));
        }
    });
}

TEST(PlannerProperty, IdempotentAndOrderIndependent) {
    for_each_case([](const CaseContext& context, const Generated& generated, const LinkPlan& plan) {
        // Same input, same plan.
        EXPECT_EQ(plan_links(generated.snapshot, generated.selection, generated.capture_inputs),
                  plan);

        // Once applied, nothing is left to do.
        const GraphSnapshot applied = gen::apply_plan(generated.snapshot, plan);
        const LinkPlan again = plan_links(applied, generated.selection, generated.capture_inputs);
        EXPECT_TRUE(again.empty()) << "to_add " << again.to_add.size() << ", to_remove "
                                   << again.to_remove.size() << " after applying the plan";

        // The order of the snapshot vectors and of the capture inputs does not matter.
        auto rng = shuffle_rng(context);
        GraphSnapshot shuffled = generated.snapshot;
        auto inputs = generated.capture_inputs;
        std::ranges::shuffle(shuffled.nodes, rng);
        std::ranges::shuffle(shuffled.ports, rng);
        std::ranges::shuffle(shuffled.links, rng);
        std::ranges::shuffle(inputs, rng);
        EXPECT_EQ(plan_links(shuffled, generated.selection, inputs), plan);
    });
}

TEST(PlannerProperty, GeneratorIsNotVacuous) {
    std::size_t total = 0;
    std::size_t with_add = 0;
    std::size_t with_remove = 0;
    std::size_t with_lookalike = 0;
    std::size_t with_extra = 0;
    for_each_case([&](const CaseContext&, const Generated& generated, const LinkPlan& plan) {
        ++total;
        with_add += plan.to_add.empty() ? 0U : 1U;
        with_remove += plan.to_remove.empty() ? 0U : 1U;
        with_lookalike += generated.lookalike_source ? 1U : 0U;
        with_extra += generated.adversarial_extra ? 1U : 0U;
    });
    ASSERT_EQ(total, kSeeds.size() * kIterations);
    EXPECT_GE(with_add * 100, total * 50) << "snapshots with a non-empty to_add: " << with_add;
    EXPECT_GE(with_remove * 100, total * 10)
        << "snapshots with a non-empty to_remove: " << with_remove;
    EXPECT_GE(with_lookalike * 100, total * 90)
        << "snapshots with a look-alike mic: " << with_lookalike;
    EXPECT_GE(with_extra * 100, total * 10)
        << "snapshots with an adversarial extra id: " << with_extra;
}

} // namespace
} // namespace pwcap::core
