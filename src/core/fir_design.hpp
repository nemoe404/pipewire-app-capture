#ifndef PWCAP_CORE_FIR_DESIGN_HPP
#define PWCAP_CORE_FIR_DESIGN_HPP

// Windowed-sinc prototype filters for the rational resampler. Pure: no I/O, no allocation other
// than the returned vector. Three candidates of increasing cost; Phase 3 (S2) measures them by
// word error rate and picks the simplest one that is within tolerance.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace pwcap::core {

enum class Window : std::uint8_t { Hann, Kaiser };

enum class FilterCandidate : std::uint8_t { Hann8, Kaiser16, Kaiser32 };

struct FilterSpec {
    std::string_view name;
    Window window;
    // Output periods (at 16 kHz) on each side of the prototype centre. Sets the taps per output
    // sample, so it is the simplicity axis of the candidates.
    std::uint32_t half_width;
    double kaiser_beta; // 0.0 for Hann
    double cutoff;      // fraction of the 8 kHz output Nyquist
};

[[nodiscard]] constexpr FilterSpec spec_for(FilterCandidate candidate) noexcept {
    switch (candidate) {
    case FilterCandidate::Hann8:
        return {.name = "hann-8",
                .window = Window::Hann,
                .half_width = 8,
                .kaiser_beta = 0.0,
                .cutoff = 0.90};
    case FilterCandidate::Kaiser16:
        return {.name = "kaiser-16",
                .window = Window::Kaiser,
                .half_width = 16,
                .kaiser_beta = 8.6,
                .cutoff = 0.90};
    case FilterCandidate::Kaiser32:
        return {.name = "kaiser-32",
                .window = Window::Kaiser,
                .half_width = 32,
                .kaiser_beta = 10.0,
                .cutoff = 0.90};
    }
    std::unreachable();
}

// Simplest first.
inline constexpr std::array<FilterCandidate, 3> kFilterCandidates{
    FilterCandidate::Hann8, FilterCandidate::Kaiser16, FilterCandidate::Kaiser32};

// Window value at tap n of a window with `length` taps (length >= 2, n < length).
[[nodiscard]] double hann(std::size_t n, std::size_t length);
[[nodiscard]] double kaiser(std::size_t n, std::size_t length, double beta);

// Low-pass prototype for converting by up/down (a decimator: down > up >= 1), designed at the
// rate input_rate * up. Length is 2 * half_width * down + 1, symmetric about the centre tap, and
// scaled so the taps sum to `up` (unity DC gain after zero-stuffing by up).
[[nodiscard]] std::vector<float> design_prototype(const FilterSpec& spec, std::uint32_t up,
                                                  std::uint32_t down);

} // namespace pwcap::core

#endif // PWCAP_CORE_FIR_DESIGN_HPP
