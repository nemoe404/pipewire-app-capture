#include "pwcap/capture.hpp"

#include "core/match.hpp"
#include "core/pcm.hpp"
#include "core/spsc_ring.hpp"
#include "shell/capture_stream.hpp"
#include "shell/consumer.hpp"
#include "shell/diagnostics.hpp"
#include "shell/pw_handles.hpp"
#include "shell/pw_init.hpp"
#include "shell/reconciler.hpp"

#include <pipewire/pipewire.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace pwcap {

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kConnectTimeout = 2000ms;
// 2 s of the fixed 48 kHz capture: 96000 frames of 8 bytes, 768 KiB.
constexpr std::size_t kRingFrames = 96000;

// Exact, case-sensitive: every public Match field becomes an Exact pattern (D-02).
[[nodiscard]] core::Match to_core_match(const Match& match) {
    const auto exact = [](const std::string& text) {
        return core::Pattern{.text = text, .kind = core::MatchKind::Exact};
    };
    return core::Match{.app_name = exact(match.app_name),
                       .binary = exact(match.binary),
                       .node_name = exact(match.node_name),
                       .media_name = exact(match.media_name)};
}

[[nodiscard]] std::string next_node_name() {
    static std::atomic<std::uint32_t> counter{0};
    return std::format("pwcap-{}-{}", ::getpid(), counter.fetch_add(1));
}

} // namespace

struct Capture::Impl {
    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    // Tears down in the order of 00-architecture section 4.
    ~Impl();

    // Builds the loop, context, core, reconciler and capture stream. The thread-loop lock is
    // scoped to this function, so it is released before a failed Impl is destroyed: the
    // destructor stops the loop thread, and stopping it while this thread holds the lock hangs.
    [[nodiscard]] std::expected<void, Error> build(core::Selection selection, bool reconnect);

    // Delivers the queued diagnostics on the calling thread. Used only while the consumer does not
    // exist yet; afterwards the consumer owns the Diagnostic.
    void deliver_pending() {
        std::ranges::for_each(queue.take(), [this](const std::string& message) {
            if (diagnostic) {
                diagnostic(message);
            }
        });
    }

    // Impl is the library's private state, defined and used only in this file; a struct with
    // public members is the intended shape.
    // Declaration order matters for the members the destructor leaves to the compiler.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    Diagnostic diagnostic;
    shell::DiagnosticQueue queue;
    // Before the stream: the stream's process callback pushes into it until the stream is gone.
    core::SpscRing<core::StereoFrame> ring{kRingFrames};
    shell::ThreadLoopPtr loop;
    shell::ContextPtr context;
    shell::CorePtr connection;
    std::unique_ptr<shell::CaptureStream> stream;
    std::unique_ptr<shell::Reconciler> reconciler;
    std::unique_ptr<shell::Consumer> consumer;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

Capture::Impl::~Impl() {
    // 1. The consumer first: it joins, and does one final drain of the ring and the queue.
    consumer.reset();
    if (loop) {
        {
            // 2. Under the lock: our links go with the reconciler, then the stream, then the core.
            const shell::LoopLock lock{loop.get()};
            reconciler.reset();
            stream.reset();
            connection.reset();
        }
        // 3. Stop the loop thread with the lock released.
        pw_thread_loop_stop(loop.get());
    }
    // 4. The context, then the loop.
    context.reset();
    loop.reset();
}

std::expected<void, Error> Capture::Impl::build(core::Selection selection, bool reconnect) {
    loop.reset(pw_thread_loop_new("pwcap", nullptr));
    if (!loop) {
        return std::unexpected{Error::NoPipeWire};
    }
    context.reset(pw_context_new(pw_thread_loop_get_loop(loop.get()), nullptr, 0));
    if (!context) {
        return std::unexpected{Error::NoPipeWire};
    }
    // Connect before the loop thread exists. With no daemon this fails at once, and the failed
    // Impl is then destroyed with a loop that never ran: pw_thread_loop_stop is a no-op. Starting
    // the thread first and stopping it a moment later loses the stop request in libpipewire
    // 1.6.2 about once in a few hundred opens, and pw_thread_loop_stop then joins forever (found
    // in a live run where `Options.SupportedFormatsPassValidation` timed out). Nothing else
    // touches the core until the thread runs, so no lock is needed yet.
    connection.reset(pw_context_connect(context.get(), nullptr, 0));
    if (!connection) {
        return std::unexpected{Error::NoPipeWire};
    }
    if (pw_thread_loop_start(loop.get()) < 0) {
        return std::unexpected{Error::NoPipeWire};
    }

    const shell::LoopLock lock{loop.get()};

    auto created = shell::Reconciler::create(
        loop.get(), connection.get(),
        shell::ReconcilerConfig{
            .selection = std::move(selection), .reconnect = reconnect, .diagnostics = &queue});
    if (!created) {
        return std::unexpected{created.error()};
    }
    reconciler = std::move(*created);

    auto capture =
        shell::CaptureStream::create(loop.get(), connection.get(), next_node_name(), ring, queue);
    if (!capture) {
        return std::unexpected{capture.error()};
    }
    stream = std::move(*capture);

    const auto node_id = stream->wait_node_id(kConnectTimeout);
    if (!node_id) {
        return std::unexpected{node_id.error()};
    }
    return reconciler->start(*node_id, kConnectTimeout);
}

std::expected<Capture, Error> Capture::open(const Options& options, Sink sink,
                                            Diagnostic diagnostic) {
    // Validation first: nothing below touches PipeWire until the arguments are known good.
    auto selection = core::Selection::make(to_core_match(options.include),
                                           options.exclude | std::views::transform(to_core_match) |
                                               std::ranges::to<std::vector>());
    if (!selection) {
        return std::unexpected{Error::InvalidMatch};
    }
    if (!sink) {
        return std::unexpected{Error::InvalidOptions};
    }
    // Only the three fixed outputs exist (D-04). Checked before any PipeWire call (API-03).
    const auto conversion =
        core::select_conversion(options.sample_rate, static_cast<std::uint32_t>(options.channels));
    if (!conversion) {
        return std::unexpected{Error::InvalidOptions};
    }

    shell::init_pipewire_once();
    auto impl = std::make_unique<Impl>();
    impl->diagnostic = std::move(diagnostic);
    if (const auto built = impl->build(std::move(*selection), options.reconnect); !built) {
        // Queued messages reach the caller before the error does.
        impl->deliver_pending();
        return std::unexpected{built.error()};
    }
    auto consumer =
        shell::Consumer::start(shell::ConsumerConfig{.conversion = *conversion,
                                                     .format = options.format,
                                                     .ring = &impl->ring,
                                                     .diagnostics = &impl->queue,
                                                     .sink = std::move(sink),
                                                     .diagnostic = std::move(impl->diagnostic)});
    if (!consumer) {
        return std::unexpected{consumer.error()};
    }
    impl->consumer = std::move(*consumer);
    return Capture{std::move(impl)};
}

Capture::Capture(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
Capture::Capture(Capture&&) noexcept = default;
Capture& Capture::operator=(Capture&&) noexcept = default;
Capture::~Capture() = default;

bool Capture::linked() const noexcept {
    return impl_ && impl_->reconciler && impl_->reconciler->linked();
}

} // namespace pwcap
