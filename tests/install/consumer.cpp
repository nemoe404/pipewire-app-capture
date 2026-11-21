// The README usage example, made testable. Uses the public header only: no PipeWire include.
//
// Usage: consumer <app_name> <seconds>
//   Captures the application for the given number of seconds, writing S16LE 16 kHz mono PCM to
//   stdout. Exit codes: 0 done, 1 Capture::open failed (the Error name is printed), 2 bad
//   arguments, 3 stdout could not be flushed, 4 an exception reached main.

#include "pwcap/capture.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <print>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>

namespace {

constexpr int kOk = 0;
constexpr int kOpenFailed = 1;
constexpr int kUsage = 2;
constexpr int kFlushFailed = 3;
constexpr int kInternal = 4;

[[nodiscard]] bool parse_seconds(std::string_view text, std::uint32_t& seconds) {
    const auto [end, ec] =
        std::from_chars(std::ranges::begin(text), std::ranges::end(text), seconds);
    return ec == std::errc{} && end == std::ranges::end(text) && !text.empty();
}

int run(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() != 3) {
        std::println(stderr, "usage: consumer <app_name> <seconds>");
        return kUsage;
    }

    std::uint32_t seconds = 0;
    if (!parse_seconds(args[2], seconds)) {
        std::println(stderr, "consumer: seconds must be a whole number");
        return kUsage;
    }

    pwcap::Options options;
    options.include.app_name = args[1];

    pwcap::Sink sink = [](std::span<const std::byte> pcm) {
        // A short write means stdout was closed. The caller judges the output by its byte count,
        // so the count is not checked here, and the Sink must not throw on the library thread.
        static_cast<void>(std::fwrite(pcm.data(), 1, pcm.size(), stdout));
    };
    pwcap::Diagnostic diagnostic = [](std::string_view message) {
        std::println(stderr, "{}", message);
    };

    {
        auto capture = pwcap::Capture::open(options, std::move(sink), std::move(diagnostic));
        if (!capture) {
            std::println(stderr, "{}", pwcap::to_string(capture.error()));
            return kOpenFailed;
        }
        std::this_thread::sleep_for(std::chrono::seconds{seconds});
        // The Capture is destroyed at the end of this block. Its destructor drains the ring into
        // the Sink, so every captured byte is written before the flush below.
    }
    if (std::fflush(stdout) != 0) {
        return kFlushFailed;
    }
    return kOk;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (...) {
        return kInternal;
    }
}
