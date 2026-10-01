// Types.hpp
// Basic data types shared by every part of the engine.
#pragma once

#include <cstdint>

namespace lob {

// Prices are stored as integer "ticks" (for example, cents) instead of doubles.
// Floating-point prices cause rounding problems when comparing two prices for
// equality, which a matching engine does constantly.
using Price = std::int64_t;
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;

enum class Side {
    Buy,
    Sell
};

// A resting (or incoming) limit order.
// `quantity` is the quantity that is still open, so it shrinks as the order fills.
struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
};

// One execution between a buyer and a seller.
struct Trade {
    OrderId buyOrderId = 0;
    OrderId sellOrderId = 0;
    Price price = 0;       // always the price of the order that was resting in the book
    Quantity quantity = 0;
};

// The kinds of messages the ingestion thread can send to the matching thread.
enum class RequestType {
    NewLimitOrder,
    NewMarketOrder,
    CancelOrder
};

// A single message passed through the lock-free queue.
// It is a plain struct (no pointers, no heap memory) so copying it into the
// ring buffer is cheap and safe.
struct Request {
    RequestType type = RequestType::NewLimitOrder;
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;           // ignored for market orders and cancels
    Quantity quantity = 0;     // ignored for cancels
    std::uint64_t enqueueTimeNs = 0;  // set by the producer; used only for latency measurement
};

}  // namespace lob
