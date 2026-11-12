#include "reference_book.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace ref {

std::vector<Trade> OrderBook::add(Order order) {
    std::vector<Trade> out;
    add(order, out);
    return out;
}

void OrderBook::add(Order order, std::vector<Trade>& out) {
    validate(order);
    seen_.insert(order.id);
    order.remaining = order.qty;
    if (order.type == OrderType::Market) order.price = 0;
    match(order, out);
    if (order.type == OrderType::Limit && order.remaining > 0) rest(order);
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Location loc = found->second;
    index_.erase(found);
    auto erase_from = [&](auto& book) {
        auto lvl = book.find(loc.price);
        lvl->second.erase(loc.it);
        if (lvl->second.empty()) book.erase(lvl);
    };
    if (loc.side == Side::Buy) erase_from(bids_);
    else erase_from(asks_);
    return true;
}

std::vector<Trade> OrderBook::modify(OrderId id, std::optional<Qty> qty, std::optional<Price> price) {
    std::vector<Trade> out;
    modify(id, qty, price, out);
    return out;
}

void OrderBook::modify(OrderId id, std::optional<Qty> qty, std::optional<Price> price, std::vector<Trade>& out) {
    auto found = index_.find(id);
    if (found == index_.end()) throw OrderBookError("order " + std::to_string(id) + " is not resting");
    Order& current = *found->second.it;
    const Qty new_qty = qty.value_or(current.remaining);
    const Price new_price = price.value_or(current.price);
    if (new_qty <= 0) throw OrderBookError("quantity must be positive; use cancel() to remove");
    if (new_price <= 0) throw OrderBookError("price must be positive");

    if (new_price == current.price && new_qty <= current.remaining) {
        current.remaining = new_qty;  // keeps its place in line
        return;
    }

    const Side side = current.side;
    cancel(id);
    Order replacement = Order::limit(id, side, new_qty, new_price);
    match(replacement, out);
    if (replacement.remaining > 0) rest(replacement);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    auto b = best_bid(), a = best_ask();
    if (!b || !a) return std::nullopt;
    return *a - *b;
}

std::vector<std::pair<Price, Qty>> OrderBook::depth(Side side, std::size_t levels) const {
    std::vector<std::pair<Price, Qty>> out;
    auto collect = [&](const auto& book) {
        for (const auto& [price, level] : book) {
            if (out.size() == levels) break;
            Qty total = 0;
            for (const Order& o : level) total += o.remaining;
            out.emplace_back(price, total);
        }
    };
    if (side == Side::Buy) collect(bids_);
    else collect(asks_);
    return out;
}

const Order* OrderBook::get(OrderId id) const {
    auto found = index_.find(id);
    return found == index_.end() ? nullptr : &*found->second.it;
}

Qty OrderBook::resting_qty() const {
    Qty total = 0;
    for (const auto& [id, loc] : index_) total += loc.it->remaining;
    return total;
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

// ---------- internals ----------

void OrderBook::validate(const Order& o) const {
    if (seen_.count(o.id)) throw OrderBookError("duplicate order id " + std::to_string(o.id));
    if (o.qty <= 0) throw OrderBookError("quantity must be positive");
    if (o.type != OrderType::Market && o.price <= 0)
        throw OrderBookError("limit and IOC orders need a positive price");
}

bool OrderBook::crosses(const Order& o, Price best) {
    if (o.type == OrderType::Market) return true;
    return o.side == Side::Buy ? best <= o.price : best >= o.price;
}

void OrderBook::match(Order& o, std::vector<Trade>& out) {
    const std::size_t first = out.size();
    if (o.side == Side::Buy) match_against(asks_, o, out);
    else match_against(bids_, o, out);
    if (keep_history_) history_.insert(history_.end(), out.begin() + first, out.end());
}

template <class Book>
void OrderBook::match_against(Book& book, Order& o, std::vector<Trade>& out) {
    while (o.remaining > 0 && !book.empty()) {
        auto lvl = book.begin();
        const Price best = lvl->first;
        if (!crosses(o, best)) break;
        Level& queue = lvl->second;
        while (o.remaining > 0 && !queue.empty()) {
            Order& resting = queue.front();
            const Qty fill = std::min(o.remaining, resting.remaining);
            o.remaining -= fill;
            resting.remaining -= fill;
            if (o.side == Side::Buy) out.push_back({o.id, resting.id, best, fill, o.side});
            else out.push_back({resting.id, o.id, best, fill, o.side});
            if (resting.remaining == 0) {
                index_.erase(resting.id);
                queue.pop_front();
            }
        }
        if (queue.empty()) book.erase(lvl);
    }
}

void OrderBook::rest(const Order& o) {
    auto place = [&](auto& book) {
        Level& level = book[o.price];
        level.push_back(o);
        index_[o.id] = Location{o.side, o.price, std::prev(level.end())};
    };
    if (o.side == Side::Buy) place(bids_);
    else place(asks_);
}

}  // namespace ref
