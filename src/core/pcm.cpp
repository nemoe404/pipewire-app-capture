#include "core/pcm.hpp"

#include "core/spsc_ring.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
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

} // namespace pwcap::core
