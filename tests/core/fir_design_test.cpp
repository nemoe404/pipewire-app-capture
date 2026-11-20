#include "core/fir_design.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

namespace pwcap::core {
namespace {

struct RateRatio {
    std::uint32_t up;
    std::uint32_t down;
};

// 48 kHz -> 16 kHz reduces to 1/3; 44.1 kHz -> 16 kHz reduces to 160/441.
constexpr std::array<RateRatio, 2> kRatios{{{.up = 1, .down = 3}, {.up = 160, .down = 441}}};

// Per-branch DC gain tolerance of each candidate (the passband tolerance of the plan).
constexpr double dc_tolerance(FilterCandidate candidate) {
    switch (candidate) {
    case FilterCandidate::Hann8:
        return 2e-2;
    case FilterCandidate::Kaiser16:
        return 3.6e-4;
    case FilterCandidate::Kaiser32:
        return 1.2e-4;
    }
    return 0.0;
}

double sum_of(std::span<const float> values) {
    return std::ranges::fold_left(values, 0.0,
                                  [](double acc, float v) { return acc + static_cast<double>(v); });
}

TEST(FirDesign, SpecTableAndCandidateOrder) {
    ASSERT_EQ(kFilterCandidates.size(), std::size_t{3});
    EXPECT_EQ(kFilterCandidates[0], FilterCandidate::Hann8);
    EXPECT_EQ(kFilterCandidates[1], FilterCandidate::Kaiser16);
    EXPECT_EQ(kFilterCandidates[2], FilterCandidate::Kaiser32);

    const FilterSpec hann8 = spec_for(FilterCandidate::Hann8);
    EXPECT_EQ(hann8.name, "hann-8");
    EXPECT_EQ(hann8.window, Window::Hann);
    EXPECT_EQ(hann8.half_width, std::uint32_t{8});
    EXPECT_DOUBLE_EQ(hann8.cutoff, 0.90);

    const FilterSpec kaiser16 = spec_for(FilterCandidate::Kaiser16);
    EXPECT_EQ(kaiser16.name, "kaiser-16");
    EXPECT_EQ(kaiser16.window, Window::Kaiser);
    EXPECT_EQ(kaiser16.half_width, std::uint32_t{16});
    EXPECT_DOUBLE_EQ(kaiser16.kaiser_beta, 8.6);
    EXPECT_DOUBLE_EQ(kaiser16.cutoff, 0.90);

    const FilterSpec kaiser32 = spec_for(FilterCandidate::Kaiser32);
    EXPECT_EQ(kaiser32.name, "kaiser-32");
    EXPECT_EQ(kaiser32.window, Window::Kaiser);
    EXPECT_EQ(kaiser32.half_width, std::uint32_t{32});
    EXPECT_DOUBLE_EQ(kaiser32.kaiser_beta, 10.0);
    EXPECT_DOUBLE_EQ(kaiser32.cutoff, 0.90);
}

TEST(FirDesign, PrototypeLengths) {
    constexpr std::array<std::size_t, 3> expected_48k{49, 97, 193};
    constexpr std::array<std::size_t, 3> expected_44k1{7057, 14113, 28225};

    for (const auto [candidate, len_48k, len_44k1] :
         std::views::zip(kFilterCandidates, expected_48k, expected_44k1)) {
        const FilterSpec spec = spec_for(candidate);
        EXPECT_EQ(design_prototype(spec, 1, 3).size(), len_48k) << spec.name;
        EXPECT_EQ(design_prototype(spec, 160, 441).size(), len_44k1) << spec.name;
    }
}

TEST(FirDesign, PrototypeIsSymmetricWithCentrePeak) {
    for (const FilterCandidate candidate : kFilterCandidates) {
        const FilterSpec spec = spec_for(candidate);
        for (const RateRatio ratio : kRatios) {
            const std::vector<float> h = design_prototype(spec, ratio.up, ratio.down);
            ASSERT_EQ(h.size() % 2, std::size_t{1}) << spec.name;
            const std::size_t last = h.size() - 1;
            const std::size_t centre = last / 2;

            const float peak =
                *std::ranges::max_element(h, {}, [](float v) { return std::fabs(v); });
            const float max_abs = std::fabs(peak);

            // Mirror pairs: compare the first half with the reversed second half.
            const bool symmetric = std::ranges::equal(
                h | std::views::take(centre), h | std::views::reverse | std::views::take(centre),
                [&](float a, float b) { return std::fabs(a - b) <= 1e-6F * max_abs; });
            EXPECT_TRUE(symmetric) << spec.name << " up=" << ratio.up;

            EXPECT_FLOAT_EQ(h[centre], max_abs) << spec.name << " up=" << ratio.up;
        }
    }
}

TEST(FirDesign, PrototypeSumEqualsUp) {
    for (const FilterCandidate candidate : kFilterCandidates) {
        const FilterSpec spec = spec_for(candidate);
        for (const RateRatio ratio : kRatios) {
            const double sum = sum_of(design_prototype(spec, ratio.up, ratio.down));
            const auto up = static_cast<double>(ratio.up);
            EXPECT_NEAR(sum, up, 1e-5 * up) << spec.name << " up=" << ratio.up;
        }
    }
}

TEST(FirDesign, EveryPolyphaseBranchHasUnityDcGain) {
    for (const FilterCandidate candidate : kFilterCandidates) {
        const FilterSpec spec = spec_for(candidate);
        for (const RateRatio ratio : kRatios) {
            const std::vector<float> h = design_prototype(spec, ratio.up, ratio.down);
            const double tol = dc_tolerance(candidate);
            for (std::uint32_t p = 0; p < ratio.up; ++p) { // one branch per output phase
                // Strided view h[p], h[p + up], h[p + 2 up], ...
                const auto branch = h | std::views::drop(p) | std::views::stride(ratio.up);
                const double gain = std::ranges::fold_left(
                    branch, 0.0, [](double acc, float v) { return acc + static_cast<double>(v); });
                ASSERT_NEAR(gain, 1.0, tol) << spec.name << " up=" << ratio.up << " branch=" << p;
            }
        }
    }
}

TEST(FirDesign, WindowEndpoints) {
    constexpr std::size_t length = 193; // odd, as every prototype is
    constexpr std::size_t centre = (length - 1) / 2;

    constexpr double beta = 8.6;
    const double i0_beta = std::cyl_bessel_i(0.0, beta);
    EXPECT_NEAR(kaiser(centre, length, beta), 1.0, 1e-12);
    EXPECT_NEAR(kaiser(0, length, beta), 1.0 / i0_beta, 1e-12);
    EXPECT_NEAR(kaiser(length - 1, length, beta), 1.0 / i0_beta, 1e-12);

    EXPECT_NEAR(hann(0, length), 0.0, 1e-12);
    EXPECT_NEAR(hann(length - 1, length), 0.0, 1e-12);
    EXPECT_NEAR(hann(centre, length), 1.0, 1e-12);
}

} // namespace
} // namespace pwcap::core
