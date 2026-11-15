"""Synthetic ITCH-style event stream, for tests and for developing the ML pipeline without the real file.

    python backtest/synth_events.py out.csv --minutes 60

Writes the same columns as `itch_replay --track SYM --events` (ts_ns, type, ref, side, shares, price,
new_ref) so `real_mm.Events.read` and `ml_signal.py` take it unchanged. A hidden fundamental drifts as a
random walk; liquidity providers rest orders a few cents around the mid, cancel them at random, and
informed takers send marketable orders toward the fundamental. Their executions show up as E events on
resting orders, the way Nasdaq reports them. There is real (weak) predictability in the flow, which is
what lets the tests check that the model beats a coin flip.
"""

from __future__ import annotations

import argparse
import math

import numpy as np
import pandas as pd

from common import BUY, SELL, obfast
from real_mm import NS, QUOTE_START

COLS = ["ts_ns", "type", "ref", "side", "shares", "price", "new_ref"]


def generate(minutes: float = 30, seed: int = 0, start_ns: int = QUOTE_START - 60 * NS,
             start_cents: int = 15_000) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    book = obfast.Book()
    rows: list[tuple] = []
    resting: list[int] = []
    next_id = 1
    fundamental = float(start_cents)
    ts = start_ns
    end = start_ns + int(minutes * 60 * NS)

    def add(side: int, qty: int, price: int) -> None:
        nonlocal next_id
        oid = next_id
        next_id += 1
        book.limit(oid, side, qty, price)
        rows.append((ts, "A", oid, "B" if side == BUY else "S", qty, price * 100, 0))
        resting.append(oid)

    # seed both sides
    for k in range(1, 6):
        add(BUY, 100 * int(rng.integers(1, 6)), start_cents - k)
        add(SELL, 100 * int(rng.integers(1, 6)), start_cents + k)

    while ts < end:
        ts += int(rng.exponential(15_000_000))  # ~70 events/s, about AAPL's rate
        fundamental += rng.normal(0, 0.03)
        bb, ba = book.best_bid(), book.best_ask()
        if bb is None or ba is None:
            if bb is None:
                add(BUY, 100, (ba or start_cents) - 1)
            if ba is None:
                add(SELL, 100, (bb or start_cents) + 1)
            continue
        mid = (bb + ba) / 2
        u = rng.random()
        if u < 0.45:  # passive add, leaning toward the fundamental
            side = BUY if rng.random() < 0.5 + 0.15 * np.tanh(fundamental - mid) else SELL
            off = int(rng.geometric(0.6))  # 1, 2, 3... cents from a reference between mid and fundamental
            ref_px = 0.7 * mid + 0.3 * fundamental
            price = int(math.floor(ref_px)) - off + 1 if side == BUY else int(math.ceil(ref_px)) + off - 1
            if side == BUY and price >= ba:
                price = ba - 1
            if side == SELL and price <= bb:
                price = bb + 1
            add(side, 100 * int(rng.integers(1, 4)), price)
        elif u < 0.88 and resting:  # cancel
            j = int(rng.integers(len(resting)))
            oid = resting[j]
            resting[j] = resting[-1]
            resting.pop()
            rem = book.remaining(oid)
            if rem is not None:
                book.cancel(oid)
                rows.append((ts, "D", oid, "", 0, 0, 0))
        else:  # taker: informed when the mid is away from the fundamental
            gap = fundamental - mid
            side = BUY if rng.random() < 0.5 + 0.4 * np.tanh(gap) else SELL
            qty = 100 * int(rng.integers(1, 5))
            for rid, _tid, price, q, *_ in book.market(next_id, side, qty):
                rows.append((ts, "E", rid, "S" if side == BUY else "B", q, price * 100, 0))
            next_id += 1
    df = pd.DataFrame(rows, columns=COLS)
    return df


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()
    df = generate(a.minutes, a.seed)
    df.to_csv(a.out, index=False)
    print(f"wrote {len(df):,} events to {a.out}")


if __name__ == "__main__":
    main()
