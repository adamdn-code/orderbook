// C API over ob::OrderBook for the browser demo (docs/engine.html). Numbers cross as doubles:
// ids, prices (cents) and quantities all stay far below 2^53.
#include "orderbook.hpp"

extern "C" [[noreturn]] __attribute__((import_module("env"), import_name("ob_fail"))) void ob_fail(const char* message);

namespace {
ob::OrderBook* book = nullptr;
std::vector<ob::Trade> trades;
double out[4096];  // results for JS to read: trades (5 per row) or depth / level rows (2 per row)

void write_trades() {
    int k = 0;
    for (const ob::Trade& t : trades) {
        if (k + 5 > 4096) break;
        out[k++] = double(t.buy_id); out[k++] = double(t.sell_id);
        out[k++] = double(t.price); out[k++] = double(t.qty);
        out[k++] = t.aggressor == ob::Side::Buy ? 0 : 1;
    }
}
}  // namespace

#define EXPORT(name) extern "C" __attribute__((export_name(#name)))

EXPORT(ob_out) double* ob_out() { return out; }

EXPORT(ob_reset) void ob_reset() {
    delete book;
    book = new ob::OrderBook(1 << 16);
    book->keep_history(false);
}

// side 0 = buy, 1 = sell; type 0 = limit, 1 = market, 2 = IOC. Returns the number of trades.
EXPORT(ob_add) int ob_add(double id, int side, int type, double qty, double price) {
    const ob::Side s = side == 0 ? ob::Side::Buy : ob::Side::Sell;
    const auto i = ob::OrderId(id);
    const auto q = ob::Qty(qty);
    const auto p = ob::Price(price);
    const ob::Order o = type == 1 ? ob::Order::market(i, s, q) : type == 2 ? ob::Order::ioc(i, s, q, p) : ob::Order::limit(i, s, q, p);
    trades.clear();
    book->add(o, trades);
    write_trades();
    return int(trades.size());
}

EXPORT(ob_cancel) int ob_cancel(double id) { return book->cancel(ob::OrderId(id)) ? 1 : 0; }

// Writes up to n (price, qty) rows for one side, best first. Returns the row count.
EXPORT(ob_depth) int ob_depth(int side, int n) {
    const auto d = book->depth(side == 0 ? ob::Side::Buy : ob::Side::Sell, std::size_t(n));
    int k = 0;
    for (const auto& [p, q] : d) { out[k++] = double(p); out[k++] = double(q); }
    return int(d.size());
}

// Writes the queue at one price, first in line first: (id, remaining) rows. Returns the row count.
EXPORT(ob_level) int ob_level(int side, double price) {
    const auto l = book->level(side == 0 ? ob::Side::Buy : ob::Side::Sell, ob::Price(price));
    int k = 0;
    for (const auto& [id, q] : l) { if (k + 2 > 4096) break; out[k++] = double(id); out[k++] = double(q); }
    return k / 2;
}

EXPORT(ob_best_bid) double ob_best_bid() { auto b = book->best_bid(); return b ? double(*b) : -1; }
EXPORT(ob_best_ask) double ob_best_ask() { auto a = book->best_ask(); return a ? double(*a) : -1; }
EXPORT(ob_resting) double ob_resting() { return double(book->size()); }

// Benchmark: n random limit orders and cancels around a mid of `mid` cents, as cpp/bench does.
// Runs on a separate book so the visitor's book is untouched. Returns the number of trades.
EXPORT(ob_bench) double ob_bench(int n, double mid) {
    ob::OrderBook b(1 << 16);
    b.keep_history(false);
    std::vector<ob::Trade> t;
    std::vector<ob::OrderId> live;
    live.reserve(std::size_t(n));
    std::uint64_t x = 0x9E3779B97F4A7C15ULL, trades_seen = 0;
    auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    for (int i = 0; i < n; ++i) {
        const std::uint64_t r = rnd();
        if (r % 10 < 3 && !live.empty()) {  // 30% cancels of a random earlier order
            const std::size_t j = std::size_t(rnd() % live.size());
            b.cancel(live[j]);
            live[j] = live.back();
            live.pop_back();
            continue;
        }
        const ob::Side s = (r >> 8) & 1 ? ob::Side::Buy : ob::Side::Sell;
        const ob::Price p = ob::Price(mid) + ob::Price((r >> 16) % 21) - 10;
        const ob::Qty q = ob::Qty(1 + (r >> 32) % 500);
        t.clear();
        b.add(ob::Order::limit(ob::OrderId(i + 1), s, q, p), t);
        trades_seen += t.size();
        live.push_back(ob::OrderId(i + 1));
    }
    return double(trades_seen);
}
