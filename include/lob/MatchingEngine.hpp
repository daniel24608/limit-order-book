// MatchingEngine.hpp
// Runs the order book on its own "matching thread" and feeds it through a
// lock-free SPSC queue.
//
//   ingestion thread                         matching thread
//   ----------------                         ---------------
//   submit(request) --> [ SpscQueue ] -->    pop request
//                                            apply to OrderBook
//                                            call the callback with the trades
//
// Rules:
//   - submit() must always be called from the SAME single thread (the producer).
//   - The callback runs on the matching thread.
//   - book() may only be read after stop() has returned.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "lob/OrderBook.hpp"
#include "lob/SpscQueue.hpp"
#include "lob/Types.hpp"

namespace lob {

class MatchingEngine {
public:
    static constexpr std::size_t kQueueCapacity = 1 << 16;  // 65,536 slots
    static constexpr int kSpinPollsBeforeYield = 20000;    // see runMatchingLoop()

    // Called on the matching thread after each request has been fully processed.
    // `trades` holds only the trades caused by that request.
    using ProcessedCallback =
        std::function<void(const Request& request, const std::vector<Trade>& trades)>;

    MatchingEngine();
    explicit MatchingEngine(ProcessedCallback callback);
    ~MatchingEngine();

    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // Launches the matching thread.
    void start();

    // Producer side. Spins (yielding the CPU) until there is room in the queue.
    void submit(const Request& request);

    // Waits until every submitted request has been processed, then stops and
    // joins the matching thread. Safe to call more than once.
    void stop();

    // Only valid after stop().
    const OrderBook& book() const { return book_; }

    std::uint64_t requestsProcessed() const {
        return requestsProcessed_.load(std::memory_order_acquire);
    }
    std::uint64_t tradesExecuted() const {
        return tradesExecuted_.load(std::memory_order_acquire);
    }

private:
    void runMatchingLoop();
    void processRequest(const Request& request);

    SpscQueue<Request, kQueueCapacity> queue_;
    OrderBook book_;
    ProcessedCallback callback_;

    std::thread matchingThread_;
    std::atomic<bool> stopRequested_{false};
    bool running_ = false;  // only touched by the thread that calls start()/stop()

    std::atomic<std::uint64_t> requestsProcessed_{0};
    std::atomic<std::uint64_t> tradesExecuted_{0};

    // Reused for every request so matching does not allocate on each order.
    std::vector<Trade> tradeBuffer_;
};

}  // namespace lob
