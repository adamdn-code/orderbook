#include "orderbook.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace ob {

namespace {

inline int count_leading_zeros(std::uint64_t x) { return __builtin_clzll(x); }   // x != 0
inline int count_trailing_zeros(std::uint64_t x) { return __builtin_ctzll(x); }  // x != 0

}  // namespace

// ---------- IdMap ----------

OrderBook::IdMap::IdMap() { rehash(1024); }

std::size_t OrderBook::IdMap::home(OrderId key) const {
    // splitmix64 finalizer: spreads sequential ids evenly over the table
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return std::size_t(key) & mask_;
}

const std::uint32_t* OrderBook::IdMap::find(OrderId key) const {
    for (std::size_t i = home(key);; i = (i + 1) & mask_) {
        if (keys_[i] == key) return &values_[i];
        if (keys_[i] == kEmpty) return nullptr;
    }
}

void OrderBook::IdMap::insert(OrderId key, std::uint32_t value) {
    if ((size_ + 1) * 2 > keys_.size()) rehash(keys_.size() * 2);  // keep load <= 50%
    std::size_t i = home(key);
    while (keys_[i] != kEmpty && keys_[i] != key) i = (i + 1) & mask_;
    if (keys_[i] == kEmpty) ++size_;
    keys_[i] = key;
    values_[i] = value;
}

bool OrderBook::IdMap::erase(OrderId key) {
    std::size_t i = home(key);
    for (;; i = (i + 1) & mask_) {
        if (keys_[i] == kEmpty) return false;
        if (keys_[i] == key) break;
    }
    // Backward-shift deletion: pull later entries of the same probe run into
    // the hole so lookups never stop early at a gap.
    for (std::size_t j = i;;) {
        j = (j + 1) & mask_;
        if (keys_[j] == kEmpty) break;
        const std::size_t h = home(keys_[j]);
        const bool stays = (i <= j) ? (i < h && h <= j) : (i < h || h <= j);
        if (!stays) {
            keys_[i] = keys_[j];
            values_[i] = values_[j];
            i = j;
        }
    }
    keys_[i] = kEmpty;
    --size_;
    return true;
}

void OrderBook::IdMap::rehash(std::size_t capacity) {
    std::vector<OrderId> old_keys(capacity, kEmpty);
    std::vector<std::uint32_t> old_values(capacity, 0);
    old_keys.swap(keys_);
    old_values.swap(values_);
    mask_ = capacity - 1;
    size_ = 0;
    for (std::size_t i = 0; i < old_keys.size(); ++i)
        if (old_keys[i] != kEmpty) insert(old_keys[i], old_values[i]);
}

// ---------- public API ----------

OrderBook::OrderBook(std::size_t max_levels) : max_levels_(std::max<std::size_t>(max_levels, 4 * kSlack)) {}

std::vector<Trade> OrderBook::add(Order order) {
    std::vector<Trade> out;
    add(order, out);
    return out;
}

void OrderBook::add(Order order, std::vector<Trade>& out) {
    validate(order);
    order.remaining = order.qty;
    if (order.type == OrderType::Market) order.price = 0;
    match(order, out);
    if (order.type == OrderType::Limit && order.remaining > 0) rest(order);
}

bool OrderBook::cancel(OrderId id) {
    const std::uint32_t* slot = index_.find(id);
    if (!slot) return false;
    const std::uint32_t n = *slot;
    index_.erase(id);
    const Order o = pool_[n].order;
    Level& level = levels_[index_of(o.price)];
    unlink(level, n);
    level.qty -= o.remaining;
    resting_ -= o.remaining;
    free_node(n);
    if (level.head == kNil) level_emptied(o.price, o.side);
    return true;
}

std::vector<Trade> OrderBook::modify(OrderId id, std::optional<Qty> qty, std::optional<Price> price) {
    std::vector<Trade> out;
    modify(id, qty, price, out);
    return out;
}

void OrderBook::modify(OrderId id, std::optional<Qty> qty, std::optional<Price> price, std::vector<Trade>& out) {
    const std::uint32_t* slot = index_.find(id);
    if (!slot) throw OrderBookError("order " + std::to_string(id) + " is not resting");
    Node& node = pool_[*slot];
    const Qty new_qty = qty.value_or(node.order.remaining);
    const Price new_price = price.value_or(node.order.price);
    if (new_qty <= 0) throw OrderBookError("quantity must be positive; use cancel() to remove");
    if (new_price <= 0) throw OrderBookError("price must be positive");

    if (new_price == node.order.price && new_qty <= node.order.remaining) {
        const Qty shrink = node.order.remaining - new_qty;  // keeps its place in line
        node.order.remaining = new_qty;
        levels_[index_of(new_price)].qty -= shrink;
        resting_ -= shrink;
        return;
    }

    check_range(new_price);  // before touching the book, so a failure changes nothing
    const Side side = node.order.side;
    cancel(id);
    Order replacement = Order::limit(id, side, new_qty, new_price);
    match(replacement, out);
    if (replacement.remaining > 0) rest(replacement);
}

std::optional<Price> OrderBook::best_bid() const {
    if (!has_bid_) return std::nullopt;
    return best_bid_;
}

std::optional<Price> OrderBook::best_ask() const {
    if (!has_ask_) return std::nullopt;
    return best_ask_;
}

std::optional<Price> OrderBook::spread() const {
    if (!has_bid_ || !has_ask_) return std::nullopt;
    return best_ask_ - best_bid_;
}

std::vector<std::pair<Price, Qty>> OrderBook::depth(Side side, std::size_t levels) const {
    std::vector<std::pair<Price, Qty>> out;
    if (side == Side::Buy) {
        if (!has_bid_) return out;
        for (std::ptrdiff_t i = std::ptrdiff_t(index_of(best_bid_)); i >= 0 && out.size() < levels;
             i = find_prev(std::size_t(i)))
            out.emplace_back(base_ + i, levels_[std::size_t(i)].qty);
    } else {
        if (!has_ask_) return out;
        for (std::ptrdiff_t i = std::ptrdiff_t(index_of(best_ask_)); i >= 0 && out.size() < levels;
             i = find_next(std::size_t(i) + 1))
            out.emplace_back(base_ + i, levels_[std::size_t(i)].qty);
    }
    return out;
}

const Order* OrderBook::get(OrderId id) const {
    const std::uint32_t* slot = index_.find(id);
    return slot ? &pool_[*slot].order : nullptr;
}

std::string OrderBook::to_string(std::size_t levels) const {
    std::ostringstream s;
    char line[64];
    s << "      price    qty\n";
    auto asks = depth(Side::Sell, levels);
    std::reverse(asks.begin(), asks.end());
    for (auto [p, q] : asks) {
        std::snprintf(line, sizeof line, "ASK %8lld %6lld\n", (long long)p, (long long)q);
        s << line;
    }
    s << "    ---------------\n";
    for (auto [p, q] : depth(Side::Buy, levels)) {
        std::snprintf(line, sizeof line, "BID %8lld %6lld\n", (long long)p, (long long)q);
        s << line;
    }
    return s.str();
}

// ---------- matching ----------

void OrderBook::validate(const Order& o) const {
    if (o.id == IdMap::kEmpty) throw OrderBookError("order id 2^64-1 is reserved");
    if (index_.find(o.id)) throw OrderBookError("duplicate order id " + std::to_string(o.id));
    if (o.qty <= 0) throw OrderBookError("quantity must be positive");
    if (o.type != OrderType::Market && o.price <= 0)
        throw OrderBookError("limit and IOC orders need a positive price");
    if (o.type == OrderType::Limit) check_range(o.price);
}

void OrderBook::check_range(Price p) const {
    if (levels_.empty()) return;
    const Price lo = std::min(base_, p);
    const Price hi = std::max(base_ + Price(levels_.size()) - 1, p);
    if (std::uint64_t(hi - lo) + 1 > max_levels_)
        throw OrderBookError("price " + std::to_string(p) + " is too far from the book's other prices");
}

void OrderBook::match(Order& o, std::vector<Trade>& out) {
    const std::size_t first = out.size();
    if (o.side == Side::Buy) {
        while (o.remaining > 0 && has_ask_) {
            const Price p = best_ask_;
            if (o.type != OrderType::Market && p > o.price) break;
            Level& level = levels_[index_of(p)];
            fill_level(level, o, p, out);
            if (level.head == kNil) level_emptied(p, Side::Sell);
        }
    } else {
        while (o.remaining > 0 && has_bid_) {
            const Price p = best_bid_;
            if (o.type != OrderType::Market && p < o.price) break;
            Level& level = levels_[index_of(p)];
            fill_level(level, o, p, out);
            if (level.head == kNil) level_emptied(p, Side::Buy);
        }
    }
    if (keep_history_) history_.insert(history_.end(), out.begin() + std::ptrdiff_t(first), out.end());
}

void OrderBook::fill_level(Level& level, Order& o, Price price, std::vector<Trade>& out) {
    while (o.remaining > 0 && level.head != kNil) {
        const std::uint32_t n = level.head;
        Order& resting = pool_[n].order;
        const Qty fill = std::min(o.remaining, resting.remaining);
        o.remaining -= fill;
        resting.remaining -= fill;
        level.qty -= fill;
        resting_ -= fill;
        if (o.side == Side::Buy) out.push_back({o.id, resting.id, price, fill, o.side});
        else out.push_back({resting.id, o.id, price, fill, o.side});
        if (resting.remaining == 0) {
            index_.erase(resting.id);
            unlink(level, n);
            free_node(n);
        }
    }
}

void OrderBook::rest(const Order& o) {
    Level& level = level_at(o.price);
    const std::uint32_t n = alloc_node(o);
    Node& node = pool_[n];
    node.prev = level.tail;
    node.next = kNil;
    if (level.tail != kNil) pool_[level.tail].next = n;
    else level.head = n;
    level.tail = n;
    level.qty += o.remaining;
    resting_ += o.remaining;
    set_bit(index_of(o.price));
    index_.insert(o.id, n);
    if (o.side == Side::Buy) {
        if (!has_bid_ || o.price > best_bid_) best_bid_ = o.price;
        has_bid_ = true;
    } else {
        if (!has_ask_ || o.price < best_ask_) best_ask_ = o.price;
        has_ask_ = true;
    }
}

// ---------- price levels ----------

OrderBook::Level& OrderBook::level_at(Price p) {
    if (levels_.empty()) {
        base_ = p - Price(kSlack);
        resize_levels(2 * kSlack + 1);
    } else if (p < base_) {
        const Price top = base_ + Price(levels_.size()) - 1;
        const Price new_base = std::max(p - Price(kSlack), top - Price(max_levels_) + 1);
        levels_.insert(levels_.begin(), std::size_t(base_ - new_base), Level{});
        base_ = new_base;
        rebuild_bits();
    } else if (p >= base_ + Price(levels_.size())) {
        const std::size_t need = std::size_t(p - base_) + 1;
        std::size_t n = std::min(std::max(levels_.size() * 2, need + kSlack), max_levels_);
        resize_levels(std::max(n, need));
    }
    return levels_[index_of(p)];
}

void OrderBook::resize_levels(std::size_t n) {
    levels_.resize(n);
    bits_.resize((n + 63) / 64, 0);
}

void OrderBook::rebuild_bits() {
    bits_.assign((levels_.size() + 63) / 64, 0);
    for (std::size_t i = 0; i < levels_.size(); ++i)
        if (levels_[i].head != kNil) set_bit(i);
}

std::ptrdiff_t OrderBook::find_prev(std::size_t i) const {
    if (i == 0) return -1;
    const std::size_t pos = std::min(i, levels_.size()) - 1;
    std::size_t w = pos >> 6;
    std::uint64_t word = bits_[w] & (~std::uint64_t(0) >> (63 - (pos & 63)));  // bits 0..pos
    for (;;) {
        if (word) return std::ptrdiff_t((w << 6) + 63 - std::size_t(count_leading_zeros(word)));
        if (w == 0) return -1;
        word = bits_[--w];
    }
}

std::ptrdiff_t OrderBook::find_next(std::size_t i) const {
    if (i >= levels_.size()) return -1;
    std::size_t w = i >> 6;
    std::uint64_t word = bits_[w] & (~std::uint64_t(0) << (i & 63));  // bits i..63
    for (;;) {
        if (word) return std::ptrdiff_t((w << 6) + std::size_t(count_trailing_zeros(word)));
        if (++w == bits_.size()) return -1;
        word = bits_[w];
    }
}

// The book is never crossed, so below the best bid every non-empty level is
// a bid and above the best ask every non-empty level is an ask.
void OrderBook::level_emptied(Price p, Side side) {
    const std::size_t i = index_of(p);
    clear_bit(i);
    if (side == Side::Buy && has_bid_ && p == best_bid_) {
        const std::ptrdiff_t prev = find_prev(i);
        has_bid_ = prev >= 0;
        if (has_bid_) best_bid_ = base_ + prev;
    } else if (side == Side::Sell && has_ask_ && p == best_ask_) {
        const std::ptrdiff_t next = find_next(i + 1);
        has_ask_ = next >= 0;
        if (has_ask_) best_ask_ = base_ + next;
    }
}

// ---------- order pool ----------

std::uint32_t OrderBook::alloc_node(const Order& o) {
    std::uint32_t n;
    if (free_ != kNil) {
        n = free_;
        free_ = pool_[n].next;
    } else {
        if (pool_.size() >= kNil) throw OrderBookError("too many resting orders");
        n = std::uint32_t(pool_.size());
        pool_.emplace_back();
    }
    pool_[n].order = o;
    return n;
}

void OrderBook::free_node(std::uint32_t n) {
    pool_[n].next = free_;
    pool_[n].prev = kNil;
    free_ = n;
}

void OrderBook::unlink(Level& level, std::uint32_t n) {
    Node& node = pool_[n];
    if (node.prev != kNil) pool_[node.prev].next = node.next;
    else level.head = node.next;
    if (node.next != kNil) pool_[node.next].prev = node.prev;
    else level.tail = node.prev;
}

}  // namespace ob
