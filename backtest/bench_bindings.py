"""Same random order stream through the pure-Python book and the C++ book (via obfast).

    python backtest/bench_bindings.py [n_orders]
"""

from __future__ import annotations

import random
import sys
import time

from common import BUY, SELL, obfast
from orderbook import Order, OrderBook, OrderType, Side  # pure-Python book


def make_stream(n: int, seed: int = 42):
    rng = random.Random(seed)
    stream = []
    for i in range(1, n + 1):
        buy = rng.random() < 0.5
        k = rng.randint(0, 9)
        kind = "L" if k < 8 else ("M" if k == 8 else "I")
        stream.append((i, buy, kind, rng.randint(1, 100), rng.randint(9_900, 10_100)))
    return stream


def run_python(stream) -> tuple[float, int]:
    book, trades = OrderBook(), 0
    types = {"L": OrderType.LIMIT, "M": OrderType.MARKET, "I": OrderType.IOC}
    t0 = time.perf_counter()
    for oid, buy, kind, qty, price in stream:
        side = Side.BUY if buy else Side.SELL
        trades += len(book.add(Order(oid, side, types[kind], qty, None if kind == "M" else price)))
    return time.perf_counter() - t0, trades


def run_cpp(stream) -> tuple[float, int]:
    book, trades = obfast.Book(), 0
    limit, market, ioc = book.limit, book.market, book.ioc
    t0 = time.perf_counter()
    for oid, buy, kind, qty, price in stream:
        side = BUY if buy else SELL
        if kind == "L":
            trades += len(limit(oid, side, qty, price))
        elif kind == "M":
            trades += len(market(oid, side, qty))
        else:
            trades += len(ioc(oid, side, qty, price))
    return time.perf_counter() - t0, trades


def main() -> None:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300_000
    stream = make_stream(n)
    py_t, py_trades = run_python(stream)
    cpp_t, cpp_trades = run_cpp(stream)
    assert py_trades == cpp_trades, (py_trades, cpp_trades)
    print(f"{n:,} orders, {py_trades:,} trades (identical in both books)")
    print(f"  pure Python book : {py_t:6.2f}s  {n / py_t / 1e6:6.2f} M orders/s")
    print(f"  C++ book (obfast): {cpp_t:6.2f}s  {n / cpp_t / 1e6:6.2f} M orders/s  ->  {py_t / cpp_t:.1f}x faster")


if __name__ == "__main__":
    main()
