#ifndef PWCAP_CORE_PCM_HPP
#define PWCAP_CORE_PCM_HPP

// Pure sample-path pieces between the PipeWire process callback and the consumer's output:
//   - StereoFrame and push_interleaved, the real-time copy into the SPSC ring;
//   - select_conversion, the table of supported output formats;
//   - interleave and downmix, the channel shuffles the consumer needs around the resampler;
//   - encode_s16le and encode_f32le, the byte encoders.
//
// No I/O, no logging, no PipeWire include. Nothing here allocates: push_interleaved runs on the
// real-time thread and the others run on the consumer thread with preallocated buffers.

#include "core/spsc_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace pwcap::core {

// The ring element is a whole stereo frame, never a single interleaved float. Drop-oldest removes
// one element, so with a float element a drop would remove one channel and swap left and right for
// the rest of the capture. With a frame, a drop always removes a left/right pair.
struct StereoFrame {
    float left{};
    float right{};

    friend bool operator==(const StereoFrame&, const StereoFrame&) = default;
};

static_assert(sizeof(StereoFrame) == 8);
static_assert(SpscElement<StereoFrame>);

// The fixed format of the capture stream the shell asks PipeWire for: F32, 48 kHz, FL/FR.
inline constexpr std::uint32_t kCaptureRate = 48000;
inline constexpr std::uint32_t kCaptureChannels = 2;
inline constexpr std::size_t kCaptureFrameBytes = sizeof(StereoFrame);

// Real-time safe. Copies every whole 8-byte frame of `f32_stereo` (native-endian floats, left
// then right) into `ring` and returns the number of frames pushed. A trailing partial frame is
// ignored. It allocates nothing, takes no lock and logs nothing.
[[nodiscard]] std::size_t push_interleaved(std::span<const std::byte> f32_stereo,
                                           SpscRing<StereoFrame>& ring) noexcept;

} // namespace pwcap::core

#endif // PWCAP_CORE_PCM_HPP
