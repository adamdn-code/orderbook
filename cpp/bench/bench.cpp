// Benchmarks:
//   1. Single book, one thread: throughput and per-order latency percentiles.
//   2. Engine: N client threads (one session each), 3 symbols. Throughput is
//      measured until every order has been matched (drain()).
//   3. Engine round-trip latency: one client sends an order, waits for its
//      result callback, repeats. Time from submit() to callback.
#include "engine.hpp"
#include "orderbook.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

using namespace ob;
using Clock = std::chrono::steady_clock;

static Order random_order(std::mt19937_64& rng, OrderId id) {
    std::uniform_int_distribution<int> kind(0, 9), px(9'900, 10'100), q(1, 100), side(0, 1);
    const Side s = side(rng) ? Side::Buy : Side::Sell;
    const int k = kind(rng);
    if (k < 8) return Order::limit(id, s, q(rng), px(rng));
    if (k == 8) return Order::market(id, s, q(rng));
    return Order::ioc(id, s, q(rng), px(rng));
}

template <class V>
static auto pct(V& sorted, double p) {
    return sorted[std::size_t(p * double(sorted.size() - 1))];
}

static void bench_single_book(std::size_t n) {
    std::mt19937_64 rng(42);
    std::vector<Order> orders;
    orders.reserve(n);
    for (std::size_t i = 0; i < n; ++i) orders.push_back(random_order(rng, i + 1));

    OrderBook book;
    book.keep_history(false);
    std::vector<Trade> out;
    out.reserve(1024);
    std::vector<std::uint32_t> lat_ns(n);
    std::size_t trades = 0;

    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const auto t0 = Clock::now();
        out.clear();
        book.add(orders[i], out);
        lat_ns[i] = std::uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        trades += out.size();
    }
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();

    std::sort(lat_ns.begin(), lat_ns.end());
    std::printf("Single book, 1 thread\n");
    std::printf("  %zu orders, %zu trades in %.3fs  ->  %.2f M orders/s\n", n, trades, secs, n / secs / 1e6);
    std::printf("  latency per order: p50 %u ns, p99 %u ns, p99.9 %u ns\n\n", pct(lat_ns, 0.50), pct(lat_ns, 0.99),
                pct(lat_ns, 0.999));
}

static void bench_engine(int threads, int per_thread) {
    const std::vector<std::string> symbols{"AAPL", "MSFT", "NVDA"};
    struct alignas(kCacheLine) Count {
        std::size_t trades = 0;
    };
    std::vector<Count> counts(symbols.size());  // each written only by its symbol's matcher
    MatchingEngine eng(symbols, [&](SymbolId s, const Trade&) { ++counts[s].trades; });
    eng.keep_history(false);

    // Pre-generate orders so the clients measure submitting, not random numbers.
    std::vector<std::vector<std::pair<SymbolId, Order>>> work(threads);
    for (int c = 0; c < threads; ++c) {
        std::mt19937_64 rng(c);
        std::uniform_int_distribution<int> pick(0, int(symbols.size()) - 1);
        work[c].reserve(per_thread);
        for (int i = 0; i < per_thread; ++i)
            work[c].emplace_back(SymbolId(pick(rng)), random_order(rng, OrderId(c) * 100'000'000 + i + 1));
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> clients;
    for (int c = 0; c < threads; ++c) {
        clients.emplace_back([&, c] {
            Session& session = eng.connect();
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (const auto& [sym, order] : work[c]) session.submit(sym, order);
        });
    }
    while (ready.load() < threads) std::this_thread::yield();

    const auto start = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& t : clients) t.join();
    eng.drain();
    const double secs = std::chrono::duration<double>(Clock::now() - start).count();

    std::size_t trades = 0;
    for (const Count& c : counts) trades += c.trades;
    const std::size_t total = std::size_t(threads) * per_thread;
    std::printf("Engine, %d client threads, %zu symbols\n", threads, symbols.size());
    std::printf("  %zu orders, %zu trades in %.3fs  ->  %.2f M orders/s\n\n", total, trades, secs,
                total / secs / 1e6);
}

static void bench_round_trip(int n) {
    std::atomic<std::uint64_t> done{0};
    MatchingEngine eng({"AAPL"}, {}, [&](SymbolId, const Result&) { done.fetch_add(1, std::memory_order_release); });
    eng.keep_history(false);
    Session& session = eng.connect();

    std::mt19937_64 rng(7);
    std::vector<std::uint32_t> lat_ns;
    lat_ns.reserve(n);
    for (int i = 0; i < n; ++i) {
        const Order o = random_order(rng, OrderId(i) + 1);
        const auto t0 = Clock::now();
        session.submit(SymbolId(0), o);
        while (done.load(std::memory_order_acquire) <= std::uint64_t(i)) {
        }
        lat_ns.push_back(std::uint32_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count()));
    }
    std::sort(lat_ns.begin(), lat_ns.end());
    std::printf("Engine round trip (submit -> result callback), 1 client\n");
    std::printf("  p50 %u ns, p99 %u ns, p99.9 %u ns  (%d orders, %zu idle naps)\n\n", pct(lat_ns, 0.50),
                pct(lat_ns, 0.99), pct(lat_ns, 0.999), n, eng.idle_naps());
}

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoul(argv[1]) : 2'000'000;
    std::printf("hardware threads: %u\n\n", std::thread::hardware_concurrency());
    bench_single_book(n);
    bench_engine(8, 5'000);    // same size as the Python demo (40k orders)
    bench_engine(8, 250'000);  // 2M orders
    bench_engine(2, 1'000'000);
    bench_round_trip(200'000);
    return 0;
}
