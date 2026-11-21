#ifndef PWCAP_SHELL_CAPTURE_STREAM_HPP
#define PWCAP_SHELL_CAPTURE_STREAM_HPP

// The capture stream: an input pw_stream that asks for F32, 48 kHz, FL/FR (D-04) and is never
// connected by PipeWire's session manager. It carries node.autoconnect=false and no target or
// passive property, and it is connected without the autoconnect flag, so nothing links it
// except the reconciler acting on core::plan_links output (SAFE-03).

#include "pwcap/capture.hpp"
#include "shell/pw_handles.hpp"

#include <pipewire/pipewire.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace pwcap::shell {

class CaptureStream {
    struct PrivateTag {
        explicit PrivateTag() = default;
    };

public:
    // Called with the thread-loop lock held.
    [[nodiscard]] static std::expected<std::unique_ptr<CaptureStream>, pwcap::Error>
    create(pw_thread_loop* loop, pw_core* core, const std::string& node_name);

    CaptureStream(PrivateTag /*key*/, pw_thread_loop* loop);
    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;
    CaptureStream(CaptureStream&&) = delete;
    CaptureStream& operator=(CaptureStream&&) = delete;
    // The caller holds the thread-loop lock. Removes the listener, then destroys the stream.
    ~CaptureStream() = default;

    // Called with the lock held. Waits (releasing it) until the stream's node exists. NoPipeWire
    // on timeout or when the stream reports an error.
    [[nodiscard]] std::expected<std::uint32_t, pwcap::Error>
    wait_node_id(std::chrono::milliseconds timeout);

private:
    friend struct StreamCallbacks;

    void on_state_changed(pw_stream_state state);
    void on_process();

    pw_thread_loop* loop_;
    // Handle first, then the hook that listens to it (see pw_handles.hpp).
    StreamPtr stream_;
    HookGuard hook_;
    bool failed_{false};
};

} // namespace pwcap::shell

#endif // PWCAP_SHELL_CAPTURE_STREAM_HPP
