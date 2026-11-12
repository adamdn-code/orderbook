# OrderBook

A single-threaded limit order book in Python with price-time priority, like the matching engine an exchange runs. No dependencies.

## Features
- Limit, market, and IOC (immediate-or-cancel) orders
- Price-time priority: best price first, then first-come-first-served
- Partial fills; trades execute at the resting order's price
- Cancel and modify (shrinking keeps your place in line; a price change or size increase sends you to the back)
- Best bid/ask, spread, and depth by price level

## How it works
Each side keeps a `dict` of price → `deque` of orders (FIFO for time priority) and a heap of prices for O(log n) best-price lookup. Emptied levels are removed from the heap lazily. Prices are integers (cents) so comparisons are exact.

## Multithreaded engine (`engine.py`)
`MatchingEngine` wraps one `OrderBook` per symbol:
- Any number of client threads call `submit`, `cancel`, `modify` at once. Each call goes into that symbol's thread-safe queue and returns a `Future` right away.
- Each symbol has a single matching thread (single writer), so price-time priority is never broken by a race. Different symbols match in parallel.
- Reads (`best_bid`, `depth`, ...) take a per-book lock, so they always see a consistent book.
- Bad orders come back as exceptions on the `Future`; an optional `on_trade` callback fires for every fill.

Why not a buy thread and a sell thread? Every buy can hit a sell, so both threads would lock the same book on every match and you'd pay for threads without getting parallelism. Real exchanges use one writer per instrument.

Python's GIL means threads give concurrency here, not a CPU speedup. The same design in C++/Rust, or one process per symbol, runs truly in parallel.

The stress test runs 8 threads × 2,000 orders across 3 symbols and checks that no quantity is lost and no book ever crosses.

## Run
```bash
python orderbook.py          # single-book demo
python engine.py             # 8 threads, 40k orders, 3 symbols
python -m unittest -v        # all tests
```

## C++ version
`cpp/` has a C++17 port of the book plus a lock-free engine (one SPSC ring per client per symbol, no per-order allocation), with tests and benchmarks. See `cpp/README.md`.

## Next steps
- One process per symbol (multiprocessing) for real CPU parallelism
- Stop orders, FOK (fill-or-kill)
- Benchmark orders/second and replay real market data
