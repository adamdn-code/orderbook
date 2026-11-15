"""Does the model's edge survive contact with the order book? Quote filter study + playground data.

    python backtest/ml_signal.py   ~/itch/AAPL_events.csv     # first: trains the model, writes results/ml/signal.npz
    python backtest/ml_backtest.py ~/itch/AAPL_events.csv     # then: this, writes results/ml/ and docs/playground_data.json

Every run quotes only in the model's *test* period (13:30-15:55), which it never saw in training or
tuning. Three filters are compared on the same market maker (join the best bid/ask, inventory skew):

    none        quote all the time
    imbalance   pull the threatened side while |L1 imbalance| is high (the existing signal)
    model       pull the threatened side while the model is confident about the next move

Fair comparison: the earlier study found that a filter can cut the loss just by trading less. So the
thresholds are not hand-picked per signal; each signal gets the thresholds at which it would have
been pulling a quote 10%, 20%, ... 60% of the time on the validation period. At equal "pull rates"
a better signal should lose less *per share* and have better markouts, not just fewer shares.

Every combination of filter x pull rate x latency x quote placement is run and saved for the
interactive playground (docs/playground.html), which therefore shows real backtest results only.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import time
from dataclasses import asdict
from pathlib import Path

import numpy as np
import pandas as pd

from ml_signal import TRAIN_END, clean, VAL_END, event_signal
from real_mm import NS, QUOTE_END, Events, Strategy, markouts, run

ROOT = Path(__file__).resolve().parents[1]
WINDOW = (VAL_END, QUOTE_END)
PULL_RATES = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]
LATENCIES_US = [0, 100, 1_000, 10_000]
PLACEMENTS = {"join": 0, "behind": 1}  # back_ticks
SKEW_STEP = 200
RECORD_NS = 30 * NS

_EV: Events | None = None
_SIG: np.ndarray | None = None


def thresholds(val_abs: np.ndarray) -> list[float]:
    """|signal| cut-offs at which the filter is active PULL_RATES of the time."""
    return [float(np.quantile(val_abs, 1 - r)) for r in PULL_RATES]


def configs(imb_thr, model_thr):
    out = []
    for place, back in PLACEMENTS.items():
        for lat in LATENCIES_US:
            base = dict(back_ticks=back, skew_step=SKEW_STEP, latency_ns=lat * 1000)
            out.append(({"filter": "none", "pull_rate": 0.0, "threshold": 0.0, "placement": place, "latency_us": lat},
                        Strategy("none", **base)))
            for r, t in zip(PULL_RATES, imb_thr):
                out.append(({"filter": "imbalance", "pull_rate": r, "threshold": t, "placement": place, "latency_us": lat},
                            Strategy("imbalance", imbalance_pull=t, **base)))
            for r, t in zip(PULL_RATES, model_thr):
                out.append(({"filter": "model", "pull_rate": r, "threshold": t, "placement": place, "latency_us": lat},
                            Strategy("model", model_pull=t, **base)))
    return out


def _one(job):
    key, strat = job
    t0 = time.perf_counter()
    summary, tl, fills, mids = run(_EV, strat, record_every_ns=RECORD_NS,
                                   signal=_SIG if strat.model_pull else None, window=WINDOW)
    mk = markouts(fills, mids, [0.1, 1, 10])
    row = {**key,
           "pnl_with_rebate": summary["pnl_with_rebate"], "pnl": summary["pnl"], "rebates": summary["rebates"],
           "shares": summary["shares"], "fills": summary["fills"], "edge_cents": summary["edge_cents"],
           "markout_0.1s": mk[0.1], "markout_1s": mk[1], "markout_10s": mk[10],
           "max_abs_inventory": summary["max_abs_inventory"], "max_drawdown": summary["max_drawdown"],
           "seconds": round(time.perf_counter() - t0, 1)}
    row["pnl_per_100_shares"] = row["pnl_with_rebate"] / max(row["shares"], 1) * 100
    tl = tl[tl["ts_ns"] <= WINDOW[1]]
    series = {"t": (tl["ts_ns"] // NS).astype(int).tolist(),
              "pnl": tl["pnl_with_rebate"].round(2).tolist(),
              "inv": tl["inventory"].astype(int).tolist(),
              "mid": tl["mid"].round(3).tolist()}
    return row, series


def main() -> None:
    global _EV, _SIG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("events")
    ap.add_argument("--ml", default=str(Path(__file__).resolve().parent / "results" / "ml"))
    ap.add_argument("--playground", default=str(ROOT / "docs" / "playground_data.json"))
    ap.add_argument("--workers", type=int, default=max(1, mp.cpu_count()))
    ap.add_argument("--quick", action="store_true", help="only 100 us latency and the join placement")
    args = ap.parse_args()
    ml = Path(args.ml)

    _EV = Events.read(args.events)
    z = np.load(ml / "signal.npz")
    if int(z["n_events"]) != _EV.n:
        raise SystemExit("signal.npz was built from a different events file; rerun ml_signal.py")
    _SIG = event_signal(_EV.n, z["snap_event_idx"], z["p_up"])

    # thresholds from the validation period's snapshots (never the test period)
    ts, p = z["ts_ns"], z["p_up"].astype(float)
    val = (ts >= TRAIN_END) & (ts < VAL_END)
    imb_thr = thresholds(np.abs(z["imb_l1"][val]))
    model_thr = thresholds(np.abs(p[val] - 0.5))
    print("imbalance thresholds", np.round(imb_thr, 3), "\nmodel thresholds    ", np.round(model_thr, 4))

    jobs = configs(imb_thr, model_thr)
    if args.quick:
        jobs = [j for j in jobs if j[0]["latency_us"] == 100 and j[0]["placement"] == "join"]
    print(f"{len(jobs)} backtests on {args.workers} workers ...")
    t0 = time.perf_counter()
    ctx = mp.get_context("fork")
    with ctx.Pool(args.workers) as pool:
        results = []
        for k, res in enumerate(pool.imap(_one, jobs), 1):
            results.append(res)
            r = res[0]
            print(f"  [{k:>3}/{len(jobs)}] {r['placement']:<6} {r['latency_us']:>6} us  {r['filter']:<9} pull {r['pull_rate']:.0%}  "
                  f"pnl ${r['pnl_with_rebate']:>8,.0f}  shares {r['shares']:>8,}  per 100 ${r['pnl_per_100_shares']:+.3f}  "
                  f"mk1s {r['markout_1s']:+.2f}c")
    print(f"done in {time.perf_counter() - t0:.0f}s")
    rows = pd.DataFrame([r for r, _ in results])
    rows.to_csv(ml / "filters.csv", index=False, float_format="%.5g")
    chart(rows, ml)

    metrics = json.loads((ml / "metrics.json").read_text())
    first = results[0][1]
    data = {
        "meta": {"symbol": "AAPL", "date": "2019-01-30", "window": "13:30–15:55 ET, the model's test period",
                 "skew_step": SKEW_STEP, "quote_size": Strategy().size, "rebate_per_share": Strategy().rebate_per_share,
                 "pull_rates": PULL_RATES, "latencies_us": LATENCIES_US, "placements": list(PLACEMENTS),
                 "imbalance_thresholds": imb_thr, "model_thresholds": model_thr},
        "model": {k: metrics[k] for k in ("labels", "samples", "split", "features", "auc_by_time", "importance",
                                          "calibration", "roc")},
        "mid": {"t": first["t"], "mid": first["mid"]},
        "runs": [{**{k: v for k, v in r.items() if k != "seconds"},
                  "series": {"pnl": s["pnl"], "inv": s["inv"]}} for r, s in results],
    }
    Path(args.playground).parent.mkdir(parents=True, exist_ok=True)
    Path(args.playground).write_text(json.dumps(clean(data), separators=(",", ":"), allow_nan=False))
    print(f"Wrote {ml}/filters.csv, filters.png and {args.playground}")


def chart(rows: pd.DataFrame, out: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    d = rows[(rows["placement"] == "join") & (rows["latency_us"] == (100 if (rows["latency_us"] == 100).any() else rows["latency_us"].iloc[0]))]
    fig, (a, b) = plt.subplots(1, 2, figsize=(11, 4.3))
    for flt, col in (("imbalance", "#c0392b"), ("model", "#1f6f8b")):
        g = d[d["filter"] == flt].sort_values("pull_rate")
        a.plot(g["shares"], g["pnl_per_100_shares"], marker="o", color=col, label=flt)
        b.plot(g["shares"], g["markout_1s"], marker="o", color=col, label=flt)
    n = d[d["filter"] == "none"]
    for ax, col in ((a, "pnl_per_100_shares"), (b, "markout_1s")):
        ax.scatter(n["shares"], n[col], color="#555", zorder=3, label="no filter")
        ax.axhline(0, color="#999", lw=0.6)
        ax.set_xlabel("shares traded (fewer = filter pulls more often)")
        ax.legend(fontsize=8)
    a.set_ylabel("PnL per 100 shares, with rebate ($)")
    b.set_ylabel("1 s markout (cents per share)")
    a.set_title("Loss per share at matched pull rates")
    b.set_title("Adverse selection at matched pull rates")
    fig.tight_layout()
    fig.savefig(out / "filters.png", dpi=130)
    plt.close(fig)


if __name__ == "__main__":
    main()
