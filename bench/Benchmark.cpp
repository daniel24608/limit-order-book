// Benchmark.cpp
// Measures three things, each repeated several times (the median run is reported):
//
//   1. Matching-only throughput: requests/sec applied directly to an OrderBook
//      on one thread. This is the raw cost of the matching logic.
//
//   2. Pipeline throughput: requests/sec through the full two-thread system
//      (ingestion thread -> SPSC queue -> matching thread), with the producer
//      pushing as fast as it can. Timed from the first submit until the matching
//      thread has processed the last request.
//
//   3. Pipeline latency: time from the moment a request is submitted to the moment
//      the matching thread has finished processing it (queue hop + matching).
//      Measured at a steady offered load of 50% of the measured pipeline throughput.
//      This matters: if you measure latency while flooding the queue, you mostly
//      measure how long requests wait in a full queue, not how fast the engine is.
//
// The workload is a reproducible random mix around a mid price:
//   70% limit orders (about a third of them cross the spread and trade),
//   25% cancels of recently submitted orders,
//    5% market orders.
// The book is pre-filled with resting orders before any timing starts.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

#include "lob/MatchingEngine.hpp"
#include "lob/OrderBook.hpp"
#include "lob/Types.hpp"

using lob::MatchingEngine;
using lob::Order;
using lob::OrderBook;
using lob::Request;
using lob::RequestType;
using lob::Side;
using lob::Trade;

namespace {

constexpr std::size_t kPrefillOrders = 20000;
constexpr std::size_t kThroughputRequests = 2000000;
constexpr std::size_t kLatencyRequests = 1000000;
constexpr int kTrials = 5;
constexpr lob::Price kMidPrice = 10000;

std::uint64_t nowNs() {
    auto sinceEpoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count());
}

// Resting orders on both sides of the mid price that do not cross each other.
std::vector<Request> makePrefill(std::uint64_t& nextId, std::mt19937_64& random) {
    std::uniform_int_distribution<int> distanceFromMid(1, 50);
    std::uniform_int_distribution<int> quantityDist(1, 100);

    std::vector<Request> requests;
    requests.reserve(kPrefillOrders);
    for (std::size_t i = 0; i < kPrefillOrders; i++) {
        Request request;
        request.type = RequestType::NewLimitOrder;
        request.id = nextId++;
        if (i % 2 == 0) {
            request.side = Side::Buy;
            request.price = kMidPrice - distanceFromMid(random);
        } else {
            request.side = Side::Sell;
            request.price = kMidPrice + distanceFromMid(random);
        }
        request.quantity = static_cast<std::uint64_t>(quantityDist(random));
        requests.push_back(request);
    }
    return requests;
}

std::vector<Request> makeWorkload(std::size_t count, std::uint64_t& nextId,
                                  std::mt19937_64& random) {
    std::uniform_int_distribution<int> percent(0, 99);
    std::uniform_int_distribution<int> passiveDistance(1, 50);
    std::uniform_int_distribution<int> aggressiveDistance(0, 3);
    std::uniform_int_distribution<int> quantityDist(1, 100);
    std::uniform_int_distribution<std::uint64_t> recentWindow(1, 2000);

    std::vector<Request> requests;
    requests.reserve(count);

    for (std::size_t i = 0; i < count; i++) {
        Request request;
        const int roll = percent(random);
        const Side side = (percent(random) < 50) ? Side::Buy : Side::Sell;

        if (roll < 70) {
            request.type = RequestType::NewLimitOrder;
            request.id = nextId++;
            request.side = side;
            request.quantity = static_cast<std::uint64_t>(quantityDist(random));

            // About one in three limit orders is aggressive (priced through the mid,
            // so it usually trades); the rest are passive and rest in the book.
            const bool aggressive = percent(random) < 33;
            if (side == Side::Buy) {
                if (aggressive) {
                    request.price = kMidPrice + aggressiveDistance(random);
                } else {
                    request.price = kMidPrice - passiveDistance(random);
                }
            } else {
                if (aggressive) {
                    request.price = kMidPrice - aggressiveDistance(random);
                } else {
                    request.price = kMidPrice + passiveDistance(random);
                }
            }
        } else if (roll < 95) {
            // Cancel one of the ~2,000 most recently issued ids.
            request.type = RequestType::CancelOrder;
            const std::uint64_t back = recentWindow(random);
            request.id = (nextId > back) ? (nextId - back) : 1;
        } else {
            request.type = RequestType::NewMarketOrder;
            request.id = nextId++;
            request.side = side;
            request.quantity = static_cast<std::uint64_t>(quantityDist(random));
        }
        requests.push_back(request);
    }
    return requests;
}

void applyToBook(const Request& request, OrderBook& book, std::vector<Trade>& trades) {
    if (request.type == RequestType::NewLimitOrder) {
        Order order;
        order.id = request.id;
        order.side = request.side;
        order.price = request.price;
        order.quantity = request.quantity;
        book.addLimitOrder(order, trades);
    } else if (request.type == RequestType::NewMarketOrder) {
        book.addMarketOrder(request.id, request.side, request.quantity, trades);
    } else {
        book.cancelOrder(request.id);
    }
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, double p) {
    std::size_t index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

// ---------------------------------------------------------------------------
// 1. Matching only (single thread, no queue)
// ---------------------------------------------------------------------------
double runMatchingOnly(const std::vector<Request>& prefill, const std::vector<Request>& work,
                       std::uint64_t& tradeCountOut) {
    OrderBook book;
    std::vector<Trade> trades;
    trades.reserve(64);

    for (const Request& request : prefill) {
        trades.clear();
        applyToBook(request, book, trades);
    }

    std::uint64_t tradeCount = 0;
    const std::uint64_t start = nowNs();
    for (const Request& request : work) {
        trades.clear();
        applyToBook(request, book, trades);
        tradeCount += trades.size();
    }
    const std::uint64_t end = nowNs();

    tradeCountOut = tradeCount;
    const double seconds = static_cast<double>(end - start) / 1e9;
    return static_cast<double>(work.size()) / seconds;
}

// ---------------------------------------------------------------------------
// 2. Pipeline throughput (producer floods the queue)
// ---------------------------------------------------------------------------
double runPipelineThroughput(const std::vector<Request>& prefill,
                             const std::vector<Request>& work) {
    MatchingEngine engine;
    engine.start();

    for (const Request& request : prefill) {
        engine.submit(request);
    }
    // Wait until the prefill is fully processed so it is not timed.
    while (engine.requestsProcessed() < prefill.size()) {
        std::this_thread::yield();
    }

    const std::uint64_t start = nowNs();
    for (const Request& request : work) {
        engine.submit(request);
    }
    engine.stop();  // returns only after every request has been processed
    const std::uint64_t end = nowNs();

    const double seconds = static_cast<double>(end - start) / 1e9;
    return static_cast<double>(work.size()) / seconds;
}

// ---------------------------------------------------------------------------
// 3. Pipeline latency at a fixed offered load
// ---------------------------------------------------------------------------
std::vector<std::uint64_t> runPipelineLatency(const std::vector<Request>& prefill,
                                              const std::vector<Request>& work,
                                              double requestsPerSecond) {
    // Filled only by the matching thread; read here only after stop() joins it.
    // assign() (not just reserve()) writes every element now, so the operating
    // system maps all the memory pages BEFORE timing starts. Otherwise the first
    // write to each page during the test triggers a page fault, which shows up
    // as a fake latency spike.
    std::vector<std::uint64_t> latencies;
    latencies.assign(work.size(), 0);
    std::size_t latencyCount = 0;
    bool recording = false;  // also only touched by the matching thread

    std::size_t prefillSeen = 0;
    const std::size_t prefillCount = prefill.size();

    MatchingEngine engine([&](const Request& request, const std::vector<Trade>&) {
        if (!recording) {
            prefillSeen++;
            if (prefillSeen == prefillCount) {
                recording = true;
            }
            return;
        }
        latencies[latencyCount] = nowNs() - request.enqueueTimeNs;
        latencyCount++;
    });
    engine.start();

    for (const Request& request : prefill) {
        engine.submit(request);
    }
    while (engine.requestsProcessed() < prefill.size()) {
        std::this_thread::yield();
    }

    const double intervalNs = 1e9 / requestsPerSecond;
    const std::uint64_t start = nowNs();

    for (std::size_t i = 0; i < work.size(); i++) {
        // Wait (busy-spin) until this request's scheduled send time.
        const std::uint64_t sendAt = start + static_cast<std::uint64_t>(intervalNs * static_cast<double>(i));
        while (nowNs() < sendAt) {
        }
        Request request = work[i];
        request.enqueueTimeNs = nowNs();
        engine.submit(request);
    }
    engine.stop();

    latencies.resize(latencyCount);
    std::sort(latencies.begin(), latencies.end());
    return latencies;
}

}  // namespace

int main(int argc, char** argv) {
    // Optional argument: offered load for the latency test as a fraction of [2].
    double loadFraction = 0.5;
    if (argc > 1) {
        loadFraction = std::atof(argv[1]);
    }

    std::cout << "Limit order book benchmark\n";
    std::cout << "  hardware threads: " << std::thread::hardware_concurrency() << "\n";
    std::cout << "  prefill orders:   " << kPrefillOrders << "\n";
    std::cout << "  trials per test:  " << kTrials << " (median reported)\n\n";

    std::mt19937_64 random(42);
    std::uint64_t nextId = 1;
    const std::vector<Request> prefill = makePrefill(nextId, random);
    const std::vector<Request> throughputWork = makeWorkload(kThroughputRequests, nextId, random);

    // --- 1 ---
    std::vector<double> matchingRates;
    std::uint64_t tradeCount = 0;
    for (int trial = 0; trial < kTrials; trial++) {
        matchingRates.push_back(runMatchingOnly(prefill, throughputWork, tradeCount));
    }
    const double matchingRate = median(matchingRates);

    // --- 2 ---
    std::vector<double> pipelineRates;
    for (int trial = 0; trial < kTrials; trial++) {
        pipelineRates.push_back(runPipelineThroughput(prefill, throughputWork));
    }
    const double pipelineRate = median(pipelineRates);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "[1] Matching only (1 thread):    " << matchingRate / 1e6 << " M requests/sec\n";
    std::cout << "    (" << throughputWork.size() << " requests -> " << tradeCount << " trades)\n";
    std::cout << "[2] Full pipeline (2 threads):   " << pipelineRate / 1e6 << " M requests/sec\n\n";

    // --- 3 ---
    const double offeredLoad = pipelineRate * loadFraction;
    std::vector<double> p50s, p90s, p99s, p999s;
    for (int trial = 0; trial < kTrials; trial++) {
        std::uint64_t latencyNextId = nextId;  // fresh ids, same distribution
        std::mt19937_64 latencyRandom(1000 + static_cast<unsigned>(trial));
        std::vector<Request> latencyWork = makeWorkload(kLatencyRequests, latencyNextId, latencyRandom);

        std::vector<std::uint64_t> sorted = runPipelineLatency(prefill, latencyWork, offeredLoad);
        p50s.push_back(static_cast<double>(percentile(sorted, 0.50)));
        p90s.push_back(static_cast<double>(percentile(sorted, 0.90)));
        p99s.push_back(static_cast<double>(percentile(sorted, 0.99)));
        p999s.push_back(static_cast<double>(percentile(sorted, 0.999)));
    }

    std::cout << "[3] Submit-to-processed latency at " << offeredLoad / 1e6
              << " M requests/sec offered load ("
              << std::setprecision(0) << loadFraction * 100 << "% of [2]):\n";
    std::cout << std::setprecision(0);
    std::cout << "      p50   " << median(p50s) << " ns\n";
    std::cout << "      p90   " << median(p90s) << " ns\n";
    std::cout << "      p99   " << median(p99s) << " ns\n";
    std::cout << "      p99.9 " << median(p999s) << " ns\n";
    return 0;
}
