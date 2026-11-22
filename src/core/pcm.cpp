#include "core/pcm.hpp"

#include "core/spsc_ring.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <ranges>
#include <span>

namespace pwcap::core {

std::size_t push_interleaved(std::span<const std::byte> f32_stereo,
                             SpscRing<StereoFrame>& ring) noexcept {
    // Integer division drops a trailing partial frame.
    const std::size_t frames = f32_stereo.size() / kCaptureFrameBytes;
    std::ranges::for_each(std::views::iota(std::size_t{0}, frames), [&](std::size_t index) {
        StereoFrame frame;
        // memcpy into a value: no pointer cast, no aliasing question, and subspan keeps the read
        // inside the span because index < frames.
        const std::span<const std::byte> source =
            f32_stereo.subspan(index * kCaptureFrameBytes, kCaptureFrameBytes);
        std::memcpy(&frame, source.data(), kCaptureFrameBytes);
        ring.push(frame);
    });
    return frames;
}

std::expected<Conversion, ConversionError> select_conversion(std::uint32_t sample_rate,
                                                             std::uint32_t channels) noexcept {
    if (sample_rate == 16000 && channels == 1) {
        return Conversion::Resample16kMono;
    }
    if (sample_rate == kCaptureRate && channels == 2) {
        return Conversion::Passthrough48kStereo;
    }
    if (sample_rate == kCaptureRate && channels == 1) {
        return Conversion::Downmix48kMono;
    }
    return std::unexpected(ConversionError::Unsupported);
}

void interleave(std::span<const StereoFrame> in, std::span<float> out) noexcept {
    assert(out.size() >= 2 * in.size());
    std::ranges::for_each(std::views::iota(std::size_t{0}, in.size()), [&](std::size_t index) {
        const std::span<float, 2> pair = out.subspan(2 * index, 2).first<2>();
        pair[0] = in[index].left;
        pair[1] = in[index].right;
    });
}

void downmix(std::span<const StereoFrame> in, std::span<float> out) noexcept {
    assert(out.size() >= in.size());
    std::ranges::transform(in, out.begin(), [](const StereoFrame& frame) {
        return 0.5F * (frame.left + frame.right);
    });
}

namespace {

// Writes the low `out.size()` bytes of `bits`, least significant first, whatever the host order.
void store_little_endian(std::span<std::byte> out, std::uint32_t bits) noexcept {
    std::uint32_t remaining = bits; // local copy: the parameter itself is never reassigned
    std::ranges::generate(out, [&remaining] {
        const auto byte = static_cast<std::byte>(remaining & 0xFFU);
        remaining >>= 8U;
        return byte;
    });
}

std::int16_t to_s16(float sample) noexcept {
    if (std::isnan(sample)) {
        return 0;
    }
    const float clamped = std::clamp(sample, -1.0F, 1.0F);
    // |clamped * 32767| <= 32767, so the rounded value is in range for the narrowing cast.
    return static_cast<std::int16_t>(std::round(clamped * 32767.0F));
}

} // namespace

std::size_t encode_s16le(std::span<const float> in, std::span<std::byte> out) noexcept {
    assert(out.size() >= 2 * in.size());
    std::ranges::for_each(std::views::iota(std::size_t{0}, in.size()), [&](std::size_t index) {
        const auto bits = std::bit_cast<std::uint16_t>(to_s16(in[index]));
        store_little_endian(out.subspan(2 * index, 2), bits);
    });
    return 2 * in.size();
}

std::size_t encode_f32le(std::span<const float> in, std::span<std::byte> out) noexcept {
    assert(out.size() >= 4 * in.size());
    std::ranges::for_each(std::views::iota(std::size_t{0}, in.size()), [&](std::size_t index) {
        store_little_endian(out.subspan(4 * index, 4), std::bit_cast<std::uint32_t>(in[index]));
    });
    return 4 * in.size();
}

} // namespace pwcap::core
