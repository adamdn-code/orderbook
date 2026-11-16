# Order book: an exchange matching engine, tested on real Nasdaq data

[![CI](https://github.com/adamdn-code/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/adamdn-code/orderbook/actions/workflows/ci.yml)

**[Playable working paper](https://orderbook-eosin.vercel.app/playground.html)** · **[Order book replay](https://orderbook-eosin.vercel.app/)** · **[Paper (PDF)](https://orderbook-eosin.vercel.app/paper.pdf)**

A C++ limit order book and lock-free matching engine, used to rebuild every stock's book from a full day of Nasdaq's raw TotalView-ITCH feed (368M messages), and then to test a machine-learning market maker on it.

> **Abstract.** A market maker quotes Apple on a day of real Nasdaq order flow, with its quotes' queue positions tracked exactly from the order-level feed. A gradient-boosted model of the next mid-price move reaches a test AUC of **0.652**, against **0.603** for the standard book-imbalance signal (gain +0.048, 95% block-bootstrap CI +0.041 to +0.054), and is used to pull quotes it expects to be run over. With every threshold and activity rate chosen on a tuning hour before the test afternoon is run, the filter cuts the afternoon's loss from **−$1,147 to −$470** at zero latency, and the ML filters lose less per share than imbalance in 126 of 132 matched settings. On a second, untouched day the frozen models still beat imbalance (AUC 0.648 vs 0.605). Nothing turns profitable: the filter reduces adverse selection, it does not create an edge.

![Figure 1: running PnL on the test afternoon, with and without the next-move filter](backtest/results/ml/headline.png)

| | Result |
|---|---|
| Book, single thread | **6.1M orders/s**, p99 352 ns (2-core VM) |
| Matching engine, 8 client threads | **12.8M orders/s**, round trip p50 291 ns / p99 601 ns (laptop) |
| Replay of a full Nasdaq day | 368M messages, 8,695 books; **0 of 198M** executions, cancels and replaces named an order the book lacked; 99.2% of executions hit the order first in line |
| Next-move model, held-out afternoon | AUC **0.652** vs 0.603 for imbalance |
| Market maker, same afternoon, 0 latency | loss **−$1,147 → −$470** with the filter (rates picked before testing) |
| Inference in C++ next to the book | 50-tree model, **405 ns** per prediction, bit-identical to Python |

**Where things are:** `cpp/` the C++ book and engine · `cpp/itch/` the ITCH parser and replay · `backtest/` the market maker, ML models and evaluation · `orderbook.py`, `engine.py` the original Python book · `docs/` the site.

## The C++ book and engine
`cpp/` has a C++17 book and a lock-free multithreaded engine, standard library only, with tests under ThreadSanitizer and AddressSanitizer.
- **Book:** a flat array of price levels with a bitmap of non-empty levels (next best price in one instruction), an order pool linked by index, and an open-addressing id map, so nothing allocates once warm. Against the first `std::map` version: 2.9 → **6.1M orders/s** and p99 1,127 → **352 ns** (2-core VM). A randomized test replays 300k operations through both books and requires identical trades and depth after every step.
- **Engine:** one matching thread per symbol and one lock-free single-producer/single-consumer ring per client per symbol, so no two threads ever write the same memory. From a mutex-and-future first version to this: 0.20 → **12.8M orders/s** with 8 client threads (64×), round trip p50 291 ns, p99 601 ns.

See `cpp/README.md` for how each step got there.

## Replaying real Nasdaq data
`cpp/itch/` parses Nasdaq's raw TotalView-ITCH 5.0 feed and rebuilds every stock's order book with the C++ book. On a full real day (2019-01-30): 368 million messages and 8,695 stocks. **0 of 198 million** executions, cancels, deletes and replaces named an order the book didn't have, the book was empty at the close as Nasdaq's is, and 99.2% of executions hit the order the book had first in line. See `cpp/itch/README.md`.

![AAPL on 2019-01-30, rebuilt from Nasdaq ITCH](cpp/itch/results/AAPL_2019-01-30.png)

## Market making on real Nasdaq data
`backtest/real_mm.py` runs a market maker against all 1.67M of Apple's order events on 2019-01-30, with quotes as shadow orders whose queue position is tracked exactly from the order-level feed. Quoting at the best bid and ask all day **loses money**. Fills average 0.6¢ better than the mid, but within 0.1 s the mid has moved about 1¢ against them (adverse selection). Every step slower, from 0 to 100 ms, loses more: −$2.7k to −$10.9k for the day. Pulling quotes when the book is lopsided cuts the loss by about 70%, but by trading less, not by trading better. See `backtest/README.md`.

![Markouts on real AAPL fills](backtest/results/real/markouts.png)

## Predicting the next price move (ML)
`backtest/ml_signal.py` trains gradient-boosted trees on 19 causal order-flow features to predict AAPL's next mid-price move:
- **Prediction:** with a chronological split and a held-out afternoon, test **AUC is 0.652 against 0.603 for order-book imbalance** (+0.048, 95% block-bootstrap CI +0.041 to +0.054).
- **Other stocks:** the same recipe beats imbalance on MSFT, INTC, SPY, NVDA and AMZN, but a model fitted on AAPL doesn't transfer to one-tick stocks.
- **Latency:** a latency-aware version (predict the first move after your cancel can land) helps when you're slow.
- **Fills:** a second model trained on the market maker's own fills ranks toxic fills.
- **Backtest:** as quote filters with matched activity, the ML filters lose less per share than imbalance in 126 of 132 settings. With activity rates picked on a tuning hour before touching the test afternoon, the best ML filter cuts the loss by 60–76% against quoting blindly, though nothing turns profitable.
- **A second, untouched day:** on 2019-12-30, with everything frozen from day 1, the model still gets AUC 0.648 vs 0.605. The fill-toxicity filter cuts the day's loss by 40–72% against quoting blindly and beats every other filter at every latency.
- **SPY:** SPY lead-lag features added nothing.
- **C++:** features and trees run in C++ with bit-identical output to Python. A 50-tree model keeps nearly all of the AUC (0.650 vs 0.652) at **405 ns per prediction**.

See `backtest/README.md` and the [playground](https://orderbook-eosin.vercel.app/playground.html).

![Loss per share at matched activity](backtest/results/ml/filters.png)

## Python bindings and market-making backtest
`backtest/` drives the C++ book from Python through `obfast`, a CPython extension module (12.9× faster than the pure-Python book on the same orders). It uses it to backtest a market-making strategy against simulated informed and uninformed order flow, sweeping quote width and inventory skew. Inventory skew cut drawdowns 7–11× and raised risk-adjusted return about 4×. See `backtest/README.md`.

![Parameter sweep](backtest/results/sweep.png)

## The Python book

A single-threaded limit order book in Python with price-time priority, like the matching engine an exchange runs. No dependencies.

### Features
- Limit, market, and IOC (immediate-or-cancel) orders
- Price-time priority: best price first, then first-come-first-served
- Partial fills; trades execute at the resting order's price
- Cancel and modify (shrinking keeps your place in line; a price change or size increase sends you to the back)
- Best bid/ask, spread, and depth by price level

### How it works
Each side keeps a `dict` of price → `deque` of orders (FIFO for time priority) and a heap of prices for O(log n) best-price lookup. Emptied levels are removed from the heap lazily. Prices are integers (cents) so comparisons are exact.

### Multithreaded engine (`engine.py`)
`MatchingEngine` wraps one `OrderBook` per symbol:
- Any number of client threads call `submit`, `cancel`, `modify` at once. Each call goes into that symbol's thread-safe queue and returns a `Future` right away.
- Each symbol has a single matching thread (single writer), so price-time priority is never broken by a race. Different symbols match in parallel.
- Reads (`best_bid`, `depth`, ...) take a per-book lock, so they always see a consistent book.
- Bad orders come back as exceptions on the `Future`; an optional `on_trade` callback fires for every fill.

Why not a buy thread and a sell thread? Every buy can hit a sell, so both threads would lock the same book on every match and you'd pay for threads without getting parallelism. Real exchanges use one writer per instrument.

Python's GIL means threads give concurrency here, not a CPU speedup. The same design in C++/Rust, or one process per symbol, runs truly in parallel.

The stress test runs 8 threads × 2,000 orders across 3 symbols and checks that no quantity is lost and no book ever crosses.

### Run
```bash
python orderbook.py          # single-book demo
python engine.py             # 8 threads, 40k orders, 3 symbols
python -m unittest -v        # all tests
```

## Next steps
- One process per symbol (multiprocessing) for real CPU parallelism
- Stop orders, FOK (fill-or-kill)
