"""Does the model's edge survive contact with the order book? Quote-filter study + playground data.

    python backtest/ml_signal.py   ~/itch/AAPL_events.csv     # first: trains the models, writes results/ml/signal.npz
    python backtest/ml_backtest.py ~/itch/AAPL_events.csv     # then: this, writes results/ml/ and docs/playground_data.json

Every reported run quotes only in the test period (13:30-15:55), which no model saw in training or
tuning. Filters, all on the same market maker (inventory skew, quoting at or 1 cent behind the best):

    none        quote all the time
    imbalance   pull the threatened side while |L1 imbalance| is high (the old signal)
    model       pull the threatened side while the next-move model is confident
    model_lat   same, with the model trained for this latency (labels: the first move after the
                cancel could land; ml_signal.LAT_LABELS). At zero latency it's the same as `model`.
    toxicity    pull a side while the fill-markout model (ml_toxicity.py) predicts a fill there loses
                more than a threshold

Each filter can either pull the threatened quote or shift it one cent further back ("shift").

Fair comparison: the earlier study found that a filter can cut the loss just by trading less. So no
threshold is hand-picked; each signal gets the thresholds at which it would have been pulling a quote
10%, 20%, ... 60% of the time in the tuning period (12:30-13:30). At equal pull rates a better signal
should lose less per share, not just trade fewer shares.

Choosing the pull rate without looking at the test period: for every filter and latency, the pull
rate is picked by running the backtest on the tuning hour with models fit on the morning only, and
taking the most profitable rate there. The headline tables report the test period at those rates
only. Every combination is still saved for the playground, which shows real backtest results only.
"""

from __future__ import annotations

import argparse
import json
import multiprocessing as mp
import time
from pathlib import Path

import numpy as np
import pandas as pd

from ml_signal import LAT_LABELS, TRAIN_END, VAL_END, build, clean, event_signal
from real_mm import NS, QUOTE_END, Events, Strategy, markouts, run

ROOT = Path(__file__).resolve().parents[1]
TEST = (VAL_END, QUOTE_END)
TUNE = (TRAIN_END, VAL_END)
PULL_RATES = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]
LATENCIES_US = [0, 100, 1_000, 10_000]
LAT_LABEL_OF_US = {0: "next_move", **{v // 1000: k for k, v in LAT_LABELS.items()}}
FILTERS = ["imbalance", "model", "model_lat", "toxicity"]
SKEW_STEP = 200
RECORD_NS = 30 * NS

_EV: Events | None = None
_ARR: dict[str, np.ndarray] = {}  # per-event signal arrays, by name


def quantile_thresholds(x: np.ndarray, rates, low_tail: bool = False) -> list[float]:
    """Cut-offs at which the filter is active `rates` of the time (x = |signal|, or a markout for low_tail)."""
    x = x[np.isfinite(x)]
    return [float(np.quantile(x, r if low_tail else 1 - r)) for r in rates]


def strategy(filt: str, thr: float, placement: str, action: str, lat_us: int) -> Strategy:
    base = dict(back_ticks=1 if placement == "behind" else 0, skew_step=SKEW_STEP, latency_ns=lat_us * 1000,
                shift=action == "shift")
    if filt == "none":
        return Strategy("none", **base)
    if filt == "imbalance":
        return Strategy(filt, imbalance_pull=thr, **base)
    if filt in ("model", "model_lat"):
        return Strategy(filt, model_pull=thr, **base)
    return Strategy(filt, tox_threshold=thr, **base)


def signal_names(filt: str, lat_us: int, period: str) -> tuple[str | None, str | None]:
    """Which per-event arrays a run needs: (p_up array, toxicity pair prefix)."""
    suffix = "_train" if period == "tune" else ""
    if filt == "model":
        return f"p_next_move{suffix}", None
    if filt == "model_lat":
        return f"p_{LAT_LABEL_OF_US[lat_us]}{suffix}", None
    if filt == "toxicity":
        return None, f"tox_{lat_us}"
    return None, None


def _one(job):
    key, strat, sig_name, tox_name, window, stop_early = job
    t0 = time.perf_counter()
    summary, tl, fills, mids = run(_EV, strat, record_every_ns=RECORD_NS, window=window, stop_early=stop_early,
                                   signal=_ARR[sig_name] if sig_name else None,
                                   tox=(_ARR[tox_name + "_bid"], _ARR[tox_name + "_ask"]) if tox_name else None)
    mk = markouts(fills, mids, [0.1, 1, 10])
    row = {**key,
           "pnl_with_rebate": summary["pnl_with_rebate"], "pnl": summary["pnl"], "rebates": summary["rebates"],
           "shares": summary["shares"], "fills": summary["fills"], "edge_cents": summary["edge_cents"],
           "markout_0.1s": mk[0.1], "markout_1s": mk[1], "markout_10s": mk[10],
           "max_abs_inventory": summary["max_abs_inventory"], "max_drawdown": summary["max_drawdown"],
           "seconds": round(time.perf_counter() - t0, 1)}
    row["pnl_per_100_shares"] = row["pnl_with_rebate"] / max(row["shares"], 1) * 100
    tl = tl[tl["ts_ns"] <= window[1]]
    series = {"t": (tl["ts_ns"] // NS).astype(int).tolist(), "pnl": tl["pnl_with_rebate"].round(2).tolist(),
              "inv": tl["inventory"].astype(int).tolist(), "mid": tl["mid"].round(3).tolist()}
    return row, series


def run_jobs(jobs, workers, label):
    print(f"{len(jobs)} {label} backtests on {workers} workers ...", flush=True)
    t0 = time.perf_counter()
    out = []
    with mp.get_context("fork").Pool(workers) as pool:
        for k, res in enumerate(pool.imap(_one, jobs, chunksize=1), 1):
            out.append(res)
            r = res[0]
            if k % 10 == 0 or k == len(jobs):
                print(f"  [{k:>3}/{len(jobs)}] {r['placement']:<6} {r['action']:<5} {r['latency_us']:>6} us  {r['filter']:<9} "
                      f"pull {r['pull_rate']:.0%}  pnl ${r['pnl_with_rebate']:>8,.0f}  shares {r['shares']:>8,}  "
                      f"per 100 ${r['pnl_per_100_shares']:+.3f}  ({time.perf_counter() - t0:.0f}s)", flush=True)
    return out


def main() -> None:
    global _EV
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("events")
    ap.add_argument("--ml", default=str(Path(__file__).resolve().parent / "results" / "ml"))
    ap.add_argument("--playground", default=str(ROOT / "docs" / "playground_data.json"))
    ap.add_argument("--workers", type=int, default=max(1, mp.cpu_count()))
    ap.add_argument("--quick", action="store_true", help="100 us latency, join placement, pull action only")
    ap.add_argument("--refresh-extra", action="store_true",
                    help="only re-read the side studies (other stocks, day 2, SPY, C++) into the playground data")
    args = ap.parse_args()
    ml = Path(args.ml)
    if args.refresh_extra:
        refresh_extra(Path(args.playground), ml)
        return
    t_start = time.perf_counter()

    ev, f, idx = build(args.events)
    _EV = ev
    z = np.load(ml / "signal.npz")
    if int(z["n_events"]) != ev.n or len(z["ts_ns"]) != len(f):
        raise SystemExit("signal.npz was built from a different events file; rerun ml_signal.py")
    ts = z["ts_ns"]
    tune_mask = (ts >= TRAIN_END) & (ts < VAL_END)
    lats = [100] if args.quick else LATENCIES_US

    # ---- per-event signals and thresholds (thresholds always from the tuning hour)
    thr: dict[tuple[str, int, str], list[float]] = {}  # (filter, latency, period) -> thresholds
    for lat in lats:
        thr[("imbalance", lat, "tune")] = thr[("imbalance", lat, "test")] = quantile_thresholds(np.abs(z["imb_l1"][tune_mask]), PULL_RATES)
        for filt in ("model", "model_lat"):
            for period in ("tune", "test"):
                name, _ = signal_names(filt, lat, period)
                if name not in _ARR:
                    _ARR[name] = event_signal(ev.n, idx, z[name])
                thr[(filt, lat, period)] = quantile_thresholds(np.abs(z[name][tune_mask].astype(float) - 0.5), PULL_RATES)

    import ml_toxicity
    tox_reports, tox_models = {}, {}
    for lat in lats:
        t0 = time.perf_counter()
        model, rep = ml_toxicity.train(ev, f, lat * 1000)
        tb, ta = ml_toxicity.signals(model, f, ev.n, idx)
        _ARR[f"tox_{lat}_bid"], _ARR[f"tox_{lat}_ask"] = tb, ta
        pb = model.predict(ml_toxicity.side_matrix(f[tune_mask], 1))
        pa = model.predict(ml_toxicity.side_matrix(f[tune_mask], -1))
        thr[("toxicity", lat, "tune")] = thr[("toxicity", lat, "test")] = quantile_thresholds(np.minimum(pb, pa), PULL_RATES, low_tail=True)
        tox_reports[lat] = rep
        tox_models[lat] = model
        print(f"toxicity model @ {lat} us: {rep['train_fills']:,} train fills, test AUC(bad fill) {rep['test_auc_bad_fill']:.3f} "
              f"vs imbalance {rep['test_auc_bad_fill_imbalance']:.3f}, test markout by predicted quintile "
              f"{np.round(rep['test_markout_by_quintile'], 2)}  ({time.perf_counter() - t0:.0f}s)", flush=True)

    # ---- 1) choose the pull rate on the tuning hour (morning-only models), join placement
    actions = ["pull"] if args.quick else ["pull", "shift"]
    tune_jobs = []
    for lat in lats:
        for filt in FILTERS:
            if filt == "model_lat" and lat == 0:
                continue
            for action in actions:
                sig, tox = signal_names(filt, lat, "tune")
                for r, t in zip(PULL_RATES, thr[(filt, lat, "tune")]):
                    key = {"filter": filt, "pull_rate": r, "threshold": t, "placement": "join", "action": action,
                           "latency_us": lat, "period": "tune"}
                    tune_jobs.append((key, strategy(filt, t, "join", action, lat), sig, tox, TUNE, True))
    tune = pd.DataFrame([r for r, _ in run_jobs(tune_jobs, args.workers, "tuning-hour")])
    best = (tune.sort_values("pnl_with_rebate", ascending=False)
            .groupby(["filter", "latency_us", "action"], as_index=False).first()[["filter", "latency_us", "action", "pull_rate"]])
    selected = {(r.filter, r.latency_us, r.action): r.pull_rate for r in best.itertuples()}

    # ---- 2) the test period: every combination
    jobs = []
    placements = ["join"] if args.quick else ["join", "behind"]
    for place in placements:
        for lat in lats:
            for action in (actions if place == "join" else ["pull"]):
                if action == "pull":
                    key = {"filter": "none", "pull_rate": 0.0, "threshold": 0.0, "placement": place, "action": "pull",
                           "latency_us": lat, "period": "test"}
                    jobs.append((key, strategy("none", 0, place, "pull", lat), None, None, TEST, False))
                for filt in FILTERS:
                    if filt == "model_lat" and lat == 0:
                        continue
                    sig, tox = signal_names(filt, lat, "test")
                    for r, t in zip(PULL_RATES, thr[(filt, lat, "test")]):
                        key = {"filter": filt, "pull_rate": r, "threshold": t, "placement": place, "action": action,
                               "latency_us": lat, "period": "test"}
                        jobs.append((key, strategy(filt, t, place, action, lat), sig, tox, TEST, False))
    results = run_jobs(jobs, args.workers, "test-period")
    rows = pd.DataFrame([r for r, _ in results])
    rows["selected"] = [selected.get((r.filter, r.latency_us, r.action)) == r.pull_rate if r.placement == "join" else False
                        for r in rows.itertuples()]
    rows.to_csv(ml / "filters.csv", index=False, float_format="%.5g")
    # everything a later, untouched day needs: thresholds and pull rates chosen here, toxicity models
    import pickle
    with open(ml / "selection.pkl", "wb") as fh:
        pickle.dump({"thresholds": {k: v for k, v in thr.items() if k[2] == "test"}, "selected": selected,
                     "pull_rates": PULL_RATES, "tox_models": tox_models}, fh)
    tune.to_csv(ml / "filters_tuning_hour.csv", index=False, float_format="%.5g")
    headline = headline_table(rows)
    headline.to_csv(ml / "headline.csv", index=False, float_format="%.5g")
    print("\nTest period at pull rates chosen on the tuning hour (join best):")
    print(headline.round(3).to_string(index=False))
    chart(rows, ml)

    metrics = json.loads((ml / "metrics.json").read_text())
    metrics["toxicity"] = {str(k): v for k, v in tox_reports.items()}
    (ml / "metrics.json").write_text(json.dumps(clean(metrics), indent=1, allow_nan=False))
    first = results[0][1]
    data = {
        "meta": {"symbol": "AAPL", "date": "2019-01-30", "window": "13:30–15:55 ET, the models' test period",
                 "skew_step": SKEW_STEP, "quote_size": Strategy().size, "rebate_per_share": Strategy().rebate_per_share,
                 "pull_rates": PULL_RATES, "latencies_us": lats, "placements": placements, "actions": actions,
                 "filters": FILTERS,
                 "selected": [{"filter": k[0], "latency_us": k[1], "action": k[2], "pull_rate": v} for k, v in selected.items()]},
        "model": {k: metrics[k] for k in ("labels", "latency_labels", "samples", "split", "features", "auc_by_time",
                                          "importance", "calibration", "roc", "toxicity") if k in metrics},
        "extra": extra_results(ml),
        "mid": {"t": first["t"], "mid": first["mid"]},
        "runs": [{**{k: v for k, v in r.items() if k not in ("seconds", "period")},
                  "series": {"pnl": s["pnl"], "inv": s["inv"]}} for r, s in results],
    }
    Path(args.playground).parent.mkdir(parents=True, exist_ok=True)
    Path(args.playground).write_text(json.dumps(clean(data), separators=(",", ":"), allow_nan=False))
    print(f"Wrote {ml}/filters.csv, filters_tuning_hour.csv, headline.csv, filters.png and {args.playground} "
          f"({time.perf_counter() - t_start:.0f}s)")


def refresh_extra(playground: Path, ml: Path) -> None:
    """Re-read the side studies into an existing playground_data.json without rerunning the backtests."""
    data = json.loads(playground.read_text())
    data["extra"] = extra_results(ml)
    playground.write_text(json.dumps(clean(data), separators=(",", ":"), allow_nan=False))


def extra_results(ml: Path) -> dict:
    """Results from the other studies (other stocks, second day, SPY features, C++ inference), if they've been run."""
    out = {}
    for name in ("cross_stock", "second_day", "spy_features", "cpp_inference", "cpp_tradeoff"):
        p = ml / f"{name}.json"
        if p.exists():
            out[name] = json.loads(p.read_text())
    return out


def headline_table(rows: pd.DataFrame) -> pd.DataFrame:
    j = rows[(rows["placement"] == "join")]
    out = []
    for lat in sorted(j["latency_us"].unique()):
        base = j[(j["latency_us"] == lat) & (j["filter"] == "none")].iloc[0]
        out.append({"latency_us": lat, "filter": "none", "action": "-", "pull_rate": 0.0,
                    **{k: base[k] for k in ("pnl_with_rebate", "shares", "pnl_per_100_shares", "markout_1s")}})
        sel = j[(j["latency_us"] == lat) & j["selected"]]
        for r in sel.sort_values(["action", "filter"]).itertuples():
            out.append({"latency_us": lat, "filter": r.filter, "action": r.action, "pull_rate": r.pull_rate,
                        "pnl_with_rebate": r.pnl_with_rebate, "shares": r.shares,
                        "pnl_per_100_shares": r.pnl_per_100_shares, "markout_1s": r.markout_1s})
    return pd.DataFrame(out)


def chart(rows: pd.DataFrame, out: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    colors = {"imbalance": "#c0392b", "model": "#1f6f8b", "model_lat": "#2e8b57", "toxicity": "#7d5ba6"}
    j = rows[(rows["placement"] == "join") & (rows["action"] == "pull")]
    lats = sorted(j["latency_us"].unique())
    fig, axes = plt.subplots(1, len(lats), figsize=(4.2 * len(lats), 4.0), sharey=False, squeeze=False)
    for ax, lat in zip(axes[0], lats):
        d = j[j["latency_us"] == lat]
        for flt, col in colors.items():
            g = d[d["filter"] == flt].sort_values("pull_rate")
            if len(g):
                ax.plot(g["shares"] / 1000, g["pnl_per_100_shares"], marker="o", ms=4, color=col, label=flt)
                s = g[g["selected"]]
                ax.scatter(s["shares"] / 1000, s["pnl_per_100_shares"], s=110, facecolors="none", edgecolors=col, lw=1.6)
        n = d[d["filter"] == "none"]
        ax.scatter(n["shares"] / 1000, n["pnl_per_100_shares"], color="#555", zorder=3, label="no filter")
        ax.axhline(0, color="#999", lw=0.6)
        ax.set_title(f"latency {lat} µs" if lat < 1000 else f"latency {lat // 1000} ms")
        ax.set_xlabel("shares traded (thousands)")
    axes[0][0].set_ylabel("PnL per 100 shares, with rebate ($)")
    axes[0][0].legend(fontsize=7)
    fig.suptitle("Test period, pull action: loss per share at matched pull rates (circled: rate chosen on the tuning hour)", fontsize=10)
    fig.tight_layout()
    fig.savefig(out / "filters.png", dpi=120)
    plt.close(fig)


if __name__ == "__main__":
    main()
