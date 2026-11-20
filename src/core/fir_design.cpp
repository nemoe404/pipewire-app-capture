#include "core/fir_design.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numbers>
#include <ranges>
#include <vector>

namespace pwcap::core {
namespace {

double hann_window(std::size_t n, std::size_t length) {
    assert(length >= 2 && n < length);
    const double phase =
        2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(length - 1);
    return 0.5 - (0.5 * std::cos(phase));
}

// i0_beta is I0(beta), passed in so a whole prototype computes it once.
double kaiser_window(std::size_t n, std::size_t length, double beta, double i0_beta) {
    assert(length >= 2 && n < length);
    const double r = ((2.0 * static_cast<double>(n)) / static_cast<double>(length - 1)) - 1.0;
    return std::cyl_bessel_i(0.0, beta * std::sqrt(std::max(0.0, 1.0 - (r * r)))) / i0_beta;
}

double sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    const double px = std::numbers::pi * x;
    return std::sin(px) / px;
}

} // namespace

double hann(std::size_t n, std::size_t length) { return hann_window(n, length); }

double kaiser(std::size_t n, std::size_t length, double beta) {
    return kaiser_window(n, length, beta, std::cyl_bessel_i(0.0, beta));
}

std::vector<float> design_prototype(const FilterSpec& spec, std::uint32_t up, std::uint32_t down) {
    assert(up >= 1 && down > up);

    const std::size_t centre = std::size_t{spec.half_width} * down;
    const std::size_t length = (2 * centre) + 1;
    // Cutoff in cycles per prototype sample. The prototype rate is 16000 * down, so the 8 kHz
    // output Nyquist is 1 / (2 * down).
    const double fc = spec.cutoff / (2.0 * static_cast<double>(down));
    const double i0_beta =
        spec.window == Window::Kaiser ? std::cyl_bessel_i(0.0, spec.kaiser_beta) : 1.0;

    const auto tap = [&](std::size_t n) {
        const double offset = static_cast<double>(n) - static_cast<double>(centre);
        const double window = spec.window == Window::Hann
                                  ? hann_window(n, length)
                                  : kaiser_window(n, length, spec.kaiser_beta, i0_beta);
        return 2.0 * fc * sinc(2.0 * fc * offset) * window;
    };

    const std::vector<double> h = std::views::iota(std::size_t{0}, length) |
                                  std::views::transform(tap) |
                                  std::ranges::to<std::vector<double>>();

    const double sum = std::ranges::fold_left(h, 0.0, std::plus<>{});
    const double scale = static_cast<double>(up) / sum;

    return h | std::views::transform([scale](double v) { return static_cast<float>(v * scale); }) |
           std::ranges::to<std::vector<float>>();
}

} // namespace pwcap::core
