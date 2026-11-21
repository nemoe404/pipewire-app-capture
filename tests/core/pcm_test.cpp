#include "core/pcm.hpp"

#include "core/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace pwcap::core {
namespace {

// Native-endian bytes of the given floats, the layout PipeWire hands the process callback.
std::vector<std::byte> bytes_of(std::span<const float> samples) {
    std::vector<std::byte> out(samples.size_bytes());
    std::memcpy(out.data(), samples.data(), samples.size_bytes());
    return out;
}

// Pops one frame; an empty ring fails the test and yields a zero frame.
StereoFrame pop_frame(SpscRing<StereoFrame>& ring) {
    const std::optional<StereoFrame> popped = ring.try_pop();
    EXPECT_TRUE(popped.has_value());
    return popped.value_or(StereoFrame{});
}

TEST(PcmPush, CopiesWholeFramesAndIgnoresTail) {
    // 3.5 frames: six floats plus a 4-byte tail that is half a frame.
    const std::array<float, 7> samples{0.1F, -0.2F, 0.3F, -0.4F, 0.5F, -0.6F, 0.7F};
    const std::vector<std::byte> bytes = bytes_of(samples);
    ASSERT_EQ(bytes.size(), 28U);

    SpscRing<StereoFrame> ring(8);
    EXPECT_EQ(push_interleaved(bytes, ring), 3U);

    for (const StereoFrame expected :
         {StereoFrame{.left = 0.1F, .right = -0.2F}, StereoFrame{.left = 0.3F, .right = -0.4F},
          StereoFrame{.left = 0.5F, .right = -0.6F}}) {
        const StereoFrame got = pop_frame(ring);
        // Bit-exact: the copy must not alter a single bit.
        EXPECT_EQ(std::bit_cast<std::uint32_t>(got.left),
                  std::bit_cast<std::uint32_t>(expected.left));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(got.right),
                  std::bit_cast<std::uint32_t>(expected.right));
    }
    EXPECT_EQ(ring.try_pop(), std::nullopt);
}

TEST(PcmPush, DropOldestDropsWholeFrames) {
    // 0, 1, 2, ...: frame k is {2k, 2k + 1}, so right == left + 1 in every intact frame.
    std::array<float, 12> samples{};
    float next = 0.0F;
    std::ranges::generate(samples, [&next] { return next++; });
    const std::vector<std::byte> bytes = bytes_of(samples);

    SpscRing<StereoFrame> ring(4);
    EXPECT_EQ(push_interleaved(bytes, ring), 6U);
    EXPECT_EQ(ring.dropped(), 2U);

    for (std::size_t k = 2; k < 6; ++k) {
        const StereoFrame got = pop_frame(ring);
        EXPECT_EQ(got.left, static_cast<float>(2 * k));
        // The channels stay paired: right is always left + 1, so no swap.
        EXPECT_EQ(got.right, got.left + 1.0F);
    }
    EXPECT_EQ(ring.try_pop(), std::nullopt);
}

TEST(PcmPush, EmptySpanPushesNothing) {
    SpscRing<StereoFrame> ring(4);
    EXPECT_EQ(push_interleaved(std::span<const std::byte>{}, ring), 0U);
    EXPECT_EQ(ring.try_pop(), std::nullopt);
    EXPECT_EQ(ring.dropped(), 0U);
}

} // namespace
} // namespace pwcap::core
