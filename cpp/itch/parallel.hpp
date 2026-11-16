// Multithreaded ITCH replay: stocks are split across worker threads.
//
// Every ITCH message that touches a book carries the stock's locate code, and each stock's book is
// independent of every other's, so the replay parallelises by stock. The calling thread reads the
// feed and copies each message into a batch for the worker that owns its stock (locate % threads).
// Full batches go to the worker over a lock-free single-producer/single-consumer ring (the same one
// the matching engine uses) and come back empty over a second ring, so after start-up nothing is
// allocated. Each worker runs an ordinary Replayer over its stocks' messages, in feed order.
//
// Result: every count, check and per-stock summary is identical to the single-threaded replay, and
// the tracked stock's CSVs are byte-for-byte the same (tests/itch_tests.cpp checks both). The one
// figure that does not decompose is the peak number of orders resting at once across all stocks:
// the threads see their peaks at different moments, so peak_live_upper_bound() reports the sum of
// the per-thread peaks, an upper bound.
#pragma once

#include "replay.hpp"
#include "spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace itch {

class ParallelReplayer {
public:
    ParallelReplayer(const ReplayOptions& options, unsigned threads);
    ~ParallelReplayer();
    ParallelReplayer(const ParallelReplayer&) = delete;
    ParallelReplayer& operator=(const ParallelReplayer&) = delete;

    // Called from one thread, in feed order.
    void apply(const std::uint8_t* message, std::size_t length);
    // Sends the last partial batches and waits for every worker. Call before reading results.
    void finish();

    unsigned threads() const { return unsigned(workers_.size()); }
    std::uint64_t messages() const { return messages_; }
    std::uint64_t malformed() const;
    std::array<std::uint64_t, 256> by_type() const;
    Counts totals() const;
    std::uint64_t live_orders() const;
    std::uint64_t peak_live_upper_bound() const;
    std::vector<SymbolSummary> summaries() const;

private:
    static constexpr std::size_t kBatchBytes = std::size_t(1) << 18;  // 256 KiB of length-prefixed messages
    static constexpr std::size_t kBatches = 16;                       // per worker

    struct Batch {
        std::vector<std::uint8_t> bytes;
        std::size_t used = 0;
        bool last = false;  // tells the worker to stop after this batch
    };

    struct Worker {
        explicit Worker(const ReplayOptions& o) : replayer(o) {}
        Replayer replayer;
        std::vector<std::unique_ptr<Batch>> storage;
        ob::SpscRing<Batch*, 32> full;   // reader -> worker
        ob::SpscRing<Batch*, 32> empty;  // worker -> reader
        Batch* current = nullptr;        // being filled by the reader
        std::thread thread;
    };

    void send(Worker& w);
    Batch* take_empty(Worker& w);
    static void run(Worker& w);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::uint64_t messages_ = 0;
    bool finished_ = false;
};

}  // namespace itch
