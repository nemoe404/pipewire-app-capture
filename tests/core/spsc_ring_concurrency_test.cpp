#include "core/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace pwcap::core {
namespace {

constexpr std::uint64_t kItems = 200000;

struct RunConfig {
    std::size_t capacity = 1;
    // When set, the consumer waits until the producer has pushed this many items.
    std::optional<std::uint64_t> hold_consumer_until = std::nullopt;
    // When set, the consumer yields after every pop.
    bool yield_after_pop = false;
};

struct RunResult {
    std::vector<std::uint64_t> popped;
    std::uint64_t dropped = 0;
};

// One producer jthread pushes 0..kItems-1, one consumer jthread pops. Both are joined before
// this returns, so the caller makes every gtest assertion on the main thread: GoogleTest is not
// tsan instrumented and its assertion macros are not safe to call from a worker thread.
RunResult run_producer_consumer(const RunConfig& config) {
    SpscRing<std::uint64_t> ring(config.capacity);
    std::atomic<bool> done{false};
    std::latch gate(config.hold_consumer_until.has_value() ? 1 : 0);

    std::vector<std::uint64_t> popped;
    popped.reserve(kItems);

    // Both threads are joined by hand below. The destructor of a jthread requests a stop before
    // it joins, which would cut the consumer short of its final drain.
    std::jthread consumer([&](const std::stop_token& stop) {
        gate.wait();
        // A raw loop: polling until the producer is done has no algorithm equivalent. `done` is
        // read before the pop, so a nullopt after `done` was seen means the ring is empty for
        // good and nothing is left to drain.
        while (!stop.stop_requested()) {
            const bool producer_finished = done.load(std::memory_order_acquire);
            if (const std::optional<std::uint64_t> value = ring.try_pop()) {
                popped.push_back(*value);
                if (config.yield_after_pop) {
                    std::this_thread::yield();
                }
            } else if (producer_finished) {
                break;
            }
        }
    });
    std::jthread producer([&] {
        // A raw loop: the producer pushes a counted sequence and has to release the latch partway.
        for (std::uint64_t i = 0; i < kItems; ++i) {
            ring.push(i);
            if (config.hold_consumer_until.has_value() && i + 1 == *config.hold_consumer_until) {
                gate.count_down();
            }
        }
        done.store(true, std::memory_order_release);
    });
    producer.join();
    consumer.join();

    return RunResult{.popped = std::move(popped), .dropped = ring.dropped()};
}

// The oracle, checked in every test.
void expect_oracle(const RunResult& result) {
    // Drop-oldest keeps the FIFO order of the survivors: strictly increasing.
    EXPECT_EQ(std::ranges::adjacent_find(result.popped, std::ranges::greater_equal{}),
              result.popped.end());
    EXPECT_TRUE(std::ranges::all_of(result.popped, [](std::uint64_t v) { return v < kItems; }));
    // Nothing lost silently, nothing duplicated.
    EXPECT_EQ(result.popped.size() + result.dropped, kItems);
}

TEST(SpscRingConcurrency, KeepsOrderAndAccountsForEveryItem) {
    const RunResult result = run_producer_consumer(
        {.capacity = 64, .hold_consumer_until = std::nullopt, .yield_after_pop = false});
    expect_oracle(result);
}

TEST(SpscRingConcurrency, ForcedDropsAreCounted) {
    constexpr std::size_t kCapacity = 16;
    constexpr std::uint64_t kHold = 1000;
    const RunResult result = run_producer_consumer(
        {.capacity = kCapacity, .hold_consumer_until = kHold, .yield_after_pop = false});
    // The consumer popped nothing while the first kHold items went in, so at most kCapacity of
    // them were still in the ring and every other one was dropped.
    EXPECT_GE(result.dropped, kHold - kCapacity);
    expect_oracle(result);
}

TEST(SpscRingConcurrency, SlowConsumerStillConsistent) {
    const RunResult result = run_producer_consumer(
        {.capacity = 16, .hold_consumer_until = std::nullopt, .yield_after_pop = true});
    // Timing decides how many items were dropped, so the count is recorded, not asserted.
    RecordProperty("dropped", std::to_string(result.dropped));
    expect_oracle(result);
}

} // namespace
} // namespace pwcap::core
