#ifndef PWCAP_SHELL_CONSUMER_HPP
#define PWCAP_SHELL_CONSUMER_HPP

// The consumer: one library thread that drains the SPSC ring the PipeWire process callback fills,
// converts to the requested output, encodes S16LE or F32LE and calls the user's Sink. It is also
// the only thread that ever calls the user's Diagnostic: messages the PipeWire loop thread posts
// to the DiagnosticQueue are delivered from here (API-02). Standard headers only: this code runs
// on its own thread and never touches PipeWire (00-architecture section 7).

#include "core/pcm.hpp"
#include "core/resampler.hpp"
#include "core/spsc_ring.hpp"
#include "pwcap/capture.hpp"
#include "shell/diagnostics.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

namespace pwcap::shell {

// The ring and the queue are non-owning: the owner keeps both alive longer than the Consumer.
struct ConsumerConfig {
    core::Conversion conversion{core::Conversion::Resample16kMono};
    pwcap::SampleFormat format{pwcap::SampleFormat::S16LE};
    core::SpscRing<core::StereoFrame>* ring{nullptr};
    DiagnosticQueue* diagnostics{nullptr};
    pwcap::Sink sink;
    pwcap::Diagnostic diagnostic;
};

class Consumer {
    struct PrivateTag {
        explicit PrivateTag() = default;
    };

public:
    // InvalidOptions when the ring or the queue is missing or the resampler cannot be built.
    // Every block buffer is allocated here, so the thread itself allocates only for diagnostics.
    [[nodiscard]] static std::expected<std::unique_ptr<Consumer>, pwcap::Error>
    start(ConsumerConfig config);

    Consumer(PrivateTag /*key*/, ConsumerConfig config, std::optional<core::Resampler> resampler);
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;
    // The thread captures this.
    Consumer(Consumer&&) = delete;
    Consumer& operator=(Consumer&&) = delete;
    // Requests stop and joins. The thread drains the ring and the queue before it returns, so
    // everything pushed before the destructor started has been delivered once it returns.
    ~Consumer() = default;

    // The thread that runs the Sink and the Diagnostic.
    [[nodiscard]] std::thread::id thread_id() const noexcept { return thread_.get_id(); }

private:
    void run(const std::stop_token& stop);
    // Moves up to one block out of the ring into frames_; returns the count.
    [[nodiscard]] std::size_t pop_block();
    void deliver_block(std::size_t frames);
    void deliver_diagnostics();
    void report_overrun(bool force);
    // Calls the user's Diagnostic; an exception from it is swallowed.
    void emit(std::string_view message);

    ConsumerConfig config_;
    std::optional<core::Resampler> resampler_;
    std::vector<core::StereoFrame> frames_;
    std::vector<float> samples_;
    std::vector<float> resampled_;
    std::vector<std::byte> bytes_;
    std::uint64_t last_dropped_{0};
    std::uint64_t pending_dropped_{0};
    std::optional<std::chrono::steady_clock::time_point> last_overrun_report_;
    bool sink_failed_{false};
    // Last: it stops and joins before any member the thread uses is destroyed.
    std::jthread thread_;
};

} // namespace pwcap::shell

#endif // PWCAP_SHELL_CONSUMER_HPP
