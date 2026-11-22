#include "shell/consumer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace pwcap::shell {

namespace {

using namespace std::chrono_literals;

// 100 ms at 48 kHz.
constexpr std::size_t kBlockFrames = 4800;
constexpr std::chrono::milliseconds kIdleWait = 5ms;
constexpr std::chrono::seconds kOverrunInterval = 1s;
// The widest encoding is interleaved F32: 2 channels of 4 bytes.
constexpr std::size_t kMaxBlockBytes = kBlockFrames * core::kCaptureFrameBytes;

} // namespace

std::expected<std::unique_ptr<Consumer>, pwcap::Error> Consumer::start(ConsumerConfig config) {
    if (config.ring == nullptr || config.diagnostics == nullptr) {
        return std::unexpected{pwcap::Error::InvalidOptions};
    }
    std::optional<core::Resampler> resampler;
    if (config.conversion == core::Conversion::Resample16kMono) {
        // hann8 is the filter chosen by the resampler quality measurement.
        auto made = core::Resampler::make(core::kCaptureRate, core::kCaptureChannels,
                                          core::FilterCandidate::Hann8);
        if (!made) {
            return std::unexpected{pwcap::Error::InvalidOptions};
        }
        resampler.emplace(std::move(*made));
    }
    auto self = std::make_unique<Consumer>(PrivateTag{}, std::move(config), std::move(resampler));
    self->thread_ =
        std::jthread{[raw = self.get()](const std::stop_token& stop) { raw->run(stop); }};
    return self;
}

Consumer::Consumer(PrivateTag /*key*/, ConsumerConfig config,
                   std::optional<core::Resampler> resampler)
    : config_{std::move(config)}, resampler_{std::move(resampler)}, frames_(kBlockFrames),
      samples_(kBlockFrames * core::kCaptureChannels),
      resampled_(resampler_ ? resampler_->max_output_frames(kBlockFrames) : 0),
      bytes_(kMaxBlockBytes) {}

void Consumer::run(const std::stop_token& stop) {
    // A raw loop: a polling consumer with a stop condition; no algorithm expresses that.
    while (!stop.stop_requested()) {
        deliver_diagnostics();
        const std::size_t frames = pop_block();
        if (frames != 0) {
            deliver_block(frames);
        }
        report_overrun(false);
        if (frames == 0) {
            std::this_thread::sleep_for(kIdleWait);
        }
    }
    // Flush: everything still in the ring goes through the normal path before the thread exits.
    // A raw loop: the ring is drained until it reports empty.
    for (std::size_t frames = pop_block(); frames != 0; frames = pop_block()) {
        deliver_block(frames);
    }
    report_overrun(true);
    deliver_diagnostics();
}

std::size_t Consumer::pop_block() {
    // A raw loop: try_pop consumes an element per call, so no algorithm fits.
    std::size_t count = 0;
    while (count < frames_.size()) {
        const std::optional<core::StereoFrame> frame = config_.ring->try_pop();
        if (!frame) {
            break;
        }
        frames_.at(count) = *frame;
        ++count;
    }
    return count;
}

void Consumer::deliver_block(std::size_t frames) {
    if (sink_failed_) {
        // The ring keeps draining so the producer never sees a stuck consumer.
        return;
    }
    const std::span<const core::StereoFrame> in = std::span{frames_}.first(frames);
    std::span<const float> to_encode;
    switch (config_.conversion) {
    case core::Conversion::Resample16kMono: {
        if (!resampler_) {
            return; // start() builds it for this conversion
        }
        core::interleave(in, samples_);
        const std::size_t produced = resampler_->process(
            std::span<const float>{samples_}.first(frames * core::kCaptureChannels), resampled_);
        to_encode = std::span<const float>{resampled_}.first(produced);
        break;
    }
    case core::Conversion::Passthrough48kStereo:
        core::interleave(in, samples_);
        to_encode = std::span<const float>{samples_}.first(frames * core::kCaptureChannels);
        break;
    case core::Conversion::Downmix48kMono:
        core::downmix(in, samples_);
        to_encode = std::span<const float>{samples_}.first(frames);
        break;
    }
    const std::size_t written = config_.format == pwcap::SampleFormat::S16LE
                                    ? core::encode_s16le(to_encode, bytes_)
                                    : core::encode_f32le(to_encode, bytes_);
    if (written == 0 || !config_.sink) {
        return;
    }
    try {
        config_.sink(std::span<const std::byte>{bytes_}.first(written));
    } catch (...) {
        sink_failed_ = true;
        emit("Stopped: the sink callback threw; audio delivery stopped");
    }
}

void Consumer::deliver_diagnostics() {
    std::ranges::for_each(config_.diagnostics->take(),
                          [this](const std::string& message) { emit(message); });
}

void Consumer::report_overrun(bool force) {
    const std::uint64_t dropped = config_.ring->dropped();
    pending_dropped_ += dropped - last_dropped_;
    last_dropped_ = dropped;
    if (pending_dropped_ == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && last_overrun_report_ && now - *last_overrun_report_ < kOverrunInterval) {
        return;
    }
    // Already on the consumer thread, so straight to the Diagnostic.
    emit(std::format("Overrun: dropped {} frames", pending_dropped_));
    pending_dropped_ = 0;
    last_overrun_report_ = now;
}

void Consumer::emit(std::string_view message) {
    if (!config_.diagnostic) {
        return;
    }
    try {
        config_.diagnostic(message);
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // A throwing Diagnostic must not take the thread down, and there is nowhere to report it.
    }
}

} // namespace pwcap::shell
