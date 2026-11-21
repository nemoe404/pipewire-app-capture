#ifndef PWCAP_SHELL_PW_HANDLES_HPP
#define PWCAP_SHELL_PW_HANDLES_HPP

// Move-only RAII handles over PipeWire's C objects. Each handle is a std::unique_ptr with a
// stateless deleter that calls the matching destroy function (MEM-02, MEM-10).
//
// Ordering rule for every class that holds a handle together with a HookGuard on that handle:
// declare each HookGuard AFTER the handle it listens to, so it is destroyed first. Removing a hook
// after its object was freed is a use-after-free.
//
// Locking: the destroy functions of objects that belong to a running thread loop must be called
// with the loop lock held (see LoopLock) or after the loop was stopped.

#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>

#include <memory>
#include <type_traits>

namespace pwcap::shell {

struct ThreadLoopDeleter {
    void operator()(pw_thread_loop* loop) const noexcept { pw_thread_loop_destroy(loop); }
};
struct MainLoopDeleter {
    void operator()(pw_main_loop* loop) const noexcept { pw_main_loop_destroy(loop); }
};
struct ContextDeleter {
    void operator()(pw_context* context) const noexcept { pw_context_destroy(context); }
};
struct CoreDeleter {
    void operator()(pw_core* core) const noexcept { static_cast<void>(pw_core_disconnect(core)); }
};
struct StreamDeleter {
    void operator()(pw_stream* stream) const noexcept { pw_stream_destroy(stream); }
};
struct ProxyDeleter {
    void operator()(pw_proxy* proxy) const noexcept { pw_proxy_destroy(proxy); }
};
struct PropertiesDeleter {
    void operator()(pw_properties* properties) const noexcept { pw_properties_free(properties); }
};

using ThreadLoopPtr = std::unique_ptr<pw_thread_loop, ThreadLoopDeleter>;
using MainLoopPtr = std::unique_ptr<pw_main_loop, MainLoopDeleter>;
using ContextPtr = std::unique_ptr<pw_context, ContextDeleter>;
using CorePtr = std::unique_ptr<pw_core, CoreDeleter>;
using StreamPtr = std::unique_ptr<pw_stream, StreamDeleter>;
using ProxyPtr = std::unique_ptr<pw_proxy, ProxyDeleter>;
using PropertiesPtr = std::unique_ptr<pw_properties, PropertiesDeleter>;

// PipeWire's C API models the registry as a proxy: pw_registry is the same object seen through
// its interface. These two helpers are the only casts between the two views.
[[nodiscard]] inline ProxyPtr make_registry_handle(pw_registry* registry) noexcept {
    // PipeWire's C API models the registry as a proxy.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ProxyPtr{reinterpret_cast<pw_proxy*>(registry)};
}

[[nodiscard]] inline pw_registry* registry_of(const ProxyPtr& handle) noexcept {
    // PipeWire's C API models the registry as a proxy.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<pw_registry*>(handle.get());
}

// Owns one spa_hook. PipeWire stores the hook's address in the listener list, so the object can
// be neither copied nor moved. reset() is idempotent.
class HookGuard {
public:
    HookGuard() noexcept = default;
    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;
    HookGuard(HookGuard&&) = delete;
    HookGuard& operator=(HookGuard&&) = delete;
    ~HookGuard() { reset(); }

    [[nodiscard]] spa_hook* get() noexcept { return &hook_; }

    void reset() noexcept {
        // A hook that was never added has a null list link; spa_hook_remove would be safe, but a
        // removed hook must not be removed twice, so the hook is cleared after removal.
        if (hook_.link.next != nullptr) {
            spa_hook_remove(&hook_);
        }
        hook_ = spa_hook{};
    }

private:
    spa_hook hook_{};
};

// Scoped pw_thread_loop_lock / pw_thread_loop_unlock.
class LoopLock {
public:
    explicit LoopLock(pw_thread_loop* loop) noexcept : loop_{loop} { pw_thread_loop_lock(loop_); }
    LoopLock(const LoopLock&) = delete;
    LoopLock& operator=(const LoopLock&) = delete;
    LoopLock(LoopLock&&) = delete;
    LoopLock& operator=(LoopLock&&) = delete;
    ~LoopLock() { pw_thread_loop_unlock(loop_); }

private:
    pw_thread_loop* loop_;
};

template <typename Handle>
inline constexpr bool kIsMoveOnlyHandle =
    !std::is_copy_constructible_v<Handle> && std::is_nothrow_move_constructible_v<Handle>;

static_assert(kIsMoveOnlyHandle<ThreadLoopPtr>);
static_assert(kIsMoveOnlyHandle<MainLoopPtr>);
static_assert(kIsMoveOnlyHandle<ContextPtr>);
static_assert(kIsMoveOnlyHandle<CorePtr>);
static_assert(kIsMoveOnlyHandle<StreamPtr>);
static_assert(kIsMoveOnlyHandle<ProxyPtr>);
static_assert(kIsMoveOnlyHandle<PropertiesPtr>);

} // namespace pwcap::shell

#endif // PWCAP_SHELL_PW_HANDLES_HPP
