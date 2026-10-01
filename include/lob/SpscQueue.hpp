// SpscQueue.hpp
// A lock-free, fixed-capacity ring buffer for exactly ONE producer thread and
// exactly ONE consumer thread (Single-Producer / Single-Consumer).
//
// How it works:
//   - `tail_` counts how many items have ever been pushed. Only the producer writes it.
//   - `head_` counts how many items have ever been popped. Only the consumer writes it.
//   - The number of items currently in the queue is (tail_ - head_).
//   - An item's slot in the buffer is (counter % Capacity). Because Capacity is a
//     power of two, we can compute that with a bit mask: (counter & (Capacity - 1)).
//
// Why no mutex is needed:
//   Each counter has exactly one writer, so two threads never race to write the
//   same variable. The only thing we must guarantee is ORDERING:
//   the consumer must not see the new tail_ before it can see the item that was
//   written into the buffer. That is what the release/acquire pair does:
//     producer: write item, then tail_.store(..., release)
//     consumer: tail_.load(acquire), then read item
//   A "release" store makes every earlier write visible to any thread that later
//   does an "acquire" load of the same variable and sees that value.
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace lob {

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    SpscQueue() : buffer_(Capacity) {}

    // Copying or moving a queue that threads are using would be a bug, so forbid it.
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Called ONLY by the producer thread.
    // Returns false (and does nothing) if the queue is full.
    bool tryPush(const T& item) {
        // We are the only writer of tail_, so a relaxed read of our own value is fine.
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire: make sure we see the consumer's latest progress.
        const std::size_t head = head_.load(std::memory_order_acquire);

        const std::size_t itemsInQueue = tail - head;
        if (itemsInQueue == Capacity) {
            return false;  // full
        }

        buffer_[tail & kIndexMask] = item;

        // Release: publish the item written above before publishing the new tail.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Called ONLY by the consumer thread.
    // Returns false (and leaves `out` unchanged) if the queue is empty.
    bool tryPop(T& out) {
        // We are the only writer of head_.
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Acquire: pairs with the producer's release store of tail_.
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return false;  // empty
        }

        out = buffer_[head & kIndexMask];

        // Release: tell the producer this slot may now be overwritten.
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Approximate size. Exact only when neither thread is currently pushing/popping.
    std::size_t sizeApprox() const {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    bool emptyApprox() const { return sizeApprox() == 0; }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    static constexpr std::size_t kIndexMask = Capacity - 1;

    // head_ and tail_ are placed on separate 64-byte cache lines. If they shared a
    // cache line, every write by one thread would invalidate the other thread's
    // cached copy ("false sharing"), which is a large, hidden slowdown.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};

    // The buffer is heap-allocated once in the constructor and never resized,
    // so no allocation happens while threads are running.
    alignas(64) std::vector<T> buffer_;
};

}  // namespace lob
