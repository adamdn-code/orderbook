// Tests for the ITCH reader and replayer, using synthetic ITCH 5.0 files
// written byte for byte to the spec.
#include "itch.hpp"
#include "replay.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>

using namespace itch;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

// ---------- synthetic ITCH writer ----------

struct Writer {
    std::vector<std::uint8_t> bytes;

    void be(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) bytes.push_back(std::uint8_t(v >> (8 * i)));
    }
    void ch(char c) { bytes.push_back(std::uint8_t(c)); }
    void sym(const std::string& s) {
        for (std::size_t i = 0; i < 8; ++i) ch(i < s.size() ? s[i] : ' ');
    }
    void header(char type, std::uint16_t locate, std::uint64_t ts, std::size_t len) {
        be(len, 2);
        ch(type);
        be(locate, 2);
        be(0, 2);  // tracking number
        be(ts, 6);
    }

    void system_event(std::uint64_t ts, char code) {
        header('S', 0, ts, 12);
        ch(code);
    }
    void directory(std::uint16_t locate, const std::string& s) {
        header('R', locate, 0, 39);
        sym(s);
        ch('Q'); ch('N'); be(100, 4); ch('N'); ch('C'); ch('Z'); ch(' ');
        ch('P'); ch('N'); ch(' '); ch('1'); ch('N'); be(0, 4); ch('N');
    }
    void add(std::uint16_t locate, const std::string& s, std::uint64_t ts, std::uint64_t ref, bool buy,
             std::uint32_t shares, std::uint32_t price, bool with_mpid = false) {
        header(with_mpid ? 'F' : 'A', locate, ts, with_mpid ? 40 : 36);
        be(ref, 8);
        ch(buy ? 'B' : 'S');
        be(shares, 4);
        sym(s);
        be(price, 4);
        if (with_mpid) {  // 4-character market participant attribution
            ch('G'); ch('S'); ch('C'); ch('O');
        }
    }
    void executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        header('E', locate, ts, 31);
        be(ref, 8);
        be(shares, 4);
        be(++match, 8);
    }
    void executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                             std::uint32_t price, bool printable = true) {
        header('C', locate, ts, 36);
        be(ref, 8);
        be(shares, 4);
        be(++match, 8);
        ch(printable ? 'Y' : 'N');
        be(price, 4);
    }
    void cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
        header('X', locate, ts, 23);
        be(ref, 8);
        be(shares, 4);
    }
    void del(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
        header('D', locate, ts, 19);
        be(ref, 8);
    }
    void replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
                 std::uint32_t shares, std::uint32_t price) {
        header('U', locate, ts, 35);
        be(old_ref, 8);
        be(new_ref, 8);
        be(shares, 4);
        be(price, 4);
    }
    void trade(std::uint16_t locate, const std::string& s, std::uint64_t ts, std::uint32_t shares, std::uint32_t price) {
        header('P', locate, ts, 44);
        be(0, 8);
        ch('B');
        be(shares, 4);
        sym(s);
        be(price, 4);
        be(++match, 8);
    }
    void cross(std::uint16_t locate, const std::string& s, std::uint64_t ts, std::uint64_t shares, std::uint32_t price) {
        header('Q', locate, ts, 40);
        be(shares, 8);
        sym(s);
        be(price, 4);
        be(++match, 8);
        ch('O');
    }
    void other(char type, std::size_t len) {  // e.g. 'H' trading action: skipped, not malformed
        header(type, 1, 0, len);
        for (std::size_t i = 11; i < len; ++i) ch(0);
    }

    void save(const std::string& path, bool gz) const {
        if (gz) {
            gzFile f = gzopen(path.c_str(), "wb");
            gzwrite(f, bytes.data(), unsigned(bytes.size()));
            gzclose(f);
        } else {
            std::FILE* f = std::fopen(path.c_str(), "wb");
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }

    std::uint64_t match = 0;
};

static std::string tmp(const std::string& name) { return "build/" + name; }

static Replayer replay_file(const std::string& path, ReplayOptions opts = {}, bool* complete = nullptr) {
    Reader r(path, 64);  // tiny buffer so messages straddle refills
    Replayer rep(std::move(opts));
    const bool ok = r.for_each([&](const std::uint8_t* m, std::size_t n) { rep.apply(m, n); });
    if (complete) *complete = ok;
    return rep;
}

constexpr std::uint32_t usd(double d) { return std::uint32_t(d * 10'000 + 0.5); }

// ---------- tests ----------

static void decodes_every_message_type() {
    Writer w;
    w.system_event(1, 'O');
    w.directory(1, "AAPL");
    w.add(1, "AAPL", 2, 10, true, 100, usd(150.25));
    w.add(1, "AAPL", 3, 11, false, 200, usd(150.30), true);
    w.executed(1, 4, 10, 40);
    w.executed_with_price(1, 5, 11, 50, usd(150.28));
    w.cancel(1, 6, 11, 50);
    w.replace(1, 7, 10, 12, 70, usd(150.26));
    w.del(1, 8, 12);
    w.trade(1, "AAPL", 9, 300, usd(150.27));
    w.cross(1, "AAPL", 10, 1000, usd(150.27));
    w.other('H', 25);
    w.save(tmp("all_types.itch"), false);

    bool complete = false;
    Replayer r = replay_file(tmp("all_types.itch"), {}, &complete);
    CHECK(complete);
    CHECK(r.messages() == 12);
    CHECK(r.malformed() == 0);
    for (char c : std::string("SRAFECXUDPQH")) CHECK(r.by_type()[std::uint8_t(c)] == 1);
    const Counts t = r.totals();
    CHECK(t.adds == 2 && t.executions == 1 && t.executions_priced == 1 && t.cancels == 1);
    CHECK(t.replaces == 1 && t.deletes == 1 && t.hidden_trades == 1 && t.crosses == 1);
    CHECK(t.missing_refs == 0 && t.over_executions == 0);
    CHECK(t.shares_traded == 40 + 50 + 300 + 1000);
    // order 11 (ask) has 200 - 50 executed - 50 cancelled = 100 left; order 10 was replaced then deleted
    auto [bid, ask] = r.top_of_book("AAPL");
    CHECK(!bid && ask == std::int64_t(usd(150.30)));
    CHECK(r.live_orders() == 1);
}

static void price_time_priority_is_checked() {
    Writer w;
    w.directory(1, "MSFT");
    w.add(1, "MSFT", 1, 1, true, 100, usd(400.00));
    w.add(1, "MSFT", 2, 2, true, 100, usd(400.00));
    w.add(1, "MSFT", 3, 3, true, 100, usd(399.99));
    w.executed(1, 4, 1, 100);  // first in line at best: counts as front + best
    w.executed(1, 5, 3, 10);   // not at the best price
    w.executed(1, 6, 2, 50);   // now first in line again
    w.save(tmp("priority.itch"), false);
    Replayer r = replay_file(tmp("priority.itch"));
    const Counts t = r.totals();
    CHECK(t.executions == 3);
    CHECK(t.executions_best == 2);
    CHECK(t.executions_front == 2);
    CHECK(r.top_of_book("MSFT").first == std::int64_t(usd(400.00)));
}

static void partial_cancel_keeps_queue_position() {
    Writer w;
    w.directory(1, "X");
    w.add(1, "X", 1, 1, false, 100, usd(10.00));
    w.add(1, "X", 2, 2, false, 100, usd(10.00));
    w.cancel(1, 3, 1, 60);   // order 1 shrinks but stays first
    w.executed(1, 4, 1, 40);
    w.save(tmp("partial.itch"), false);
    Replayer r = replay_file(tmp("partial.itch"));
    CHECK(r.totals().executions_front == 1);
    CHECK(r.live_orders() == 1);
}

static void replace_goes_to_back_of_queue() {
    Writer w;
    w.directory(1, "X");
    w.add(1, "X", 1, 1, true, 100, usd(10.00));
    w.add(1, "X", 2, 2, true, 100, usd(10.00));
    w.replace(1, 3, 1, 3, 100, usd(10.00));  // same price, but now behind order 2
    w.executed(1, 4, 2, 100);
    w.save(tmp("replace.itch"), false);
    Replayer r = replay_file(tmp("replace.itch"));
    CHECK(r.totals().executions_front == 1);
    CHECK(r.totals().missing_refs == 0);
}

static void sub_dollar_stock_uses_tick_book() {
    Writer w;
    w.directory(1, "PENNY");
    w.add(1, "PENNY", 1, 1, true, 1000, 5'123);  // $0.5123
    w.add(1, "PENNY", 2, 2, false, 1000, 5'130);
    w.executed(1, 3, 1, 500);
    w.save(tmp("penny.itch"), false);
    Replayer r = replay_file(tmp("penny.itch"));
    auto [bid, ask] = r.top_of_book("PENNY");
    CHECK(bid == 5'123 && ask == 5'130);
    CHECK(r.totals().side_table_adds == 0);
    CHECK(r.totals().executions_front == 1);
}

static void orders_the_book_cant_hold_still_apply() {
    Writer w;
    w.directory(1, "X");
    w.add(1, "X", 1, 1, true, 100, usd(20.00));
    w.add(1, "X", 2, 3, false, 100, usd(50'000.00));  // far outside the flat array
    w.add(1, "X", 3, 4, false, 100, usd(19.00));      // would cross the $20 bid
    w.cancel(1, 6, 3, 30);
    w.del(1, 7, 4);
    w.save(tmp("side.itch"), false);
    Replayer r = replay_file(tmp("side.itch"));
    const Counts t = r.totals();
    CHECK(t.side_table_adds == 2);
    CHECK(t.crossing_adds == 1);
    CHECK(t.switched_books == 0);
    CHECK(t.missing_refs == 0);
    CHECK(r.live_orders() == 2);  // order 1, and order 3 with 70 shares
}

// A stock trading around 1 cent whose first order is a stub quote at $5:
// it starts in the cents book and must move to the tick book, keeping queues.
static void sub_penny_order_switches_book_and_keeps_queues() {
    Writer w;
    w.directory(1, "HMNY");
    w.add(1, "HMNY", 1, 1, false, 100, usd(5.00));    // stub ask -> cents book
    w.add(1, "HMNY", 2, 2, true, 1000, usd(0.01));    // whole cent: fits the cents book
    w.add(1, "HMNY", 3, 3, true, 1000, usd(0.01));
    w.add(1, "HMNY", 4, 4, true, 5000, 111);          // $0.0111: sub-penny -> switch
    w.add(1, "HMNY", 5, 5, false, 5000, 115);         // $0.0115
    w.add(1, "HMNY", 6, 6, true, 2000, 111);
    w.executed(1, 7, 4, 5000);                        // best bid $0.0111, first in line: order 4
    w.executed(1, 8, 6, 500);                         // then order 6
    w.executed(1, 9, 5, 100);                         // best ask $0.0115
    w.del(1, 10, 6);
    w.executed(1, 11, 2, 1000);                       // $0.01 level: order 2 was first, before the switch
    w.executed(1, 12, 3, 1000);
    w.save(tmp("hmny.itch"), false);
    Replayer r = replay_file(tmp("hmny.itch"));
    const Counts t = r.totals();
    CHECK(t.switched_books == 1);
    CHECK(t.side_table_adds == 0);
    CHECK(t.missing_refs == 0);
    CHECK(t.executions == 5);
    CHECK(t.executions_front == 5);
    CHECK(t.executions_best == 5);
    auto [bid, ask] = r.top_of_book("HMNY");
    CHECK(!bid && ask == 115);
}

static void depth_csv_has_levels() {
    Writer w;
    w.directory(1, "D");
    w.add(1, "D", 1'000'000'000, 1, true, 100, usd(10.00));
    w.add(1, "D", 1'000'000'001, 2, true, 200, usd(9.99));
    w.add(1, "D", 1'000'000'002, 3, false, 300, usd(10.02));
    w.add(1, "D", 2'000'000'000, 4, true, 50, usd(10.00));
    w.save(tmp("depth.itch"), false);
    ReplayOptions opts;
    opts.track = "D";
    opts.out_dir = "build";
    opts.depth = 3;
    { Replayer r = replay_file(tmp("depth.itch"), opts); }
    std::ifstream f("build/D_book.csv");
    std::string header, row1, row2;
    std::getline(f, header);
    std::getline(f, row1);
    std::getline(f, row2);
    CHECK(header == "ts_ns,bid,bid_qty,ask,ask_qty,bid2,bid_qty2,ask2,ask_qty2,bid3,bid_qty3,ask3,ask_qty3");
    CHECK(row1 == "1000000000,10.0000,100,0.0000,0,0.0000,0,0.0000,0,0.0000,0,0.0000,0");
    CHECK(row2 == "2000000000,10.0000,150,10.0200,300,9.9900,200,0.0000,0,0.0000,0,0.0000,0");
}

static void events_csv_lists_every_order_event() {
    Writer w;
    w.directory(1, "EV");
    w.add(1, "EV", 1, 10, true, 100, usd(10.00));
    w.executed(1, 2, 10, 40);
    w.cancel(1, 3, 10, 10);
    w.replace(1, 4, 10, 11, 70, usd(10.01));
    w.trade(1, "EV", 5, 300, usd(10.005));
    w.del(1, 6, 11);
    w.save(tmp("events.itch"), false);
    ReplayOptions opts;
    opts.track = "EV";
    opts.out_dir = "build";
    opts.events = true;
    { Replayer r = replay_file(tmp("events.itch"), opts); }
    std::ifstream f("build/EV_events.csv");
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    CHECK(lines.size() == 7);
    if (lines.size() == 7) {
        CHECK(lines[1] == "1,A,10,B,100,100000,0");
        CHECK(lines[2] == "2,E,10,B,40,100000,0");
        CHECK(lines[3] == "3,X,10,B,10,100000,0");
        CHECK(lines[4] == "4,U,10,B,70,100100,11");
        CHECK(lines[5] == "5,P,0,B,300,100050,0");
        CHECK(lines[6] == "6,D,11,B,70,100100,0");
    }
}

static void unknown_orders_and_over_executions_are_counted() {
    Writer w;
    w.directory(1, "X");
    w.add(1, "X", 1, 1, true, 100, usd(10.00));
    w.executed(1, 2, 99, 10);  // never added
    w.del(1, 3, 98);
    w.executed(1, 4, 1, 150);  // more than the order had
    w.save(tmp("bad_refs.itch"), false);
    Replayer r = replay_file(tmp("bad_refs.itch"));
    CHECK(r.totals().missing_refs == 2);
    CHECK(r.totals().over_executions == 1);
    CHECK(r.live_orders() == 0);
}

static void gzip_and_plain_files_match_and_truncation_is_reported() {
    Writer w;
    w.directory(1, "X");
    for (std::uint64_t i = 1; i <= 500; ++i) w.add(1, "X", i, i, i % 2, 100, usd(10.00) + std::uint32_t(i % 7) * 100);
    w.save(tmp("plain.itch"), false);
    w.save(tmp("zipped.itch.gz"), true);
    Replayer a = replay_file(tmp("plain.itch"));
    Replayer b = replay_file(tmp("zipped.itch.gz"));
    CHECK(a.messages() == 501 && b.messages() == 501);
    CHECK(a.top_of_book("X") == b.top_of_book("X"));

    Writer cut = w;
    cut.bytes.resize(cut.bytes.size() - 5);
    cut.save(tmp("cut.itch"), false);
    bool complete = true;
    Replayer c = replay_file(tmp("cut.itch"), {}, &complete);
    CHECK(!complete);
    CHECK(c.messages() == 500);
}

static void malformed_length_is_counted() {
    Writer w;
    w.directory(1, "X");
    w.header('A', 1, 1, 20);  // an add must be 36 bytes
    for (int i = 0; i < 9; ++i) w.ch(0);
    w.save(tmp("malformed.itch"), false);
    Replayer r = replay_file(tmp("malformed.itch"));
    CHECK(r.malformed() == 1);
    CHECK(r.totals().adds == 0);
}

static void symbol_filter_and_tracking_csv() {
    Writer w;
    w.directory(1, "AAPL");
    w.directory(2, "MSFT");
    w.add(1, "AAPL", 1'000'000'000, 1, true, 100, usd(150.00));
    w.add(2, "MSFT", 1'000'000'001, 2, true, 100, usd(400.00));
    w.add(1, "AAPL", 2'500'000'000, 3, false, 100, usd(150.05));
    w.executed(1, 3'200'000'000, 3, 40);
    w.save(tmp("filter.itch"), false);
    ReplayOptions opts;
    opts.symbols = {"AAPL"};
    opts.track = "AAPL";
    opts.out_dir = "build";
    {
        Replayer r = replay_file(tmp("filter.itch"), opts);
        CHECK(r.totals().adds == 2);
        CHECK(!r.top_of_book("MSFT").first);
        CHECK(r.summaries().size() == 1 && r.summaries()[0].symbol == "AAPL");
    }  // closes the CSVs
    std::ifstream book("build/AAPL_book.csv"), trades("build/AAPL_trades.csv");
    std::string line;
    int book_rows = -1, trade_rows = -1;
    while (std::getline(book, line)) ++book_rows;
    while (std::getline(trades, line)) ++trade_rows;
    CHECK(book_rows == 3);  // one sample per second of feed time with activity
    CHECK(trade_rows == 1);
}

// Random feed generated from an independent "true" book: adds never cross,
// executions always hit the true first-in-line order. The replay must agree
// with the truth at every checkpoint and find no inconsistencies.
static void random_feed_matches_independent_truth() {
    struct TrueOrder {
        bool buy;
        std::uint32_t price;
        std::uint32_t shares;
    };
    std::map<std::uint32_t, std::deque<std::uint64_t>, std::greater<>> bids;
    std::map<std::uint32_t, std::deque<std::uint64_t>> asks;
    std::map<std::uint64_t, TrueOrder> live;
    auto erase_from_level = [&](std::uint64_t ref) {
        const TrueOrder o = live.at(ref);
        auto drop = [&](auto& side) {
            auto& q = side.at(o.price);
            q.erase(std::find(q.begin(), q.end(), ref));
            if (q.empty()) side.erase(o.price);
        };
        if (o.buy) drop(bids);
        else drop(asks);
        live.erase(ref);
    };

    Writer w;
    w.directory(1, "RAND");
    std::mt19937_64 rng(5);
    std::uint64_t next_ref = 1, ts = 0;
    std::vector<std::pair<std::size_t, std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>>> checkpoints;
    std::size_t messages = 1;
    for (int step = 0; step < 60'000; ++step) {
        ts += 1000;
        const int op = int(rng() % 100);
        auto random_live = [&]() { auto it = live.begin(); std::advance(it, rng() % live.size()); return it->first; };
        if (op < 45 || live.size() < 10) {
            const bool buy = rng() % 2;
            const std::uint32_t best_bid = bids.empty() ? usd(99.99) : bids.begin()->first;
            const std::uint32_t best_ask = asks.empty() ? usd(100.01) : asks.begin()->first;
            std::uint32_t price = buy ? best_ask - 100 * std::uint32_t(1 + rng() % 8)
                                      : best_bid + 100 * std::uint32_t(1 + rng() % 8);
            const std::uint32_t shares = 100 * std::uint32_t(1 + rng() % 5);
            const std::uint64_t ref = next_ref++;
            w.add(1, "RAND", ts, ref, buy, shares, price, rng() % 4 == 0);
            live[ref] = {buy, price, shares};
            (buy ? bids[price] : asks[price]).push_back(ref);
        } else if (op < 65) {  // execute the true first-in-line order on a random side
            const bool buy = rng() % 2;
            if (buy ? bids.empty() : asks.empty()) continue;
            const std::uint64_t ref = buy ? bids.begin()->second.front() : asks.begin()->second.front();
            const std::uint32_t qty = std::min<std::uint32_t>(live[ref].shares, 100 * std::uint32_t(1 + rng() % 3));
            w.executed(1, ts, ref, qty);
            if ((live[ref].shares -= qty) == 0) erase_from_level(ref);
        } else if (op < 75) {
            const std::uint64_t ref = random_live();
            const std::uint32_t qty = std::min<std::uint32_t>(live[ref].shares, 100);
            w.cancel(1, ts, ref, qty);
            if ((live[ref].shares -= qty) == 0) erase_from_level(ref);
        } else if (op < 90) {
            const std::uint64_t ref = random_live();
            w.del(1, ts, ref);
            erase_from_level(ref);
        } else {
            const std::uint64_t ref = random_live();
            const TrueOrder o = live[ref];
            erase_from_level(ref);
            const std::uint32_t best_other = o.buy ? (asks.empty() ? usd(100.01) : asks.begin()->first)
                                                   : (bids.empty() ? usd(99.99) : bids.begin()->first);
            const std::uint32_t price = o.buy ? best_other - 100 * std::uint32_t(1 + rng() % 6)
                                              : best_other + 100 * std::uint32_t(1 + rng() % 6);
            const std::uint64_t new_ref = next_ref++;
            w.replace(1, ts, ref, new_ref, o.shares, price);
            live[new_ref] = {o.buy, price, o.shares};
            (o.buy ? bids[price] : asks[price]).push_back(new_ref);
        }
        ++messages;
        if (step % 500 == 0)
            checkpoints.push_back({messages, {bids.empty() ? std::nullopt : std::optional<std::int64_t>(bids.begin()->first),
                                              asks.empty() ? std::nullopt : std::optional<std::int64_t>(asks.begin()->first)}});
    }
    w.save(tmp("random.itch.gz"), true);

    Reader reader(tmp("random.itch.gz"), 4096);
    Replayer rep{ReplayOptions{}};
    std::size_t next_checkpoint = 0;
    bool all_match = true;
    reader.for_each([&](const std::uint8_t* m, std::size_t n) {
        rep.apply(m, n);
        if (next_checkpoint < checkpoints.size() && rep.messages() == checkpoints[next_checkpoint].first)
            all_match &= rep.top_of_book("RAND") == checkpoints[next_checkpoint++].second;
    });
    const Counts t = rep.totals();
    CHECK(all_match);
    CHECK(next_checkpoint == checkpoints.size());
    CHECK(t.missing_refs == 0 && t.over_executions == 0 && t.crossing_adds == 0 && t.side_table_adds == 0);
    CHECK(t.executions > 5'000);
    CHECK(t.executions_front == t.executions);
    CHECK(rep.live_orders() == live.size());
}

int main() {
    std::filesystem::create_directories("build");  // test files go in ./build
    const std::pair<const char*, void (*)()> tests[] = {
        {"decodes_every_message_type", decodes_every_message_type},
        {"price_time_priority_is_checked", price_time_priority_is_checked},
        {"partial_cancel_keeps_queue_position", partial_cancel_keeps_queue_position},
        {"replace_goes_to_back_of_queue", replace_goes_to_back_of_queue},
        {"sub_dollar_stock_uses_tick_book", sub_dollar_stock_uses_tick_book},
        {"orders_the_book_cant_hold_still_apply", orders_the_book_cant_hold_still_apply},
        {"sub_penny_order_switches_book_and_keeps_queues", sub_penny_order_switches_book_and_keeps_queues},
        {"depth_csv_has_levels", depth_csv_has_levels},
        {"events_csv_lists_every_order_event", events_csv_lists_every_order_event},
        {"unknown_orders_and_over_executions_are_counted", unknown_orders_and_over_executions_are_counted},
        {"gzip_and_plain_files_match_and_truncation_is_reported", gzip_and_plain_files_match_and_truncation_is_reported},
        {"malformed_length_is_counted", malformed_length_is_counted},
        {"symbol_filter_and_tracking_csv", symbol_filter_and_tracking_csv},
        {"random_feed_matches_independent_truth", random_feed_matches_independent_truth},
    };
    for (auto& [name, fn] : tests) {
        const int before = g_failed;
        fn();
        std::printf("%s %s\n", g_failed == before ? "ok  " : "FAIL", name);
    }
    std::printf("\n%d checks, %d failed, %zu tests\n", g_checks, g_failed, std::size(tests));
    return g_failed ? 1 : 0;
}
