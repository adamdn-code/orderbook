// itch_replay: rebuild order books from a Nasdaq TotalView-ITCH 5.0 file.
//
//   itch_replay FILE [--symbols AAPL,MSFT] [--track AAPL] [--out DIR] [--sample-ms 1000] [--threads N]
//
// FILE can be gzipped (as Nasdaq ships it) or already decompressed.
#include "itch.hpp"
#include "parallel.hpp"
#include "replay.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <memory>
#include <thread>

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: itch_replay FILE [--symbols A,B,..] [--track SYM] [--out DIR] [--sample-ms N] [--depth N] [--events] [--threads N]\n"
                 "  --symbols   only rebuild these symbols (default: all)\n"
                 "  --track     write SYM_book.csv (top of book) and SYM_trades.csv to --out\n"
                 "  --out       directory for CSV output (default: .)\n"
                 "  --sample-ms book sample interval for --track (default: 1000)\n"
                 "  --depth     price levels per side to record for --track (default: 1)\n"
                 "  --events    also write SYM_events.csv: every order event for --track\n"
                 "  --threads   rebuild stocks on N threads (default 1; 0 = one per core). Every count is the\n"
                 "              same; only the peak resting-order count becomes an upper bound\n");
}

double pct(std::uint64_t part, std::uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string path;
    itch::ReplayOptions opts;
    unsigned threads = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--symbols") {
            std::stringstream ss(next());
            for (std::string sym; std::getline(ss, sym, ',');)
                if (!sym.empty()) opts.symbols.insert(sym);
        } else if (a == "--track") {
            opts.track = next();
        } else if (a == "--out") {
            opts.out_dir = next();
        } else if (a == "--sample-ms") {
            opts.sample_ns = std::stoull(next()) * 1'000'000ULL;
        } else if (a == "--events") {
            opts.events = true;
        } else if (a == "--threads") {
            threads = unsigned(std::stoul(next()));
            if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
        } else if (a == "--depth") {
            opts.depth = std::max(1, std::stoi(next()));
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (path.empty()) {
            path = a;
        } else {
            usage();
            return 2;
        }
    }
    if (!opts.track.empty() && !opts.symbols.empty()) opts.symbols.insert(opts.track);

    try {
        itch::Reader reader(path);
        std::unique_ptr<itch::Replayer> serial;
        std::unique_ptr<itch::ParallelReplayer> parallel;
        if (threads > 1) parallel = std::make_unique<itch::ParallelReplayer>(opts, threads);
        else serial = std::make_unique<itch::Replayer>(opts);
        std::uint64_t seen = 0;
        const auto start = std::chrono::steady_clock::now();
        auto last_report = start;
        const bool complete = reader.for_each([&](const std::uint8_t* m, std::size_t len) {
            if (parallel) parallel->apply(m, len);
            else serial->apply(m, len);
            if ((++seen & 0xFFFFF) == 0) {  // progress every ~1M messages
                const auto now = std::chrono::steady_clock::now();
                if (now - last_report > std::chrono::seconds(5)) {
                    last_report = now;
                    const double s = std::chrono::duration<double>(now - start).count();
                    std::fprintf(stderr, "  %.0fM messages, %.1f GB, %.0fs\n", seen / 1e6, reader.bytes() / 1e9, s);
                }
            }
        });
        if (parallel) parallel->finish();
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const itch::Counts t = parallel ? parallel->totals() : serial->totals();
        const std::uint64_t messages = parallel ? parallel->messages() : serial->messages();
        const std::uint64_t malformed = parallel ? parallel->malformed() : serial->malformed();
        const auto bt = parallel ? parallel->by_type() : serial->by_type();
        const auto summaries = parallel ? parallel->summaries() : serial->summaries();
        const std::uint64_t live = parallel ? parallel->live_orders() : serial->live_orders();

        std::printf("\n%s\n", path.c_str());
        if (!complete) std::printf("  WARNING: file ended partway through a message (truncated download?)\n");
        std::printf("  %llu messages, %.2f GB uncompressed, %.1f s on %u thread%s  ->  %.2f M messages/s\n",
                    (unsigned long long)messages, reader.bytes() / 1e9, secs, threads, threads > 1 ? "s" : "",
                    messages / secs / 1e6);
        std::printf("  by type:");
        for (int c = 0; c < 256; ++c)
            if (bt[c]) std::printf(" %c=%llu", char(c), (unsigned long long)bt[c]);
        std::printf("\n  malformed messages: %llu\n", (unsigned long long)malformed);

        std::printf("\nOrder books\n");
        std::printf("  symbols with activity: %zu\n", summaries.size());
        if (parallel)
            std::printf("  orders added: %llu, peak resting at once: at most %llu (summed over threads), resting at end: %llu\n",
                        (unsigned long long)t.adds, (unsigned long long)parallel->peak_live_upper_bound(),
                        (unsigned long long)live);
        else
            std::printf("  orders added: %llu, peak resting at once: %llu, resting at end: %llu\n",
                        (unsigned long long)t.adds, (unsigned long long)serial->peak_live_orders(), (unsigned long long)live);
        std::printf("  executions: %llu at the order's price (E), %llu at another price (C)\n",
                    (unsigned long long)t.executions, (unsigned long long)t.executions_priced);
        std::printf("  shares traded: %llu (incl. hidden and crosses), $%.0f notional\n",
                    (unsigned long long)t.shares_traded, t.notional);

        std::printf("\nChecks against Nasdaq\n");
        const std::uint64_t refs = t.executions + t.executions_priced + t.cancels + t.deletes + t.replaces;
        std::printf("  messages naming an order our book didn't have: %llu of %llu (%.4f%%)\n",
                    (unsigned long long)t.missing_refs, (unsigned long long)refs, pct(t.missing_refs, refs));
        std::printf("  executions/cancels larger than the order:       %llu\n", (unsigned long long)t.over_executions);
        std::printf("  adds that would have crossed our book:          %llu of %llu (%.4f%%)\n",
                    (unsigned long long)t.crossing_adds, (unsigned long long)t.adds, pct(t.crossing_adds, t.adds));
        std::printf("  orders kept off the main book (side table):     %llu (%.4f%%)\n",
                    (unsigned long long)t.side_table_adds, pct(t.side_table_adds, t.adds));
        std::printf("  stocks switched to the sub-penny book:          %llu\n", (unsigned long long)t.switched_books);
        std::printf("  executions at our best price:                   %.3f%%\n", pct(t.executions_best, t.executions));
        std::printf("  executions hitting our first order in line:     %.3f%%\n", pct(t.executions_front, t.executions));

        std::printf("\nMost traded\n  %-8s %14s %12s %12s %10s %8s\n", "symbol", "shares", "VWAP", "executions",
                    "orders", "front%");
        for (std::size_t i = 0; i < summaries.size() && i < 15; ++i) {
            const auto& s = summaries[i];
            const auto& c = s.counts;
            std::printf("  %-8s %14llu %12.4f %12llu %10llu %7.2f%%\n", s.symbol.c_str(),
                        (unsigned long long)c.shares_traded, c.shares_traded ? c.notional / double(c.shares_traded) : 0.0,
                        (unsigned long long)(c.executions + c.executions_priced), (unsigned long long)c.adds,
                        pct(c.executions_front, c.executions));
        }
        if (!opts.track.empty())
            std::printf("\nWrote %s/%s_book.csv and %s_trades.csv%s\n", opts.out_dir.c_str(), opts.track.c_str(),
                        opts.track.c_str(), opts.events ? (" and " + opts.track + "_events.csv").c_str() : "");
        return complete ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
