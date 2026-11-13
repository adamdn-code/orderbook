// Order book, ring buffer and engine tests, plus a randomized comparison
// against the original std::map book and a multithreaded stress test.
#include "engine.hpp"
#include "orderbook.hpp"
#include "reference_book.hpp"

#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

using namespace ob;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

#define CHECK_THROWS(expr, Ex)                                                         \
    do {                                                                               \
        bool threw = false;                                                            \
        try { expr; } catch (const Ex&) { threw = true; }                              \
        CHECK(threw && #expr " should throw " #Ex);                                    \
    } while (0)

constexpr Side B = Side::Buy;
constexpr Side S = Side::Sell;

// ---------- OrderBook ----------

static void non_crossing_orders_rest() {
    OrderBook b;
    b.add(Order::limit(1, B, 10, 99));
    b.add(Order::limit(2, S, 10, 101));
    CHECK(b.best_bid() == 99);
    CHECK(b.best_ask() == 101);
    CHECK(b.spread() == 2);
    CHECK(b.size() == 2);
}

static void limit_cross_trades_at_resting_price() {
    OrderBook b;
    b.add(Order::limit(1, S, 10, 100));
    auto t = b.add(Order::limit(2, B, 10, 105));
    CHECK(t.size() == 1);
    CHECK(t[0].price == 100 && t[0].qty == 10);
    CHECK(b.size() == 0);
}

static void time_priority_at_same_price() {
    OrderBook b;
    b.add(Order::limit(1, S, 5, 100));
    b.add(Order::limit(2, S, 5, 100));
    auto t = b.add(Order::limit(3, B, 5, 100));
    CHECK(t[0].sell_id == 1);
    CHECK(b.get(2) != nullptr);
}

static void price_priority_and_walking_the_book() {
    OrderBook b;
    b.add(Order::limit(1, S, 5, 102));
    b.add(Order::limit(2, S, 5, 101));
    auto t = b.add(Order::limit(3, B, 8, 102));
    CHECK(t.size() == 2);
    CHECK(t[0].sell_id == 2 && t[0].price == 101 && t[0].qty == 5);
    CHECK(t[1].sell_id == 1 && t[1].price == 102 && t[1].qty == 3);
    CHECK(b.get(1)->remaining == 2);
}

static void partial_limit_rests_remainder() {
    OrderBook b;
    b.add(Order::limit(1, S, 4, 100));
    b.add(Order::limit(2, B, 10, 100));
    CHECK(b.get(2)->remaining == 6);
    CHECK(b.best_bid() == 100);
    CHECK(!b.best_ask());
}

static void market_order_drops_unfilled() {
    OrderBook b;
    b.add(Order::limit(1, S, 3, 100));
    auto t = b.add(Order::market(2, B, 10));
    CHECK(t.size() == 1 && t[0].qty == 3);
    CHECK(b.get(2) == nullptr);
    CHECK(b.size() == 0);
}

static void ioc_respects_price_and_never_rests() {
    OrderBook b;
    b.add(Order::limit(1, S, 5, 100));
    b.add(Order::limit(2, S, 5, 103));
    auto t = b.add(Order::ioc(3, B, 10, 101));
    Qty filled = 0;
    for (auto& x : t) filled += x.qty;
    CHECK(filled == 5);
    CHECK(b.get(3) == nullptr);
    CHECK(b.best_ask() == 103);
}

static void cancel_works() {
    OrderBook b;
    b.add(Order::limit(1, B, 5, 100));
    b.add(Order::limit(2, B, 5, 99));
    CHECK(b.cancel(1));
    CHECK(!b.cancel(1));
    CHECK(b.best_bid() == 99);
    CHECK(b.cancel(2));
    CHECK(!b.best_bid());
}

static void modify_shrink_keeps_priority() {
    OrderBook b;
    b.add(Order::limit(1, S, 10, 100));
    b.add(Order::limit(2, S, 10, 100));
    b.modify(1, 4, std::nullopt);
    auto t = b.add(Order::limit(3, B, 4, 100));
    CHECK(t[0].sell_id == 1);
}

static void modify_grow_loses_priority() {
    OrderBook b;
    b.add(Order::limit(1, S, 10, 100));
    b.add(Order::limit(2, S, 10, 100));
    b.modify(1, 20, std::nullopt);
    auto t = b.add(Order::limit(3, B, 5, 100));
    CHECK(t[0].sell_id == 2);
}

static void modify_price_can_trade() {
    OrderBook b;
    b.add(Order::limit(1, S, 5, 100));
    b.add(Order::limit(2, B, 5, 95));
    auto t = b.modify(2, std::nullopt, 100);
    CHECK(t.size() == 1 && t[0].buy_id == 2 && t[0].qty == 5);
}

static void level_lists_queue_in_order() {
    OrderBook b;
    b.add(Order::limit(1, B, 5, 100));
    b.add(Order::limit(2, B, 7, 100));
    b.add(Order::limit(3, B, 1, 99));
    b.add(Order::limit(4, S, 2, 101));
    b.modify(1, 3, std::nullopt);  // shrink keeps place
    using L = std::vector<std::pair<OrderId, Qty>>;
    CHECK(b.level(B, 100) == L({{1, 3}, {2, 7}}));
    CHECK(b.level(B, 99) == L({{3, 1}}));
    CHECK(b.level(S, 101) == L({{4, 2}}));
    CHECK(b.level(S, 100).empty());   // bids live there, not asks
    CHECK(b.level(B, 101).empty());
    CHECK(b.level(B, 5'000).empty()); // outside the array
    b.modify(1, 10, std::nullopt);    // grow: back of the queue
    CHECK(b.level(B, 100) == L({{2, 7}, {1, 10}}));
}

static void depth_aggregates_levels() {
    OrderBook b;
    b.add(Order::limit(1, B, 5, 100));
    b.add(Order::limit(2, B, 7, 100));
    b.add(Order::limit(3, B, 1, 98));
    auto d = b.depth(B);
    CHECK(d.size() == 2);
    CHECK(d[0] == std::make_pair(Price{100}, Qty{12}));
    CHECK(d[1] == std::make_pair(Price{98}, Qty{1}));
}

static void validation() {
    OrderBook b;
    CHECK_THROWS(b.add(Order::limit(1, B, 0, 100)), OrderBookError);
    CHECK_THROWS(b.add(Order::limit(2, B, 5, 0)), OrderBookError);
    b.add(Order::limit(4, B, 5, 100));
    CHECK_THROWS(b.add(Order::limit(4, S, 5, 110)), OrderBookError);
    CHECK_THROWS(b.modify(99, 1, std::nullopt), OrderBookError);
    CHECK_THROWS(b.modify(4, 0, std::nullopt), OrderBookError);
}

// ---------- flat-array book internals ----------

static void reserved_id_and_resting_duplicates() {
    OrderBook b;
    CHECK_THROWS(b.add(Order::limit(~OrderId(0), B, 1, 100)), OrderBookError);
    b.add(Order::limit(1, B, 5, 100));
    CHECK_THROWS(b.add(Order::limit(1, B, 5, 101)), OrderBookError);  // still resting
    b.cancel(1);
    b.add(Order::limit(1, B, 5, 101));  // id is free again once the order is gone
    CHECK(b.best_bid() == 101);
}

static void price_range_limit_rejects_without_side_effects() {
    OrderBook b(1024);
    b.add(Order::limit(1, S, 5, 10'000));
    CHECK_THROWS(b.add(Order::limit(2, B, 5, 10'000 + 5'000)), OrderBookError);   // would rest too far away
    CHECK(b.best_ask() == 10'000 && b.size() == 1);                                // and nothing traded
    CHECK_THROWS(b.add(Order::limit(3, S, 5, 10'000 - 5'000)), OrderBookError);
    // Market and IOC orders never rest, so they aren't range-limited.
    CHECK(b.add(Order::ioc(4, B, 2, 1'000'000)).size() == 1);
    CHECK_THROWS(b.modify(1, std::nullopt, 50'000), OrderBookError);
    CHECK(b.get(1) && b.get(1)->remaining == 3);  // failed modify left it alone
}

static void levels_grow_in_both_directions() {
    OrderBook b;
    b.add(Order::limit(1, B, 1, 50'000));
    b.add(Order::limit(2, B, 1, 49'000));  // far below: array grows downward
    b.add(Order::limit(3, S, 1, 52'000));  // far above: array grows upward
    b.add(Order::limit(4, B, 1, 1'000));
    CHECK(b.best_bid() == 50'000 && b.best_ask() == 52'000);
    auto d = b.depth(B, 10);
    CHECK(d.size() == 3 && d[1].first == 49'000 && d[2].first == 1'000);
}

static void best_price_found_across_bitmap_words() {
    OrderBook b;
    // Levels spread over many 64-bit bitmap words.
    const Price bids[] = {10'000, 9'990, 9'800, 9'000, 8'999, 7'000};
    OrderId id = 1;
    for (Price p : bids) b.add(Order::limit(id++, B, 1, p));
    for (Price expect : bids) {
        CHECK(b.best_bid() == expect);
        b.add(Order::market(id++, S, 1));  // eats the best level
    }
    CHECK(!b.best_bid() && b.size() == 0);
    // Same going up on the ask side, emptied by cancels from the middle out.
    b.add(Order::limit(100, S, 1, 500));
    b.add(Order::limit(101, S, 1, 700));
    b.add(Order::limit(102, S, 1, 900));
    b.cancel(101);
    CHECK(b.best_ask() == 500);
    b.cancel(100);
    CHECK(b.best_ask() == 900);
}

static void pool_slots_are_reused() {
    OrderBook b;
    for (int round = 0; round < 5; ++round) {
        for (OrderId i = 1; i <= 1000; ++i) b.add(Order::limit(round * 10'000 + i, B, 1, 100 + Price(i % 10)));
        CHECK(b.size() == 1000 && b.resting_qty() == 1000);
        b.add(Order::market(round * 10'000 + 5000, S, 1000));
        CHECK(b.size() == 0 && b.resting_qty() == 0 && !b.best_bid());
    }
}

// Replays one random stream of adds, cancels and modifies through the new book
// and the original std::map book; every trade and every book state must match.
static void matches_reference_book() {
    OrderBook fast;
    ref::OrderBook slow;
    std::mt19937_64 rng(2026);
    std::uniform_int_distribution<int> op(0, 99), q(1, 50), side(0, 1), near(-30, 30);
    std::uniform_int_distribution<Price> jump(5'000, 15'000);
    std::vector<OrderId> live;  // ids we've sent (may since have filled)
    OrderId next_id = 1;
    Price mid = 10'000;
    bool same = true;
    int mismatches = 0;

    for (int step = 0; step < 300'000 && mismatches < 5; ++step) {
        const int k = op(rng);
        std::vector<Trade> a, b;
        bool threw_a = false, threw_b = false;
        if (k < 1) mid = jump(rng);  // occasionally move the market far away
        if (k < 60) {
            const Side sd = side(rng) ? B : S;
            const int t = op(rng);
            Order o = t < 80 ? Order::limit(next_id, sd, q(rng), std::max<Price>(1, mid + near(rng)))
                    : t < 90 ? Order::market(next_id, sd, q(rng))
                             : Order::ioc(next_id, sd, q(rng), std::max<Price>(1, mid + near(rng)));
            live.push_back(next_id++);
            fast.add(o, a);
            slow.add(o, b);
        } else if (k < 85 && !live.empty()) {
            const OrderId id = live[std::size_t(rng() % live.size())];
            same &= fast.cancel(id) == slow.cancel(id);
        } else if (!live.empty()) {
            const OrderId id = live[std::size_t(rng() % live.size())];
            const bool change_price = op(rng) < 50;
            std::optional<Qty> nq = change_price ? std::nullopt : std::optional<Qty>(q(rng));
            std::optional<Price> np = change_price ? std::optional<Price>(std::max<Price>(1, mid + near(rng))) : std::nullopt;
            try { fast.modify(id, nq, np, a); } catch (const OrderBookError&) { threw_a = true; }
            try { slow.modify(id, nq, np, b); } catch (const OrderBookError&) { threw_b = true; }
        }
        bool ok = threw_a == threw_b && a.size() == b.size();
        for (std::size_t i = 0; ok && i < a.size(); ++i)
            ok = a[i].buy_id == b[i].buy_id && a[i].sell_id == b[i].sell_id && a[i].price == b[i].price &&
                 a[i].qty == b[i].qty && a[i].aggressor == b[i].aggressor;
        ok = ok && fast.best_bid() == slow.best_bid() && fast.best_ask() == slow.best_ask() &&
             fast.size() == slow.size();
        if (step % 1000 == 0) {
            ok = ok && fast.resting_qty() == slow.resting_qty() && fast.depth(B, 10) == slow.depth(B, 10) &&
                 fast.depth(S, 10) == slow.depth(S, 10);
            const auto fo = fast.orders(), so = slow.orders();
            ok = ok && fo.size() == so.size();
            for (std::size_t i = 0; ok && i < fo.size(); ++i)
                ok = fo[i].id == so[i].id && fo[i].remaining == so[i].remaining && fo[i].price == so[i].price;
        }
        if (!ok) {
            ++mismatches;
            std::printf("  mismatch at step %d\n", step);
        }
        same &= ok;
    }
    CHECK(same);
}

// ---------- SpscRing ----------

static void ring_full_empty_and_wraparound() {
    SpscRing<int, 8> r;
    CHECK(r.empty() && r.front() == nullptr);
    for (int i = 0; i < 8; ++i) CHECK(r.try_push(i));
    CHECK(!r.try_push(99));  // full
    for (int round = 0; round < 3; ++round) {  // wrap around a few times
        for (int i = 0; i < 8; ++i) {
            CHECK(r.front() && *r.front() == i);
            r.pop();
            CHECK(r.try_push(i));
        }
    }
}

static void ring_two_threads_keep_order() {
    static SpscRing<std::uint64_t, 1024> r;
    constexpr std::uint64_t kN = 2'000'000;
    std::thread producer([] {
        for (std::uint64_t i = 0; i < kN; ++i)
            while (!r.try_push(i)) std::this_thread::yield();
    });
    bool in_order = true;
    for (std::uint64_t expect = 0; expect < kN;) {
        if (std::uint64_t* v = r.front()) {
            in_order &= *v == expect;
            r.pop();
            ++expect;
        }
    }
    producer.join();
    CHECK(in_order);
    CHECK(r.empty());
}

// ---------- MatchingEngine ----------

// Collects callbacks per symbol. Each symbol's vectors are written only by
// that symbol's matching thread; read them after engine.drain().
struct Recorder {
    explicit Recorder(std::size_t symbols) : trades(symbols), results(symbols) {}
    MatchingEngine::TradeCallback on_trade() {
        return [this](SymbolId s, const Trade& t) { trades[s].push_back(t); };
    }
    MatchingEngine::ResultCallback on_result() {
        return [this](SymbolId s, const Result& r) { results[s].push_back(r); };
    }
    std::vector<std::vector<Trade>> trades;
    std::vector<std::vector<Result>> results;
};

static void engine_submit_reports_trades() {
    Recorder rec(1);
    MatchingEngine eng({"X"}, rec.on_trade(), rec.on_result());
    Session& s = eng.connect();
    s.submit("X", Order::limit(1, S, 10, 100));
    s.submit("X", Order::limit(2, B, 4, 100));
    eng.drain();
    CHECK(rec.trades[0].size() == 1);
    CHECK(rec.trades[0][0].buy_id == 2 && rec.trades[0][0].sell_id == 1 && rec.trades[0][0].qty == 4);
    CHECK(rec.results[0].size() == 2);
    CHECK(rec.results[0][0].ok && rec.results[0][0].rested == 10);
    CHECK(rec.results[0][1].filled == 4 && rec.results[0][1].rested == 0);
    CHECK(eng.best_ask("X") == 100);
}

static void engine_keeps_order_within_session() {
    Recorder rec(1);
    MatchingEngine eng({"X"}, rec.on_trade());
    Session& s = eng.connect();
    for (OrderId i = 1; i <= 50; ++i) s.submit("X", Order::limit(i, S, 1, 100));
    s.submit("X", Order::limit(999, B, 50, 100));
    eng.drain();
    CHECK(rec.trades[0].size() == 50);
    bool in_order = true;
    for (std::size_t i = 0; i < rec.trades[0].size(); ++i) in_order &= rec.trades[0][i].sell_id == i + 1;
    CHECK(in_order);
}

static void engine_cancel_and_modify() {
    Recorder rec(1);
    MatchingEngine eng({"X"}, rec.on_trade(), rec.on_result());
    Session& s = eng.connect();
    s.submit("X", Order::limit(1, B, 5, 99));
    s.submit("X", Order::limit(2, B, 5, 98));
    s.cancel("X", 1);
    s.cancel("X", 1);
    eng.drain();
    CHECK(rec.results[0][2].kind == Result::Kind::Cancel && rec.results[0][2].ok);
    CHECK(!rec.results[0][3].ok);
    CHECK(eng.best_bid("X") == 98);
    s.submit("X", Order::limit(3, S, 5, 100));
    s.modify("X", 2, std::nullopt, 100);
    eng.drain();
    const Result& m = rec.results[0].back();
    CHECK(m.kind == Result::Kind::Modify && m.ok && m.filled == 5 && m.rested == 0);
}

static void engine_rejects_come_back_as_results() {
    Recorder rec(1);
    MatchingEngine eng({"X"}, {}, rec.on_result());
    Session& s = eng.connect();
    s.submit("X", Order::limit(1, B, 0, 100));        // bad quantity
    s.modify("X", 42, 5, std::nullopt);               // unknown order
    s.submit("X", Order::limit(2, B, 1, 100));        // matcher survived
    eng.drain();
    CHECK(rec.results[0].size() == 3);
    CHECK(!rec.results[0][0].ok && !rec.results[0][1].ok && rec.results[0][2].ok);
}

static void engine_unknown_symbol() {
    MatchingEngine eng({"X"});
    Session& s = eng.connect();
    CHECK_THROWS(s.submit("Y", Order::limit(1, B, 1, 100)), std::out_of_range);
    CHECK_THROWS(s.submit(SymbolId(7), Order::limit(1, B, 1, 100)), std::out_of_range);
}

static void engine_stop_drains_and_rejects_new_work() {
    MatchingEngine eng({"X"});
    Session& s = eng.connect();
    for (OrderId i = 1; i <= 1000; ++i) s.submit("X", Order::limit(i, B, 1, 100));
    eng.stop();
    CHECK(eng.resting_qty("X") == 1000);
    CHECK_THROWS(s.submit("X", Order::limit(5000, B, 1, 100)), EngineStopped);
    CHECK_THROWS(eng.connect(), EngineStopped);
}

// Far more orders than the ring holds, from one session, without waiting:
// exercises wraparound and the full-ring backpressure path.
static void engine_backpressure() {
    Recorder rec(1);
    MatchingEngine eng({"X"}, {}, rec.on_result());
    Session& s = eng.connect();
    constexpr OrderId kN = 50 * Session::kRingSize;
    for (OrderId i = 1; i <= kN; ++i) s.submit("X", Order::limit(i, B, 1, 100 + Price(i % 7)));
    eng.drain();
    CHECK(rec.results[0].size() == kN);
    CHECK(eng.resting_qty("X") == Qty(kN));
}

static void engine_bad_callback_does_not_kill_matcher() {
    Recorder rec(1);
    MatchingEngine eng({"X"}, [](SymbolId, const Trade&) { throw std::runtime_error("bug"); }, rec.on_result());
    Session& s = eng.connect();
    s.submit("X", Order::limit(1, S, 1, 100));
    s.submit("X", Order::limit(2, B, 1, 100));
    s.submit("X", Order::limit(3, B, 1, 90));
    eng.drain();
    CHECK(rec.results[0].size() == 3);
    CHECK(eng.callback_errors("X") == 1);
}

// Many threads hammer several symbols; nothing may be lost and no book may cross.
static void engine_stress() {
    const std::vector<std::string> symbols{"A", "B", "C"};
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50'000;

    struct alignas(kCacheLine) Stats {  // one per symbol, written only by its matcher
        Qty traded = 0, dropped = 0;
        std::size_t trades = 0, results = 0, rejects = 0;
    };
    std::vector<Stats> stats(symbols.size());
    MatchingEngine eng(
        symbols,
        [&](SymbolId s, const Trade& t) {
            stats[s].traded += t.qty;
            ++stats[s].trades;
        },
        [&](SymbolId s, const Result& r) {
            ++stats[s].results;
            stats[s].dropped += r.dropped;
            if (!r.ok) ++stats[s].rejects;
        });

    std::mutex mtx;
    std::vector<Qty> submitted(symbols.size(), 0);
    std::atomic<bool> crossed{false};

    auto client = [&](int idx) {
        Session& session = eng.connect();
        std::mt19937_64 rng(idx);
        std::uniform_int_distribution<int> pick(0, 2), kind(0, 9), px(95, 105), q(1, 20), side(0, 1);
        std::vector<Qty> local(symbols.size(), 0);
        for (int i = 0; i < kPerThread; ++i) {
            const SymbolId sym = SymbolId(pick(rng));
            const OrderId id = OrderId(idx) * 1'000'000 + i;
            const Side sd = side(rng) ? B : S;
            const Qty qty = q(rng);
            const int k = kind(rng);
            Order o = k < 6 ? Order::limit(id, sd, qty, px(rng))
                    : k < 8 ? Order::market(id, sd, qty)
                            : Order::ioc(id, sd, qty, px(rng));
            session.submit(sym, o);
            local[sym] += qty;
            if (i % 2000 == 0) {
                auto [bid, ask] = eng.top_of_book(symbols[sym]);
                if (bid && ask && *bid >= *ask) crossed = true;
            }
        }
        std::lock_guard lock(mtx);
        for (std::size_t s = 0; s < symbols.size(); ++s) submitted[s] += local[s];
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(client, i);
    for (auto& t : threads) t.join();
    eng.drain();

    CHECK(!crossed);
    std::size_t total_results = 0;
    for (std::size_t s = 0; s < symbols.size(); ++s) {
        const Stats& st = stats[s];
        total_results += st.results;
        CHECK(st.rejects == 0);
        // every unit submitted is resting, traded (counted once per side), or dropped
        CHECK(submitted[s] == eng.resting_qty(symbols[s]) + 2 * st.traded + st.dropped);
        CHECK(st.trades == eng.trades(symbols[s]).size());
        auto [bid, ask] = eng.top_of_book(symbols[s]);
        CHECK(!(bid && ask) || *bid < *ask);
    }
    CHECK(total_results == std::size_t(kThreads) * kPerThread);
}

int main() {
    const std::pair<const char*, void (*)()> tests[] = {
        {"non_crossing_orders_rest", non_crossing_orders_rest},
        {"limit_cross_trades_at_resting_price", limit_cross_trades_at_resting_price},
        {"time_priority_at_same_price", time_priority_at_same_price},
        {"price_priority_and_walking_the_book", price_priority_and_walking_the_book},
        {"partial_limit_rests_remainder", partial_limit_rests_remainder},
        {"market_order_drops_unfilled", market_order_drops_unfilled},
        {"ioc_respects_price_and_never_rests", ioc_respects_price_and_never_rests},
        {"cancel_works", cancel_works},
        {"modify_shrink_keeps_priority", modify_shrink_keeps_priority},
        {"modify_grow_loses_priority", modify_grow_loses_priority},
        {"modify_price_can_trade", modify_price_can_trade},
        {"depth_aggregates_levels", depth_aggregates_levels},
        {"level_lists_queue_in_order", level_lists_queue_in_order},
        {"validation", validation},
        {"reserved_id_and_resting_duplicates", reserved_id_and_resting_duplicates},
        {"price_range_limit_rejects_without_side_effects", price_range_limit_rejects_without_side_effects},
        {"levels_grow_in_both_directions", levels_grow_in_both_directions},
        {"best_price_found_across_bitmap_words", best_price_found_across_bitmap_words},
        {"pool_slots_are_reused", pool_slots_are_reused},
        {"matches_reference_book", matches_reference_book},
        {"ring_full_empty_and_wraparound", ring_full_empty_and_wraparound},
        {"ring_two_threads_keep_order", ring_two_threads_keep_order},
        {"engine_submit_reports_trades", engine_submit_reports_trades},
        {"engine_keeps_order_within_session", engine_keeps_order_within_session},
        {"engine_cancel_and_modify", engine_cancel_and_modify},
        {"engine_rejects_come_back_as_results", engine_rejects_come_back_as_results},
        {"engine_unknown_symbol", engine_unknown_symbol},
        {"engine_stop_drains_and_rejects_new_work", engine_stop_drains_and_rejects_new_work},
        {"engine_backpressure", engine_backpressure},
        {"engine_bad_callback_does_not_kill_matcher", engine_bad_callback_does_not_kill_matcher},
        {"engine_stress", engine_stress},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed, %zu tests\n", g_checks, g_failed, std::size(tests));
    return g_failed ? 1 : 0;
}
