// Real-time proof for core::push_interleaved: it allocates nothing.
//
// The allocation counter has two sources, chosen at compile time, and the test never skips:
//   - address-sanitizer and thread-sanitizer builds install the sanitizer's malloc hook, because
//     replacing operator new fails to link under -fsanitize=thread (duplicate definition with the
//     tsan C++ runtime), and the hook sees operator new allocations in both sanitizers;
//   - the plain debug build replaces the global operator new and operator delete.
//
// This file is excluded from cpp-lint on purpose: the operator new replacement has to forward to
// the C allocator (std::malloc, std::free), which the linter flags as a C allocation call.

#include "core/pcm.hpp"

#include "core/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PWCAP_RT_USE_SANITIZER_HOOK 1
#include <sanitizer/allocator_interface.h>
#else
#define PWCAP_RT_USE_SANITIZER_HOOK 0
#endif

namespace {

std::atomic<std::uint64_t> g_allocations{0};

} // namespace

#if PWCAP_RT_USE_SANITIZER_HOOK

namespace {

void count_malloc(const volatile void* /*ptr*/, std::size_t /*size*/) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
}

void ignore_free(const volatile void* /*ptr*/) {}

// Installs the hooks once, before main runs the tests. A static initialiser keeps the install out
// of the measured region.
[[maybe_unused]] const auto g_hooks_installed =
    __sanitizer_install_malloc_and_free_hooks(count_malloc, ignore_free);

} // namespace

#else

// Replacement allocation functions: count, then forward to the C allocator.
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads)
void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* ptr = std::malloc(size != 0 ? size : 1);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t /*size*/) noexcept {
    std::free(ptr);
}
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,misc-new-delete-overloads)

#endif

namespace pwcap::core {
namespace {

static_assert(noexcept(push_interleaved(std::declval<std::span<const std::byte>>(),
                                        std::declval<SpscRing<StereoFrame>&>())),
              "the process callback calls push_interleaved: it must be noexcept");

TEST(PcmRealTime, PushInterleavedAllocatesNothing) {
    // Both are built before the first counter read: their construction allocates.
    const std::vector<std::byte> buffer(4096, std::byte{0x3F});
    SpscRing<StereoFrame> ring(256); // 512 frames per call: every call overflows and drops

    const std::uint64_t before = g_allocations.load(std::memory_order_relaxed);
    std::size_t frames = 0;
    for (std::size_t call = 0; call < 1000; ++call) { // a repeat counter, no algorithm needed
        frames += push_interleaved(buffer, ring);
    }
    const std::uint64_t after = g_allocations.load(std::memory_order_relaxed);

    EXPECT_EQ(frames, 1000U * 512U);
    EXPECT_GT(ring.dropped(), 0U); // the drop-oldest path really ran
    EXPECT_EQ(after - before, 0U);
}

} // namespace
} // namespace pwcap::core
