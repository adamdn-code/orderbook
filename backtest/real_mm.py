"""Market-making backtest on real Nasdaq order flow, with exact queue position.

    python backtest/real_mm.py ~/itch/AAPL_events.csv          # runs the study, writes backtest/results/real/

Input is `itch_replay --track SYM --events` output: every order event for one
stock for a whole day. The real book is rebuilt event by event with the C++
book (obfast). The market maker's orders are *shadow orders*: they sit in a
virtual queue alongside the real one but don't change what anyone else did.
That's the standard way to backtest passive strategies on historical
order-by-order data, and its main limitation (no market impact) is listed in
the README.

When does a shadow quote fill? ITCH names every order, so when a quote is
placed we record exactly which real orders are ahead of it at its price. Then:
  1. an execution or cancel of an order ahead of us moves us up the line,
  2. an execution at our price of an order *behind* us (or that joined after
     us) means the incoming order reached us first: we fill,
  3. an execution at a price worse than ours (the market traded through our
     level) fills us completely,
  4. a new opposite-side order priced through our quote would have traded
     with us: we fill.
Actions (place, cancel) take effect after a configurable latency, so a quote
can be hit while its cancel is still in flight, which is how latency costs
money in practice. Even at zero latency an action only takes effect after
the current timestamp: one incoming order that sweeps several levels shows
up as several executions with the same timestamp, and nobody can react in
the middle of it.

Prices are cents (AAPL trades in whole cents); PnL is in dollars.
"""

from __future__ import annotations

import argparse
import math
from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np
import pandas as pd

from common import BUY, SELL, obfast

NS = 1_000_000_000
QUOTE_START = int(9.5 * 3600 + 5 * 60) * NS  # 9:35, after the opening auction settles
QUOTE_END = int(15 * 3600 + 55 * 60) * NS  # 15:55, before the closing auction


@dataclass(frozen=True)
class Strategy:
    name: str = "join best"
    size: int = 100  # shares per quote
    back_ticks: int = 0  # 0 = join the best price; 1 = one cent behind it
    skew_step: int = 0  # 0 = no skew; else step one more cent back per this many shares of inventory
    max_inventory: int = 1_000
    latency_ns: int = 0  # time for a place or cancel to take effect
    rebate_per_share: float = 0.0020  # dollars for each share filled as the passive side (assumed)
    # Signal: top-of-book imbalance I = (bid size - ask size) / (bid size + ask size), in [-1, 1].
    # Strongly positive means buyers are stacked up and the price tends to tick up next, so a resting
    # ask is about to be picked off: pull it while I > threshold (and the bid while I < -threshold).
    imbalance_pull: float = 0.0  # 0 = off
    # Signal: a best level with few shares left is about to break, and a quote at the back of it
    # would fill just before the price moves through. Pull our quote there while the real size
    # at that best price is below this many shares.
    min_level_shares: int = 0  # 0 = off


@dataclass
class Quote:
    side: int
    price: int
    qty: int
    placed_ns: int
    ahead: dict[int, int] = field(default_factory=dict)  # real order id -> shares, all ahead of us
    ahead_shares: int = 0
    ahead_shares_at_place: int = 0
    live: bool = False  # becomes live once the placement latency has passed
    cancel_at: int | None = None  # cancel in flight; still fillable until then


class Events:
    """Column lists of an events CSV (lists are much faster than DataFrame rows in a loop)."""

    def __init__(self, df: pd.DataFrame):
        df = df.sort_values("ts_ns", kind="stable")
        self.ts = df["ts_ns"].astype("int64").tolist()
        self.type = df["type"].tolist()
        self.ref = df["ref"].astype("int64").tolist()
        self.side = df["side"].map({"B": BUY, "S": SELL}).fillna(-1).astype(int).tolist()
        self.shares = df["shares"].astype("int64").tolist()
        self.price = df["price"].astype("int64").tolist()  # ITCH units: dollars * 10,000
        self.new_ref = df["new_ref"].astype("int64").tolist()
        self.n = len(self.ts)

    @classmethod
    def read(cls, path) -> "Events":
        return cls(pd.read_csv(path, dtype={"type": "string", "side": "string"}))


class ShadowMaker:
    def __init__(self, strat: Strategy):
        self.s = strat
        self.inventory = 0
        self.cash = 0.0  # dollars
        self.rebates = 0.0
        self.quotes: dict[int, Quote | None] = {BUY: None, SELL: None}
        self.pending: list[tuple[int, str, Quote]] = []  # (effective_ns, "place"/"cancel", quote)
        self.fills: list[tuple[int, int, int, int, int, int]] = []  # ns, sign, price, qty, wait_ns, ahead_at_place

    # ---------- fills ----------
    def fill(self, q: Quote, ts: int, qty: int) -> None:
        qty = min(qty, q.qty)
        if qty <= 0:
            return
        sign = 1 if q.side == BUY else -1
        self.inventory += sign * qty
        self.cash -= sign * qty * q.price / 100
        self.rebates += qty * self.s.rebate_per_share
        self.fills.append((ts, sign, q.price, qty, ts - q.placed_ns, q.ahead_shares_at_place))
        q.qty -= qty

    def active(self):
        for side in (BUY, SELL):
            q = self.quotes[side]
            if q is not None and q.live and q.qty > 0:
                yield q

    def on_event(self, ts: int, typ: str, ref: int, side: int, shares: int, price_c: int,
                 removed: int, new_side_price: int | None) -> None:
        """Apply one real event to our quotes. `removed` is how many shares left the referenced
        order (for E/C/X/D/U), `price_c` is the event price in cents."""
        for q in self.active():
            if typ in ("E", "C") and side == q.side:
                if ref in q.ahead:
                    self._advance(q, ref, removed)
                elif price_c == q.price or (q.side == BUY and price_c < q.price) or (q.side == SELL and price_c > q.price):
                    # Rule 2: an order behind us traded at our price -> we'd have traded first.
                    # Rule 3: the market traded through our price -> we'd have been filled entirely.
                    self.fill(q, ts, q.qty if price_c != q.price else shares)
            elif typ in ("X", "D", "U") and ref in q.ahead:
                self._advance(q, ref, removed)
            if typ in ("A", "U") and new_side_price is not None:
                nside, nprice = side, new_side_price
                if nside != q.side and ((q.side == BUY and nprice <= q.price) or (q.side == SELL and nprice >= q.price)):
                    self.fill(q, ts, shares)  # Rule 4: incoming order priced through our quote
        for side in (BUY, SELL):
            q = self.quotes[side]
            if q is not None and q.qty <= 0:
                self.quotes[side] = None

    @staticmethod
    def _advance(q: Quote, ref: int, removed: int) -> None:
        left = q.ahead[ref] - removed
        q.ahead_shares -= min(removed, q.ahead[ref])
        if left <= 0:
            del q.ahead[ref]
        else:
            q.ahead[ref] = left

    # ---------- quoting ----------
    def targets(self, book: "obfast.Book", ts: int) -> dict[int, int | None]:
        best_bid, best_ask = book.best_bid(), book.best_ask()
        if not (QUOTE_START <= ts < QUOTE_END) or best_bid is None or best_ask is None:
            return {BUY: None, SELL: None}
        s, inv = self.s, self.inventory
        pull_bid = pull_ask = False
        if s.imbalance_pull or s.min_level_shares:
            bq, aq = book.depth(BUY, 1)[0][1], book.depth(SELL, 1)[0][1]
            if s.imbalance_pull:
                imb = (bq - aq) / (bq + aq)
                pull_bid, pull_ask = imb < -s.imbalance_pull, imb > s.imbalance_pull
            if s.min_level_shares:
                pull_bid = pull_bid or bq < s.min_level_shares
                pull_ask = pull_ask or aq < s.min_level_shares
        skew = (abs(inv) // s.skew_step) if s.skew_step else 0
        bid = best_bid - s.back_ticks - (skew if inv > 0 else 0)
        ask = best_ask + s.back_ticks + (skew if inv < 0 else 0)
        return {
            BUY: bid if inv + s.size <= s.max_inventory and not pull_bid else None,
            SELL: ask if inv - s.size >= -s.max_inventory and not pull_ask else None,
        }

    def requote(self, book: "obfast.Book", ts: int) -> None:
        want = self.targets(book, ts)
        for side in (BUY, SELL):
            q = self.quotes[side]
            target = want[side]
            if q is not None and q.cancel_at is None and q.price == target:
                continue  # keep our place in line
            delay = max(1, self.s.latency_ns)  # never mid-event: a sweep's executions share one timestamp
            if q is not None and q.cancel_at is None:
                q.cancel_at = ts + delay
                self.pending.append((q.cancel_at, "cancel", q))
            if target is not None and not any(p[1] == "place" and p[2].side == side for p in self.pending):
                nq = Quote(side=side, price=target, qty=self.s.size, placed_ns=ts + delay)
                self.pending.append((nq.placed_ns, "place", nq))

    def apply_pending(self, book: "obfast.Book", ts: int) -> None:
        """Make placements and cancels whose latency has passed take effect (in time order)."""
        if not any(p[0] <= ts for p in self.pending):
            return
        due = sorted((p for p in self.pending if p[0] <= ts), key=lambda p: p[0])
        self.pending = [p for p in self.pending if p[0] > ts]
        for _, action, q in due:
            if action == "cancel":
                if self.quotes[q.side] is q:
                    self.quotes[q.side] = None
            else:
                old = self.quotes[q.side]
                if old is not None and old.cancel_at is None:
                    continue  # superseded
                # Everyone resting at our price right now is ahead of us.
                q.ahead = dict(book.level(q.side, q.price))
                q.ahead_shares = sum(q.ahead.values())
                q.ahead_shares_at_place = q.ahead_shares
                q.live = True
                self.quotes[q.side] = q


def run(events: Events, strat: Strategy, record_every_ns: int = 10 * NS):
    """Replay the day once with one strategy. Returns (summary, timeline DataFrame, fills DataFrame, mids)."""
    book = obfast.Book()
    mm = ShadowMaker(strat)
    mid_t: list[int] = []
    mid_v: list[float] = []
    rows = []
    next_record = QUOTE_START
    last_bb = last_ba = None
    close_price = None
    unexpected_trades = 0
    off_book = 0  # orders the book can't hold (e.g. $199,999.99 stub quotes); they never trade

    def add(ref: int, side: int, shares: int, price_c: int) -> bool:
        nonlocal unexpected_trades, off_book
        try:
            unexpected_trades += len(book.limit(ref, side, shares, price_c))
            return True
        except obfast.OrderBookError:
            off_book += 1
            return False

    ts_l, typ_l, ref_l, side_l, sh_l, px_l, nref_l = (events.ts, events.type, events.ref, events.side,
                                                      events.shares, events.price, events.new_ref)
    for i in range(events.n):
        ts, typ, ref, side, shares, px = ts_l[i], typ_l[i], ref_l[i], side_l[i], sh_l[i], px_l[i]
        mm.apply_pending(book, ts)
        price_c = px // 100
        removed = 0
        new_price = None
        # --- apply the real event to the real book ---
        if typ == "A":
            if px % 100 == 0 and side >= 0 and add(ref, side, shares, price_c):
                new_price = price_c
        elif typ in ("E", "C", "X"):
            rem = book.remaining(ref)
            if rem is not None:
                removed = min(shares, rem)
                if shares >= rem:
                    book.cancel(ref)
                else:
                    book.modify(ref, qty=rem - shares)
        elif typ == "D":
            rem = book.remaining(ref)
            if rem is not None:
                removed = rem
                book.cancel(ref)
        elif typ == "U":
            rem = book.remaining(ref)
            if rem is not None:
                removed = rem
                book.cancel(ref)
                if px % 100 == 0 and side >= 0 and add(events.new_ref[i], side, shares, price_c):
                    new_price = price_c
        elif typ == "Q" and ts > 15 * 3600 * NS and shares > 0:
            close_price = px / 10_000  # closing cross
        # --- what it means for our shadow quotes ---
        if typ in ("E", "C") and side >= 0:
            mm.on_event(ts, typ, ref, side, shares, price_c, removed, None)
        elif typ in ("X", "D"):
            mm.on_event(ts, typ, ref, side, shares, price_c, removed, None)
        elif typ == "U":
            mm.on_event(ts, typ, ref, side, shares, price_c, removed, new_price)
        elif typ == "A":
            mm.on_event(ts, typ, ref, side, shares, price_c, 0, new_price)
        # --- top of book changed: record mid, maybe requote ---
        bb, ba = book.best_bid(), book.best_ask()
        if bb != last_bb or ba != last_ba:
            last_bb, last_ba = bb, ba
            if bb is not None and ba is not None:
                mid_t.append(ts)
                mid_v.append((bb + ba) / 2)
            mm.requote(book, ts)
        elif strat.imbalance_pull or strat.min_level_shares or mm.quotes[BUY] is None or mm.quotes[SELL] is None:
            mm.requote(book, ts)  # the signal can change without the best prices changing
        if ts >= next_record and mid_v:
            next_record = (ts // record_every_ns + 1) * record_every_ns
            if QUOTE_START <= ts <= QUOTE_END + 5 * 60 * NS:
                m = mid_v[-1] / 100
                rows.append({"ts_ns": ts, "mid": m, "inventory": mm.inventory,
                             "pnl": mm.cash + mm.inventory * m, "pnl_with_rebate": mm.cash + mm.inventory * m + mm.rebates})

    mids = (np.array(mid_t, dtype=np.int64), np.array(mid_v) / 100)
    mark = close_price if close_price is not None else float(mids[1][-1])
    fills = pd.DataFrame(mm.fills, columns=["ts_ns", "sign", "price_c", "qty", "wait_ns", "ahead_at_place"])
    timeline = pd.DataFrame(rows)
    summary = summarize(strat, mm, fills, timeline, mids, mark, unexpected_trades)
    summary["off_book_orders"] = off_book
    return summary, timeline, fills, mids


MARKOUT_HORIZONS_S = [0.1, 0.5, 1, 5, 10, 30, 60]


def markouts(fills: pd.DataFrame, mids, horizons_s=MARKOUT_HORIZONS_S) -> dict[float, float]:
    """Average cents per share earned if the fill were marked to the mid h seconds later."""
    if fills.empty:
        return {h: float("nan") for h in horizons_s}
    t, m = mids
    out = {}
    for h in horizons_s:
        idx = np.searchsorted(t, fills["ts_ns"].to_numpy() + int(h * NS), side="right") - 1
        later = m[np.clip(idx, 0, len(m) - 1)]
        per_share = fills["sign"].to_numpy() * (later * 100 - fills["price_c"].to_numpy())
        out[h] = float(np.average(per_share, weights=fills["qty"].to_numpy()))
    return out


def summarize(strat, mm, fills, timeline, mids, mark, unexpected_trades) -> dict:
    pnl_final = mm.cash + mm.inventory * mark
    volume = int(fills["qty"].sum()) if len(fills) else 0
    pnl_curve = timeline["pnl"].to_numpy() if len(timeline) else np.zeros(1)
    t, m = mids
    if len(fills):
        # mid just before the fill's event (that event may itself move the mid against us)
        idx = np.searchsorted(t, fills["ts_ns"].to_numpy(), side="left") - 1
        mid_at = m[np.clip(idx, 0, len(m) - 1)]
        edge = float(np.average(fills["sign"] * (mid_at * 100 - fills["price_c"]), weights=fills["qty"]))
    else:
        edge = float("nan")
    mk = markouts(fills, mids)
    out = {k: v for k, v in asdict(strat).items()}
    out.update({
        "pnl": pnl_final,
        "rebates": mm.rebates,
        "pnl_with_rebate": pnl_final + mm.rebates,
        "fills": len(fills),
        "shares": volume,
        "inventory_at_close": mm.inventory,
        "max_abs_inventory": int(timeline["inventory"].abs().max()) if len(timeline) else 0,
        "max_drawdown": float(np.max(np.maximum.accumulate(pnl_curve) - pnl_curve)),
        "edge_cents": edge,
        **{f"markout_{h:g}s_cents": v for h, v in mk.items()},
        "median_wait_s": float(fills["wait_ns"].median() / NS) if len(fills) else float("nan"),
        "median_ahead_at_place": float(fills["ahead_at_place"].median()) if len(fills) else float("nan"),
        "unexpected_trades": unexpected_trades,
    })
    return out


# ---------------------------------------------------------------- study

STRATEGIES = [
    Strategy("Join best", back_ticks=0),
    Strategy("Join best + inventory skew", back_ticks=0, skew_step=200),
    Strategy("One cent behind + skew", back_ticks=1, skew_step=200),
    Strategy("Skew + imbalance signal", skew_step=200, imbalance_pull=0.2),
    Strategy("Skew + thin-level signal", skew_step=200, min_level_shares=200),
]
QUEUE_BINS = [-1, 0, 100, 300, 600, 1500, 10**12]
QUEUE_LABELS = ["0 (alone)", "1-100", "101-300", "301-600", "601-1500", ">1500"]


def queue_breakdown(fills: pd.DataFrame, mids) -> pd.DataFrame:
    """Fill quality by how many real shares were ahead of the quote when it was placed."""
    t, m = mids
    out = fills.copy()
    for h in (0.1, 1, 10):
        idx = np.searchsorted(t, out["ts_ns"].to_numpy() + int(h * NS), side="right") - 1
        out[f"mk_{h:g}s"] = out["sign"].to_numpy() * (m[np.clip(idx, 0, len(m) - 1)] * 100 - out["price_c"].to_numpy())
    out["shares_ahead"] = pd.cut(out["ahead_at_place"], bins=QUEUE_BINS, labels=QUEUE_LABELS)
    rows = []
    for label, d in out.groupby("shares_ahead", observed=True):
        rows.append({"shares_ahead_at_placement": label, "fills": len(d), "shares": int(d["qty"].sum()),
                     **{f"markout_{h}_cents": float(np.average(d[f"mk_{h}"], weights=d["qty"])) for h in ("0.1s", "1s", "10s")},
                     "median_wait_s": float(d["wait_ns"].median() / NS)})
    return pd.DataFrame(rows)
LATENCIES_US = [0, 100, 1_000, 10_000, 100_000]


def main() -> None:
    import time

    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("events", help="SYM_events.csv from itch_replay --track SYM --events")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "results" / "real"))
    ap.add_argument("--latency-us", type=int, default=100, help="latency for the strategy comparison (default 100 us)")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    t0 = time.perf_counter()
    ev = Events.read(args.events)
    print(f"{ev.n:,} events loaded in {time.perf_counter() - t0:.1f}s")

    results, timelines, fills_by = [], {}, {}
    mids = None
    for base in STRATEGIES:
        strat = Strategy(**{**asdict(base), "latency_ns": args.latency_us * 1000})
        t1 = time.perf_counter()
        summary, tl, fills, mids = run(ev, strat)
        results.append(summary)
        timelines[strat.name], fills_by[strat.name] = tl, fills
        print(f"  {strat.name:<28} pnl ${summary['pnl']:>8,.0f}  with rebate ${summary['pnl_with_rebate']:>8,.0f}  "
              f"fills {summary['fills']:>6,}  shares {summary['shares']:>8,}  ({time.perf_counter() - t1:.0f}s)")

    lat_rows = []
    skew = STRATEGIES[1]
    for us in LATENCIES_US:
        strat = Strategy(**{**asdict(skew), "latency_ns": us * 1000, "name": f"{skew.name} @ {us} us"})
        summary, *_ = run(ev, strat)
        summary["latency_us"] = us
        lat_rows.append(summary)
        print(f"  latency {us:>7,} us   pnl ${summary['pnl']:>8,.0f}  with rebate ${summary['pnl_with_rebate']:>8,.0f}  "
              f"markout 1s {summary['markout_1s_cents']:+.2f}c")

    df = pd.DataFrame(results)
    df["pnl_per_100_shares"] = df["pnl_with_rebate"] / df["shares"].clip(lower=1) * 100
    lat = pd.DataFrame(lat_rows)
    df.to_csv(out / "strategies.csv", index=False, float_format="%.4g")
    qb = queue_breakdown(fills_by[STRATEGIES[1].name], mids)
    qb.to_csv(out / "queue.csv", index=False, float_format="%.4g")
    print(qb.round(3).to_string(index=False))
    lat.to_csv(out / "latency.csv", index=False, float_format="%.4g")
    charts(plt, out, df, lat, timelines, fills_by, mids)
    print(f"Wrote {out}/strategies.csv, latency.csv, queue.csv, day.png, markouts.png, latency.png")


def charts(plt, out: Path, df, lat, timelines, fills_by, mids) -> None:
    colors = ["#c0392b", "#1f6f8b", "#7d5ba6", "#2e8b57", "#b8860b"]
    hours = lambda ns: ns / NS / 3600  # noqa: E731

    fig, (a, b, c) = plt.subplots(3, 1, figsize=(11, 8.5), sharex=True, gridspec_kw={"height_ratios": [2, 1.2, 1]})
    for (name, tl), col in zip(timelines.items(), colors):
        a.plot(hours(tl["ts_ns"]), tl["pnl_with_rebate"], color=col, lw=1.3, label=f"{name} (with rebate)")
        a.plot(hours(tl["ts_ns"]), tl["pnl"], color=col, lw=0.8, ls="--", alpha=0.7)
        b.plot(hours(tl["ts_ns"]), tl["inventory"], color=col, lw=0.8)
    tl = next(iter(timelines.values()))
    c.plot(hours(tl["ts_ns"]), tl["mid"], color="#555", lw=0.9)
    a.axhline(0, color="#999", lw=0.6)
    a.set_ylabel("PnL ($)")
    a.set_title("Market making AAPL on real Nasdaq order flow, 2019-01-30 (dashed: before rebates)")
    a.legend(loc="upper left", fontsize=8)
    b.set_ylabel("inventory (shares)")
    b.axhline(0, color="#999", lw=0.6)
    c.set_ylabel("mid ($)")
    c.set_xlabel("hour of day (ET)")
    fig.tight_layout()
    fig.savefig(out / "day.png", dpi=130)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 4.6))
    for (name, fills), col in zip(fills_by.items(), colors):
        mk = markouts(fills, mids)
        ax.plot(list(mk.keys()), list(mk.values()), marker="o", color=col, label=name)
    ax.set_xscale("log")
    ax.axhline(0, color="#999", lw=0.6)
    ax.set_xlabel("seconds after the fill")
    ax.set_ylabel("cents per share vs. later mid")
    ax.set_title("Markouts: what each fill was worth after the price moved")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "markouts.png", dpi=130)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(8, 4.2))
    x = [max(u, 10) for u in lat["latency_us"]]
    ax.plot(x, lat["pnl_with_rebate"], marker="o", color="#1f6f8b", label="with rebate")
    ax.plot(x, lat["pnl"], marker="o", color="#c0392b", ls="--", label="before rebate")
    ax.set_xscale("log")
    ax.set_xticks(x, ["0" if u == 0 else (f"{u // 1000} ms" if u >= 1000 else f"{u} µs") for u in lat["latency_us"]])
    ax.axhline(0, color="#999", lw=0.6)
    ax.set_xlabel("latency to place or cancel a quote")
    ax.set_ylabel("PnL for the day ($)")
    ax.set_title("How much speed is worth")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out / "latency.png", dpi=130)
    plt.close(fig)


if __name__ == "__main__":
    main()
