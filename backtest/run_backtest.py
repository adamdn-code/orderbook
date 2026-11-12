"""Sweep the market maker's spread and inventory skew, then chart the results.

    python backtest/run_backtest.py            # full sweep, 50k steps per run
    python backtest/run_backtest.py --quick    # 10k steps, fewer settings (writes results/quick/)

Writes to backtest/results/:
    sweep.csv     one row per (half_spread, skew) with PnL, Sharpe, drawdown, ...
    sweep.png     heatmaps of PnL, Sharpe and max drawdown
    compare.png   PnL and inventory over time: no skew vs the best skewed setting
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

from sim import MarketParams, MMParams, run  # noqa: E402

OUT = Path(__file__).resolve().parent / "results"


def sweep(market: MarketParams, spreads, skews) -> pd.DataFrame:
    rows = []
    for hs in spreads:
        for sk in skews:
            t0 = time.perf_counter()
            summary, _, _ = run(market, MMParams(half_spread=hs, skew=sk))
            rows.append(summary)
            print(
                f"  half_spread={hs:<4} skew={sk:<5} pnl=${summary['pnl']:>9,.0f}  sharpe={summary['sharpe_1k']:6.2f}"
                f"  max_dd=${summary['max_drawdown']:>7,.0f}  fills={summary['fills']:>6,}  ({time.perf_counter() - t0:.1f}s)"
            )
    return pd.DataFrame(rows)


def heatmaps(df: pd.DataFrame, path: Path) -> None:
    panels = [
        ("pnl", "Final PnL ($)", "RdYlGn", "{:,.0f}"),
        ("sharpe_1k", "Sharpe (1k-step blocks)", "RdYlGn", "{:.1f}"),
        ("max_drawdown", "Max drawdown ($)", "RdYlGn_r", "{:,.0f}"),
    ]
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.6))
    for ax, (col, title, cmap, fmt) in zip(axes, panels):
        grid = df.pivot(index="half_spread", columns="skew", values=col).sort_index(ascending=False)
        im = ax.imshow(grid.to_numpy(), cmap=cmap, aspect="auto")
        ax.set_xticks(range(grid.shape[1]), [f"{c:g}" for c in grid.columns])
        ax.set_yticks(range(grid.shape[0]), [f"{r:g}" for r in grid.index])
        ax.set_xlabel("inventory skew (ticks per 100 shares)")
        ax.set_ylabel("half spread (ticks)")
        ax.set_title(title)
        for (i, j), v in np.ndenumerate(grid.to_numpy()):
            ax.text(j, i, fmt.format(v), ha="center", va="center", fontsize=8)
        fig.colorbar(im, ax=ax, shrink=0.8)
    fig.suptitle("Market-making parameter sweep (same simulated order flow for every cell)")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def compare(market: MarketParams, a: MMParams, b: MMParams, path: Path) -> None:
    _, ta, _ = run(market, a)
    _, tb, _ = run(market, b)
    fig, (top, mid, bot) = plt.subplots(3, 1, figsize=(11, 8), sharex=True, gridspec_kw={"height_ratios": [2, 1.4, 1]})
    for tl, p, color in ((ta, a, "#c0392b"), (tb, b, "#1f6f8b")):
        label = f"half_spread={p.half_spread:g}, skew={p.skew:g}"
        top.plot(tl["step"], tl["pnl"], color=color, label=label, lw=1.4)
        mid.plot(tl["step"], tl["inventory"], color=color, lw=1)
    bot.plot(ta["step"], ta["mid"] / 100, color="#555", lw=1, label="book mid")
    bot.plot(ta["step"], ta["fundamental"] / 100, color="#bbb", lw=1, label="hidden fundamental")
    top.set_ylabel("PnL ($)")
    top.legend(loc="upper left")
    top.set_title("Same market, with and without inventory skew")
    mid.set_ylabel("inventory (shares)")
    mid.axhline(0, color="#999", lw=0.6)
    bot.set_ylabel("price ($)")
    bot.set_xlabel("step")
    bot.legend(loc="upper left")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="10k steps and a smaller grid (for a fast check)")
    ap.add_argument("--seed", type=int, default=MarketParams.seed)
    args = ap.parse_args()

    if args.quick:
        market = MarketParams(steps=10_000, seed=args.seed)
        spreads, skews = [1.0, 2.0], [0.0, 0.5]
    else:
        market = MarketParams(seed=args.seed)
        spreads, skews = [0.5, 1.0, 1.5, 2.0, 3.0, 4.0], [0.0, 0.25, 0.5, 1.0]

    out = OUT / "quick" if args.quick else OUT
    out.mkdir(parents=True, exist_ok=True)
    print(f"Sweeping {len(spreads) * len(skews)} settings, {market.steps:,} steps each")
    t0 = time.perf_counter()
    df = sweep(market, spreads, skews)
    print(f"done in {time.perf_counter() - t0:.0f}s")

    cols = ["half_spread", "skew", "pnl", "sharpe_1k", "max_drawdown", "fills", "volume",
            "mean_abs_inventory", "max_abs_inventory", "edge_ticks", "markout_100_ticks", "pnl_per_100_shares"]
    df[cols].to_csv(out / "sweep.csv", index=False, float_format="%.4g")
    heatmaps(df, out / "sweep.png")

    best = df.loc[df["sharpe_1k"].idxmax()]
    no_skew = df[df["skew"] == 0].loc[lambda d: d["pnl"].idxmax()]
    compare(market, MMParams(half_spread=no_skew["half_spread"], skew=0.0),
            MMParams(half_spread=best["half_spread"], skew=best["skew"]), out / "compare.png")

    print("\nBest Sharpe:", f"half_spread={best['half_spread']:g}, skew={best['skew']:g}, "
          f"pnl=${best['pnl']:,.0f}, max drawdown=${best['max_drawdown']:,.0f}")
    print("Best with no skew:", f"half_spread={no_skew['half_spread']:g}, pnl=${no_skew['pnl']:,.0f}, "
          f"max drawdown=${no_skew['max_drawdown']:,.0f}")
    print(f"Wrote {out / 'sweep.csv'}, sweep.png, compare.png")


if __name__ == "__main__":
    main()
