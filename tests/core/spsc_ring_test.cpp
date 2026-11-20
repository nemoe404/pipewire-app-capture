#include "core/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace pwcap::core {
namespace {

// Pops until the ring reports empty and returns how many elements came out.
std::size_t drain(SpscRing<std::uint32_t>& ring) {
    std::size_t count = 0;
    while (ring.try_pop().has_value()) {
        ++count;
    }
    return count;
}

TEST(SpscRing, NewRingIsEmpty) {
    SpscRing<float> ring(4);
    EXPECT_EQ(ring.try_pop(), std::nullopt);
    EXPECT_EQ(ring.dropped(), 0U);
    EXPECT_EQ(ring.capacity(), 4U);
}

TEST(SpscRing, PopsInFifoOrder) {
    SpscRing<std::uint32_t> ring(4);
    for (std::uint32_t value : {1U, 2U, 3U, 4U}) {
        ring.push(value);
    }
    for (std::uint32_t expected : {1U, 2U, 3U, 4U}) {
        EXPECT_EQ(ring.try_pop(), std::optional<std::uint32_t>{expected});
    }
    EXPECT_EQ(ring.try_pop(), std::nullopt);
    EXPECT_EQ(ring.dropped(), 0U);
}

TEST(SpscRing, WrapsAroundForMoreThanTenLaps) {
    // 43 rounds of 3 elements put the monotonic index at 129, past 10 laps of a capacity-4 ring.
    constexpr std::uint32_t kRounds = 43;
    constexpr std::uint32_t kPerRound = 3;
    SpscRing<std::uint32_t> ring(4);
    std::uint32_t next_push = 0;
    std::uint32_t next_pop = 0;
    for (std::uint32_t round = 0; round < kRounds; ++round) {
        for (std::uint32_t i = 0; i < kPerRound; ++i) {
            ring.push(next_push++);
        }
        for (std::uint32_t i = 0; i < kPerRound; ++i) {
            ASSERT_EQ(ring.try_pop(), std::optional<std::uint32_t>{next_pop++});
        }
    }
    EXPECT_EQ(next_pop, kRounds * kPerRound);
    EXPECT_EQ(ring.try_pop(), std::nullopt);
    EXPECT_EQ(ring.dropped(), 0U);
}

TEST(SpscRing, DropsOldestWhenFull) {
    SpscRing<std::uint32_t> ring(4);
    for (std::uint32_t value = 1; value <= 10; ++value) {
        ring.push(value);
    }
    EXPECT_EQ(ring.dropped(), 6U);
    for (std::uint32_t expected : {7U, 8U, 9U, 10U}) {
        EXPECT_EQ(ring.try_pop(), std::optional<std::uint32_t>{expected});
    }
    EXPECT_EQ(ring.try_pop(), std::nullopt);
}

TEST(SpscRing, DropsAccumulateAcrossEpisodes) {
    SpscRing<std::uint32_t> ring(4);
    for (std::uint32_t value = 0; value < 6; ++value) { // overflow by 2
        ring.push(value);
    }
    EXPECT_EQ(ring.dropped(), 2U);
    EXPECT_EQ(drain(ring), 4U);
    for (std::uint32_t value = 0; value < 7; ++value) { // overflow by 3
        ring.push(value);
    }
    EXPECT_EQ(ring.dropped(), 5U);
    EXPECT_EQ(drain(ring), 4U);
}

TEST(SpscRing, CapacityOneKeepsNewest) {
    SpscRing<std::uint32_t> ring(1);
    ring.push(11U);
    ring.push(22U);
    EXPECT_EQ(ring.dropped(), 1U);
    EXPECT_EQ(ring.try_pop(), std::optional<std::uint32_t>{22U});
    EXPECT_EQ(ring.try_pop(), std::nullopt);
}

TEST(SpscRing, SizeApproxTracksUnreadElements) {
    SpscRing<std::uint32_t> ring(4);
    EXPECT_EQ(ring.size_approx(), 0U);
    ring.push(1U);
    ring.push(2U);
    EXPECT_EQ(ring.size_approx(), 2U);
    EXPECT_TRUE(ring.try_pop().has_value());
    EXPECT_EQ(ring.size_approx(), 1U);
    for (std::uint32_t value = 0; value < 9; ++value) {
        ring.push(value);
        EXPECT_LE(ring.size_approx(), ring.capacity());
    }
    EXPECT_EQ(ring.size_approx(), 4U);
    EXPECT_EQ(drain(ring), 4U);
    EXPECT_EQ(ring.size_approx(), 0U);
}

} // namespace
} // namespace pwcap::core
