#include "core/pcm.hpp"

#include "core/fir_design.hpp"
#include "core/resampler.hpp"
#include "core/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
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

// True when (rate, channels) is rejected as Unsupported.
bool is_unsupported(std::uint32_t rate, std::uint32_t channels) {
    const std::expected<Conversion, ConversionError> selected = select_conversion(rate, channels);
    return !selected.has_value() && selected.error() == ConversionError::Unsupported;
}

TEST(PcmConversion, SupportedTable) {
    struct Row {
        std::uint32_t rate;
        std::uint32_t channels;
        Conversion conversion;
        std::uint32_t out_channels;
    };
    for (const Row row : {Row{.rate = 16000,
                              .channels = 1,
                              .conversion = Conversion::Resample16kMono,
                              .out_channels = 1},
                          Row{.rate = 48000,
                              .channels = 2,
                              .conversion = Conversion::Passthrough48kStereo,
                              .out_channels = 2},
                          Row{.rate = 48000,
                              .channels = 1,
                              .conversion = Conversion::Downmix48kMono,
                              .out_channels = 1}}) {
        const std::expected<Conversion, ConversionError> selected =
            select_conversion(row.rate, row.channels);
        ASSERT_TRUE(selected.has_value()) << row.rate << "/" << row.channels;
        EXPECT_EQ(selected.value(), row.conversion) << row.rate << "/" << row.channels;
        EXPECT_EQ(output_channels(row.conversion), row.out_channels);
    }
}

TEST(PcmConversion, RejectsEverythingElse) {
    for (const auto [rate, channels] : {std::pair<std::uint32_t, std::uint32_t>{16000, 2},
                                        {44100, 1},
                                        {44100, 2},
                                        {8000, 1},
                                        {96000, 2},
                                        {48000, 3},
                                        {0, 1},
                                        {48000, 0}}) {
        EXPECT_TRUE(is_unsupported(rate, channels)) << rate << "/" << channels;
    }
}

// Reads little-endian signed 16-bit samples back, independent of host endianness.
std::vector<std::int16_t> decode_s16le(std::span<const std::byte> bytes) {
    std::vector<std::int16_t> out(bytes.size() / 2);
    std::ranges::generate(out, [&, index = std::size_t{0}]() mutable {
        const auto low = std::to_integer<std::uint16_t>(bytes[2 * index]);
        const auto high = std::to_integer<std::uint16_t>(bytes[(2 * index) + 1]);
        ++index;
        return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(low | (high << 8)));
    });
    return out;
}

TEST(PcmEncode, S16RoundsAndClampsLittleEndian) {
    const std::array<float, 11> in{1.0F,
                                   -1.0F,
                                   2.0F,
                                   -2.0F,
                                   0.5F,
                                   -0.5F,
                                   0.0F,
                                   std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::infinity(),
                                   -std::numeric_limits<float>::infinity(),
                                   1e-6F};
    const std::array<std::int16_t, 11> expected{32767, -32767, 32767, -32767, 16384, -16384,
                                                0,     0,      32767, -32767, 0};
    std::vector<std::byte> out(22);
    EXPECT_EQ(encode_s16le(in, out), 22U);

    const std::vector<std::int16_t> decoded = decode_s16le(out);
    ASSERT_EQ(decoded.size(), expected.size());
    EXPECT_TRUE(std::ranges::equal(decoded, expected));

    // 1.0 is 0x7FFF: the low byte comes first.
    EXPECT_EQ(out.at(0), std::byte{0xFF});
    EXPECT_EQ(out.at(1), std::byte{0x7F});
    // -1.0 is -32767 = 0x8001: low byte 0x01, high byte 0x80.
    EXPECT_EQ(out.at(2), std::byte{0x01});
    EXPECT_EQ(out.at(3), std::byte{0x80});
}

TEST(PcmEncode, F32IsLittleEndianBitExact) {
    const std::array<float, 3> in{1.0F, -2.5F, std::numeric_limits<float>::denorm_min()};
    std::vector<std::byte> out(12);
    EXPECT_EQ(encode_f32le(in, out), 12U);

    const std::array<std::byte, 8> expected_head{std::byte{0x00}, std::byte{0x00}, std::byte{0x80},
                                                 std::byte{0x3F}, std::byte{0x00}, std::byte{0x00},
                                                 std::byte{0x20}, std::byte{0xC0}};
    EXPECT_TRUE(std::ranges::equal(std::span(out).first<8>(), expected_head));

    // The denormal must survive the round trip bit-exact: 0x00000001 little-endian.
    EXPECT_EQ(out.at(8), std::byte{0x01});
    EXPECT_EQ(out.at(9), std::byte{0x00});
    EXPECT_EQ(out.at(10), std::byte{0x00});
    EXPECT_EQ(out.at(11), std::byte{0x00});
}

TEST(PcmDownmix, AveragesAndInterleaveKeepsOrder) {
    const std::array<StereoFrame, 3> frames{StereoFrame{.left = 1.0F, .right = 0.0F},
                                            StereoFrame{.left = 0.5F, .right = -0.5F},
                                            StereoFrame{.left = -1.0F, .right = -1.0F}};
    std::array<float, 3> mono{};
    downmix(frames, mono);
    EXPECT_TRUE(std::ranges::equal(mono, std::array<float, 3>{0.5F, 0.0F, -1.0F}));

    std::array<float, 6> interleaved{};
    interleave(frames, interleaved);
    EXPECT_TRUE(std::ranges::equal(interleaved,
                                   std::array<float, 6>{1.0F, 0.0F, 0.5F, -0.5F, -1.0F, -1.0F}));
}

// Goertzel power of `samples` at `freq_hz`, for a signal sampled at `rate`.
double goertzel_power(std::span<const double> samples, double freq_hz, double rate) {
    const double coefficient = 2.0 * std::cos(2.0 * std::numbers::pi * freq_hz / rate);
    double previous = 0.0;
    double before_previous = 0.0;
    std::ranges::for_each(samples, [&](double sample) {
        const double current = sample + (coefficient * previous) - before_previous;
        before_previous = previous;
        previous = current;
    });
    return (previous * previous) + (before_previous * before_previous) -
           (coefficient * previous * before_previous);
}

// The exact chain the consumer runs for a 16 kHz mono output: interleave, hann8 resampler, S16LE.
// hann8 is the filter chosen by the resampler quality measurement.
TEST(PcmChain, Resample48kStereoTo16kMonoKeepsTone) {
    constexpr std::size_t kSeconds = 1;
    constexpr std::size_t kTotalFrames = kCaptureRate * kSeconds;
    constexpr std::size_t kBlockFrames = 4800;
    constexpr double kToneHz = 440.0;
    constexpr double kOtherHz = 1000.0;

    std::vector<StereoFrame> frames(kTotalFrames);
    std::ranges::generate(frames, [index = std::size_t{0}]() mutable {
        const double phase = 2.0 * std::numbers::pi * kToneHz * static_cast<double>(index) /
                             static_cast<double>(kCaptureRate);
        ++index;
        const auto sample = static_cast<float>(0.5 * std::sin(phase));
        return StereoFrame{.left = sample, .right = sample};
    });

    std::expected<Resampler, ResamplerError> made =
        Resampler::make(kCaptureRate, kCaptureChannels, FilterCandidate::Hann8);
    ASSERT_TRUE(made.has_value());
    Resampler resampler = std::move(made).value();

    std::vector<float> interleaved(2 * kBlockFrames);
    std::vector<float> block_out(resampler.max_output_frames(kBlockFrames));
    std::vector<float> mono;
    for (std::size_t offset = 0; offset < kTotalFrames; offset += kBlockFrames) {
        // Ten blocks, in order, each feeding the resampler's running state.
        interleave(std::span(frames).subspan(offset, kBlockFrames), interleaved);
        const std::size_t written = resampler.process(interleaved, block_out);
        mono.insert(mono.end(), block_out.begin(),
                    block_out.begin() + static_cast<std::ptrdiff_t>(written));
    }

    constexpr std::size_t kExpectedFrames = kOutputRate * kSeconds;
    EXPECT_GE(mono.size(), kExpectedFrames - resampler.latency_frames() - 1);
    EXPECT_LE(mono.size(), kExpectedFrames + 1);

    std::vector<std::byte> bytes(2 * mono.size());
    ASSERT_EQ(encode_s16le(mono, bytes), bytes.size());
    const std::vector<std::int16_t> decoded = decode_s16le(bytes);
    ASSERT_GE(decoded.size(), 8000U);

    std::vector<double> tail(8000);
    std::ranges::transform(std::span(decoded).last(8000), tail.begin(),
                           [](std::int16_t value) { return static_cast<double>(value) / 32767.0; });
    const double at_tone = goertzel_power(tail, kToneHz, kOutputRate);
    const double at_other = goertzel_power(tail, kOtherHz, kOutputRate);
    EXPECT_GT(at_tone, 0.0);
    EXPECT_GE(at_tone, 1000.0 * at_other);
}

} // namespace
} // namespace pwcap::core
