# OrderBook (C++)

C++17 order book and lock-free multithreaded matching engine. Standard library only.

## Layout
```
include/orderbook.hpp   single-threaded book (limit / market / IOC, cancel, modify)
include/spsc_ring.hpp   lock-free single-producer/single-consumer ring buffer
include/engine.hpp      matching engine: one thread per symbol, one ring per client per symbol
src/                    implementations
tests/tests.cpp         30 tests incl. a 300k-step randomized comparison against the
                        original book, ring ordering, backpressure, and an 8-thread stress test
tests/reference_book.*  the original std::map book, kept to check and benchmark against
bench/bench.cpp         throughput + latency benchmarks
bindings/obfast.cpp     Python extension module exposing the book (see ../backtest)
```

## Build and run
```bash
make test     # build + run tests
make bench    # build + run benchmark
make tsan     # run tests under ThreadSanitizer (data-race checker)
make asan     # run tests under AddressSanitizer + UBSan (memory/UB checker)
make python   # build the obfast Python module (needs python3-dev)
```

## Usage
```cpp
ob::MatchingEngine engine({"AAPL", "MSFT"},
    [](ob::SymbolId s, const ob::Trade& t) { /* fill */ },
    [](ob::SymbolId s, const ob::Result& r) { /* order accepted / rejected / done */ });

ob::Session& session = engine.connect();   // one per client thread
session.submit("AAPL", ob::Order::limit(1, ob::Side::Buy, 100, 18'950));
session.cancel("AAPL", 1);
engine.drain();                            // wait until everything sent is matched
```
Callbacks for a symbol always run on that symbol's matching thread, one at a time.

## How it works
**Book.** Built so that adding, matching and cancelling allocate no memory once it's warmed up:
- *Flat array of price levels*, indexed by `price - base`. Finding a level is an array index instead of a `std::map` tree walk. The array grows in either direction as new prices appear.
- *Bitmap of non-empty levels.* When the best level empties, the next best price is found 64 levels at a time with one CPU instruction (count leading/trailing zeros). Because the book is never crossed, everything below the best bid is a bid and everything above the best ask is an ask, so one bitmap serves both sides.
- *Order pool.* Orders live in one vector and are linked into their level's FIFO queue by index (an intrusive doubly linked list), so cancel is O(1). Freed slots go on a free list and get reused.
- *Open-addressing hash map* from order id to pool slot: one flat array with linear probing and backward-shift deletion, instead of `std::unordered_map`, which allocates a node per entry.
- *Correctness check.* The old `std::map` book is kept in `tests/`. A randomized test replays 300k adds, cancels and modifies, including large price jumps, through both books and requires identical trades, best prices and depth after every step.

Limits: prices in one book must stay within 4M ticks of each other (configurable), and order ids must be unique among resting orders.

**Engine.**
- *One ring per (client, symbol).* Every queue has exactly one writer and one reader, so it needs no lock, just two atomic indices. Each index sits on its own 64-byte cache line, so producer and consumer never invalidate each other's memory (no false sharing). Each side caches the other's index and only rereads it when the ring looks full or empty.
- *One matching thread per symbol.* It polls its rings round-robin, up to 256 commands per ring per pass so no client starves the others. Being the only writer to its book keeps price-time priority exact.
- *No allocation per order.* Commands are plain structs copied into the ring. Results come back through callbacks instead of a `std::future` per order.
- *Idle strategy.* With no work, the matcher spins with the CPU `pause` instruction, then yields, then naps 50 µs. A busy engine never sleeps, and an idle one doesn't burn a core forever.
- *Safe shutdown.* `submit()` and `stop()` do a small handshake with two sequentially consistent atomics, so an order can't slip in after a matcher has decided it's done.

## Evolution and results
### Book (single thread, 2M random orders)
| | Machine | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| Old: `std::map` + `std::list` + `unordered_map` | 2-core VM | 2.90 M/s | 153 ns | 1,127 ns | 3,202 ns |
| New: flat levels + bitmap + pool + open addressing | 2-core VM | **6.13 M/s** | **110 ns** | **352 ns** | **593 ns** |
| Old | 16-thread laptop | 2.63 M/s | 150 ns | 2,205 ns | 7,063 ns |
| New | 16-thread laptop | **3.54 M/s** | 160 ns | **692 ns** | **1,112 ns** |

The tail latency improves most (3–6× at p99/p99.9): the old book's slow orders were the ones that hit the memory allocator or a cache miss in the tree. Median latency is about the same because a typical order touches only the best level either way, and at ~150 ns the timer's own overhead is part of the number.

### Engine
Engine versions, same workload (8 client threads, 3 symbols, 2M random orders):

| Version | 2-core cloud VM | 16-thread laptop |
|---|---|---|
| v1: mutex queue + condition variable + future per order | 0.75 M/s | 0.20 M/s |
| v2: wake matcher only when asleep | 0.79 M/s | 0.27 M/s |
| v3: lock-free SPSC rings, no per-order allocation | 3.08 M/s | 3.73 M/s (18× v1) |
| v4: v3 + new book | **12.36 M/s** | **12.75 M/s** (64× v1) |

v1 got *slower* on more cores: every order woke a sleeping thread through the OS, and 8 clients fought over 3 locks whose cache lines bounced between cores. v3 removes all shared writes between clients.

On the laptop, v4 does 16.8 M orders/s on a 40k burst (v1: 0.05 M/s) and 16.5 M/s with 2 clients. Engine round trip (submit → result callback) is p50 291 ns, p99 601 ns (v3: 301 ns / 1,032 ns).

After v3, each matching thread was limited by the book itself, which is why v4 rebuilt the book's data structures.

## Next steps
- Pin matching threads to cores
- Stop orders, FOK (fill-or-kill)
