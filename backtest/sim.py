"""Market simulator and market-making strategy, running on the C++ order book.

The market
----------
A hidden "fundamental" price follows a random walk with occasional jumps
("news"). Each step, background
traders send orders into the book:
  - limit orders resting a few ticks away from the fundamental (liquidity),
  - market orders whose direction leans toward the fundamental: when the
    book's mid is below the fundamental, buys are more likely. These are
    "informed" traders, and they're why market making is hard: the orders
    that hit your quotes tend to be the ones that know where price is going.
  - cancels: each background limit order lives for a random time
    (exponential, mean `lifetime` steps) and is then cancelled if it's
    still resting, which keeps the book at a steady depth.

The market maker
----------------
Every step it cancels its quotes and re-quotes around the book's mid price:
    bid = mid - half_spread - skew * inventory / size
    ask = mid + half_spread - skew * inventory / size
A wider spread earns more per fill but fills less. Skew shifts both quotes
against the current inventory (long -> quote lower, so it's more likely to
sell), which keeps inventory, and the risk that comes with it, near zero.

Prices are integer ticks (1 tick = $0.01). PnL is marked to the book's mid.
"""

from __future__ import annotations

import math
from dataclasses import asdict, dataclass

import numpy as np
import pandas as pd

from common import BUY, SELL, obfast

MM_BASE = 1 << 48  # market-maker order ids live above this; background ids below


@dataclass(frozen=True)
class MarketParams:
    steps: int = 50_000
    start_price: int = 10_000  # ticks ($100.00)
    vol: float = 0.25  # std dev of the fundamental's move per step, in ticks
    jump_prob: float = 0.003  # chance per step of a news jump
    jump_size: float = 4.0  # mean jump size in ticks (exponential, random sign)
    limit_rate: float = 2.0  # background limit orders per step (Poisson mean)
    market_rate: float = 0.5  # background market orders per step
    lifetime: float = 20.0  # mean steps a background limit order rests before it's cancelled
    depth_scale: float = 4.0  # mean distance (ticks) of background limits from the fundamental
    informed: float = 1.0  # how strongly market-order direction follows (fundamental - mid)
    lot: int = 100  # background order sizes are 1..5 lots
    seed: int = 7


@dataclass(frozen=True)
class MMParams:
    half_spread: float = 2.0  # ticks from mid
    size: int = 100  # shares per quote
    skew: float = 0.0  # ticks shifted per `size` shares of inventory
    max_inventory: int = 2_000  # stop quoting the side that would add to inventory beyond this


class MarketMaker:
    def __init__(self, params: MMParams):
        self.p = params
        self.inventory = 0
        self.cash = 0  # ticks * shares
        self.next_id = MM_BASE
        self.bid_id: int | None = None
        self.ask_id: int | None = None
        self.fills: list[tuple[int, int, int, int]] = []  # (step, +1 buy / -1 sell, price, qty)
        self.step = 0

    def on_trade(self, trade: tuple[int, int, int, int, int]) -> None:
        buy_id, sell_id, price, qty, _ = trade
        if buy_id >= MM_BASE:
            self.inventory += qty
            self.cash -= price * qty
            self.fills.append((self.step, 1, price, qty))
        if sell_id >= MM_BASE:
            self.inventory -= qty
            self.cash += price * qty
            self.fills.append((self.step, -1, price, qty))

    def requote(self, book: "obfast.Book", handle) -> None:
        for oid in (self.bid_id, self.ask_id):
            if oid is not None:
                book.cancel(oid)
        self.bid_id = self.ask_id = None
        mid = book.mid()  # mid of everyone else's orders, since ours are gone
        if mid is None:
            return
        p = self.p
        center = mid - p.skew * self.inventory / p.size
        bid = math.floor(center - p.half_spread)
        ask = math.ceil(center + p.half_spread)
        if ask <= bid:
            ask = bid + 1
        if self.inventory + p.size <= p.max_inventory and bid > 0:
            self.bid_id = self.next_id
            self.next_id += 1
            handle(book.limit(self.bid_id, BUY, p.size, bid))
        if self.inventory - p.size >= -p.max_inventory:
            self.ask_id = self.next_id
            self.next_id += 1
            handle(book.limit(self.ask_id, SELL, p.size, ask))

    def pnl(self, mark: float) -> float:
        return self.cash + self.inventory * mark


def run(market: MarketParams = MarketParams(), mm: MMParams | None = MMParams(), record_every: int = 50):
    """Run one backtest. Returns (summary dict, timeline DataFrame, fills DataFrame)."""
    rng = np.random.default_rng(market.seed)
    book = obfast.Book()
    maker = MarketMaker(mm) if mm is not None else None

    def handle(trades):
        if maker is not None:
            for t in trades:
                maker.on_trade(t)

    # Every strategy sees the same sequence of random draws (same fundamental
    # path, same arrivals, sizes and prices); only how the book reacts differs.
    steps = market.steps
    moves = rng.normal(0.0, market.vol, steps)
    jumps = rng.random(steps) < market.jump_prob
    moves[jumps] += rng.choice([-1.0, 1.0], jumps.sum()) * rng.exponential(market.jump_size, jumps.sum())
    n_limit = rng.poisson(market.limit_rate, steps)
    n_market = rng.poisson(market.market_rate, steps)

    fundamental = float(market.start_price)
    next_id = 1
    expiries: dict[int, list[int]] = {}  # step -> background ids to cancel then

    def schedule(oid: int, now: int) -> None:
        expiries.setdefault(now + 1 + int(rng.exponential(market.lifetime)), []).append(oid)
    mids = np.empty(steps)
    fund = np.empty(steps)
    rows = []

    # Seed the book with some liquidity so there's a mid on step 0.
    for k in range(1, 11):
        book.limit(next_id, BUY, market.lot, market.start_price - k)
        book.limit(next_id + 1, SELL, market.lot, market.start_price + k)
        schedule(next_id, 0)
        schedule(next_id + 1, 0)
        next_id += 2

    for step in range(steps):
        fundamental += moves[step]
        if maker is not None:
            maker.step = step
            maker.requote(book, handle)

        for oid in expiries.pop(step, ()):
            book.cancel(oid)  # False if it already filled; that's fine

        events = ["L"] * n_limit[step] + ["M"] * n_market[step]
        for i in rng.permutation(len(events)):
            if events[i] == "L":
                side = BUY if rng.random() < 0.5 else SELL
                dist = 1 + int(rng.exponential(market.depth_scale))
                price = round(fundamental) - dist if side == BUY else round(fundamental) + dist
                qty = market.lot * int(rng.integers(1, 6))
                handle(book.limit(next_id, side, qty, max(1, price)))
                schedule(next_id, step)
                next_id += 1
            else:
                mid = book.mid()
                gap = 0.0 if mid is None else fundamental - mid
                p_buy = 1.0 / (1.0 + math.exp(-market.informed * gap))
                side = BUY if rng.random() < p_buy else SELL
                qty = market.lot * int(rng.integers(1, 6))
                handle(book.market(next_id, side, qty))
                next_id += 1

        mid = book.mid()
        mids[step] = mid if mid is not None else (mids[step - 1] if step else market.start_price)
        fund[step] = fundamental
        if step % record_every == 0 or step == steps - 1:
            rows.append(
                {
                    "step": step,
                    "mid": mids[step],
                    "fundamental": fundamental,
                    "spread": book.spread(),
                    "inventory": maker.inventory if maker else 0,
                    "pnl": maker.pnl(mids[step]) / 100 if maker else 0.0,  # dollars
                }
            )

    timeline = pd.DataFrame(rows)
    fills = pd.DataFrame(maker.fills if maker else [], columns=["step", "sign", "price", "qty"])
    summary = summarize(market, mm, maker, timeline, fills, mids)
    return summary, timeline, fills


def summarize(market, mm, maker, timeline: pd.DataFrame, fills: pd.DataFrame, mids: np.ndarray, horizon: int = 100):
    out = {**{f"m_{k}": v for k, v in asdict(market).items()}}
    if mm is not None:
        out.update(asdict(mm))
    if maker is None:
        return out

    pnl = timeline["pnl"].to_numpy()
    blocks = timeline.set_index("step")["pnl"].groupby(timeline["step"].to_numpy() // 1000).last().diff().dropna()
    volume = int(fills["qty"].sum()) if len(fills) else 0

    # Edge: how far from mid we traded (what the spread earned).
    # Markout: where mid went `horizon` steps later (what informed flow took back).
    if len(fills):
        f = fills.to_numpy()
        steps_, sign, price, qty = f[:, 0], f[:, 1], f[:, 2], f[:, 3]
        mid_now = mids[steps_]
        mid_later = mids[np.minimum(steps_ + horizon, len(mids) - 1)]
        edge = float(np.sum(sign * (mid_now - price) * qty) / qty.sum())
        markout = float(np.sum(sign * (mid_later - price) * qty) / qty.sum())
    else:
        edge = markout = 0.0

    out.update(
        {
            "pnl": float(pnl[-1]),
            "max_drawdown": float(np.max(np.maximum.accumulate(pnl) - pnl)),
            "sharpe_1k": float(blocks.mean() / blocks.std() * math.sqrt(len(blocks))) if blocks.std() > 0 else 0.0,
            "volume": volume,
            "fills": len(fills),
            "mean_abs_inventory": float(timeline["inventory"].abs().mean()),
            "max_abs_inventory": int(timeline["inventory"].abs().max()),
            "edge_ticks": edge,
            f"markout_{horizon}_ticks": markout,
            "pnl_per_100_shares": float(pnl[-1] / volume * 100) if volume else 0.0,
        }
    )
    return out
