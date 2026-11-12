// Rebuilds every stock's order book from a Nasdaq ITCH 5.0 feed and checks
// that the rebuilt books agree with what Nasdaq reports.
//
// ITCH is an order-by-order log of Nasdaq's own book: adds, executions,
// cancels, deletes and replaces. Replaying it means applying each message to
// a book for that stock. Nasdaq already did the matching, so adds should
// never cross the book, and each execution names the order that traded.
// That gives three checks that the book is right:
//   - every execution / cancel / delete / replace names an order we hold,
//   - no add would cross our book (bid >= ask),
//   - an execution hits the order our book says is first in line at the best
//     price (price-time priority).
//
// Book choice per stock: prices of $1 and up are in whole cents (Reg NMS
// forbids sub-penny quotes there), so those stocks use the fast flat-array
// book in cents. Stocks whose first order is under $1 use the map-based book
// in raw 1/10,000-dollar ticks. Any order a book can't hold (a sub-penny price
// in a cents book, or a price far outside the flat array) is kept in a side
// table so its later executions and cancels still apply; these are counted.
#pragma once

#include "orderbook.hpp"
#include "reference_book.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace itch {

struct ReplayOptions {
    std::unordered_set<std::string> symbols;  // empty = every symbol
    std::string track;                        // symbol to write CSVs for ("" = none)
    std::string out_dir = ".";
    std::uint64_t sample_ns = 1'000'000'000;  // top-of-book sample interval for the tracked symbol
};

struct Counts {
    std::uint64_t adds = 0;
    std::uint64_t executions = 0;        // 'E' (at the resting order's price)
    std::uint64_t executions_front = 0;  // ... that hit the first order in line at the best price
    std::uint64_t executions_best = 0;   // ... at the best price on that side
    std::uint64_t executions_priced = 0; // 'C'
    std::uint64_t cancels = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t hidden_trades = 0;     // 'P'
    std::uint64_t crosses = 0;           // 'Q'
    std::uint64_t shares_traded = 0;
    double notional = 0;                 // dollars
    std::uint64_t missing_refs = 0;      // message named an order we don't have
    std::uint64_t over_executions = 0;   // executed/cancelled more shares than the order had
    std::uint64_t crossing_adds = 0;     // add would have crossed our book
    std::uint64_t side_table_adds = 0;   // add kept off the main book (see header comment)

    void merge(const Counts& o);
};

struct SymbolSummary {
    std::string symbol;
    Counts counts;
    std::size_t resting_at_close = 0;
};

class Replayer {
public:
    explicit Replayer(ReplayOptions options);
    ~Replayer();
    Replayer(Replayer&&) = default;  // tracking files live in the heap-allocated stocks, so moving is safe
    Replayer& operator=(Replayer&&) = default;

    // Returns false for a malformed message.
    bool apply(const std::uint8_t* message, std::size_t length);

    Counts totals() const;  // summed over all stocks
    std::uint64_t messages() const { return messages_; }
    std::uint64_t malformed() const { return malformed_; }
    const std::array<std::uint64_t, 256>& by_type() const { return by_type_; }
    std::uint64_t live_orders() const { return live_; }
    std::uint64_t peak_live_orders() const { return peak_live_; }
    std::vector<SymbolSummary> summaries() const;  // stocks that had any orders, busiest first

    // Best bid/ask for a symbol in dollars*10,000 (ITCH units); nullopt if empty or unknown.
    std::pair<std::optional<std::int64_t>, std::optional<std::int64_t>> top_of_book(const std::string& symbol) const;

    // ITCH visitor interface (see itch.hpp)
    void on_system_event(std::uint64_t ts, char code);
    void on_stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view symbol);
    void on_add(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, bool buy, std::uint32_t shares, std::uint32_t price);
    void on_executed(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares);
    void on_executed_with_price(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares,
                                bool printable, std::uint32_t price);
    void on_cancel(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares);
    void on_delete(std::uint16_t locate, std::uint64_t ts, std::uint64_t ref);
    void on_replace(std::uint16_t locate, std::uint64_t ts, std::uint64_t old_ref, std::uint64_t new_ref,
                    std::uint32_t shares, std::uint32_t price);
    void on_trade(std::uint16_t locate, std::uint64_t ts, bool buy, std::uint32_t shares, std::uint32_t price);
    void on_cross(std::uint16_t locate, std::uint64_t ts, std::uint64_t shares, std::uint32_t price);
    void on_other(char) {}

private:
    struct SideOrder {
        bool buy;
        std::uint32_t price;
        std::int64_t shares;
    };

    struct Stock {
        std::string symbol;
        bool active = true;                     // false if filtered out
        std::unique_ptr<ob::OrderBook> cents;   // flat book, prices in cents
        std::unique_ptr<ref::OrderBook> ticks;  // map book, prices in ITCH units
        std::unordered_map<std::uint64_t, SideOrder> side_table;
        std::vector<ob::Trade> scratch;
        Counts counts;
        // tracking output
        std::FILE* book_csv = nullptr;
        std::FILE* trades_csv = nullptr;
        std::uint64_t next_sample = 0;
    };

    Stock* stock(std::uint16_t locate);
    void place(Stock& s, std::uint64_t ref, bool buy, std::uint32_t shares, std::uint32_t price);
    bool remove(Stock& s, std::uint64_t ref);  // whole order; false if unknown
    // Removes `shares` from an order (execution or cancel). Returns false if the order is unknown.
    bool reduce(Stock& s, std::uint64_t ref, std::int64_t shares);
    void record_trade(Stock& s, std::uint64_t ts, char kind, std::uint64_t shares, std::uint32_t price);
    void after(Stock& s, std::uint64_t ts);  // writes a top-of-book sample when one is due
    void open_tracking(Stock& s);

    ReplayOptions opts_;
    std::vector<std::unique_ptr<Stock>> stocks_;  // indexed by stock locate
    std::array<std::uint64_t, 256> by_type_{};
    std::uint64_t messages_ = 0;
    std::uint64_t malformed_ = 0;
    std::uint64_t live_ = 0;
    std::uint64_t peak_live_ = 0;
};

}  // namespace itch
