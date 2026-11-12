"""Multithreaded matching engine built on the single-threaded OrderBook.

Design (single writer per book):
  - Any number of client threads call submit()/cancel()/modify() at the same
    time. Each call drops a command into that symbol's thread-safe queue and
    immediately gets back a Future.
  - Each symbol has exactly one matching thread that pulls commands off its
    queue in arrival order and applies them to its book. Because only one
    thread ever writes to a given book, matching stays deterministic and
    price-time priority is never broken by a race.
  - Different symbols match in parallel on their own threads.
  - Reads (best_bid, depth, ...) take a per-book lock so they never see a book
    halfway through a match.

Why not one thread for buys and one for sells? Every buy can trade against a
sell, so both threads would need to lock the same book on every match and you
would get the cost of threads without the parallelism. Exchanges usually use
one writer per instrument and scale by running instruments in parallel.

Note on Python: the GIL means only one thread runs Python bytecode at a time,
so this design gives you concurrency (many clients, no blocking, clean
ordering) rather than a raw CPU speedup. The same architecture in C++/Rust,
or with one process per symbol, does run in true parallel.
"""

from __future__ import annotations

import threading
import queue
from concurrent.futures import Future
from typing import Callable, Iterable

from orderbook import Order, OrderBook, Side, Trade

TradeCallback = Callable[[str, Trade], None]

_STOP = object()


class EngineStopped(RuntimeError):
    pass


class _SymbolWorker:
    def __init__(self, symbol: str, on_trade: TradeCallback | None) -> None:
        self.symbol = symbol
        self.book = OrderBook()
        self.lock = threading.Lock()
        self.inbox: queue.Queue = queue.Queue()
        self.on_trade = on_trade
        self.callback_errors = 0
        self.thread = threading.Thread(target=self._run, name=f"match-{symbol}", daemon=True)

    def _run(self) -> None:
        while True:
            item = self.inbox.get()
            if item is _STOP:
                return
            fn, args, kwargs, future = item
            if not future.set_running_or_notify_cancel():
                continue
            try:
                with self.lock:
                    result = fn(self.book, *args, **kwargs)
            except Exception as exc:  # bad order etc. goes back to the caller
                future.set_exception(exc)
                continue
            future.set_result(result)
            if self.on_trade and isinstance(result, list):
                for trade in result:
                    try:
                        self.on_trade(self.symbol, trade)
                    except Exception:
                        self.callback_errors += 1  # never let a callback kill the matcher


class MatchingEngine:
    def __init__(self, symbols: Iterable[str], on_trade: TradeCallback | None = None) -> None:
        self._workers = {s: _SymbolWorker(s, on_trade) for s in symbols}
        if not self._workers:
            raise ValueError("need at least one symbol")
        self._state_lock = threading.Lock()
        self._started = False
        self._stopped = False

    # ---------- lifecycle ----------

    def start(self) -> "MatchingEngine":
        with self._state_lock:
            if self._started:
                raise RuntimeError("engine already started")
            self._started = True
        for w in self._workers.values():
            w.thread.start()
        return self

    def stop(self) -> None:
        """Finish every command already queued, then shut the threads down."""
        with self._state_lock:
            if self._stopped or not self._started:
                self._stopped = True
                return
            self._stopped = True
            for w in self._workers.values():
                w.inbox.put(_STOP)
        for w in self._workers.values():
            w.thread.join()

    def __enter__(self) -> "MatchingEngine":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    # ---------- commands (safe to call from any thread) ----------

    def submit(self, symbol: str, order: Order) -> "Future[list[Trade]]":
        return self._enqueue(symbol, OrderBook.add, order)

    def cancel(self, symbol: str, order_id: int) -> "Future[bool]":
        return self._enqueue(symbol, OrderBook.cancel, order_id)

    def modify(self, symbol: str, order_id: int, *, qty: int | None = None,
               price: int | None = None) -> "Future[list[Trade]]":
        return self._enqueue(symbol, OrderBook.modify, order_id, qty=qty, price=price)

    # ---------- reads (consistent snapshots) ----------

    def best_bid(self, symbol: str) -> int | None:
        return self._read(symbol, OrderBook.best_bid)

    def best_ask(self, symbol: str) -> int | None:
        return self._read(symbol, OrderBook.best_ask)

    def spread(self, symbol: str) -> int | None:
        return self._read(symbol, OrderBook.spread)

    def top_of_book(self, symbol: str) -> tuple[int | None, int | None]:
        """Best bid and best ask read together under one lock.

        Two separate best_bid()/best_ask() calls can straddle a trade and
        disagree, which can make a healthy book look crossed.
        """
        return self._read(symbol, lambda b: (b.best_bid(), b.best_ask()))

    def depth(self, symbol: str, side: Side, levels: int = 5) -> list[tuple[int, int]]:
        return self._read(symbol, OrderBook.depth, side, levels)

    def trades(self, symbol: str) -> list[Trade]:
        return self._read(symbol, lambda b: list(b.trades))

    def resting_qty(self, symbol: str) -> int:
        return self._read(symbol, lambda b: sum(o.remaining for o in b._resting.values()))

    def snapshot(self, symbol: str) -> str:
        return self._read(symbol, str)

    @property
    def symbols(self) -> list[str]:
        return list(self._workers)

    # ---------- internals ----------

    def _worker(self, symbol: str) -> _SymbolWorker:
        try:
            return self._workers[symbol]
        except KeyError:
            raise KeyError(f"unknown symbol {symbol!r}") from None

    def _enqueue(self, symbol, fn, *args, **kwargs) -> Future:
        worker = self._worker(symbol)
        future: Future = Future()
        with self._state_lock:  # so nothing slips in after the stop marker
            if not self._started:
                raise EngineStopped("engine not started")
            if self._stopped:
                raise EngineStopped("engine is stopped")
            worker.inbox.put((fn, args, kwargs, future))
        return future

    def _read(self, symbol, fn, *args):
        worker = self._worker(symbol)
        with worker.lock:
            return fn(worker.book, *args)


if __name__ == "__main__":
    import random
    import time
    from orderbook import OrderType

    SYMBOLS = ["AAPL", "MSFT", "NVDA"]
    THREADS, PER_THREAD = 8, 5_000
    ids = iter(range(1, 10**9))
    id_lock = threading.Lock()
    trade_count = 0
    count_lock = threading.Lock()

    def on_trade(symbol: str, trade: Trade) -> None:
        global trade_count
        with count_lock:
            trade_count += 1

    def client(seed: int, engine: MatchingEngine) -> None:
        rng = random.Random(seed)
        futures = []
        for _ in range(PER_THREAD):
            with id_lock:
                oid = next(ids)
            side = rng.choice([Side.BUY, Side.SELL])
            kind = rng.choices([OrderType.LIMIT, OrderType.MARKET, OrderType.IOC], [8, 1, 1])[0]
            price = None if kind is OrderType.MARKET else rng.randint(9_900, 10_100)
            futures.append(engine.submit(rng.choice(SYMBOLS), Order(oid, side, kind, rng.randint(1, 100), price)))
        for f in futures:
            f.result()

    start = time.perf_counter()
    with MatchingEngine(SYMBOLS, on_trade=on_trade) as engine:
        clients = [threading.Thread(target=client, args=(i, engine)) for i in range(THREADS)]
        for t in clients:
            t.start()
        for t in clients:
            t.join()
        elapsed = time.perf_counter() - start
        total = THREADS * PER_THREAD
        print(f"{THREADS} client threads sent {total:,} orders across {len(SYMBOLS)} symbols")
        print(f"{trade_count:,} trades in {elapsed:.2f}s ({total / elapsed:,.0f} orders/s)\n")
        for s in SYMBOLS:
            print(f"{s}: bid {engine.best_bid(s)}  ask {engine.best_ask(s)}  spread {engine.spread(s)}")
