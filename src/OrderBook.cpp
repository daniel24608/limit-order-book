// OrderBook.cpp
#include "lob/OrderBook.hpp"

#include <iterator>  // std::prev

namespace lob {

OrderBook::OrderBook(std::size_t expectedOrders) {
    orderIndex_.reserve(expectedOrders);
}

bool OrderBook::addLimitOrder(const Order& order, std::vector<Trade>& tradesOut) {
    // --- Validation ---
    if (order.quantity == 0) {
        return false;
    }
    if (order.price <= 0) {
        return false;
    }
    if (orderIndex_.count(order.id) > 0) {
        return false;  // an order with this id is already resting
    }

    Order incoming = order;  // copy, because matching reduces its quantity

    // --- Matching ---
    if (incoming.side == Side::Buy) {
        // A buy crosses any ask priced at or below the buy's limit.
        const Price limit = incoming.price;
        auto buyCrosses = [limit](Price askPrice) { return askPrice <= limit; };
        matchAgainst(incoming, asks_, buyCrosses, tradesOut);
    } else {
        // A sell crosses any bid priced at or above the sell's limit.
        const Price limit = incoming.price;
        auto sellCrosses = [limit](Price bidPrice) { return bidPrice >= limit; };
        matchAgainst(incoming, bids_, sellCrosses, tradesOut);
    }

    // --- Rest whatever is left ---
    if (incoming.quantity > 0) {
        restOrder(incoming);
    }
    return true;
}

bool OrderBook::addMarketOrder(OrderId id, Side side, Quantity quantity,
                               std::vector<Trade>& tradesOut) {
    if (quantity == 0) {
        return false;
    }

    Order incoming;
    incoming.id = id;
    incoming.side = side;
    incoming.price = 0;  // unused: a market order accepts any price
    incoming.quantity = quantity;

    auto acceptsAnyPrice = [](Price) { return true; };

    if (side == Side::Buy) {
        matchAgainst(incoming, asks_, acceptsAnyPrice, tradesOut);
    } else {
        matchAgainst(incoming, bids_, acceptsAnyPrice, tradesOut);
    }
    // Remaining quantity (if any) is intentionally dropped.
    return true;
}

bool OrderBook::cancelOrder(OrderId id) {
    auto indexIt = orderIndex_.find(id);
    if (indexIt == orderIndex_.end()) {
        return false;
    }

    const OrderLocation location = indexIt->second;

    if (location.side == Side::Buy) {
        auto levelIt = bids_.find(location.price);
        levelIt->second.erase(location.position);
        if (levelIt->second.empty()) {
            bids_.erase(levelIt);
        }
    } else {
        auto levelIt = asks_.find(location.price);
        levelIt->second.erase(location.position);
        if (levelIt->second.empty()) {
            asks_.erase(levelIt);
        }
    }

    orderIndex_.erase(indexIt);
    return true;
}

void OrderBook::restOrder(const Order& order) {
    PriceLevel* level = nullptr;
    if (order.side == Side::Buy) {
        level = &bids_[order.price];  // creates the level if it doesn't exist yet
    } else {
        level = &asks_[order.price];
    }

    level->push_back(order);

    // std::prev(end()) is an iterator to the element we just added.
    OrderLocation location;
    location.side = order.side;
    location.price = order.price;
    location.position = std::prev(level->end());
    orderIndex_[order.id] = location;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Quantity OrderBook::volumeAtPrice(Side side, Price price) const {
    const PriceLevel* level = nullptr;

    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end()) {
            level = &it->second;
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            level = &it->second;
        }
    }

    if (level == nullptr) {
        return 0;
    }

    Quantity total = 0;
    for (const Order& order : *level) {
        total += order.quantity;
    }
    return total;
}

std::size_t OrderBook::orderCount() const {
    return orderIndex_.size();
}

std::size_t OrderBook::priceLevelCount(Side side) const {
    if (side == Side::Buy) {
        return bids_.size();
    }
    return asks_.size();
}

bool OrderBook::hasOrder(OrderId id) const {
    return orderIndex_.count(id) > 0;
}

std::vector<OrderId> OrderBook::ordersAtPrice(Side side, Price price) const {
    std::vector<OrderId> ids;

    const PriceLevel* level = nullptr;
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        if (it != bids_.end()) {
            level = &it->second;
        }
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end()) {
            level = &it->second;
        }
    }

    if (level != nullptr) {
        for (const Order& order : *level) {
            ids.push_back(order.id);
        }
    }
    return ids;
}

}  // namespace lob
