// Reference order book (the original std::map version).
#pragma once

#include "orderbook.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ref {

using ob::OrderId;
using ob::Price;
using ob::Qty;
using ob::Side;
using ob::OrderType;
using ob::Order;
using ob::Trade;
using ob::OrderBookError;

// Original std::map / std::list book, kept as a reference implementation:
// tests replay random order streams through both books and require
// identical trades, and the benchmark compares their speed.
class OrderBook {
public:
    // Submit an order. Trades it causes are appended to `out`.
    void add(Order order, std::vector<Trade>& out);
    std::vector<Trade> add(Order order);

    // Remove a resting order. Returns false if it isn't resting.
    bool cancel(OrderId id);

    // Shrinking the quantity keeps the order's place in line. Changing the
    // price or increasing the quantity sends it to the back (it may trade).
    void modify(OrderId id, std::optional<Qty> qty, std::optional<Price> price, std::vector<Trade>& out);
    std::vector<Trade> modify(OrderId id, std::optional<Qty> qty, std::optional<Price> price);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::optional<Price> spread() const;
    std::vector<std::pair<Price, Qty>> depth(Side side, std::size_t levels = 5) const;

    const Order* get(OrderId id) const;
    std::size_t size() const { return index_.size(); }
    Qty resting_qty() const;

    // Every trade so far. Turn off for long benchmarks to save memory.
    const std::vector<Trade>& trades() const { return history_; }
    void keep_history(bool on) { keep_history_ = on; }

    std::string to_string(std::size_t levels = 5) const;

private:
    using Level = std::list<Order>;  // FIFO; list iterators stay valid on erase
    using Bids = std::map<Price, Level, std::greater<Price>>;  // best (highest) first
    using Asks = std::map<Price, Level, std::less<Price>>;     // best (lowest) first

    struct Location {
        Side side;
        Price price;
        Level::iterator it;
    };

    void validate(const Order& o) const;
    void match(Order& o, std::vector<Trade>& out);
    template <class Book>
    void match_against(Book& book, Order& o, std::vector<Trade>& out);
    void rest(const Order& o);
    static bool crosses(const Order& o, Price best);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Location> index_;
    std::unordered_set<OrderId> seen_;
    std::vector<Trade> history_;
    bool keep_history_ = true;
};

}  // namespace ref
