// Consumer tests: no PipeWire session. A producer thread pushes frames into the ring the way the
// process callback does; the consumer converts, encodes and calls the Sink and the Diagnostic.
//
// GoogleTest is not instrumented by ThreadSanitizer, so every assertion runs on the main thread,
// after the consumer (and any helper thread) has been joined. The callbacks only record into
// plain containers; the join is the happens-before edge that makes reading them afterwards safe.

#include "core/pcm.hpp"
#include "core/resampler.hpp"
#include "core/spsc_ring.hpp"
#include "pwcap/capture.hpp"
#include "shell/consumer.hpp"
#include "shell/diagnostics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numbers>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pwcap::shell {
namespace {

using namespace std::chrono_literals;
using core::Conversion;
using core::StereoFrame;

constexpr std::size_t kRingFrames = 96000;

// Goertzel power of `samples` at `freq_hz`, for a signal sampled at `rate_hz`.
double goertzel_power(std::span<const double> samples, double freq_hz, double rate_hz) {
    const double coefficient = 2.0 * std::cos(2.0 * std::numbers::pi * freq_hz / rate_hz);
    double previous = 0.0;
    double before_previous = 0.0;
    std::ranges::for_each(samples, [&](double sample) {
        const double current = sample + (coefficient * previous) - before_previous;
        before_previous = previous;
        previous = current;
    });
    return (previous * previous) + (before_previous * before_previous) -
           (coefficient * previous * before_previous);
}

// `count` frames of a stereo tone with the same signal on both channels.
std::vector<StereoFrame> tone_frames(std::size_t count, double freq_hz, double amplitude) {
    std::vector<StereoFrame> frames(count);
    std::ranges::generate(frames, [&, index = std::size_t{0}]() mutable {
        const double phase = 2.0 * std::numbers::pi * freq_hz * static_cast<double>(index) /
                             static_cast<double>(core::kCaptureRate);
        ++index;
        const auto value = static_cast<float>(amplitude * std::sin(phase));
        return StereoFrame{.left = value, .right = value};
    });
    return frames;
}

// Native-endian decode; the target is little-endian.
std::vector<std::int16_t> decode_s16(std::span<const std::byte> bytes) {
    std::vector<std::int16_t> samples(bytes.size() / sizeof(std::int16_t));
    std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(std::int16_t));
    return samples;
}

std::vector<float> decode_f32(std::span<const std::byte> bytes) {
    std::vector<float> samples(bytes.size() / sizeof(float));
    std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(float));
    return samples;
}

// Everything the callbacks saw. Written on the consumer thread, read after it was joined. A plain
// struct with public members is the intended shape for a test fixture.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct Harness {
    explicit Harness(std::size_t capacity = kRingFrames) : ring{capacity} {}

    ConsumerConfig config(Conversion conversion, pwcap::SampleFormat format) {
        return ConsumerConfig{
            .conversion = conversion,
            .format = format,
            .ring = &ring,
            .diagnostics = &queue,
            .sink =
                [this](std::span<const std::byte> pcm) {
                    sink_threads.push_back(std::this_thread::get_id());
                    bytes.insert(bytes.end(), pcm.begin(), pcm.end());
                },
            .diagnostic =
                [this](std::string_view message) {
                    messages.emplace_back(std::this_thread::get_id(), std::string{message});
                },
        };
    }

    // The unique_ptr is a std::expected payload: the tests need the consumer to exist.
    std::unique_ptr<Consumer> start(Conversion conversion, pwcap::SampleFormat format) {
        auto started = Consumer::start(config(conversion, format));
        return started ? std::move(*started) : nullptr;
    }

    core::SpscRing<StereoFrame> ring;
    DiagnosticQueue queue;
    std::vector<std::byte> bytes;
    std::vector<std::thread::id> sink_threads;
    std::vector<std::pair<std::thread::id, std::string>> messages;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

void push_all(core::SpscRing<StereoFrame>& ring, std::span<const StereoFrame> frames) {
    std::ranges::for_each(frames, [&](const StereoFrame& frame) { ring.push(frame); });
}

template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // A raw loop: a poll with a deadline has no algorithm equivalent.
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

TEST(Consumer, Resamples48kStereoTo16kMonoS16) {
    constexpr std::size_t kTotalFrames = 48000;
    constexpr std::size_t kChunk = 480;
    const auto resampler = core::Resampler::make(core::kCaptureRate, core::kCaptureChannels,
                                                 core::FilterCandidate::Hann8);
    ASSERT_TRUE(resampler.has_value());
    const std::uint32_t latency = resampler->latency_frames();

    Harness harness;
    auto consumer = harness.start(Conversion::Resample16kMono, pwcap::SampleFormat::S16LE);
    ASSERT_NE(consumer, nullptr);
    {
        const std::vector<StereoFrame> frames = tone_frames(kTotalFrames, 440.0, 0.5);
        const std::jthread producer{[&] {
            std::ranges::for_each(frames | std::views::chunk(kChunk),
                                  [&](auto chunk) { push_all(harness.ring, chunk); });
        }};
    } // joined: every frame is in the ring
    EXPECT_EQ(harness.ring.dropped(), 0U);
    consumer.reset(); // flushes

    const std::size_t samples = harness.bytes.size() / 2;
    EXPECT_EQ(harness.bytes.size() % 2, 0U);
    EXPECT_GE(samples, 16000U - latency - 1);
    EXPECT_LE(samples, 16000U + 1);

    const std::vector<std::int16_t> decoded = decode_s16(harness.bytes);
    ASSERT_GE(decoded.size(), 8000U);
    std::vector<double> tail(8000);
    std::ranges::transform(decoded | std::views::drop(decoded.size() - tail.size()), tail.begin(),
                           [](std::int16_t value) { return static_cast<double>(value); });
    const double at_tone = goertzel_power(tail, 440.0, 16000.0);
    const double at_other = goertzel_power(tail, 1000.0, 16000.0);
    EXPECT_GT(at_tone, 0.0);
    EXPECT_GE(at_tone, 1000.0 * at_other);
}

TEST(Consumer, PassthroughF32KeepsEveryFrame) {
    constexpr std::size_t kFrames = 4800;
    std::vector<StereoFrame> frames(kFrames);
    std::ranges::generate(frames, [index = std::size_t{0}]() mutable {
        const auto base = static_cast<float>(index++) * 0.0001F;
        return StereoFrame{.left = base, .right = -base - 0.25F};
    });

    Harness harness;
    auto consumer = harness.start(Conversion::Passthrough48kStereo, pwcap::SampleFormat::F32LE);
    ASSERT_NE(consumer, nullptr);
    push_all(harness.ring, frames);
    consumer.reset();

    ASSERT_EQ(harness.bytes.size(), kFrames * 8);
    const std::vector<float> decoded = decode_f32(harness.bytes);
    ASSERT_EQ(decoded.size(), kFrames * 2);
    const bool exact =
        std::ranges::all_of(std::views::iota(std::size_t{0}, kFrames), [&](std::size_t index) {
            return decoded.at(2 * index) == frames.at(index).left &&
                   decoded.at((2 * index) + 1) == frames.at(index).right;
        });
    EXPECT_TRUE(exact);
}

TEST(Consumer, DownmixS16) {
    constexpr std::size_t kFrames = 4800;
    std::vector<StereoFrame> frames(kFrames);
    std::ranges::generate(frames, [index = std::size_t{0}]() mutable {
        const bool first_half = index++ < kFrames / 2;
        return first_half ? StereoFrame{.left = 0.5F, .right = -0.5F}
                          : StereoFrame{.left = 0.5F, .right = 0.5F};
    });

    Harness harness;
    auto consumer = harness.start(Conversion::Downmix48kMono, pwcap::SampleFormat::S16LE);
    ASSERT_NE(consumer, nullptr);
    push_all(harness.ring, frames);
    consumer.reset();

    ASSERT_EQ(harness.bytes.size(), kFrames * 2);
    const std::vector<std::int16_t> decoded = decode_s16(harness.bytes);
    ASSERT_EQ(decoded.size(), kFrames);
    EXPECT_TRUE(std::ranges::all_of(decoded | std::views::take(kFrames / 2),
                                    [](std::int16_t value) { return value == 0; }));
    EXPECT_TRUE(std::ranges::all_of(decoded | std::views::drop(kFrames / 2),
                                    [](std::int16_t value) { return value == 16384; }));
}

TEST(Consumer, CallbacksRunOnConsumerThreadOnly) {
    constexpr int kMessages = 200;
    Harness harness;
    auto consumer = harness.start(Conversion::Passthrough48kStereo, pwcap::SampleFormat::F32LE);
    ASSERT_NE(consumer, nullptr);
    const std::thread::id consumer_id = consumer->thread_id();
    const std::thread::id main_id = std::this_thread::get_id();

    std::thread::id poster_id;
    {
        const std::jthread poster{[&] {
            poster_id = std::this_thread::get_id();
            std::ranges::for_each(std::views::iota(0, kMessages), [&](int index) {
                harness.queue.post("second-" + std::to_string(index));
            });
        }};
        std::ranges::for_each(std::views::iota(0, kMessages), [&](int index) {
            harness.queue.post("main-" + std::to_string(index));
        });
        push_all(harness.ring, tone_frames(4800, 440.0, 0.25));
    } // poster joined
    consumer.reset();

    EXPECT_NE(consumer_id, main_id);
    EXPECT_NE(consumer_id, poster_id);
    EXPECT_FALSE(harness.sink_threads.empty());
    EXPECT_TRUE(std::ranges::all_of(harness.sink_threads,
                                    [&](std::thread::id id) { return id == consumer_id; }));
    EXPECT_TRUE(std::ranges::all_of(
        harness.messages, [&](const auto& message) { return message.first == consumer_id; }));

    const auto sequence_of = [&](std::string_view prefix) {
        std::vector<std::string> sequence;
        std::ranges::for_each(harness.messages, [&](const auto& message) {
            if (message.second.starts_with(prefix)) {
                sequence.push_back(message.second);
            }
        });
        return sequence;
    };
    const auto expected_of = [&](std::string_view prefix) {
        return std::views::iota(0, kMessages) | std::views::transform([&](int index) {
                   return std::string{prefix} + std::to_string(index);
               }) |
               std::ranges::to<std::vector>();
    };
    EXPECT_EQ(sequence_of("main-"), expected_of("main-"));
    EXPECT_EQ(sequence_of("second-"), expected_of("second-"));
    EXPECT_EQ(harness.messages.size(), 2U * kMessages);
}

TEST(Consumer, StopsPromptlyAndFlushes) {
    constexpr std::size_t kFrames = 4800;
    Harness harness;
    auto consumer = harness.start(Conversion::Passthrough48kStereo, pwcap::SampleFormat::F32LE);
    ASSERT_NE(consumer, nullptr);
    push_all(harness.ring, tone_frames(kFrames, 440.0, 0.25));

    const auto before = std::chrono::steady_clock::now();
    consumer.reset();
    const auto elapsed = std::chrono::steady_clock::now() - before;

    EXPECT_EQ(harness.bytes.size(), kFrames * 8);
    EXPECT_LT(elapsed, 200ms);
}

TEST(Consumer, ReportsOverrun) {
    Harness harness{64};
    push_all(harness.ring, tone_frames(1000, 440.0, 0.25)); // before the consumer exists
    ASSERT_GE(harness.ring.dropped(), 1U);
    auto consumer = harness.start(Conversion::Passthrough48kStereo, pwcap::SampleFormat::F32LE);
    ASSERT_NE(consumer, nullptr);
    consumer.reset();

    constexpr std::string_view kPrefix = "Overrun: dropped ";
    const auto found = std::ranges::find_if(
        harness.messages, [&](const auto& message) { return message.second.starts_with(kPrefix); });
    ASSERT_NE(found, harness.messages.end()) << "no Overrun diagnostic";
    const std::string rest = found->second.substr(kPrefix.size());
    const std::uint64_t named = std::stoull(rest);
    EXPECT_GE(named, 1U);
    EXPECT_EQ(named, harness.ring.dropped());
}

TEST(Consumer, SinkExceptionStopsDeliveryWithDiagnostic) {
    Harness harness;
    std::atomic<int> calls{0};
    ConsumerConfig config =
        harness.config(Conversion::Passthrough48kStereo, pwcap::SampleFormat::F32LE);
    config.sink = [&calls](std::span<const std::byte> /*pcm*/) {
        calls.fetch_add(1);
        throw std::runtime_error{"sink failure"};
    };
    auto started = Consumer::start(std::move(config));
    ASSERT_TRUE(started.has_value());
    std::unique_ptr<Consumer> consumer = std::move(*started);

    push_all(harness.ring, tone_frames(4800, 440.0, 0.25));
    ASSERT_TRUE(wait_for([&] { return calls.load() >= 1; }, 2000ms));
    push_all(harness.ring, tone_frames(4800, 440.0, 0.25));
    // The consumer keeps draining after the failure, so the producer never sees a stuck ring.
    ASSERT_TRUE(wait_for([&] { return harness.ring.size_approx() == 0; }, 2000ms));
    consumer.reset();

    EXPECT_EQ(calls.load(), 1);
    const auto stopped = std::ranges::count_if(harness.messages, [](const auto& message) {
        return message.second.starts_with("Stopped:");
    });
    EXPECT_EQ(stopped, 1);
}

} // namespace
} // namespace pwcap::shell
