// MatchingEngineTests.cpp
// Tests for the two-thread pipeline (ingestion thread -> SPSC queue -> matching thread).
// These are the tests that matter most under ThreadSanitizer.
#include <cstdint>
#include <random>
#include <vector>

#include "TestFramework.hpp"
#include "lob/MatchingEngine.hpp"

using lob::MatchingEngine;
using lob::Order;
using lob::OrderBook;
using lob::Request;
using lob::RequestType;
using lob::Side;
using lob::Trade;

// Builds a reproducible random mix of limit orders, market orders and cancels.
static std::vector<Request> makeRandomRequests(std::size_t count, unsigned seed) {
    std::mt19937_64 random(seed);
    std::uniform_int_distribution<int> percent(0, 99);
    std::uniform_int_distribution<int> priceOffset(-20, 20);
    std::uniform_int_distribution<int> quantityDist(1, 100);

    std::vector<Request> requests;
    requests.reserve(count);
    std::uint64_t nextId = 1;

    for (std::size_t i = 0; i < count; i++) {
        Request request;
        const int roll = percent(random);

        if (roll < 75) {
            request.type = RequestType::NewLimitOrder;
            request.id = nextId++;
            request.side = (percent(random) < 50) ? Side::Buy : Side::Sell;
            request.price = 1000 + priceOffset(random);
            request.quantity = static_cast<std::uint64_t>(quantityDist(random));
        } else if (roll < 80) {
            request.type = RequestType::NewMarketOrder;
            request.id = nextId++;
            request.side = (percent(random) < 50) ? Side::Buy : Side::Sell;
            request.quantity = static_cast<std::uint64_t>(quantityDist(random));
        } else {
            // Cancel some earlier id (it may already be filled; that is fine).
            request.type = RequestType::CancelOrder;
            std::uniform_int_distribution<std::uint64_t> pastId(1, nextId);
            request.id = pastId(random);
        }
        requests.push_back(request);
    }
    return requests;
}

// Applies requests directly to an OrderBook on this thread (the reference result).
static std::vector<Trade> applySingleThreaded(const std::vector<Request>& requests,
                                              OrderBook& book) {
    std::vector<Trade> allTrades;
    for (const Request& request : requests) {
        if (request.type == RequestType::NewLimitOrder) {
            Order order;
            order.id = request.id;
            order.side = request.side;
            order.price = request.price;
            order.quantity = request.quantity;
            book.addLimitOrder(order, allTrades);
        } else if (request.type == RequestType::NewMarketOrder) {
            book.addMarketOrder(request.id, request.side, request.quantity, allTrades);
        } else {
            book.cancelOrder(request.id);
        }
    }
    return allTrades;
}

TEST(engine_pipeline_matches_single_threaded_result) {
    // Push 50,000 random requests through the two-thread pipeline and check the
    // result is identical, trade for trade, to applying them on one thread.
    const std::vector<Request> requests = makeRandomRequests(50000, 12345);

    OrderBook referenceBook;
    const std::vector<Trade> expectedTrades = applySingleThreaded(requests, referenceBook);

    // The callback runs on the matching thread and is the only writer of this vector
    // until stop() joins that thread.
    std::vector<Trade> pipelineTrades;
    MatchingEngine engine([&pipelineTrades](const Request&, const std::vector<Trade>& trades) {
        for (const Trade& trade : trades) {
            pipelineTrades.push_back(trade);
        }
    });

    engine.start();
    for (const Request& request : requests) {
        engine.submit(request);
    }
    engine.stop();

    CHECK_EQ(engine.requestsProcessed(), static_cast<std::uint64_t>(requests.size()));
    CHECK_EQ(pipelineTrades.size(), expectedTrades.size());

    bool allTradesMatch = true;
    for (std::size_t i = 0; i < expectedTrades.size(); i++) {
        const Trade& a = pipelineTrades[i];
        const Trade& b = expectedTrades[i];
        if (a.buyOrderId != b.buyOrderId || a.sellOrderId != b.sellOrderId ||
            a.price != b.price || a.quantity != b.quantity) {
            allTradesMatch = false;
            break;
        }
    }
    CHECK(allTradesMatch);
    CHECK_EQ(engine.book().orderCount(), referenceBook.orderCount());
    CHECK(engine.book().bestBid() == referenceBook.bestBid());
    CHECK(engine.book().bestAsk() == referenceBook.bestAsk());
}

TEST(engine_stop_drains_all_pending_requests) {
    // Submit a burst larger than the queue and stop immediately:
    // nothing may be lost.
    MatchingEngine engine;
    engine.start();

    const std::uint64_t kCount = MatchingEngine::kQueueCapacity * 2;
    for (std::uint64_t i = 1; i <= kCount; i++) {
        Request request;
        request.type = RequestType::NewLimitOrder;
        request.id = i;
        request.side = Side::Buy;
        request.price = 100;  // all buys at one price: they rest, never trade
        request.quantity = 1;
        engine.submit(request);
    }
    engine.stop();

    CHECK_EQ(engine.requestsProcessed(), kCount);
    CHECK_EQ(engine.tradesExecuted(), 0u);
    CHECK_EQ(engine.book().orderCount(), static_cast<std::size_t>(kCount));
    CHECK_EQ(engine.book().volumeAtPrice(Side::Buy, 100), kCount);
}
