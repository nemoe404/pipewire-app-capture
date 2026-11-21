// S2 measurement driver (outside the repo): resample a raw float32 mono 48 kHz file to 16 kHz with
// one candidate filter from src/core. Usage: driver <hann8|kaiser16|kaiser32> <in.raw> <out.raw>
#include "core/fir_design.hpp"
#include "core/resampler.hpp"

#include <cstdio>
#include <cstring>
#include <expected>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const std::string_view name{argv[1]};
    pwcap::core::FilterCandidate cand{};
    if (name == "hann8") cand = pwcap::core::FilterCandidate::Hann8;
    else if (name == "kaiser16") cand = pwcap::core::FilterCandidate::Kaiser16;
    else if (name == "kaiser32") cand = pwcap::core::FilterCandidate::Kaiser32;
    else return 3;

    std::ifstream in{argv[2], std::ios::binary};
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), {}};
    std::vector<float> samples(bytes.size() / sizeof(float));
    std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(float));  // float32 LE
    // Pad 0.25 s of silence so the filter's latency tail is flushed; trimmed by the caller's alignment.
    samples.resize(samples.size() + 12000, 0.0F);

    auto made = pwcap::core::Resampler::make(48000, 1, cand);
    if (!made) return 4;
    auto& rs = *made;
    std::vector<float> out(rs.max_output_frames(samples.size()));
    const std::size_t n = rs.process(samples, out);
    std::ofstream o{argv[3], std::ios::binary};
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(n * sizeof(float)));
    std::printf("%s frames_out=%zu latency=%u\n", argv[1], n, rs.latency_frames());
    return 0;
}
