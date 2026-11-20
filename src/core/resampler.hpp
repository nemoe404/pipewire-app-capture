#ifndef PWCAP_CORE_RESAMPLER_HPP
#define PWCAP_CORE_RESAMPLER_HPP

// Rational polyphase FIR resampler: 48 kHz or 44.1 kHz interleaved float input to 16 kHz mono.
// Pure: no I/O, no logging. All memory is allocated in make(); process() allocates nothing, so it
// is safe on the consumer thread with a predictable cost.

#include "core/fir_design.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace pwcap::core {

enum class ResamplerError : std::uint8_t { UnsupportedRate, InvalidChannelCount };

inline constexpr std::uint32_t kOutputRate = 16000;
inline constexpr std::uint32_t kMaxChannels = 64;

class Resampler {
public:
    // input_rate must be 48000 or 44100; channels in [1, kMaxChannels].
    [[nodiscard]] static std::expected<Resampler, ResamplerError>
    make(std::uint32_t input_rate, std::uint32_t channels, FilterCandidate candidate);

    // Consumes whole frames of `interleaved` (size a multiple of channels()), writes mono 16 kHz
    // frames to `out` (size >= max_output_frames(frames)) and returns the number written.
    [[nodiscard]] std::size_t process(std::span<const float> interleaved, std::span<float> out);

    // Upper bound on the frames one process() call can write for `input_frames` input frames:
    // ceil(input_frames * up / down).
    [[nodiscard]] std::size_t max_output_frames(std::size_t input_frames) const noexcept;

    // Output frames by which the output lags the input (the prototype's half width).
    [[nodiscard]] std::uint32_t latency_frames() const noexcept { return latency_frames_; }

    [[nodiscard]] std::uint32_t input_rate() const noexcept { return input_rate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint32_t up() const noexcept { return up_; }
    [[nodiscard]] std::uint32_t down() const noexcept { return down_; }

private:
    Resampler(std::uint32_t input_rate, std::uint32_t channels, std::uint32_t up,
              std::uint32_t down, std::uint32_t latency_frames, std::size_t taps,
              std::vector<float> branches);

    std::uint32_t input_rate_;
    std::uint32_t channels_;
    std::uint32_t up_;
    std::uint32_t down_;
    std::uint32_t latency_frames_;
    std::size_t taps_;            // T: taps per polyphase branch
    std::vector<float> branches_; // up_ branches of taps_ coefficients, branch p at p * taps_
    std::vector<float> history_;  // 2 * taps_: each mono sample is stored at pos_ and pos_ + taps_
    std::size_t pos_ = 0;         // slot of the newest sample, in [0, taps_)
    std::uint64_t input_index_ = 0; // index of the next input frame
    std::uint64_t next_t_ = 0;      // n * down for the next output n
};

} // namespace pwcap::core

#endif // PWCAP_CORE_RESAMPLER_HPP
