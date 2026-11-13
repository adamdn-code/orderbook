"""Chart one stock's day from itch_replay --track output.

    python cpp/itch/plot_symbol.py AAPL [--dir DIR] [--out AAPL_day.png]

Reads DIR/AAPL_book.csv (top of book, sampled) and DIR/AAPL_trades.csv and draws:
  1. best bid / best ask through the day, with trades on top
  2. the bid-ask spread in cents
  3. shares traded per 5 minutes, split into lit executions, hidden executions and crosses
"""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

OPEN, CLOSE = 9.5, 16.0 + 1 / 60  # regular session (+1 min so the closing cross shows), hours after midnight ET


def load(directory: Path, symbol: str):
    book = pd.read_csv(directory / f"{symbol}_book.csv")
    trades = pd.read_csv(directory / f"{symbol}_trades.csv")
    for df in (book, trades):
        df["hour"] = df["ts_ns"] / 3.6e12
    book = book[(book["bid"] > 0) & (book["ask"] > 0) & (book["ask"] > book["bid"])]
    return book, trades


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("symbol")
    ap.add_argument("--dir", default=".", help="where itch_replay wrote the CSVs (its --out)")
    ap.add_argument("--out", help="output PNG (default: DIR/SYMBOL_day.png)")
    ap.add_argument("--all-hours", action="store_true", help="include pre-market and after-hours")
    args = ap.parse_args()

    directory = Path(args.dir)
    book, trades = load(directory, args.symbol)
    if not args.all_hours:
        book = book[(book["hour"] >= OPEN) & (book["hour"] <= CLOSE)]
        trades = trades[(trades["hour"] >= OPEN) & (trades["hour"] <= CLOSE)]
    if book.empty:
        raise SystemExit(f"no two-sided book for {args.symbol} in that time range")

    fig, (px, spread, vol) = plt.subplots(3, 1, figsize=(12, 9), sharex=True,
                                          gridspec_kw={"height_ratios": [2.4, 1, 1.2]})
    px.fill_between(book["hour"], book["bid"], book["ask"], step="post", color="#9ecae1", alpha=0.6, label="bid-ask")
    px.plot(book["hour"], (book["bid"] + book["ask"]) / 2, color="#08519c", lw=0.7, label="mid")
    lit = trades[trades["kind"].isin(["E", "C"])]
    sample = lit.sample(min(len(lit), 20_000), random_state=0) if len(lit) else lit
    px.scatter(sample["hour"], sample["price"], s=1, color="#e6550d", alpha=0.25, label="executions (sampled)")
    crosses = trades[trades["kind"] == "Q"]
    if len(crosses):
        px.scatter(crosses["hour"], crosses["price"], marker="D", s=40, color="black", zorder=5, label="opening/closing cross")
    px.set_ylabel("price ($)")
    legend = px.legend(loc="best")
    for handle in getattr(legend, "legend_handles", getattr(legend, "legendHandles", [])):
        if hasattr(handle, "set_sizes"):
            handle.set_sizes([25])  # same legend marker size for tiny dots and big diamonds
            handle.set_alpha(1)

    cents = (book["ask"] - book["bid"]) * 100
    span_h = book["hour"].max() - book["hour"].min()
    bucket = 1 / 60 if span_h > 1 else max(span_h / 120, 1e-9)  # 1-minute buckets for a full day
    by_bucket = cents.groupby((book["hour"] // bucket) * bucket)
    spread.fill_between(by_bucket.min().index, by_bucket.min(), by_bucket.max(), color="#a1d99b", alpha=0.6,
                        step="post", label="min-max")
    spread.plot(by_bucket.mean().index, by_bucket.mean(), color="#31a354", lw=1, drawstyle="steps-post", label="average")
    spread.set_ylabel("spread (cents)")
    spread.set_ylim(0, max(1.0, by_bucket.max().quantile(0.98) * 1.2))
    spread.legend(loc="upper right", fontsize=8)

    span = trades["hour"].max() - trades["hour"].min() if len(trades) else 0.0
    width = 5 / 60 if span > 1 else max(span / 60, 1e-6)  # 5-minute bars, or 60 bars for short files
    kinds = {"E": "lit", "C": "lit", "P": "hidden", "Q": "cross"}
    binned = trades.assign(bin=(trades["hour"] // width) * width, kind=trades["kind"].map(kinds))
    per_bin = binned.groupby(["bin", "kind"])["shares"].sum().unstack(fill_value=0)
    bottom = 0
    for kind, color in (("lit", "#3182bd"), ("hidden", "#9e9ac8"), ("cross", "#636363")):
        if kind in per_bin:
            vol.bar(per_bin.index, per_bin[kind] / 1e3, width=width, align="edge", bottom=bottom, color=color, label=kind)
            bottom = bottom + per_bin[kind] / 1e3
    minutes = round(width * 60)
    vol.set_ylabel(f"shares / {minutes} min (thousands)" if minutes >= 1 else "shares per bar (thousands)")
    vol.set_xlabel("hour of day (ET)")
    vol.legend(loc="upper center", ncol=3)

    total = trades["shares"].sum()
    vwap = (trades["price"] * trades["shares"]).sum() / total if total else float("nan")
    px.set_title(f"{args.symbol}: Nasdaq order book rebuilt from ITCH  |  {total:,.0f} shares, VWAP ${vwap:,.2f}")
    fig.tight_layout()
    out = Path(args.out) if args.out else directory / f"{args.symbol}_day.png"
    fig.savefig(out, dpi=130)
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
