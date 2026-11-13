#include "replay.hpp"

#include "itch.hpp"

#include <algorithm>

namespace itch {

namespace {

constexpr std::size_t kCentsBookLevels = std::size_t(1) << 20;  // $10,485 of one-cent levels
constexpr std::uint32_t kCentsThreshold = 10'000;               // $1.00 in ITCH units

// Runs fn(book, units_per_book_tick) on whichever book the stock uses.
// Returns fallback if the stock has no book yet.
template <class Stock, class Fn, class R>
R with_book(Stock& s, R fallback, Fn&& fn) {
    if (s.cents) return fn(*s.cents, std::int64_t(100));
    if (s.ticks) return fn(*s.ticks, std::int64_t(1));
    return fallback;
}

}  // namespace

void Counts::merge(const Counts& o) {
    adds += o.adds;
    executions += o.executions;
    executions_front += o.executions_front;
    executions_best += o.executions_best;
    executions_priced += o.executions_priced;
    cancels += o.cancels;
    deletes += o.deletes;
    replaces += o.replaces;
    hidden_trades += o.hidden_trades;
    crosses += o.crosses;
    shares_traded += o.shares_traded;
    notional += o.notional;
    missing_refs += o.missing_refs;
    over_executions += o.over_executions;
    crossing_adds += o.crossing_adds;
    side_table_adds += o.side_table_adds;
    switched_books += o.switched_books;
}

Replayer::Replayer(ReplayOptions options) : opts_(std::move(options)), stocks_(65536) {}

Replayer::~Replayer() {
    for (auto& s : stocks_) {
        if (!s) continue;
        if (s->book_csv) std::fclose(s->book_csv);
        if (s->trades_csv) std::fclose(s->trades_csv);
        if (s->events_csv) std::fclose(s->events_csv);
    }
}

bool Replayer::apply(const std::uint8_t* m, std::size_t len) {
    ++messages_;
    if (len) ++by_type_[m[0]];
    if (dispatch(m, len, *this)) return true;
    ++malformed_;
    return false;
}

Counts Replayer::totals() const {
    Counts c;
    for (const auto& s : stocks_)
        if (s) c.merge(s->counts);
    return c;
}

std::vector<SymbolSummary> Replayer::summaries() const {
    std::vector<SymbolSummary> out;
    for (const auto& s : stocks_) {
        if (!s || !s->active || (s->counts.adds == 0 && s->counts.shares_traded == 0)) continue;
        std::size_t resting = s->side_table.size();
        if (s->cents) resting += s->cents->size();
        if (s->ticks) resting += s->ticks->size();
        out.push_back({s->symbol, s->counts, resting});
    }
    std::sort(out.begin(), out.end(),
              [](const SymbolSummary& a, const SymbolSummary& b) { return a.counts.shares_traded > b.counts.shares_traded; });
    return out;
}

std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>> Replayer::top_of_book(const std::string& symbol) const {
    for (const auto& s : stocks_) {
        if (!s || s->symbol != symbol) continue;
        using P = std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>>;
        return with_book(*s, P{}, [](auto& book, std::int64_t unit) {
            auto b = book.best_bid(), a = book.best_ask();
            return P{b ? std::optional<std::int64_t>(*b * unit) : std::nullopt,
                     a ? std::optional<std::int64_t>(*a * unit) : std::nullopt};
        });
    }
    return {};
}

// ---------- stocks ----------

Replayer::Stock* Replayer::stock(std::uint16_t locate) {
    auto& s = stocks_[locate];
    if (!s) {  // order for a stock we never saw a directory message for
        s = std::make_unique<Stock>();
        s->symbol = "#" + std::to_string(locate);
        s->active = opts_.symbols.empty();
    }
    return s->active ? s.get() : nullptr;
}

void Replayer::on_stock_directory(std::uint16_t locate, std::uint64_t, std::string_view symbol) {
    auto& s = stocks_[locate];
    if (!s) s = std::make_unique<Stock>();
    s->symbol = std::string(symbol);
    s->active = opts_.symbols.empty() || opts_.symbols.count(s->symbol) > 0;
    if (!opts_.track.empty() && s->symbol == opts_.track) open_tracking(*s);
}

void Replayer::on_system_event(std::uint64_t, char) {}

// ---------- order book messages ----------

void Replayer::on_add(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, bool buy, std::uint32_t shares,
                      std::uint32_t price) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.adds;
    if (s->events_csv) event(*s, ts, 'A', ref, buy ? 'B' : 'S', shares, price);
    place(*s, ref, buy, shares, price);
    after(*s, ts);
}

void Replayer::on_executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.executions;
    if (s->events_csv) {
        const OrderInfo o = info(*s, ref);
        event(*s, ts, 'E', ref, o.found ? (o.buy ? 'B' : 'S') : '?', shares, o.price);
    }
    // Before applying it, check that this is the order price-time priority says trades next.
    const std::int64_t price = with_book(*s, std::int64_t(-1), [&](auto& book, std::int64_t unit) -> std::int64_t {
        const ob::Order* o = book.get(ref);
        if (!o) return -1;
        const auto best = o->side == ob::Side::Buy ? book.best_bid() : book.best_ask();
        if (best && *best == o->price) ++s->counts.executions_best;
        if (book.front(o->side) == ref) ++s->counts.executions_front;
        return o->price * unit;
    });
    std::uint32_t raw = price >= 0 ? std::uint32_t(price) : 0;
    if (price < 0) {
        auto it = s->side_table.find(ref);
        if (it == s->side_table.end()) {
            ++s->counts.missing_refs;
            return;
        }
        raw = it->second.price;
    }
    reduce(*s, ref, shares);
    record_trade(*s, ts, 'E', shares, raw);
    after(*s, ts);
}

void Replayer::on_executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                                      bool printable, std::uint32_t price) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.executions_priced;
    if (s->events_csv) {
        const OrderInfo o = info(*s, ref);
        event(*s, ts, 'C', ref, o.found ? (o.buy ? 'B' : 'S') : '?', shares, price);
    }
    if (!reduce(*s, ref, shares)) {
        ++s->counts.missing_refs;
        return;
    }
    if (printable) record_trade(*s, ts, 'C', shares, price);
    after(*s, ts);
}

void Replayer::on_cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.cancels;
    if (s->events_csv) {
        const OrderInfo o = info(*s, ref);
        event(*s, ts, 'X', ref, o.found ? (o.buy ? 'B' : 'S') : '?', shares, o.price);
    }
    if (!reduce(*s, ref, shares)) ++s->counts.missing_refs;
    after(*s, ts);
}

void Replayer::on_delete(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.deletes;
    if (s->events_csv) {
        const OrderInfo o = info(*s, ref);
        event(*s, ts, 'D', ref, o.found ? (o.buy ? 'B' : 'S') : '?', o.remaining, o.price);
    }
    if (!remove(*s, ref)) ++s->counts.missing_refs;
    after(*s, ts);
}

void Replayer::on_replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
                          std::uint32_t shares, std::uint32_t price) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.replaces;
    if (s->events_csv) {
        const OrderInfo o = info(*s, old_ref);
        event(*s, ts, 'U', old_ref, o.found ? (o.buy ? 'B' : 'S') : '?', shares, price, new_ref);
    }
    // The replacement keeps the original's side but goes to the back of the queue.
    const int side = with_book(*s, -1, [&](auto& book, std::int64_t) {
        const ob::Order* o = book.get(old_ref);
        return o ? (o->side == ob::Side::Buy ? 1 : 0) : -1;
    });
    bool buy;
    if (side >= 0) {
        buy = side == 1;
    } else {
        auto it = s->side_table.find(old_ref);
        if (it == s->side_table.end()) {
            ++s->counts.missing_refs;
            return;
        }
        buy = it->second.buy;
    }
    remove(*s, old_ref);
    place(*s, new_ref, buy, shares, price);
    after(*s, ts);
}

void Replayer::on_trade(std::uint16_t locate, std::uint64_t ts, bool buy, std::uint32_t shares, std::uint32_t price) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.hidden_trades;
    if (s->events_csv) event(*s, ts, 'P', 0, buy ? 'B' : 'S', shares, price);
    record_trade(*s, ts, 'P', shares, price);
}

void Replayer::on_cross(std::uint16_t locate, std::uint64_t ts, std::uint64_t shares, std::uint32_t price) {
    Stock* s = stock(locate);
    if (!s) return;
    ++s->counts.crosses;
    if (s->events_csv) event(*s, ts, 'Q', 0, '-', std::int64_t(shares), price);
    if (shares) record_trade(*s, ts, 'Q', shares, price);
}

// ---------- book operations ----------

template <class Book>
bool Replayer::try_add(Stock& s, Book& book, std::uint64_t ref, bool buy, std::int64_t shares, std::int64_t p) {
    const auto bid = book.best_bid(), ask = book.best_ask();
    if (buy ? (ask && p >= *ask) : (bid && p <= *bid)) {
        ++s.counts.crossing_adds;  // Nasdaq's book shouldn't cross; keep it out of ours
        return false;
    }
    s.scratch.clear();
    try {
        book.add(ob::Order::limit(ref, buy ? ob::Side::Buy : ob::Side::Sell, shares, p), s.scratch);
    } catch (const ob::OrderBookError&) {  // e.g. price far outside the flat array
        return false;
    }
    return true;
}

void Replayer::place(Stock& s, std::uint64_t ref, bool buy, std::uint32_t shares, std::uint32_t price) {
    if (!s.cents && !s.ticks) {
        if (price >= kCentsThreshold) {
            s.cents = std::make_unique<ob::OrderBook>(kCentsBookLevels);
            s.cents->keep_history(false);
        } else {
            s.ticks = std::make_unique<ref::OrderBook>();
            s.ticks->keep_history(false);
        }
    }
    if (s.cents && price % 100 != 0) switch_to_ticks(s);  // a sub-penny price: this stock trades under $1
    const bool placed = s.cents ? try_add(s, *s.cents, ref, buy, shares, price / 100)
                                : try_add(s, *s.ticks, ref, buy, shares, price);
    if (!placed) {
        s.side_table[ref] = SideOrder{buy, price, std::int64_t(shares)};
        ++s.counts.side_table_adds;
    }
    peak_live_ = std::max(peak_live_, ++live_);
}

void Replayer::switch_to_ticks(Stock& s) {
    auto ticks = std::make_unique<ref::OrderBook>();
    ticks->keep_history(false);
    // Re-adding in priority order (each level oldest first) rebuilds the same queues.
    for (const ob::Order& o : s.cents->orders()) {
        s.scratch.clear();
        ticks->add(ob::Order::limit(o.id, o.side, o.remaining, o.price * 100), s.scratch);
    }
    s.cents.reset();
    s.ticks = std::move(ticks);
    ++s.counts.switched_books;
    // Side-table orders that the cents book couldn't hold may fit now (at the back of their queues).
    for (auto it = s.side_table.begin(); it != s.side_table.end();) {
        const SideOrder& o = it->second;
        const auto bid = s.ticks->best_bid(), ask = s.ticks->best_ask();
        const std::int64_t p = o.price;
        const bool crosses = o.buy ? (ask && p >= *ask) : (bid && p <= *bid);
        if (!crosses) {
            s.scratch.clear();
            s.ticks->add(ob::Order::limit(it->first, o.buy ? ob::Side::Buy : ob::Side::Sell, o.shares, p), s.scratch);
            it = s.side_table.erase(it);
        } else {
            ++it;
        }
    }
}

bool Replayer::reduce(Stock& s, std::uint64_t ref, std::int64_t shares) {
    const int found = with_book(s, 0, [&](auto& book, std::int64_t) {
        const ob::Order* o = book.get(ref);
        if (!o) return 0;
        const std::int64_t left = o->remaining;
        if (shares > left) ++s.counts.over_executions;
        if (shares >= left) {
            book.cancel(ref);
            --live_;
        } else {
            book.modify(ref, left - shares, std::nullopt);  // shrinking keeps its place in line
        }
        return 1;
    });
    if (found) return true;
    auto it = s.side_table.find(ref);
    if (it == s.side_table.end()) return false;
    if (shares > it->second.shares) ++s.counts.over_executions;
    it->second.shares -= shares;
    if (it->second.shares <= 0) {
        s.side_table.erase(it);
        --live_;
    }
    return true;
}

bool Replayer::remove(Stock& s, std::uint64_t ref) {
    if (with_book(s, false, [&](auto& book, std::int64_t) { return book.cancel(ref); })) {
        --live_;
        return true;
    }
    if (s.side_table.erase(ref)) {
        --live_;
        return true;
    }
    return false;
}

// ---------- output ----------

void Replayer::record_trade(Stock& s, std::uint64_t ts, char kind, std::uint64_t shares, std::uint32_t price) {
    s.counts.shares_traded += shares;
    s.counts.notional += double(shares) * double(price) / 10'000.0;
    if (s.trades_csv)
        std::fprintf(s.trades_csv, "%llu,%c,%.4f,%llu\n", (unsigned long long)ts, kind, price / 10'000.0,
                     (unsigned long long)shares);
}

void Replayer::after(Stock& s, std::uint64_t ts) {
    if (!s.book_csv || ts < s.next_sample) return;
    s.next_sample = (ts / opts_.sample_ns + 1) * opts_.sample_ns;
    if (opts_.depth > 1) {
        write_depth(s, ts);
        return;
    }
    struct Top {
        double bid = 0, ask = 0;
        long long bid_qty = 0, ask_qty = 0;
    };
    const Top t = with_book(s, Top{}, [](auto& book, std::int64_t unit) {
        Top r;
        auto b = book.depth(ob::Side::Buy, 1), a = book.depth(ob::Side::Sell, 1);
        if (!b.empty()) {
            r.bid = double(b[0].first * unit) / 10'000.0;
            r.bid_qty = (long long)b[0].second;
        }
        if (!a.empty()) {
            r.ask = double(a[0].first * unit) / 10'000.0;
            r.ask_qty = (long long)a[0].second;
        }
        return r;
    });
    std::fprintf(s.book_csv, "%llu,%.4f,%lld,%.4f,%lld\n", (unsigned long long)ts, t.bid, t.bid_qty, t.ask, t.ask_qty);
}

void Replayer::write_depth(Stock& s, std::uint64_t ts) {
    const std::size_t n = std::size_t(opts_.depth);
    std::vector<std::pair<ob::Price, ob::Qty>> bids, asks;
    std::int64_t unit = 1;
    with_book(s, 0, [&](auto& book, std::int64_t u) {
        bids = book.depth(ob::Side::Buy, n);
        asks = book.depth(ob::Side::Sell, n);
        unit = u;
        return 0;
    });
    std::fprintf(s.book_csv, "%llu", (unsigned long long)ts);
    // Level 1 first (same columns as without --depth), then levels 2..n.
    for (std::size_t i = 0; i < n; ++i) {
        const double bp = i < bids.size() ? double(bids[i].first * unit) / 10'000.0 : 0.0;
        const long long bq = i < bids.size() ? (long long)bids[i].second : 0;
        const double ap = i < asks.size() ? double(asks[i].first * unit) / 10'000.0 : 0.0;
        const long long aq = i < asks.size() ? (long long)asks[i].second : 0;
        std::fprintf(s.book_csv, ",%.4f,%lld,%.4f,%lld", bp, bq, ap, aq);
    }
    std::fputc('\n', s.book_csv);
}

Replayer::OrderInfo Replayer::info(Stock& s, std::uint64_t ref) {
    OrderInfo r;
    with_book(s, 0, [&](auto& book, std::int64_t unit) {
        if (const ob::Order* o = book.get(ref))
            r = {true, o->side == ob::Side::Buy, std::uint32_t(o->price * unit), o->remaining};
        return 0;
    });
    if (!r.found) {
        auto it = s.side_table.find(ref);
        if (it != s.side_table.end()) r = {true, it->second.buy, it->second.price, it->second.shares};
    }
    return r;
}

void Replayer::event(Stock& s, std::uint64_t ts, char type, std::uint64_t ref, char side, std::int64_t shares,
                     std::uint32_t price, std::uint64_t new_ref) {
    std::fprintf(s.events_csv, "%llu,%c,%llu,%c,%lld,%u,%llu\n", (unsigned long long)ts, type,
                 (unsigned long long)ref, side, (long long)shares, price, (unsigned long long)new_ref);
}

void Replayer::open_tracking(Stock& s) {
    if (s.book_csv) return;
    const std::string base = opts_.out_dir + "/" + s.symbol;
    s.book_csv = std::fopen((base + "_book.csv").c_str(), "w");
    s.trades_csv = std::fopen((base + "_trades.csv").c_str(), "w");
    if (!s.book_csv || !s.trades_csv) throw std::runtime_error("can't write CSVs to " + opts_.out_dir);
    std::fprintf(s.book_csv, "ts_ns,bid,bid_qty,ask,ask_qty");
    for (int i = 2; i <= opts_.depth; ++i) std::fprintf(s.book_csv, ",bid%d,bid_qty%d,ask%d,ask_qty%d", i, i, i, i);
    std::fputc('\n', s.book_csv);
    std::fprintf(s.trades_csv, "ts_ns,kind,price,shares\n");
    if (opts_.events) {
        s.events_csv = std::fopen((base + "_events.csv").c_str(), "w");
        if (!s.events_csv) throw std::runtime_error("can't write CSVs to " + opts_.out_dir);
        // price is in ITCH units (dollars * 10,000); for E/X/D it is the order's own price
        std::fprintf(s.events_csv, "ts_ns,type,ref,side,shares,price,new_ref\n");
    }
}

}  // namespace itch
