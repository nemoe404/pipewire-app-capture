#ifndef PWCAP_TESTS_PIPEWIRE_GOERTZEL_HPP
#define PWCAP_TESTS_PIPEWIRE_GOERTZEL_HPP

// In-memory level checks for the integration tests. Everything works on buffers the test already
// holds; no PCM is written to any file. Levels corroborate the graph assertions, they
// never replace them.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <numeric>
#include <span>
#include <vector>

namespace pwcap::test {

// Goertzel power of `samples` at `freq_hz` for a signal sampled at `rate_hz`.
[[nodiscard]] inline double goertzel_power(std::span<const float> samples, double freq_hz,
                                           double rate_hz) {
    const double coefficient = 2.0 * std::cos(2.0 * std::numbers::pi * freq_hz / rate_hz);
    double previous = 0.0;
    double before_previous = 0.0;
    std::ranges::for_each(samples, [&](float sample) {
        const double current =
            static_cast<double>(sample) + (coefficient * previous) - before_previous;
        before_previous = previous;
        previous = current;
    });
    return (previous * previous) + (before_previous * before_previous) -
           (coefficient * previous * before_previous);
}

[[nodiscard]] inline double rms(std::span<const float> samples) {
    if (samples.empty()) {
        return 0.0;
    }
    const double sum =
        std::accumulate(samples.begin(), samples.end(), 0.0, [](double acc, float value) {
            return acc + (static_cast<double>(value) * static_cast<double>(value));
        });
    return std::sqrt(sum / static_cast<double>(samples.size()));
}

// S16LE bytes to floats in int16 units (not scaled to [-1, 1]), so an RMS reads like the PCM.
// The target is little-endian.
[[nodiscard]] inline std::vector<float> decode_s16le(std::span<const std::byte> bytes) {
    std::vector<std::int16_t> raw(bytes.size() / sizeof(std::int16_t));
    std::memcpy(raw.data(), bytes.data(), raw.size() * sizeof(std::int16_t));
    std::vector<float> samples(raw.size());
    std::ranges::transform(raw, samples.begin(),
                           [](std::int16_t value) { return static_cast<float>(value); });
    return samples;
}

// One channel of interleaved F32LE bytes. `channels` must be non-zero and `channel < channels`.
[[nodiscard]] inline std::vector<float>
decode_f32le_channel(std::span<const std::byte> bytes, std::size_t channels, std::size_t channel) {
    std::vector<float> all(bytes.size() / sizeof(float));
    std::memcpy(all.data(), bytes.data(), all.size() * sizeof(float));
    std::vector<float> picked;
    picked.reserve(all.size() / channels);
    // A raw loop: a strided pick; the stride is not a view this header needs to depend on.
    for (std::size_t index = channel; index < all.size(); index += channels) {
        picked.push_back(all.at(index));
    }
    return picked;
}

} // namespace pwcap::test

#endif // PWCAP_TESTS_PIPEWIRE_GOERTZEL_HPP
