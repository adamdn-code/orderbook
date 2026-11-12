# OrderBook (C++)

C++17 order book and lock-free multithreaded matching engine. Standard library only.

## Layout
```
include/orderbook.hpp   single-threaded book (limit / market / IOC, cancel, modify)
include/spsc_ring.hpp   lock-free single-producer/single-consumer ring buffer
include/engine.hpp      matching engine: one thread per symbol, one ring per client per symbol
src/                    implementations
tests/tests.cpp         24 tests incl. ring ordering, backpressure, and an 8-thread stress test
bench/bench.cpp         throughput + latency benchmarks
```

## Build and run
```bash
make test     # build + run tests
make bench    # build + run benchmark
make tsan     # run tests under ThreadSanitizer (data-race checker)
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
**Book.** Each side is a `std::map<Price, std::list<Order>>`. Bids are sorted high to low and asks low to high, so the best price is always `begin()`. Each level is a FIFO list for time priority. An `unordered_map` from order id to its list iterator makes cancel O(1) once the level is found.

**Engine.**
- *One ring per (client, symbol).* Every queue has exactly one writer and one reader, so it needs no lock, just two atomic indices. Each index sits on its own 64-byte cache line, so producer and consumer never invalidate each other's memory (no false sharing). Each side caches the other's index and only rereads it when the ring looks full or empty.
- *One matching thread per symbol.* It polls its rings round-robin, up to 256 commands per ring per pass so no client starves the others. Being the only writer to its book keeps price-time priority exact.
- *No allocation per order.* Commands are plain structs copied into the ring. Results come back through callbacks instead of a `std::future` per order.
- *Idle strategy.* With no work, the matcher spins with the CPU `pause` instruction, then yields, then naps 50 µs. A busy engine never sleeps, and an idle one doesn't burn a core forever.
- *Safe shutdown.* `submit()` and `stop()` do a small handshake with two sequentially consistent atomics, so an order can't slip in after a matcher has decided it's done.

## Evolution and results
Three engine versions, same workload (8 client threads, 3 symbols, 2M random orders):

| Version | 2-core cloud VM | 16-thread laptop |
|---|---|---|
| v1: mutex queue + condition variable + future per order | 0.75 M/s | 0.20 M/s |
| v2: wake matcher only when asleep | 0.79 M/s | 0.27 M/s |
| v3: lock-free SPSC rings, no per-order allocation | 3.08 M/s | **3.73 M/s** (18× v1) |

v1 got *slower* on more cores: every order woke a sleeping thread through the OS, and 8 clients fought over 3 locks whose cache lines bounced between cores. v3 removes all shared writes between clients.

On the laptop, v3 also does 9.28 M orders/s on a 40k burst (v1: 0.05 M/s) and 5.10 M/s with 2 clients. Engine round trip (submit → result callback) is p50 301 ns, p99 1.0 µs.

Single book: ~2.1–2.8 M orders/s, p50 ~140–170 ns per order. Each matching thread is now limited by the book itself, which is why the next step is the book's data structures, not the engine.

## Next steps
- Pool-allocate orders and use a flat array of price levels instead of `std::map`/`std::list`
- Pin matching threads to cores
- Stop orders, FOK (fill-or-kill)
