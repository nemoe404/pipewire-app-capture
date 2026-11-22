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

// The supported output formats (user decision D-04). The capture stream is always F32 48 kHz FL/FR;
// these are the only conversions from it to what the consumer asked for.
enum class Conversion : std::uint8_t {
    Resample16kMono,      // 48 kHz stereo -> 16 kHz mono through the resampler
    Passthrough48kStereo, // 48 kHz stereo as captured
    Downmix48kMono,       // 48 kHz stereo -> 48 kHz mono, the mean of the two channels
};

enum class ConversionError : std::uint8_t { Unsupported };

// Exactly (16000, 1), (48000, 2) and (48000, 1) are supported; every other pair is Unsupported.
// The shell maps Unsupported to InvalidOptions before it makes any PipeWire call.
[[nodiscard]] std::expected<Conversion, ConversionError>
select_conversion(std::uint32_t sample_rate, std::uint32_t channels) noexcept;

// Channels in the stream a conversion produces.
[[nodiscard]] constexpr std::uint32_t output_channels(Conversion conversion) noexcept {
    return conversion == Conversion::Passthrough48kStereo ? 2U : 1U;
}

// Writes left, right, left, right, ... Requires out.size() >= 2 * in.size().
void interleave(std::span<const StereoFrame> in, std::span<float> out) noexcept;

// Writes 0.5 * (left + right) per frame. Requires out.size() >= in.size().
void downmix(std::span<const StereoFrame> in, std::span<float> out) noexcept;

// Signed 16-bit little-endian PCM. NaN becomes 0; other values are clamped to [-1, 1], scaled by
// 32767 (so -1.0 is -32767, symmetric) and rounded half away from zero. Returns the bytes written,
// 2 * in.size(). Requires out.size() >= 2 * in.size().
[[nodiscard]] std::size_t encode_s16le(std::span<const float> in,
                                       std::span<std::byte> out) noexcept;

// IEEE-754 binary32 little-endian. Returns the bytes written, 4 * in.size(). Requires
// out.size() >= 4 * in.size().
[[nodiscard]] std::size_t encode_f32le(std::span<const float> in,
                                       std::span<std::byte> out) noexcept;

} // namespace pwcap::core

#endif // PWCAP_CORE_PCM_HPP
