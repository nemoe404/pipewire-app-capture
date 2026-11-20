#ifndef PWCAP_CORE_SPSC_RING_HPP
#define PWCAP_CORE_SPSC_RING_HPP

// SpscRing<T>: the boundary between the PipeWire process callback (the single producer) and the
// consumer thread (the single consumer). Fixed capacity, lock-free, drop-oldest when full, and a
// counter of the drops.
//
// Real-time contract (AGENTS.md): push() allocates nothing, takes no lock, logs nothing and does
// a bounded amount of work, at most one compare-and-swap and one store. All storage is allocated
// once, in the constructor.
//
// Element storage is std::atomic<T> so T is limited to trivially copyable types whose atomic is
// always lock-free: audio samples and small frames. With atomic slots, a consumer that copies an
// element while the producer overwrites it (the drop-oldest case) is not a data race. A plain T
// array would race in exactly that case. A T that does not fit this is rejected at compile time.
//
// Protocol. ThreadSanitizer alone cannot prove this, so it is written down.
//
//   Indices write_ and read_ are monotonically increasing std::uint64_t values. They are never
//   wrapped; the slot of index i is i modulo capacity. 2^64 samples at 48 kHz cannot overflow in
//   practice, so there is no ABA on read_.
//   Only the producer stores write_. Both threads compare-and-swap read_: the consumer to claim
//   an element, the producer to drop one.
//
//   Producer push:
//     1. w = write_ (relaxed, the producer owns it), r = read_ (acquire).
//     2. If w - r >= capacity the ring is full. Try read_.compare_exchange_strong(r, r + 1)
//        (acq_rel on success, acquire on failure).
//          success: the oldest element is dropped, dropped_ is incremented.
//          failure: the consumer just claimed that element, which also freed a slot.
//                   No drop is counted.
//     3. Store the value into slot w % capacity (relaxed).
//     4. write_.store(w + 1, release).
//
//   Consumer try_pop:
//     1. Loop: r = read_ (acquire), w = write_ (acquire). If r == w the ring is empty.
//     2. Copy slot r % capacity (relaxed). Then try read_.compare_exchange_strong(r, r + 1)
//        (acq_rel).
//          success: return the copy.
//          failure: the producer dropped element r and may already have overwritten the slot.
//                   Discard the copy and retry.
//
//   Why a copy that passes the compare-and-swap is never a newer value: the producer writes slot
//   r % capacity with element r + capacity only after it has observed read_ > r. If the
//   consumer's compare-and-swap succeeded, read_ was still r, so that store had not happened yet.
//   The producer's acquire on read_ (in step 1 or on its CAS) orders its slot store after the
//   consumer's copy, because the consumer's successful CAS is a release on read_.
//
//   Every index in [0, total pushed) leaves the ring through exactly one increment of read_: a
//   consumer claim (popped) or a producer drop (dropped_). So popped + dropped() == pushed once
//   both threads are done.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

namespace pwcap::core {

// An element type the ring can hold: copyable by memcpy, default constructible for the slot
// array, and with an atomic that never falls back to a lock.
template <typename T>
concept SpscElement = std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T> &&
                      std::atomic<T>::is_always_lock_free;

// Indices and the producer's counter live on different cache lines, because the producer and the
// consumer write them from different cores. Sharing a line would make every push and pop
// invalidate the other thread's copy of it (false sharing).
inline constexpr std::size_t kSpscCacheLine = 64;

template <typename T>
    requires SpscElement<T>
// The padding is the point: the cache-line alignment of write_ and read_ is deliberate.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class SpscRing {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

public:
    // The capacity is fixed here and the slots are allocated here, once.
    explicit SpscRing(std::size_t capacity) : capacity_(capacity), slots_(capacity) {
        assert(capacity >= 1);
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    ~SpscRing() = default;

    // Producer thread only. Never blocks; when the ring is full the oldest unread element is
    // dropped to make room.
    void push(T value) noexcept {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        std::uint64_t r = read_.load(std::memory_order_acquire);
        if (w - r >= capacity_) {
            if (read_.compare_exchange_strong(r, r + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            // On failure the consumer claimed element r, which freed a slot. No drop.
        }
        slots_[slot_of(w)].store(value, std::memory_order_relaxed);
        write_.store(w + 1, std::memory_order_release);
    }

    // Consumer thread only. The oldest unread element, or std::nullopt when the ring is empty.
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        // A raw loop: a compare-and-swap retry with no algorithm equivalent. It repeats only
        // when the producer dropped the element being read, so it makes progress every pass.
        while (true) {
            std::uint64_t r = read_.load(std::memory_order_acquire);
            const std::uint64_t w = write_.load(std::memory_order_acquire);
            if (r == w) {
                return std::nullopt;
            }
            const T value = slots_[slot_of(r)].load(std::memory_order_relaxed);
            if (read_.compare_exchange_strong(r, r + 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                return value;
            }
            // The producer dropped element r and may have overwritten the slot: discard the copy.
        }
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Elements dropped because the ring was full. Safe from any thread.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    // Unread elements, exact when no other thread is running and a snapshot otherwise.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        // read_ first, write_ second. In the other order a concurrent pop could make the loaded
        // read_ exceed the loaded write_, the unsigned subtraction would wrap, and a nearly
        // empty ring would report itself full.
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(std::min<std::uint64_t>(w - r, capacity_));
    }

private:
    [[nodiscard]] std::size_t slot_of(std::uint64_t index) const noexcept {
        // The remainder is below capacity_, so it fits in size_t.
        return static_cast<std::size_t>(index % capacity_);
    }

    // Read-only after construction, shared by both threads. alignas below keeps it off the
    // lines that are written.
    std::size_t capacity_;
    std::vector<std::atomic<T>> slots_;

    // Written by the producer; dropped_ shares its line because only the producer writes it.
    alignas(kSpscCacheLine) std::atomic<std::uint64_t> write_{0};
    std::atomic<std::uint64_t> dropped_{0};

    // Written by both threads, on its own line.
    alignas(kSpscCacheLine) std::atomic<std::uint64_t> read_{0};
};

} // namespace pwcap::core

#endif // PWCAP_CORE_SPSC_RING_HPP
