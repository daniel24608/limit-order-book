// OrderBookTests.cpp
// Tests for validation, price-time priority matching, market orders and cancels.
#include <vector>

#include "TestFramework.hpp"
#include "lob/OrderBook.hpp"

using lob::Order;
using lob::OrderBook;
using lob::OrderId;
using lob::Price;
using lob::Quantity;
using lob::Side;
using lob::Trade;

// Helper so tests read clearly: makeOrder(id, side, price, quantity).
static Order makeOrder(OrderId id, Side side, Price price, Quantity quantity) {
    Order order;
    order.id = id;
    order.side = side;
    order.price = price;
    order.quantity = quantity;
    return order;
}

// ===================================================================
// Resting orders and validation
// ===================================================================

TEST(empty_book_has_no_best_prices) {
    OrderBook book;
    CHECK(!book.bestBid().has_value());
    CHECK(!book.bestAsk().has_value());
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(resting_buy_sets_best_bid) {
    OrderBook book;
    std::vector<Trade> trades;
    CHECK(book.addLimitOrder(makeOrder(1, Side::Buy, 100, 10), trades));
    CHECK(trades.empty());
    CHECK_EQ(book.bestBid().value(), 100);
    CHECK(!book.bestAsk().has_value());
}

TEST(resting_sell_sets_best_ask) {
    OrderBook book;
    std::vector<Trade> trades;
    CHECK(book.addLimitOrder(makeOrder(1, Side::Sell, 105, 10), trades));
    CHECK(trades.empty());
    CHECK_EQ(book.bestAsk().value(), 105);
    CHECK(!book.bestBid().has_value());
}

TEST(best_bid_is_highest_price) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Buy, 99, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Buy, 101, 10), trades);
    book.addLimitOrder(makeOrder(3, Side::Buy, 100, 10), trades);
    CHECK_EQ(book.bestBid().value(), 101);
}

TEST(best_ask_is_lowest_price) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 106, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 104, 10), trades);
    book.addLimitOrder(makeOrder(3, Side::Sell, 105, 10), trades);
    CHECK_EQ(book.bestAsk().value(), 104);
}

TEST(volume_at_price_sums_all_orders_at_level) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Buy, 100, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Buy, 100, 25), trades);
    book.addLimitOrder(makeOrder(3, Side::Buy, 99, 7), trades);
    CHECK_EQ(book.volumeAtPrice(Side::Buy, 100), 35u);
    CHECK_EQ(book.volumeAtPrice(Side::Buy, 99), 7u);
    CHECK_EQ(book.volumeAtPrice(Side::Buy, 98), 0u);
}

TEST(rejects_zero_quantity) {
    OrderBook book;
    std::vector<Trade> trades;
    CHECK(!book.addLimitOrder(makeOrder(1, Side::Buy, 100, 0), trades));
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(rejects_non_positive_price) {
    OrderBook book;
    std::vector<Trade> trades;
    CHECK(!book.addLimitOrder(makeOrder(1, Side::Buy, 0, 10), trades));
    CHECK(!book.addLimitOrder(makeOrder(2, Side::Sell, -5, 10), trades));
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(rejects_duplicate_order_id) {
    OrderBook book;
    std::vector<Trade> trades;
    CHECK(book.addLimitOrder(makeOrder(1, Side::Buy, 100, 10), trades));
    CHECK(!book.addLimitOrder(makeOrder(1, Side::Buy, 101, 5), trades));
    CHECK_EQ(book.orderCount(), 1u);
    CHECK_EQ(book.bestBid().value(), 100);
}

// ===================================================================
// Matching
// ===================================================================

TEST(non_crossing_orders_do_not_trade) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Buy, 100, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 101, 10), trades);
    CHECK(trades.empty());
    CHECK_EQ(book.orderCount(), 2u);
}

TEST(buy_crossing_ask_fills_completely) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Buy, 100, 10), trades);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].buyOrderId, 2u);
    CHECK_EQ(trades[0].sellOrderId, 1u);
    CHECK_EQ(trades[0].quantity, 10u);
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(sell_crossing_bid_fills_completely) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Buy, 100, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 100, 10), trades);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].buyOrderId, 1u);
    CHECK_EQ(trades[0].sellOrderId, 2u);
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(trade_executes_at_resting_order_price) {
    // Resting ask at 100, aggressive buy willing to pay 105: the buyer gets 100.
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Buy, 105, 10), trades);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].price, 100);
}

TEST(incoming_remainder_rests_after_partial_fill) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 4), trades);
    book.addLimitOrder(makeOrder(2, Side::Buy, 100, 10), trades);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 4u);
    CHECK(book.hasOrder(2));
    CHECK_EQ(book.bestBid().value(), 100);
    CHECK_EQ(book.volumeAtPrice(Side::Buy, 100), 6u);
    CHECK(!book.bestAsk().has_value());
}

TEST(resting_order_keeps_remaining_quantity_after_partial_fill) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 10), trades);
    book.addLimitOrder(makeOrder(2, Side::Buy, 100, 3), trades);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 3u);
    CHECK(book.hasOrder(1));
    CHECK(!book.hasOrder(2));
    CHECK_EQ(book.volumeAtPrice(Side::Sell, 100), 7u);
}

TEST(time_priority_within_price_level) {
    // Two asks at the same price: the one that arrived first must fill first.
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(3, Side::Buy, 100, 5), trades);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].sellOrderId, 1u);
    CHECK(!book.hasOrder(1));
    CHECK(book.hasOrder(2));
}

TEST(price_priority_across_levels) {
    // The better (lower) ask must fill first even though it arrived later.
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 102, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 101, 5), trades);
    book.addLimitOrder(makeOrder(3, Side::Buy, 102, 5), trades);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].sellOrderId, 2u);
    CHECK_EQ(trades[0].price, 101);
}

TEST(incoming_order_sweeps_multiple_levels) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 101, 5), trades);
    book.addLimitOrder(makeOrder(3, Side::Sell, 102, 5), trades);
    book.addLimitOrder(makeOrder(4, Side::Buy, 102, 15), trades);
    CHECK_EQ(trades.size(), 3u);
    CHECK_EQ(trades[0].price, 100);
    CHECK_EQ(trades[1].price, 101);
    CHECK_EQ(trades[2].price, 102);
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(sweep_stops_at_limit_price) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 101, 5), trades);
    book.addLimitOrder(makeOrder(3, Side::Sell, 103, 5), trades);
    book.addLimitOrder(makeOrder(4, Side::Buy, 101, 20), trades);
    CHECK_EQ(trades.size(), 2u);                  // 100 and 101 only
    CHECK_EQ(book.bestAsk().value(), 103);        // 103 was not touched
    CHECK_EQ(book.bestBid().value(), 101);        // remainder rests at its limit
    CHECK_EQ(book.volumeAtPrice(Side::Buy, 101), 10u);
}

TEST(filled_orders_are_removed_from_index) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Buy, 100, 5), trades);
    CHECK(!book.hasOrder(1));
    CHECK(!book.hasOrder(2));
    CHECK(!book.cancelOrder(1));  // nothing left to cancel
}

TEST(empty_price_level_is_removed) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 101, 5), trades);
    CHECK_EQ(book.priceLevelCount(Side::Sell), 2u);
    book.addLimitOrder(makeOrder(3, Side::Buy, 100, 5), trades);
    CHECK_EQ(book.priceLevelCount(Side::Sell), 1u);
    CHECK_EQ(book.bestAsk().value(), 101);
}

// ===================================================================
// Market orders
// ===================================================================

TEST(market_buy_consumes_best_asks) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 150, 5), trades);
    CHECK(book.addMarketOrder(3, Side::Buy, 8, trades));
    CHECK_EQ(trades.size(), 2u);
    CHECK_EQ(trades[0].price, 100);
    CHECK_EQ(trades[0].quantity, 5u);
    CHECK_EQ(trades[1].price, 150);
    CHECK_EQ(trades[1].quantity, 3u);
    CHECK_EQ(book.volumeAtPrice(Side::Sell, 150), 2u);
}

TEST(market_order_remainder_is_discarded) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Buy, 100, 5), trades);
    CHECK(book.addMarketOrder(2, Side::Sell, 20, trades));
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].quantity, 5u);
    CHECK(!book.hasOrder(2));            // never rests
    CHECK(!book.bestAsk().has_value());
    CHECK_EQ(book.orderCount(), 0u);
}

TEST(market_order_on_empty_book_produces_no_trades) {
    OrderBook book;
    std::vector<Trade> trades;
    CHECK(book.addMarketOrder(1, Side::Buy, 10, trades));
    CHECK(trades.empty());
    CHECK_EQ(book.orderCount(), 0u);
}

// ===================================================================
// Cancels
// ===================================================================

TEST(cancel_removes_resting_order) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Buy, 100, 10), trades);
    CHECK(book.cancelOrder(1));
    CHECK(!book.hasOrder(1));
    CHECK(!book.bestBid().has_value());
    CHECK_EQ(book.priceLevelCount(Side::Buy), 0u);
}

TEST(cancel_unknown_id_returns_false) {
    OrderBook book;
    CHECK(!book.cancelOrder(999));
}

TEST(cancel_twice_returns_false_second_time) {
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 10), trades);
    CHECK(book.cancelOrder(1));
    CHECK(!book.cancelOrder(1));
}

TEST(cancel_keeps_time_priority_of_remaining_orders) {
    // Cancel the middle order of three; the other two keep their original order,
    // and the cancelled order can no longer be matched.
    OrderBook book;
    std::vector<Trade> trades;
    book.addLimitOrder(makeOrder(1, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(2, Side::Sell, 100, 5), trades);
    book.addLimitOrder(makeOrder(3, Side::Sell, 100, 5), trades);
    CHECK(book.cancelOrder(2));

    std::vector<OrderId> expected = {1, 3};
    CHECK(book.ordersAtPrice(Side::Sell, 100) == expected);

    book.addLimitOrder(makeOrder(4, Side::Buy, 100, 10), trades);
    CHECK_EQ(trades.size(), 2u);
    CHECK_EQ(trades[0].sellOrderId, 1u);
    CHECK_EQ(trades[1].sellOrderId, 3u);
}
