// A single-threaded limit order book with price-time priority.
//
// Prices are integers in ticks (e.g. cents) so comparisons are exact.
//
// Order types:
//   Limit  - trade at this price or better; any leftover rests in the book.
//   Market - trade immediately at the best prices available; leftover is dropped.
//   IOC    - immediate-or-cancel: like Limit, but leftover is dropped, never rests.
//
// Matching rule: best price first; at the same price, earliest order first.
// Trades execute at the resting order's price.
#pragma once

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

namespace ob {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market, IOC };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

struct Order {
    OrderId id{};
    Side side{};
    OrderType type{};
    Qty qty{};
    Price price{};  // ignored for Market orders
    Qty remaining{};

    static Order limit(OrderId id, Side s, Qty q, Price p) { return {id, s, OrderType::Limit, q, p, q}; }
    static Order market(OrderId id, Side s, Qty q) { return {id, s, OrderType::Market, q, 0, q}; }
    static Order ioc(OrderId id, Side s, Qty q, Price p) { return {id, s, OrderType::IOC, q, p, q}; }
};

struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;
    Qty qty;
    Side aggressor;  // side of the incoming order that caused the trade
};

struct OrderBookError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

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

}  // namespace ob
