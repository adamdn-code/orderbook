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
//
// Data layout (built so the hot path does no memory allocation):
//   - Price levels live in one flat array indexed by (price - base). Finding a
//     level is an array index, not a tree walk.
//   - A bitmap marks which levels have orders, so after a level empties the
//     next best price is found 64 levels at a time with a single CPU
//     instruction (count leading/trailing zeros).
//   - Orders live in a pool (one vector) and are linked into their level's
//     FIFO queue by index. Freed slots are reused, so after warm-up adding
//     an order allocates nothing.
//   - Order ids map to pool slots through an open-addressing hash table
//     (one flat array, linear probing), not std::unordered_map's per-node
//     allocations.
//
// Limits: the array spans every price the book has rested at, so prices must
// stay within `max_levels` ticks of each other (default 4M ticks, ample for
// a single instrument). Order ids must be unique among resting orders, and
// the id 2^64-1 is reserved.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
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
    static constexpr std::size_t kDefaultMaxLevels = std::size_t(1) << 22;

    explicit OrderBook(std::size_t max_levels = kDefaultMaxLevels);

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

    // Pointer into the pool; valid until the next change to the book.
    const Order* get(OrderId id) const;
    std::size_t size() const { return index_.size(); }
    Qty resting_qty() const { return resting_; }

    // Every trade so far. Turn off for long benchmarks to save memory.
    const std::vector<Trade>& trades() const { return history_; }
    void keep_history(bool on) { keep_history_ = on; }

    std::string to_string(std::size_t levels = 5) const;

private:
    static constexpr std::uint32_t kNil = ~std::uint32_t(0);
    static constexpr std::size_t kSlack = 64;  // extra levels added around a new price

    struct Node {
        Order order;
        std::uint32_t prev = kNil;
        std::uint32_t next = kNil;  // also links the free list
    };

    struct Level {
        std::uint32_t head = kNil;  // oldest order (trades first)
        std::uint32_t tail = kNil;
        Qty qty = 0;
    };

    // OrderId -> pool slot. Open addressing with linear probing; erase uses
    // backward shifting, so there are no tombstones.
    class IdMap {
    public:
        static constexpr OrderId kEmpty = ~OrderId(0);
        IdMap();
        const std::uint32_t* find(OrderId key) const;
        void insert(OrderId key, std::uint32_t value);
        bool erase(OrderId key);
        std::size_t size() const { return size_; }

    private:
        std::size_t home(OrderId key) const;
        void rehash(std::size_t capacity);
        std::vector<OrderId> keys_;
        std::vector<std::uint32_t> values_;
        std::size_t mask_ = 0;
        std::size_t size_ = 0;
    };

    void validate(const Order& o) const;
    void check_range(Price p) const;
    void match(Order& o, std::vector<Trade>& out);
    void fill_level(Level& level, Order& o, Price price, std::vector<Trade>& out);
    void rest(const Order& o);

    Level& level_at(Price p);  // grows the array if needed
    void resize_levels(std::size_t n);
    void rebuild_bits();
    std::size_t index_of(Price p) const { return std::size_t(p - base_); }
    void set_bit(std::size_t i) { bits_[i >> 6] |= std::uint64_t(1) << (i & 63); }
    void clear_bit(std::size_t i) { bits_[i >> 6] &= ~(std::uint64_t(1) << (i & 63)); }
    std::ptrdiff_t find_prev(std::size_t i) const;  // highest set index < i, or -1
    std::ptrdiff_t find_next(std::size_t i) const;  // lowest set index >= i, or -1
    void level_emptied(Price p, Side side);

    std::uint32_t alloc_node(const Order& o);
    void free_node(std::uint32_t n);
    void unlink(Level& level, std::uint32_t n);

    std::size_t max_levels_;
    Price base_ = 0;
    std::vector<Level> levels_;
    std::vector<std::uint64_t> bits_;
    bool has_bid_ = false;
    bool has_ask_ = false;
    Price best_bid_ = 0;
    Price best_ask_ = 0;

    std::vector<Node> pool_;
    std::uint32_t free_ = kNil;
    IdMap index_;
    Qty resting_ = 0;

    std::vector<Trade> history_;
    bool keep_history_ = true;
};

}  // namespace ob
