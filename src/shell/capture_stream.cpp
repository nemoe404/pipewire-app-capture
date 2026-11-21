#include "shell/capture_stream.hpp"

#include <spa/debug/types.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <format>
#include <span>
#include <string>
#include <utility>

namespace pwcap::shell {

namespace {

// The capture stream always runs as F32, 48 kHz, FL/FR; the stream adapter converts from whatever
// the graph runs at (D-04).
constexpr std::uint32_t kStreamRate = 48000;
constexpr std::uint32_t kStreamChannels = 2;

} // namespace

struct StreamCallbacks {
    static void state_changed(void* data, pw_stream_state /*old*/, pw_stream_state state,
                              const char* error) {
        static_cast<CaptureStream*>(data)->on_state_changed(state, error);
    }
    static void param_changed(void* data, std::uint32_t id, const spa_pod* param) {
        static_cast<CaptureStream*>(data)->on_param_changed(id, param);
    }
    static void process(void* data) { static_cast<CaptureStream*>(data)->on_process(); }
};

namespace {

// Built by assignment: a partial designated initialiser of this C struct would trip
// -Wmissing-designated-field-initializers.
constexpr pw_stream_events make_stream_events() noexcept {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = &StreamCallbacks::state_changed;
    events.param_changed = &StreamCallbacks::param_changed;
    events.process = &StreamCallbacks::process;
    return events;
}

constexpr pw_stream_events kStreamEvents = make_stream_events();

} // namespace

CaptureStream::CaptureStream(PrivateTag /*key*/, pw_thread_loop* loop,
                             core::SpscRing<core::StereoFrame>& ring, DiagnosticQueue& diagnostics)
    : loop_{loop}, ring_{&ring}, diagnostics_{&diagnostics} {}

std::expected<std::unique_ptr<CaptureStream>, pwcap::Error>
CaptureStream::create(pw_thread_loop* loop, pw_core* core, const std::string& node_name,
                      core::SpscRing<core::StereoFrame>& ring, DiagnosticQueue& diagnostics) {
    // Routing is only ever done by links the reconciler makes: node.autoconnect=false, and no
    // target or passive property of any kind (SAFE-03).
    const std::array<spa_dict_item, 4> items{{
        {.key = PW_KEY_MEDIA_TYPE, .value = "Audio"},
        {.key = PW_KEY_MEDIA_CATEGORY, .value = "Capture"},
        {.key = PW_KEY_NODE_NAME, .value = node_name.c_str()},
        {.key = PW_KEY_NODE_AUTOCONNECT, .value = "false"},
    }};
    const spa_dict dict{
        .flags = 0, .n_items = static_cast<std::uint32_t>(items.size()), .items = items.data()};
    PropertiesPtr props{pw_properties_new_dict(&dict)};
    if (!props) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }

    auto self = std::make_unique<CaptureStream>(PrivateTag{}, loop, ring, diagnostics);
    // pw_stream_new takes ownership of the properties.
    self->stream_.reset(pw_stream_new(core, "pwcap capture", props.release()));
    if (!self->stream_) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }
    pw_stream_add_listener(self->stream_.get(), self->hook_.get(), &kStreamEvents, self.get());

    // The EnumFormat: F32, 48 kHz, FL/FR. spa_pod_builder_init avoids the initialiser macro,
    // which expands to a compound literal.
    alignas(8) std::array<std::byte, 1024> pod_buffer{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer.data(),
                         static_cast<std::uint32_t>(pod_buffer.size()));
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_F32;
    format.rate = kStreamRate;
    format.channels = kStreamChannels;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;
    std::array<const spa_pod*, 1> params{
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
    if (params[0] == nullptr) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }

    // No autoconnect flag: the session manager must not choose a source for this stream.
    // A bit-flag enum: the OR of two enumerators is valid but is not an enumerator itself.
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto flags =
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    const int res = pw_stream_connect(self->stream_.get(), PW_DIRECTION_INPUT, PW_ID_ANY, flags,
                                      params.data(), static_cast<std::uint32_t>(params.size()));
    if (res < 0) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }
    return self;
}

std::expected<std::uint32_t, pwcap::Error>
CaptureStream::wait_node_id(std::chrono::milliseconds timeout) {
    timespec deadline{};
    const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    if (pw_thread_loop_get_time(loop_, &deadline, timeout_ns) < 0) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }
    // A raw loop: the wait primitive releases the lock, and no algorithm expresses that.
    while (!failed_ && pw_stream_get_node_id(stream_.get()) == PW_ID_ANY) {
        if (pw_thread_loop_timed_wait_full(loop_, &deadline) != 0) {
            return std::unexpected{pwcap::Error::NoPipeWire};
        }
    }
    if (failed_) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }
    return pw_stream_get_node_id(stream_.get());
}

void CaptureStream::on_state_changed(pw_stream_state state, const char* error) {
    if (state == PW_STREAM_STATE_ERROR) {
        failed_ = true;
        diagnostics_->post(
            std::format("Stopped: capture stream error: {}", error != nullptr ? error : "unknown"));
    }
    pw_thread_loop_signal(loop_, false);
}

// Runs on the loop thread. The stream asks for F32, 48 kHz, FL/FR and the adapter converts, so
// anything else negotiated is reported rather than fed to the converter as if it were F32 stereo.
void CaptureStream::on_param_changed(std::uint32_t id, const spa_pod* param) {
    if (id != SPA_PARAM_Format || param == nullptr) {
        return;
    }
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) < 0) {
        diagnostics_->post("Stopped: unexpected capture format (not raw audio)");
        return;
    }
    if (info.format == SPA_AUDIO_FORMAT_F32 && info.rate == kStreamRate &&
        info.channels == kStreamChannels) {
        return;
    }
    // The PipeWire C API takes the type table as a pointer to its first element.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    const char* name = spa_debug_type_find_short_name(spa_type_audio_format, info.format);
    diagnostics_->post(std::format("Stopped: unexpected capture format {}/{}/{}",
                                   name != nullptr ? name : "unknown", info.rate, info.channels));
}

void CaptureStream::on_process() noexcept {
    // Real-time callback: no allocation, no lock, no logging.
    // pwcap-rt-begin
    pw_buffer* const buffer = pw_stream_dequeue_buffer(stream_.get());
    if (buffer == nullptr) {
        return;
    }
    const spa_buffer* const spa = buffer->buffer;
    if (spa != nullptr) {
        const std::span<const spa_data> blocks{spa->datas, spa->n_datas};
        if (!blocks.empty() && blocks.front().data != nullptr && blocks.front().chunk != nullptr) {
            const spa_data& block = blocks.front();
            const std::uint32_t offset = std::min(block.chunk->offset, block.maxsize);
            const std::uint32_t size = std::min(block.chunk->size, block.maxsize - offset);
            const std::span<const std::byte> whole{static_cast<const std::byte*>(block.data),
                                                   block.maxsize};
            (void)core::push_interleaved(whole.subspan(offset, size), *ring_);
        }
    }
    pw_stream_queue_buffer(stream_.get(), buffer);
    // pwcap-rt-end
}

} // namespace pwcap::shell
