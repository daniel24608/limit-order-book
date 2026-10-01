// MatchingEngine.cpp
#include "lob/MatchingEngine.hpp"

namespace lob {

MatchingEngine::MatchingEngine() : MatchingEngine(ProcessedCallback()) {}

MatchingEngine::MatchingEngine(ProcessedCallback callback) : callback_(std::move(callback)) {
    tradeBuffer_.reserve(64);
}

MatchingEngine::~MatchingEngine() {
    stop();
}

void MatchingEngine::start() {
    if (running_) {
        return;
    }
    stopRequested_.store(false, std::memory_order_release);
    running_ = true;
    matchingThread_ = std::thread(&MatchingEngine::runMatchingLoop, this);
}

void MatchingEngine::submit(const Request& request) {
    // If the queue is full, the matching thread is behind. Give up our time
    // slice and try again rather than dropping the request.
    while (!queue_.tryPush(request)) {
        std::this_thread::yield();
    }
}

void MatchingEngine::stop() {
    if (!running_) {
        return;
    }
    // The matching loop only exits once it sees the stop flag AND an empty queue,
    // so every request submitted before stop() is still processed.
    stopRequested_.store(true, std::memory_order_release);
    matchingThread_.join();
    running_ = false;
}

void MatchingEngine::runMatchingLoop() {
    Request request;
    int emptyPolls = 0;
    while (true) {
        if (queue_.tryPop(request)) {
            processRequest(request);
            emptyPolls = 0;
            continue;
        }

        // Queue looked empty. Check whether we have been asked to stop.
        if (stopRequested_.load(std::memory_order_acquire)) {
            // Re-check the queue: the producer may have pushed something
            // between our failed pop and our read of the stop flag.
            if (queue_.tryPop(request)) {
                processRequest(request);
                continue;
            }
            break;
        }

        // Nothing to do right now. Keep polling (busy-spin) for a short while,
        // because a new request usually arrives within microseconds and
        // yield() is a system call that can delay our reaction to it. Only after
        // many empty polls in a row do we yield so an idle engine does not hog
        // a CPU core forever.
        emptyPolls++;
        if (emptyPolls >= kSpinPollsBeforeYield) {
            std::this_thread::yield();
            emptyPolls = 0;
        }
    }
}

void MatchingEngine::processRequest(const Request& request) {
    tradeBuffer_.clear();

    if (request.type == RequestType::NewLimitOrder) {
        Order order;
        order.id = request.id;
        order.side = request.side;
        order.price = request.price;
        order.quantity = request.quantity;
        book_.addLimitOrder(order, tradeBuffer_);
    } else if (request.type == RequestType::NewMarketOrder) {
        book_.addMarketOrder(request.id, request.side, request.quantity, tradeBuffer_);
    } else if (request.type == RequestType::CancelOrder) {
        book_.cancelOrder(request.id);
    }

    if (callback_) {
        callback_(request, tradeBuffer_);
    }

    tradesExecuted_.fetch_add(tradeBuffer_.size(), std::memory_order_relaxed);
    requestsProcessed_.fetch_add(1, std::memory_order_release);
}

}  // namespace lob
