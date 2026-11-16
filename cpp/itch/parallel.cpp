#include "parallel.hpp"

#include <algorithm>
#include <cstring>

namespace itch {

namespace {

inline void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

// Spin briefly, then yield: the other side is usually only microseconds away.
template <class Fn>
void wait_until(Fn&& ready) {
    for (int spins = 0; !ready(); ++spins) {
        if (spins < 256) relax();
        else std::this_thread::yield();
    }
}

}  // namespace

ParallelReplayer::ParallelReplayer(const ReplayOptions& options, unsigned threads) {
    threads = std::max(1u, threads);
    for (unsigned i = 0; i < threads; ++i) {
        auto w = std::make_unique<Worker>(options);
        for (std::size_t b = 0; b < kBatches; ++b) {
            w->storage.push_back(std::make_unique<Batch>());
            w->storage.back()->bytes.resize(kBatchBytes);
            w->empty.try_push(w->storage.back().get());
        }
        workers_.push_back(std::move(w));
    }
    for (auto& w : workers_) {
        w->current = take_empty(*w);
        Worker* raw = w.get();
        w->thread = std::thread([raw] { run(*raw); });
    }
}

ParallelReplayer::~ParallelReplayer() { finish(); }

ParallelReplayer::Batch* ParallelReplayer::take_empty(Worker& w) {
    Batch* b = nullptr;
    wait_until([&] {
        Batch** f = w.empty.front();
        if (!f) return false;
        b = *f;
        w.empty.pop();
        return true;
    });
    b->used = 0;
    b->last = false;
    return b;
}

void ParallelReplayer::send(Worker& w) {
    Batch* b = w.current;
    wait_until([&] { return w.full.try_push(b); });
    w.current = b->last ? nullptr : take_empty(w);
}

void ParallelReplayer::apply(const std::uint8_t* m, std::size_t len) {
    ++messages_;
    // Messages too short to carry a locate (malformed) go to worker 0, which counts them.
    const std::uint16_t locate = len >= 3 ? std::uint16_t(m[1] << 8 | m[2]) : 0;
    Worker& w = *workers_[locate % workers_.size()];
    if (w.current->used + 2 + len > kBatchBytes) send(w);
    std::uint8_t* p = w.current->bytes.data() + w.current->used;
    p[0] = std::uint8_t(len >> 8);
    p[1] = std::uint8_t(len);
    std::memcpy(p + 2, m, len);
    w.current->used += 2 + len;
}

void ParallelReplayer::run(Worker& w) {
    for (;;) {
        Batch* b = nullptr;
        wait_until([&] {
            Batch** f = w.full.front();
            if (!f) return false;
            b = *f;
            w.full.pop();
            return true;
        });
        for (std::size_t i = 0; i < b->used;) {
            const std::size_t len = std::size_t(b->bytes[i]) << 8 | b->bytes[i + 1];
            w.replayer.apply(b->bytes.data() + i + 2, len);
            i += 2 + len;
        }
        const bool last = b->last;
        w.empty.try_push(b);  // never full: there are only kBatches batches
        if (last) return;
    }
}

void ParallelReplayer::finish() {
    if (finished_) return;
    finished_ = true;
    for (auto& w : workers_) {
        w->current->last = true;
        send(*w);
    }
    for (auto& w : workers_) w->thread.join();
}

std::uint64_t ParallelReplayer::malformed() const {
    std::uint64_t n = 0;
    for (const auto& w : workers_) n += w->replayer.malformed();
    return n;
}

std::array<std::uint64_t, 256> ParallelReplayer::by_type() const {
    std::array<std::uint64_t, 256> t{};
    for (const auto& w : workers_)
        for (int c = 0; c < 256; ++c) t[c] += w->replayer.by_type()[c];
    return t;
}

Counts ParallelReplayer::totals() const {
    Counts c;
    for (const auto& w : workers_) c.merge(w->replayer.totals());
    return c;
}

std::uint64_t ParallelReplayer::live_orders() const {
    std::uint64_t n = 0;
    for (const auto& w : workers_) n += w->replayer.live_orders();
    return n;
}

std::uint64_t ParallelReplayer::peak_live_upper_bound() const {
    std::uint64_t n = 0;
    for (const auto& w : workers_) n += w->replayer.peak_live_orders();
    return n;
}

std::vector<SymbolSummary> ParallelReplayer::summaries() const {
    std::vector<SymbolSummary> out;
    for (const auto& w : workers_) {
        auto s = w->replayer.summaries();
        out.insert(out.end(), s.begin(), s.end());
    }
    std::stable_sort(out.begin(), out.end(), [](const SymbolSummary& a, const SymbolSummary& b) {
        return a.counts.shares_traded != b.counts.shares_traded ? a.counts.shares_traded > b.counts.shares_traded
                                                                : a.symbol < b.symbol;
    });
    return out;
}

}  // namespace itch
