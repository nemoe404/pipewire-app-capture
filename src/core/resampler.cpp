#include "core/resampler.hpp"

#include "core/fir_design.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <numeric>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace pwcap::core {
namespace {

constexpr std::uint32_t kRate48k = 48000;
constexpr std::uint32_t kRate44k1 = 44100;

// Splits the prototype h into `up` branches of `taps` coefficients: branch p holds
// h[p + j * up] for j in [0, taps), zero past the end of h.
std::vector<float> split_branches(const std::vector<float>& h, std::size_t up, std::size_t taps) {
    return std::views::iota(std::size_t{0}, up * taps) |
           std::views::transform([&h, up, taps](std::size_t flat) {
               const std::size_t index = (flat / taps) + ((flat % taps) * up);
               return index < h.size() ? h[index] : 0.0F;
           }) |
           std::ranges::to<std::vector<float>>();
}

} // namespace

Resampler::Resampler(std::uint32_t input_rate, std::uint32_t channels, std::uint32_t up,
                     std::uint32_t down, std::uint32_t latency_frames, std::size_t taps,
                     std::vector<float> branches)
    : input_rate_{input_rate}, channels_{channels}, up_{up}, down_{down},
      latency_frames_{latency_frames}, taps_{taps}, branches_{std::move(branches)},
      history_(2 * taps, 0.0F) {}

std::expected<Resampler, ResamplerError>
Resampler::make(std::uint32_t input_rate, std::uint32_t channels, FilterCandidate candidate) {
    if (input_rate != kRate48k && input_rate != kRate44k1) {
        return std::unexpected{ResamplerError::UnsupportedRate};
    }
    if (channels < 1 || channels > kMaxChannels) {
        return std::unexpected{ResamplerError::InvalidChannelCount};
    }

    const std::uint32_t divisor = std::gcd(kOutputRate, input_rate);
    const std::uint32_t up_factor = kOutputRate / divisor;
    const std::uint32_t down_factor = input_rate / divisor;

    const FilterSpec spec = spec_for(candidate);
    const std::vector<float> prototype = design_prototype(spec, up_factor, down_factor);
    const std::size_t taps = (prototype.size() + up_factor - 1) / up_factor;

    return Resampler{input_rate,
                     channels,
                     up_factor,
                     down_factor,
                     spec.half_width,
                     taps,
                     split_branches(prototype, up_factor, taps)};
}

std::size_t Resampler::max_output_frames(std::size_t input_frames) const noexcept {
    const std::size_t numerator = input_frames * up_;
    return (numerator + down_ - 1) / down_;
}

std::size_t Resampler::process(std::span<const float> interleaved, std::span<float> out) {
    assert(interleaved.size() % channels_ == 0);
    [[maybe_unused]] const std::size_t frames =
        interleaved.size() / channels_; // used only by the assert in Release
    assert(out.size() >= max_output_frames(frames));

    std::size_t written = 0;
    const auto channel_count = static_cast<float>(channels_);

    const auto consume_frame = [&](std::span<const float> frame) {
        // Mono downmix: float sum of the channels in index order, divided by the channel count.
        const float mono = std::ranges::fold_left(frame, 0.0F, std::plus<>{}) / channel_count;

        // Double-write into the history so the newest taps_ samples are contiguous.
        pos_ = (pos_ + 1) % taps_;
        history_[pos_] = mono;
        history_[pos_ + taps_] = mono;

        // Output n needs x[k0] with k0 = n * down / up. Since down > up, at most one output
        // becomes ready per input frame.
        if (next_t_ / up_ != input_index_) {
            ++input_index_;
            return;
        }
        const auto phase = static_cast<std::size_t>(next_t_ % up_);
        const auto branch = std::span<const float>{branches_}.subspan(phase * taps_, taps_);
        // history_[pos_ + 1 .. pos_ + taps_] is oldest..newest; reversed it is x[k], x[k-1], ...
        const auto window = std::span<const float>{history_}.subspan(pos_ + 1, taps_);

        // Fixed ascending-j order, accumulated in double, so block splits give identical bits.
        const double acc = std::ranges::fold_left(
            std::views::zip(branch, window | std::views::reverse), 0.0, [](double sum, auto pair) {
                return sum + (static_cast<double>(std::get<0>(pair)) *
                              static_cast<double>(std::get<1>(pair)));
            });
        out[written] = static_cast<float>(acc);
        ++written;
        next_t_ += down_;
        ++input_index_;
    };

    std::ranges::for_each(interleaved | std::views::chunk(channels_), consume_frame);
    return written;
}

} // namespace pwcap::core
