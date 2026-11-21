#include "pwcap/capture.hpp"

#include "core/match.hpp"
#include "shell/capture_stream.hpp"
#include "shell/diagnostics.hpp"
#include "shell/pw_handles.hpp"
#include "shell/reconciler.hpp"

#include <pipewire/pipewire.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <ranges>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pwcap {

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kConnectTimeout = 2000ms;
constexpr std::chrono::milliseconds kConsumerPoll = 5ms;

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

// pw_init once per process; never de-initialised (it ran clean under ASan and TSan that way).
void init_pipewire_once() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
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

    // The consumer thread body: delivers diagnostics to the user's callback.
    void consume(const std::stop_token& stop);

    // Delivers the queued diagnostics on the calling thread.
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
    Sink sink;
    Diagnostic diagnostic;
    shell::DiagnosticQueue queue;
    shell::ThreadLoopPtr loop;
    shell::ContextPtr context;
    shell::CorePtr connection;
    std::unique_ptr<shell::CaptureStream> stream;
    std::unique_ptr<shell::Reconciler> reconciler;
    std::jthread consumer;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

Capture::Impl::~Impl() {
    // 1. The consumer first: it does one final drain when stop is requested.
    if (consumer.joinable()) {
        consumer.request_stop();
        consumer.join();
    }
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
    if (pw_thread_loop_start(loop.get()) < 0) {
        return std::unexpected{Error::NoPipeWire};
    }

    const shell::LoopLock lock{loop.get()};
    connection.reset(pw_context_connect(context.get(), nullptr, 0));
    if (!connection) {
        return std::unexpected{Error::NoPipeWire};
    }

    auto created = shell::Reconciler::create(
        loop.get(), connection.get(),
        shell::ReconcilerConfig{
            .selection = std::move(selection), .reconnect = reconnect, .diagnostics = &queue});
    if (!created) {
        return std::unexpected{created.error()};
    }
    reconciler = std::move(*created);

    auto capture = shell::CaptureStream::create(loop.get(), connection.get(), next_node_name());
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

void Capture::Impl::consume(const std::stop_token& stop) {
    // A raw loop: a polling consumer with a stop condition; no algorithm expresses that.
    while (!stop.stop_requested()) {
        deliver_pending();
        std::this_thread::sleep_for(kConsumerPoll);
    }
    deliver_pending();
}

std::expected<Capture, Error> Capture::open(Options options, Sink sink, Diagnostic diagnostic) {
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

    init_pipewire_once();
    auto impl = std::make_unique<Impl>();
    impl->sink = std::move(sink);
    impl->diagnostic = std::move(diagnostic);
    if (const auto built = impl->build(std::move(*selection), options.reconnect); !built) {
        // Queued messages reach the caller before the error does.
        impl->deliver_pending();
        return std::unexpected{built.error()};
    }
    impl->consumer =
        std::jthread{[raw = impl.get()](const std::stop_token& stop) { raw->consume(stop); }};
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
