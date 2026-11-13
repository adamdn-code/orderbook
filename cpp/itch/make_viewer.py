"""Build the order book replay viewer: one self-contained HTML page.

    python cpp/itch/make_viewer.py AAPL --dir ~/itch --date 2019-01-30 \
        --summary ~/itch/run2.txt --out docs/index.html

Reads DIR/SYM_book.csv (from itch_replay --track SYM --depth N) and
DIR/SYM_trades.csv, keeps the regular session, packs everything into compact
integer arrays and writes them into viewer_template.html.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
OPEN_S, CLOSE_S = 9.5 * 3600, 16 * 3600 + 60  # regular session, plus a minute for the closing cross
KINDS = {"E": 0, "C": 0, "P": 1, "Q": 2}  # lit, hidden, cross


def parse_summary(path: Path | None) -> dict:
    """Pull the headline numbers out of itch_replay's printed summary."""
    if not path or not path.exists():
        return {}
    text = path.read_text()

    def grab(pattern, cast=float):
        m = re.search(pattern, text)
        return cast(m.group(1).replace(",", "")) if m else None

    return {
        "messages": grab(r"(\d+) messages, [\d.]+ GB", int),
        "stocks": grab(r"symbols with activity: (\d+)", int),
        "orders": grab(r"orders added: (\d+)", int),
        "missing": grab(r"didn't have:\s+(\d+) of", int),
        "refs": grab(r"didn't have:\s+\d+ of (\d+)", int),
        "front_pct": grab(r"first order in line:\s+([\d.]+)%"),
        "best_pct": grab(r"at our best price:\s+([\d.]+)%"),
        "resting_end": grab(r"resting at end: (\d+)", int),
        "seconds": grab(r"GB uncompressed, ([\d.]+) s"),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("symbol")
    ap.add_argument("--dir", default=".")
    ap.add_argument("--date", default="", help="trading date shown on the page, e.g. 2019-01-30")
    ap.add_argument("--summary", help="itch_replay output saved to a file (for the headline checks)")
    ap.add_argument("--step", type=float, default=2.0, help="seconds between book snapshots kept (default 2)")
    ap.add_argument("--out", default="docs/index.html")
    args = ap.parse_args()

    d = Path(args.dir).expanduser()
    book = pd.read_csv(d / f"{args.symbol}_book.csv")
    trades = pd.read_csv(d / f"{args.symbol}_trades.csv")
    book["s"] = book["ts_ns"] / 1e9
    trades["s"] = trades["ts_ns"] / 1e9
    book = book[(book["s"] >= OPEN_S) & (book["s"] <= CLOSE_S)]
    trades = trades[(trades["s"] >= OPEN_S) & (trades["s"] <= CLOSE_S)]
    book = book[(book["bid"] > 0) & (book["ask"] > book["bid"])]
    if book.empty:
        raise SystemExit("no two-sided book in the regular session; was itch_replay run with --track?")

    # One snapshot per step: the last sample in each bucket.
    book = book.groupby((book["s"] // args.step).astype(int)).tail(1).reset_index(drop=True)

    depth = 1 + sum(1 for c in book.columns if re.fullmatch(r"bid\d+", c))
    def col(prefix, i):
        return prefix if i == 1 else f"{prefix}{i}"

    prices = np.concatenate([book[col(p, i)].to_numpy() for p in ("bid", "ask") for i in range(1, depth + 1)]
                            + [trades["price"].to_numpy()])
    nonzero = prices[prices > 0]
    scale = 100 if np.allclose(nonzero * 100, np.round(nonzero * 100)) else 10_000  # cents if possible

    def ticks(series):
        return np.round(series.to_numpy() * scale).astype(np.int64)

    bp, ap_ = ticks(book["bid"]), ticks(book["ask"])
    bq = np.stack([book[col("bid_qty", i)].to_numpy() for i in range(1, depth + 1)], axis=1).astype(np.int64)
    aq = np.stack([book[col("ask_qty", i)].to_numpy() for i in range(1, depth + 1)], axis=1).astype(np.int64)
    # Deeper levels as distances from the best price (small numbers; 0 = level empty).
    bo = np.stack([np.where(book[col("bid", i)] > 0, bp - ticks(book[col("bid", i)]), 0) for i in range(2, depth + 1)],
                  axis=1) if depth > 1 else np.zeros((len(book), 0), dtype=np.int64)
    ao = np.stack([np.where(book[col("ask", i)] > 0, ticks(book[col("ask", i)]) - ap_, 0) for i in range(2, depth + 1)],
                  axis=1) if depth > 1 else np.zeros((len(book), 0), dtype=np.int64)

    trades = trades.sort_values("ts_ns", kind="stable")
    t_ms = np.round(trades["s"].to_numpy() * 1000).astype(np.int64)
    data = {
        "symbol": args.symbol,
        "date": args.date,
        "scale": scale,
        "depth": depth,
        "step": args.step,
        "snap_t": np.round(book["s"].to_numpy(), 3).tolist(),
        "bp": bp.tolist(),
        "ap": ap_.tolist(),
        "bq": bq.ravel().tolist(),
        "aq": aq.ravel().tolist(),
        "bo": bo.ravel().tolist(),
        "ao": ao.ravel().tolist(),
        "trade_t0": int(t_ms[0]) if len(t_ms) else 0,
        "trade_dt": np.diff(t_ms, prepend=t_ms[0] if len(t_ms) else 0).tolist(),  # ms since previous trade
        "trade_p": ticks(trades["price"]).tolist(),
        "trade_q": trades["shares"].astype(np.int64).tolist(),
        "trade_k": trades["kind"].map(KINDS).fillna(0).astype(int).tolist(),
        "summary": parse_summary(Path(args.summary).expanduser() if args.summary else None),
    }

    template = (HERE / "viewer_template.html").read_text()
    blob = json.dumps(data, separators=(",", ":"))
    html = template.replace("/*__DATA__*/null", blob).replace("__SYMBOL__", args.symbol)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(html)
    print(f"Wrote {out} ({out.stat().st_size / 1e6:.1f} MB): {len(book):,} book snapshots, "
          f"{len(trades):,} trades, {depth} levels")


if __name__ == "__main__":
    main()
