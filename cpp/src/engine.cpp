#include "engine.hpp"

#include <chrono>

namespace ob {

namespace {

// Tell the CPU we're in a spin loop (saves power, frees the sibling
// hyperthread, avoids a pipeline flush when the value finally changes).
inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

constexpr std::size_t kBudgetPerRing = 256;  // per pass, so one busy client can't starve the rest
constexpr unsigned kSpinPasses = 4096;       // empty passes spent spinning before yielding
constexpr unsigned kYieldPasses = 64;        // then yield this many times before napping
constexpr auto kNap = std::chrono::microseconds(50);

}  // namespace

// ---------- Session ----------

Session::Session(MatchingEngine& engine, std::size_t symbols) : engine_(engine) {
    rings_.reserve(symbols);
    for (std::size_t i = 0; i < symbols; ++i) rings_.push_back(std::make_unique<Ring>());
}

bool Session::push(SymbolId symbol, const Command& cmd, bool wait) {
    if (symbol >= rings_.size()) throw std::out_of_range("unknown symbol id " + std::to_string(symbol));
    // busy_/stopping_ form a handshake with stop(): whichever store comes
    // first, the other side is guaranteed to see it, so a command can never
    // slip in after the matcher has decided it's finished.
    busy_.store(true, std::memory_order_seq_cst);
    if (engine_.stopping_.load(std::memory_order_seq_cst)) {
        busy_.store(false, std::memory_order_release);
        throw EngineStopped("engine is stopped");
    }
    Ring& ring = *rings_[symbol];
    unsigned spins = 0;
    while (!ring.try_push(cmd)) {  // ring full: the matcher is behind
        if (!wait) {
            busy_.store(false, std::memory_order_release);
            return false;
        }
        if (++spins < 1024) cpu_relax();
        else std::this_thread::yield();
    }
    busy_.store(false, std::memory_order_release);
    return true;
}

void Session::submit(SymbolId symbol, const Order& order) {
    Command c;
    c.kind = Command::Kind::Add;
    c.order = order;
    push(symbol, c, true);
}

bool Session::try_submit(SymbolId symbol, const Order& order) {
    Command c;
    c.kind = Command::Kind::Add;
    c.order = order;
    return push(symbol, c, false);
}

void Session::cancel(SymbolId symbol, OrderId id) {
    Command c;
    c.kind = Command::Kind::Cancel;
    c.id = id;
    push(symbol, c, true);
}

void Session::modify(SymbolId symbol, OrderId id, std::optional<Qty> qty, std::optional<Price> price) {
    Command c;
    c.kind = Command::Kind::Modify;
    c.id = id;
    c.has_qty = qty.has_value();
    c.qty = qty.value_or(0);
    c.has_price = price.has_value();
    c.price = price.value_or(0);
    push(symbol, c, true);
}

void Session::submit(const std::string& s, const Order& o) { submit(engine_.symbol_id(s), o); }
void Session::cancel(const std::string& s, OrderId id) { cancel(engine_.symbol_id(s), id); }
void Session::modify(const std::string& s, OrderId id, std::optional<Qty> q, std::optional<Price> p) {
    modify(engine_.symbol_id(s), id, q, p);
}

// ---------- MatchingEngine ----------

MatchingEngine::MatchingEngine(const std::vector<std::string>& symbols, TradeCallback on_trade,
                               ResultCallback on_result)
    : on_trade_(std::move(on_trade)), on_result_(std::move(on_result)) {
    if (symbols.empty()) throw std::invalid_argument("need at least one symbol");
    for (const auto& s : symbols) {
        if (ids_.count(s)) throw std::invalid_argument("duplicate symbol " + s);
        auto w = std::make_unique<Worker>();
        w->id = SymbolId(workers_.size());
        w->symbol = s;
        ids_.emplace(s, w->id);
        workers_.push_back(std::move(w));
    }
    for (auto& w : workers_) {
        Worker* ptr = w.get();
        w->thread = std::thread([this, ptr] { run(*ptr); });
    }
}

MatchingEngine::~MatchingEngine() { stop(); }

Session& MatchingEngine::connect() {
    std::lock_guard lock(sessions_mtx_);
    if (stopping_.load()) throw EngineStopped("engine is stopped");
    if (sessions_.size() == kMaxSessions) throw std::length_error("too many sessions");
    const std::size_t idx = sessions_.size();
    sessions_.push_back(std::unique_ptr<Session>(new Session(*this, workers_.size())));
    Session& s = *sessions_.back();
    for (auto& w : workers_) {
        w->rings[idx].store(s.rings_[w->id].get(), std::memory_order_release);
        w->ring_count.store(idx + 1, std::memory_order_release);
    }
    return s;
}

void MatchingEngine::stop() {
    std::lock_guard guard(stop_mtx_);
    if (joined_) return;
    stopping_.store(true, std::memory_order_seq_cst);
    {
        std::lock_guard lock(sessions_mtx_);
        for (auto& s : sessions_)
            while (s->busy_.load(std::memory_order_seq_cst)) std::this_thread::yield();
    }
    closed_.store(true, std::memory_order_release);
    for (auto& w : workers_) w->thread.join();
    joined_ = true;
}

void MatchingEngine::drain() const {
    for (const auto& w : workers_) {
        const std::size_t n = w->ring_count.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i) {
            const Session::Ring* ring = w->rings[i].load(std::memory_order_acquire);
            unsigned spins = 0;
            while (!ring->empty()) {
                if (++spins < 1024) cpu_relax();
                else std::this_thread::yield();
            }
        }
    }
}

bool MatchingEngine::all_rings_empty(const Worker& w) const {
    const std::size_t n = w.ring_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i)
        if (!w.rings[i].load(std::memory_order_acquire)->empty()) return false;
    return true;
}

void MatchingEngine::run(Worker& w) {
    std::vector<Trade> out;
    out.reserve(256);
    unsigned idle = 0;
    for (;;) {
        std::size_t done = 0;
        const std::size_t n = w.ring_count.load(std::memory_order_acquire);
        {
            std::lock_guard lock(w.book_mtx);
            for (std::size_t i = 0; i < n; ++i) {
                Session::Ring* ring = w.rings[i].load(std::memory_order_acquire);
                for (std::size_t k = 0; k < kBudgetPerRing; ++k) {
                    Command* cmd = ring->front();
                    if (!cmd) break;
                    apply(w, *cmd, out);
                    ring->pop();  // free the slot only after its callbacks ran (drain() relies on this)
                    ++done;
                }
            }
        }
        if (done) {
            idle = 0;
            continue;
        }
        if (closed_.load(std::memory_order_acquire) && all_rings_empty(w)) return;
        ++idle;
        if (idle < kSpinPasses) {
            cpu_relax();
        } else if (idle < kSpinPasses + kYieldPasses) {
            std::this_thread::yield();
        } else {
            w.naps.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(kNap);
        }
    }
}

void MatchingEngine::apply(Worker& w, const Command& cmd, std::vector<Trade>& out) {
    out.clear();
    switch (cmd.kind) {
        case Command::Kind::Add: {
            Result r{Result::Kind::Add, cmd.order.id, true, 0, 0, 0};
            try {
                w.book.add(cmd.order, out);
            } catch (const OrderBookError&) {
                r.ok = false;
                emit_result(w, r);
                return;
            }
            for (const Trade& t : out) r.filled += t.qty;
            if (cmd.order.type == OrderType::Limit) r.rested = cmd.order.qty - r.filled;
            else r.dropped = cmd.order.qty - r.filled;
            emit_trades(w, out);
            emit_result(w, r);
            return;
        }
        case Command::Kind::Cancel: {
            emit_result(w, Result{Result::Kind::Cancel, cmd.id, w.book.cancel(cmd.id), 0, 0, 0});
            return;
        }
        case Command::Kind::Modify: {
            Result r{Result::Kind::Modify, cmd.id, true, 0, 0, 0};
            try {
                w.book.modify(cmd.id, cmd.has_qty ? std::optional<Qty>(cmd.qty) : std::nullopt,
                              cmd.has_price ? std::optional<Price>(cmd.price) : std::nullopt, out);
            } catch (const OrderBookError&) {
                r.ok = false;
                emit_result(w, r);
                return;
            }
            for (const Trade& t : out) r.filled += t.qty;
            if (const Order* o = w.book.get(cmd.id)) r.rested = o->remaining;
            emit_trades(w, out);
            emit_result(w, r);
            return;
        }
    }
}

void MatchingEngine::emit_trades(Worker& w, const std::vector<Trade>& out) {
    if (!on_trade_) return;
    for (const Trade& t : out) {
        try {
            on_trade_(w.id, t);
        } catch (...) {
            ++w.callback_errors;  // never let a callback kill the matcher
        }
    }
}

void MatchingEngine::emit_result(Worker& w, const Result& r) {
    if (!on_result_) return;
    try {
        on_result_(w.id, r);
    } catch (...) {
        ++w.callback_errors;
    }
}

// ---------- lookups and reads ----------

SymbolId MatchingEngine::symbol_id(const std::string& symbol) const {
    auto it = ids_.find(symbol);
    if (it == ids_.end()) throw std::out_of_range("unknown symbol " + symbol);
    return it->second;
}

const std::string& MatchingEngine::symbol_name(SymbolId id) const { return workers_.at(id)->symbol; }

std::optional<Price> MatchingEngine::best_bid(const std::string& s) const {
    return read(s, [](const OrderBook& b) { return b.best_bid(); });
}
std::optional<Price> MatchingEngine::best_ask(const std::string& s) const {
    return read(s, [](const OrderBook& b) { return b.best_ask(); });
}
std::optional<Price> MatchingEngine::spread(const std::string& s) const {
    return read(s, [](const OrderBook& b) { return b.spread(); });
}
std::pair<std::optional<Price>, std::optional<Price>> MatchingEngine::top_of_book(const std::string& s) const {
    return read(s, [](const OrderBook& b) { return std::make_pair(b.best_bid(), b.best_ask()); });
}
std::vector<std::pair<Price, Qty>> MatchingEngine::depth(const std::string& s, Side side, std::size_t levels) const {
    return read(s, [&](const OrderBook& b) { return b.depth(side, levels); });
}
std::vector<Trade> MatchingEngine::trades(const std::string& s) const {
    return read(s, [](const OrderBook& b) { return b.trades(); });
}
Qty MatchingEngine::resting_qty(const std::string& s) const {
    return read(s, [](const OrderBook& b) { return b.resting_qty(); });
}
std::string MatchingEngine::snapshot(const std::string& s) const {
    return read(s, [](const OrderBook& b) { return b.to_string(); });
}
std::size_t MatchingEngine::callback_errors(const std::string& s) const {
    const Worker& w = *workers_[symbol_id(s)];
    std::lock_guard lock(w.book_mtx);
    return w.callback_errors;
}

void MatchingEngine::keep_history(bool on) {
    for (auto& w : workers_) {
        std::lock_guard lock(w->book_mtx);
        w->book.keep_history(on);
    }
}

std::size_t MatchingEngine::idle_naps() const {
    std::size_t total = 0;
    for (const auto& w : workers_) total += w->naps.load(std::memory_order_relaxed);
    return total;
}

}  // namespace ob
