// Replay a day of order events through the C++ book, compute the order-flow features and score
// the next-move model on every snapshot, all in C++. Two jobs:
//
//   1. parity: --dump writes every snapshot's features and probability, which
//      backtest/cpp_parity.py compares with the Python pipeline value by value;
//   2. cost: how many nanoseconds the model adds to each decision. The day is replayed three
//      ways (book only; book + features; book + features + model) and the differences are divided
//      by the number of decisions. A tight predict-only loop is timed as well.
//
//   ml_replay AAPL_events.csv next_move.gbdt [--dump out.bin] [--json out.json] [--repeat 5]
//
// Event handling mirrors ml_signal.snapshots() exactly (same skips for sub-penny and off-book orders).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "features.hpp"
#include "gbdt.hpp"
#include "orderbook.hpp"

namespace {

constexpr std::int64_t NS = 1'000'000'000;
constexpr std::int64_t kQuoteStart = std::int64_t(9.5 * 3600 + 5 * 60) * NS;
constexpr std::int64_t kQuoteEnd = std::int64_t(15 * 3600 + 55 * 60) * NS;

struct Event {
    std::int64_t ts;
    char type;
    std::uint64_t ref;
    int side;  // 0 buy, 1 sell, -1 unknown
    std::int64_t shares;
    std::int64_t price;  // ITCH units: dollars * 10,000
    std::uint64_t new_ref;
};

std::vector<Event> read_events(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "can't open %s\n", path);
        std::exit(1);
    }
    std::string line;
    std::getline(in, line);  // header
    std::vector<Event> ev;
    ev.reserve(2'000'000);
    while (std::getline(in, line)) {
        const char* p = line.c_str();
        char* end = nullptr;
        Event e{};
        e.ts = std::strtoll(p, &end, 10);
        p = end + 1;
        e.type = *p;
        p += 2;
        e.ref = std::strtoull(p, &end, 10);
        p = end + 1;
        e.side = *p == 'B' ? 0 : *p == 'S' ? 1 : -1;
        while (*p != ',') ++p;
        ++p;
        e.shares = std::strtoll(p, &end, 10);
        p = end + 1;
        e.price = std::strtoll(p, &end, 10);
        p = end + 1;
        e.new_ref = std::strtoull(p, &end, 10);
        ev.push_back(e);
    }
    std::stable_sort(ev.begin(), ev.end(), [](const Event& a, const Event& b) { return a.ts < b.ts; });
    return ev;
}

enum class Mode { BookOnly, Features, Model };

struct Pass {
    double seconds = 0;
    std::size_t snapshots = 0, recorded = 0;
    double checksum = 0;
};

Pass replay(const std::vector<Event>& ev, const ml::Gbdt& model, Mode mode, std::vector<double>* dump) {
    ob::OrderBook book;
    book.keep_history(false);
    ml::OrderFlowFeatures feats(kQuoteStart - 15 * NS, kQuoteEnd);
    std::vector<ob::Trade> trades;
    double x[ml::kFeatures];
    Pass out;
    const auto t0 = std::chrono::steady_clock::now();
    auto add = [&](std::uint64_t id, int side, std::int64_t qty, std::int64_t price_c) {
        try {
            book.add(ob::Order::limit(id, side == 0 ? ob::Side::Buy : ob::Side::Sell, qty, price_c), trades);
        } catch (const ob::OrderBookError&) {
        }
        trades.clear();
    };
    const std::size_t n = ev.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Event& e = ev[i];
        const std::int64_t price_c = e.price / 100;
        switch (e.type) {
            case 'A':
                if (e.price % 100 == 0 && e.side >= 0) add(e.ref, e.side, e.shares, price_c);
                break;
            case 'E': case 'C': case 'X': {
                const ob::Order* o = book.get(e.ref);
                if (!o) break;
                const std::int64_t rem = o->remaining;
                if (e.type != 'X' && mode != Mode::BookOnly)
                    feats.on_execution(o->side == ob::Side::Sell, std::min(e.shares, rem));
                if (e.shares >= rem) book.cancel(e.ref);
                else {
                    book.modify(e.ref, rem - e.shares, std::nullopt, trades);
                    trades.clear();
                }
                break;
            }
            case 'D':
                book.cancel(e.ref);
                break;
            case 'U': {
                const ob::Order* o = book.get(e.ref);
                if (!o) break;
                const int s = o->side == ob::Side::Buy ? 0 : 1;
                book.cancel(e.ref);
                if (e.price % 100 == 0) add(e.new_ref, s, e.shares, price_c);
                break;
            }
            default:
                break;
        }
        if (i + 1 < n && ev[i + 1].ts == e.ts) continue;  // mid-sweep
        if (mode == Mode::BookOnly) {
            out.checksum += double(book.best_bid().value_or(0));
            continue;
        }
        const auto b = book.depth(ob::Side::Buy, 5);
        const auto a = book.depth(ob::Side::Sell, 5);
        if (b.empty() || a.empty()) continue;
        ++out.snapshots;
        if (!feats.on_snapshot(e.ts, b.data(), int(b.size()), a.data(), int(a.size()), x)) continue;
        ++out.recorded;
        if (mode == Mode::Model) {
            const double p = model.proba(x);
            out.checksum += p;
            if (dump) {
                dump->push_back(double(e.ts));
                dump->insert(dump->end(), x, x + ml::kFeatures);
                dump->push_back(p);
            }
        } else {
            out.checksum += x[1];
        }
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ml_replay EVENTS.csv MODEL.gbdt [--dump out.bin] [--json out.json] [--repeat N]\n");
        return 2;
    }
    const char* dump_path = nullptr;
    const char* json_path = nullptr;
    int repeat = 5;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) dump_path = argv[++i];
        else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
        else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::atoi(argv[++i]);
    }
    const auto ev = read_events(argv[1]);
    const auto model = ml::Gbdt::load(argv[2]);
    std::printf("%zu events, model: %zu trees, %zu nodes, max depth %d, %zu features\n", ev.size(), model.n_trees(),
                model.n_nodes(), model.max_depth(), model.n_features());

    std::vector<double> dump;
    replay(ev, model, Mode::Model, dump_path ? &dump : nullptr);  // warm-up + dump
    double best[3] = {1e30, 1e30, 1e30};
    Pass last[3];
    for (int r = 0; r < repeat; ++r) {
        for (int m = 0; m < 3; ++m) {
            last[m] = replay(ev, model, Mode(m), nullptr);
            best[m] = std::min(best[m], last[m].seconds);
        }
    }
    const double rec = double(last[2].recorded);
    const double feat_ns = (best[1] - best[0]) / rec * 1e9;   // features (incl. reading 5 levels) per decision
    const double model_ns = (best[2] - best[1]) / rec * 1e9;  // model per decision, in the replay

    // predict-only loop over the recorded feature vectors (warm cache, back to back)
    std::vector<double> X;
    {
        std::vector<double> d;
        replay(ev, model, Mode::Model, &d);
        const std::size_t stride = ml::kFeatures + 2;
        for (std::size_t i = 0; i < d.size(); i += stride) X.insert(X.end(), d.begin() + long(i) + 1, d.begin() + long(i) + 1 + ml::kFeatures);
    }
    const std::size_t rows = X.size() / ml::kFeatures;
    std::vector<double> lat(rows);
    double sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < rows; ++i) sink += model.proba(&X[i * ml::kFeatures]);
    const double loop_ns = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / double(rows) * 1e9;
    // per-call latency distribution on a sample (each timed on its own; includes ~20 ns clock overhead)
    const std::size_t step = std::max<std::size_t>(1, rows / 200'000);
    std::vector<double> single;
    for (std::size_t i = 0; i < rows; i += step) {
        const auto a = std::chrono::steady_clock::now();
        sink += model.proba(&X[i * ml::kFeatures]);
        const auto b = std::chrono::steady_clock::now();
        single.push_back(std::chrono::duration<double, std::nano>(b - a).count());
    }
    std::sort(single.begin(), single.end());
    auto pct = [&](double q) { return single[std::size_t(q * double(single.size() - 1))]; };

    std::printf("decisions (recorded snapshots): %.0f\n", rec);
    std::printf("replay, book only:            %.3f s  (%.0f ns/event)\n", best[0], best[0] / double(ev.size()) * 1e9);
    std::printf("  + features:                 %.3f s  -> %.0f ns per decision\n", best[1], feat_ns);
    std::printf("  + model:                    %.3f s  -> %.0f ns per decision\n", best[2], model_ns);
    std::printf("predict-only loop:            %.0f ns per prediction\n", loop_ns);
    std::printf("single predictions:           p50 %.0f ns, p99 %.0f ns, p99.9 %.0f ns (incl. clock overhead)\n", pct(0.5),
                pct(0.99), pct(0.999));
    std::printf("(checksum %.6f)\n", sink + last[2].checksum);

    if (dump_path) {
        std::ofstream out(dump_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(dump.data()), std::streamsize(dump.size() * sizeof(double)));
        std::printf("wrote %zu rows to %s\n", dump.size() / (ml::kFeatures + 2), dump_path);
    }
    if (json_path) {
        std::ofstream j(json_path);
        j << "{\"events\":" << ev.size() << ",\"decisions\":" << last[2].recorded << ",\"max_depth\":" << model.max_depth() << ",\"trees\":" << model.n_trees()
          << ",\"nodes\":" << model.n_nodes() << ",\"replay_book_s\":" << best[0] << ",\"replay_features_s\":" << best[1]
          << ",\"replay_model_s\":" << best[2] << ",\"features_ns_per_decision\":" << feat_ns
          << ",\"model_ns_per_decision\":" << model_ns << ",\"predict_loop_ns\":" << loop_ns << ",\"predict_p50_ns\":" << pct(0.5)
          << ",\"predict_p99_ns\":" << pct(0.99) << ",\"predict_p999_ns\":" << pct(0.999) << "}\n";
    }
    return 0;
}
