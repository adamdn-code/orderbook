// Lock-free multithreaded matching engine built on the single-threaded OrderBook.
//
// Design:
//   - Each client thread calls connect() once and gets its own Session.
//   - A Session owns one lock-free SPSC ring per symbol. The client is the
//     only producer and that symbol's matching thread is the only consumer,
//     so clients never share a queue, a lock, or a cache line.
//   - Each symbol has exactly one matching thread (single writer), so
//     price-time priority is never broken by a race. Within one session,
//     commands for a symbol are applied in the order they were sent.
//   - Submitting allocates nothing: commands are plain structs copied into
//     the ring. Results come back through callbacks that run on the matching
//     thread, so there's no future/promise per order.
//   - When idle, a matching thread spins briefly, then yields, then naps,
//     so a busy engine never sleeps and an idle one doesn't burn a core.
//
// Callbacks for one symbol always run on that symbol's thread, one at a
// time, so state a callback keeps per symbol needs no locking. Call drain()
// before reading that state from another thread.
#pragma once

#include "orderbook.hpp"
#include "spsc_ring.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ob {

using SymbolId = std::uint32_t;

struct EngineStopped : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// What happened to one command.
struct Result {
    enum class Kind : std::uint8_t { Add, Cancel, Modify };
    Kind kind;
    OrderId id;
    bool ok;      // false: rejected (bad order, unknown id, ...)
    Qty filled;   // quantity traded by this command
    Qty rested;   // quantity now resting in the book for this order
    Qty dropped;  // unfilled market/IOC quantity that was discarded
};

struct Command {
    enum class Kind : std::uint8_t { Add, Cancel, Modify };
    Kind kind{};
    bool has_qty = false;
    bool has_price = false;
    Order order{};  // Add
    OrderId id{};   // Cancel / Modify
    Qty qty{};      // Modify
    Price price{};  // Modify
};

class MatchingEngine;

class Session {
public:
    static constexpr std::size_t kRingSize = 4096;
    using Ring = SpscRing<Command, kRingSize>;

    // Each Session must be used by one thread at a time. These wait (spin) if
    // the symbol's ring is full; that's backpressure from a slow matcher.
    void submit(SymbolId symbol, const Order& order);
    void cancel(SymbolId symbol, OrderId id);
    void modify(SymbolId symbol, OrderId id, std::optional<Qty> qty, std::optional<Price> price);

    void submit(const std::string& symbol, const Order& order);
    void cancel(const std::string& symbol, OrderId id);
    void modify(const std::string& symbol, OrderId id, std::optional<Qty> qty, std::optional<Price> price);

    // Like submit() but returns false instead of waiting when the ring is full.
    bool try_submit(SymbolId symbol, const Order& order);

private:
    friend class MatchingEngine;
    Session(MatchingEngine& engine, std::size_t symbols);
    bool push(SymbolId symbol, const Command& cmd, bool wait);

    MatchingEngine& engine_;
    std::vector<std::unique_ptr<Ring>> rings_;  // one per symbol
    alignas(kCacheLine) std::atomic<bool> busy_{false};
};

class MatchingEngine {
public:
    static constexpr std::size_t kMaxSessions = 64;

    using TradeCallback = std::function<void(SymbolId, const Trade&)>;
    using ResultCallback = std::function<void(SymbolId, const Result&)>;

    explicit MatchingEngine(const std::vector<std::string>& symbols, TradeCallback on_trade = {},
                            ResultCallback on_result = {});
    ~MatchingEngine();
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;

    // One per client thread. The session lives as long as the engine.
    Session& connect();

    // Wait until every command sent so far has been applied and its callbacks
    // have run. Callback side effects are visible to the caller afterwards.
    void drain() const;

    // Wait for in-flight submits, apply everything queued, join the threads.
    // Don't call while other threads are still submitting.
    void stop();

    SymbolId symbol_id(const std::string& symbol) const;
    const std::string& symbol_name(SymbolId id) const;
    std::size_t symbol_count() const { return workers_.size(); }

    // Consistent reads (each takes the book's lock briefly).
    std::optional<Price> best_bid(const std::string& symbol) const;
    std::optional<Price> best_ask(const std::string& symbol) const;
    std::optional<Price> spread(const std::string& symbol) const;
    std::pair<std::optional<Price>, std::optional<Price>> top_of_book(const std::string& symbol) const;
    std::vector<std::pair<Price, Qty>> depth(const std::string& symbol, Side side, std::size_t levels = 5) const;
    std::vector<Trade> trades(const std::string& symbol) const;
    Qty resting_qty(const std::string& symbol) const;
    std::string snapshot(const std::string& symbol) const;
    std::size_t callback_errors(const std::string& symbol) const;

    void keep_history(bool on);

    // Times a matching thread ran out of work long enough to nap.
    std::size_t idle_naps() const;

private:
    friend class Session;

    struct Worker {
        SymbolId id{};
        std::string symbol;
        OrderBook book;
        mutable std::mutex book_mtx;  // taken once per polling pass, and by readers
        std::array<std::atomic<Session::Ring*>, kMaxSessions> rings{};
        std::atomic<std::size_t> ring_count{0};
        std::size_t callback_errors = 0;  // guarded by book_mtx
        std::atomic<std::size_t> naps{0};
        std::thread thread;
    };

    void run(Worker& w);
    void apply(Worker& w, const Command& cmd, std::vector<Trade>& out);
    void emit_trades(Worker& w, const std::vector<Trade>& out);
    void emit_result(Worker& w, const Result& r);
    bool all_rings_empty(const Worker& w) const;

    template <class Fn>
    auto read(const std::string& symbol, Fn fn) const {
        const Worker& w = *workers_[symbol_id(symbol)];
        std::lock_guard lock(w.book_mtx);
        return fn(w.book);
    }

    TradeCallback on_trade_;
    ResultCallback on_result_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::unordered_map<std::string, SymbolId> ids_;

    std::mutex sessions_mtx_;  // connect() and stop() only, never on the hot path
    std::vector<std::unique_ptr<Session>> sessions_;

    std::atomic<bool> stopping_{false};  // no new commands accepted
    std::atomic<bool> closed_{false};    // all producers done; matchers may exit once drained
    std::mutex stop_mtx_;
    bool joined_ = false;
};

}  // namespace ob
