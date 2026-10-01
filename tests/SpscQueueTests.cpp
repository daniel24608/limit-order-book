// SpscQueueTests.cpp
// Tests for the lock-free single-producer / single-consumer ring buffer.
#include <cstdint>
#include <thread>

#include "TestFramework.hpp"
#include "lob/SpscQueue.hpp"

using lob::SpscQueue;

TEST(spsc_new_queue_is_empty) {
    SpscQueue<int, 8> queue;
    CHECK(queue.emptyApprox());
    CHECK_EQ(queue.sizeApprox(), 0u);
}

TEST(spsc_push_then_pop_returns_same_item) {
    SpscQueue<int, 8> queue;
    CHECK(queue.tryPush(42));
    int value = 0;
    CHECK(queue.tryPop(value));
    CHECK_EQ(value, 42);
}

TEST(spsc_preserves_fifo_order) {
    SpscQueue<int, 8> queue;
    for (int i = 1; i <= 5; i++) {
        CHECK(queue.tryPush(i));
    }
    for (int i = 1; i <= 5; i++) {
        int value = 0;
        CHECK(queue.tryPop(value));
        CHECK_EQ(value, i);
    }
}

TEST(spsc_push_fails_when_full) {
    SpscQueue<int, 4> queue;
    CHECK(queue.tryPush(1));
    CHECK(queue.tryPush(2));
    CHECK(queue.tryPush(3));
    CHECK(queue.tryPush(4));
    CHECK(!queue.tryPush(5));  // full: capacity is 4
    CHECK_EQ(queue.sizeApprox(), 4u);
}

TEST(spsc_pop_fails_when_empty) {
    SpscQueue<int, 4> queue;
    int value = 7;
    CHECK(!queue.tryPop(value));
    CHECK_EQ(value, 7);  // unchanged on failure
}

TEST(spsc_wraps_around_many_times) {
    // Push and pop far more items than the capacity so the indices wrap
    // around the ring buffer many times.
    SpscQueue<int, 4> queue;
    for (int i = 0; i < 1000; i++) {
        CHECK(queue.tryPush(i));
        CHECK(queue.tryPush(i + 1));
        int first = -1;
        int second = -1;
        CHECK(queue.tryPop(first));
        CHECK(queue.tryPop(second));
        CHECK_EQ(first, i);
        CHECK_EQ(second, i + 1);
    }
    CHECK(queue.emptyApprox());
}

TEST(spsc_two_threads_transfer_all_items_in_order) {
    // The real concurrency test: one producer thread, one consumer thread,
    // a small queue (so it is constantly full/empty), and many items.
    // Run under ThreadSanitizer (`make tsan`) to check for data races.
    constexpr std::uint64_t kItemCount = 200000;
    SpscQueue<std::uint64_t, 64> queue;

    std::thread producer([&queue]() {
        for (std::uint64_t i = 0; i < kItemCount; i++) {
            while (!queue.tryPush(i)) {
                std::this_thread::yield();
            }
        }
    });

    bool orderCorrect = true;
    std::uint64_t received = 0;
    while (received < kItemCount) {
        std::uint64_t value = 0;
        if (queue.tryPop(value)) {
            if (value != received) {
                orderCorrect = false;
            }
            received++;
        } else {
            std::this_thread::yield();
        }
    }

    producer.join();
    CHECK(orderCorrect);
    CHECK_EQ(received, kItemCount);
    CHECK(queue.emptyApprox());
}
