#include "core/resampler.hpp"

#include "core/fir_design.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace pwcap::core {
namespace {

constexpr std::size_t kOutputFramesPerSecond = 16000;
constexpr std::size_t kMaxBlockFrames = 4096;

// Thresholds per candidate, from the plan. They are not to be loosened to make a test pass.
struct Tolerance {
    double passband;    // relative amplitude error and phase error in radians at 1 kHz
    double stopband_db; // minimum attenuation of the 12 kHz tone
};

constexpr Tolerance tolerance_for(FilterCandidate candidate) {
    switch (candidate) {
    case FilterCandidate::Hann8:
        return {.passband = 2e-2, .stopband_db = 40.0};
    case FilterCandidate::Kaiser16:
        return {.passband = 3.6e-4, .stopband_db = 75.0};
    case FilterCandidate::Kaiser32:
        return {.passband = 1.2e-4, .stopband_db = 85.0};
    }
    return {.passband = 0.0, .stopband_db = 0.0};
}

std::vector<float> sine(std::uint32_t rate, double freq_hz, double amplitude, std::size_t frames) {
    std::vector<float> x(frames);
    std::size_t k = 0;
    std::ranges::generate(x, [&] {
        const double phase =
            2.0 * std::numbers::pi * freq_hz * static_cast<double>(k) / static_cast<double>(rate);
        ++k;
        return static_cast<float>(amplitude * std::sin(phase));
    });
    return x;
}

// Interleaves a mono signal into `channels` channels, channel c scaled by gains[c].
std::vector<float> interleave(std::span<const float> mono, std::span<const float> gains) {
    std::vector<float> out;
    out.reserve(mono.size() * gains.size());
    for (const float v :
         mono) { // two nested appends; no algorithm expresses the interleave shorter
        for (const float g : gains) {
            out.push_back(v * g);
        }
    }
    return out;
}

Resampler make_or_die(std::uint32_t rate, std::uint32_t channels, FilterCandidate candidate) {
    auto made = Resampler::make(rate, channels, candidate);
    EXPECT_TRUE(made.has_value());
    return std::move(made).value();
}

std::vector<float> run_one_shot(Resampler& resampler, std::span<const float> input) {
    const std::size_t frames = input.size() / resampler.channels();
    std::vector<float> out(resampler.max_output_frames(frames));
    const std::size_t written = resampler.process(input, out);
    out.resize(written);
    return out;
}

std::size_t expected_cumulative(const Resampler& resampler, std::size_t input_frames) {
    const std::size_t up = resampler.up();
    const std::size_t down = resampler.down();
    return (input_frames * up + down - 1) / down; // ceil(M * up / down)
}

// A deterministic generator on purpose, so a failure reproduces exactly.
std::mt19937_64 make_rng() {
    // deterministic seed by design, for reproducible failures
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
    return std::mt19937_64{0x5EEDCAFEULL};
}

// Feeds `input` in the given block sizes; checks the length invariants after every call.
std::vector<float> run_blocks(Resampler& resampler, std::span<const float> input,
                              std::span<const std::size_t> block_sizes) {
    const std::size_t channels = resampler.channels();
    const std::size_t total_frames = input.size() / channels;
    std::vector<float> out;
    std::size_t consumed = 0;
    std::size_t block_index = 0;
    while (consumed < total_frames) { // block sizes are data-driven, so the loop is over a cursor
        const std::size_t want = block_sizes[block_index % block_sizes.size()];
        const std::size_t frames = std::min(want, total_frames - consumed);
        ++block_index;

        const auto block = input.subspan(consumed * channels, frames * channels);
        std::vector<float> scratch(resampler.max_output_frames(frames));
        const std::size_t written = resampler.process(block, scratch);
        EXPECT_LE(written, resampler.max_output_frames(frames));
        out.insert(out.end(), scratch.begin(),
                   scratch.begin() + static_cast<std::ptrdiff_t>(written));

        consumed += frames;
        EXPECT_EQ(out.size(), expected_cumulative(resampler, consumed));
    }
    return out;
}

std::vector<std::size_t> random_block_sizes(std::size_t count) {
    std::mt19937_64 rng = make_rng();
    std::uniform_int_distribution<std::size_t> pick(1, kMaxBlockFrames);
    std::vector<std::size_t> sizes(count);
    std::ranges::generate(sizes, [&] { return pick(rng); });
    return sizes;
}

struct ToneFit {
    double amplitude;
    double phase; // radians, y = A sin(omega n + phase)
};

// Least-squares fit of a sine at omega (radians per output sample) over [first, first + length).
ToneFit fit_tone(std::span<const float> y, std::size_t first, std::size_t length, double omega) {
    double a = 0.0;
    double b = 0.0;
    for (std::size_t n = first; n < first + length; ++n) { // ordered fold of two sums over a window
        const double angle = omega * static_cast<double>(n);
        a += static_cast<double>(y[n]) * std::sin(angle);
        b += static_cast<double>(y[n]) * std::cos(angle);
    }
    const double scale = 2.0 / static_cast<double>(length);
    a *= scale;
    b *= scale;
    return {.amplitude = std::hypot(a, b), .phase = std::atan2(b, a)};
}

double rms(std::span<const float> y, std::size_t first, std::size_t length) {
    double sum = 0.0;
    for (std::size_t n = first; n < first + length; ++n) { // ordered fold over a window
        sum += static_cast<double>(y[n]) * static_cast<double>(y[n]);
    }
    return std::sqrt(sum / static_cast<double>(length));
}

// Steady-state window: skip 4 * half_width output frames, then a whole number of 1 kHz periods
// (16 output samples per period).
struct Window16 {
    std::size_t first;
    std::size_t length;
};

Window16 steady_window(FilterCandidate candidate, std::size_t output_frames) {
    const std::size_t first = std::size_t{4} * spec_for(candidate).half_width;
    const std::size_t length = ((output_frames - first) / 16) * 16;
    return {.first = first, .length = length};
}

double wrap_to_pi(double angle) { return std::remainder(angle, 2.0 * std::numbers::pi); }

TEST(ResamplerConfig, RejectsUnsupportedConfigs) {
    for (const std::uint32_t rate : {22050U, 16000U}) {
        const auto made = Resampler::make(rate, 1, FilterCandidate::Hann8);
        ASSERT_FALSE(made.has_value()) << rate;
        EXPECT_EQ(made.error(), ResamplerError::UnsupportedRate) << rate;
    }
    for (const std::uint32_t channels : {0U, 65U}) {
        const auto made = Resampler::make(48000, channels, FilterCandidate::Hann8);
        ASSERT_FALSE(made.has_value()) << channels;
        EXPECT_EQ(made.error(), ResamplerError::InvalidChannelCount) << channels;
    }
    EXPECT_TRUE(Resampler::make(44100, 2, FilterCandidate::Kaiser32).has_value());
}

TEST(ResamplerConfig, LatencyIsHalfWidthForEveryCandidateAndRate) {
    for (const FilterCandidate candidate : kFilterCandidates) {
        for (const std::uint32_t rate : {48000U, 44100U}) {
            const auto made = Resampler::make(rate, 1, candidate);
            ASSERT_TRUE(made.has_value());
            EXPECT_EQ(made->latency_frames(), spec_for(candidate).half_width) << rate;
        }
    }
}

using ToneParam = std::tuple<FilterCandidate, std::uint32_t>;

class ResamplerTone : public testing::TestWithParam<ToneParam> {
protected:
    [[nodiscard]] FilterCandidate candidate() const { return std::get<0>(GetParam()); }
    [[nodiscard]] std::uint32_t rate() const { return std::get<1>(GetParam()); }
    [[nodiscard]] std::vector<float> one_second(double freq_hz, double amplitude) const {
        return sine(rate(), freq_hz, amplitude, rate());
    }
};

TEST_P(ResamplerTone, LengthMatchesCeilAcrossRandomBlocks) {
    Resampler resampler = make_or_die(rate(), 1, candidate());
    const std::vector<float> input = one_second(1000.0, 0.5);
    const std::vector<std::size_t> sizes = random_block_sizes(64);

    const std::vector<float> out = run_blocks(resampler, input, sizes);
    EXPECT_EQ(out.size(), kOutputFramesPerSecond);
}

TEST_P(ResamplerTone, BlockSplitIsBitIdentical) {
    const std::vector<float> input = one_second(1000.0, 0.5);

    Resampler whole = make_or_die(rate(), 1, candidate());
    const std::vector<float> reference = run_one_shot(whole, input);

    // Random splits.
    Resampler random_split = make_or_die(rate(), 1, candidate());
    const std::vector<std::size_t> sizes = random_block_sizes(64);
    EXPECT_EQ(run_blocks(random_split, input, sizes), reference);

    // A run of 1-frame blocks, then random sizes for the rest of the second.
    Resampler singles = make_or_die(rate(), 1, candidate());
    std::vector<std::size_t> mixed(1000, 1);
    mixed.insert(mixed.end(), sizes.begin(), sizes.end());
    EXPECT_EQ(run_blocks(singles, input, mixed), reference);
}

TEST_P(ResamplerTone, PassbandAmplitudeWithinTolerance) {
    Resampler resampler = make_or_die(rate(), 1, candidate());
    const std::vector<float> y = run_one_shot(resampler, one_second(1000.0, 0.5));
    ASSERT_EQ(y.size(), kOutputFramesPerSecond);

    const Window16 w = steady_window(candidate(), y.size());
    const double omega = 2.0 * std::numbers::pi * 1000.0 / 16000.0;
    const ToneFit fit = fit_tone(y, w.first, w.length, omega);
    EXPECT_NEAR(fit.amplitude / 0.5, 1.0, tolerance_for(candidate()).passband)
        << "measured amplitude " << fit.amplitude;
}

TEST_P(ResamplerTone, PhaseMatchesLatency) {
    Resampler resampler = make_or_die(rate(), 1, candidate());
    const std::vector<float> y = run_one_shot(resampler, one_second(1000.0, 0.5));
    ASSERT_EQ(y.size(), kOutputFramesPerSecond);

    const Window16 w = steady_window(candidate(), y.size());
    const double omega = 2.0 * std::numbers::pi * 1000.0 / 16000.0;
    const ToneFit fit = fit_tone(y, w.first, w.length, omega);
    const double expected = -omega * static_cast<double>(resampler.latency_frames());
    const double error = wrap_to_pi(fit.phase - expected);
    EXPECT_LE(std::fabs(error), tolerance_for(candidate()).passband)
        << "measured phase " << fit.phase << " expected " << expected;
}

TEST_P(ResamplerTone, StopbandAttenuation) {
    Resampler resampler = make_or_die(rate(), 1, candidate());
    const std::vector<float> y = run_one_shot(resampler, one_second(12000.0, 0.5));
    ASSERT_EQ(y.size(), kOutputFramesPerSecond);

    const Window16 w = steady_window(candidate(), y.size());
    const double reference_rms = 0.5 / std::numbers::sqrt2;
    const double attenuation_db = -20.0 * std::log10(rms(y, w.first, w.length) / reference_rms);
    EXPECT_GE(attenuation_db, tolerance_for(candidate()).stopband_db)
        << "measured attenuation " << attenuation_db << " dB";
}

TEST_P(ResamplerTone, DownmixMatchesMono) {
    const std::vector<float> mono = one_second(1000.0, 0.5);

    Resampler mono_resampler = make_or_die(rate(), 1, candidate());
    const std::vector<float> reference = run_one_shot(mono_resampler, mono);

    constexpr std::array<float, 2> same{1.0F, 1.0F};
    Resampler stereo_same = make_or_die(rate(), 2, candidate());
    EXPECT_EQ(run_one_shot(stereo_same, interleave(mono, same)), reference);

    constexpr std::array<float, 2> opposite{1.0F, -1.0F};
    Resampler stereo_opposite = make_or_die(rate(), 2, candidate());
    const std::vector<float> cancelled = run_one_shot(stereo_opposite, interleave(mono, opposite));
    ASSERT_EQ(cancelled.size(), reference.size());
    EXPECT_TRUE(std::ranges::all_of(cancelled, [](float v) { return v == 0.0F; }));
}

std::string param_name(const testing::TestParamInfo<ToneParam>& info) {
    std::string name{spec_for(std::get<0>(info.param)).name};
    std::ranges::replace(name, '-', '_');
    return name + "_at_" + std::to_string(std::get<1>(info.param));
}

INSTANTIATE_TEST_SUITE_P(AllCandidates, ResamplerTone,
                         testing::Combine(testing::ValuesIn(kFilterCandidates),
                                          testing::Values(48000U, 44100U)),
                         param_name);

} // namespace
} // namespace pwcap::core
