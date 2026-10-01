// OrderBook.hpp
// A single-instrument limit order book with price-time priority matching.
//
// Price-time priority means:
//   1. Price priority: the best price trades first
//      (highest bid for buyers' side, lowest ask for sellers' side).
//   2. Time priority: among orders at the SAME price, the one that arrived
//      first trades first (first in, first out).
//
// This class is NOT thread-safe on purpose. Only the matching thread touches it,
// which is the whole point of putting a queue in front of it: no locks are needed
// inside the book.
#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/Types.hpp"

namespace lob {

class OrderBook {
public:
    // `expectedOrders` pre-sizes the order-id index. Without this, the hash map
    // rehashes (copies every entry into a bigger table) each time it grows. With
    // hundreds of thousands of resting orders one rehash takes milliseconds, and
    // the matching thread is frozen for that whole time.
    explicit OrderBook(std::size_t expectedOrders = kDefaultExpectedOrders);

    static constexpr std::size_t kDefaultExpectedOrders = 1 << 20;  // ~1M orders

    // Adds a limit order. It first trades against the opposite side for as long as
    // prices cross; whatever quantity is left rests in the book.
    // Any trades produced are appended to `tradesOut`.
    // Returns false (and changes nothing) if the order is invalid:
    // zero quantity, non-positive price, or an id that is already resting.
    bool addLimitOrder(const Order& order, std::vector<Trade>& tradesOut);

    // Adds a market order: trades against the best available prices until it is
    // filled or the opposite side is empty. Any unfilled remainder is discarded
    // (a market order never rests in the book).
    // Returns false if the quantity is zero.
    bool addMarketOrder(OrderId id, Side side, Quantity quantity, std::vector<Trade>& tradesOut);

    // Removes a resting order. Returns false if no resting order has this id.
    bool cancelOrder(OrderId id);

    // --- Read-only queries (used by tests and the benchmark) ---
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    Quantity volumeAtPrice(Side side, Price price) const;
    std::size_t orderCount() const;
    std::size_t priceLevelCount(Side side) const;
    bool hasOrder(OrderId id) const;
    // Returns the ids resting at one price level, in time-priority order.
    std::vector<OrderId> ordersAtPrice(Side side, Price price) const;

private:
    // All orders at one price, oldest first. std::list is used because:
    //   - pushing to the back and popping from the front are O(1), and
    //   - erasing from the middle (a cancel) is O(1) if we already hold an iterator,
    //   - iterators stay valid when OTHER elements are inserted or erased.
    using PriceLevel = std::list<Order>;

    // Bids are sorted highest price first, asks lowest price first,
    // so in both maps `begin()` is the best price.
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    // Where a resting order lives, so a cancel can find it without searching.
    struct OrderLocation {
        Side side;
        Price price;
        PriceLevel::iterator position;
    };

    // Trades `incoming` against the levels of `oppositeSide`, best price first.
    // `pricesCross(levelPrice)` returns true if the incoming order is willing to
    // trade at that level's price. Stops when the incoming order is filled or
    // prices no longer cross. Reduces incoming.quantity by what was filled.
    template <typename OppositeMap, typename CrossFunction>
    void matchAgainst(Order& incoming, OppositeMap& oppositeSide,
                      CrossFunction pricesCross, std::vector<Trade>& tradesOut);

    // Puts the (remaining part of an) order at the back of its price level.
    void restOrder(const Order& order);

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, OrderLocation> orderIndex_;
};

// ---------------------------------------------------------------------------
// Template definition (must be visible in the header).
// ---------------------------------------------------------------------------
template <typename OppositeMap, typename CrossFunction>
void OrderBook::matchAgainst(Order& incoming, OppositeMap& oppositeSide,
                             CrossFunction pricesCross, std::vector<Trade>& tradesOut) {
    while (incoming.quantity > 0 && !oppositeSide.empty()) {
        auto bestLevelIt = oppositeSide.begin();
        const Price levelPrice = bestLevelIt->first;

        if (!pricesCross(levelPrice)) {
            break;  // best opposite price is worse than our limit: stop matching
        }

        PriceLevel& level = bestLevelIt->second;

        // Walk the level from oldest to newest (time priority).
        while (incoming.quantity > 0 && !level.empty()) {
            Order& resting = level.front();

            Quantity fillQuantity = incoming.quantity;
            if (resting.quantity < fillQuantity) {
                fillQuantity = resting.quantity;
            }

            Trade trade;
            trade.price = levelPrice;  // trade at the resting order's price
            trade.quantity = fillQuantity;
            if (incoming.side == Side::Buy) {
                trade.buyOrderId = incoming.id;
                trade.sellOrderId = resting.id;
            } else {
                trade.buyOrderId = resting.id;
                trade.sellOrderId = incoming.id;
            }
            tradesOut.push_back(trade);

            incoming.quantity -= fillQuantity;
            resting.quantity -= fillQuantity;

            if (resting.quantity == 0) {
                orderIndex_.erase(resting.id);
                level.pop_front();
            }
        }

        if (level.empty()) {
            oppositeSide.erase(bestLevelIt);  // don't keep empty price levels around
        }
    }
}

}  // namespace lob
