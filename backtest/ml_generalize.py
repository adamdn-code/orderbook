"""Does the next-move model hold up away from the afternoon it was tested on?

    # other stocks, same day (2019-01-30): transfer the AAPL model, and train each stock's own
    python backtest/ml_generalize.py stocks ~/itch/MSFT_events.csv ~/itch/INTC_events.csv ...

    # a second day the models never saw (any part of): AAPL on 2019-12-30
    python backtest/ml_generalize.py day2 ~/itch/d2/AAPL_events.csv

    # every other sample day Nasdaq publishes (see cpp/itch/fetch_days.sh), each scored the same way
    python backtest/ml_generalize.py days ~/itch/days/*/AAPL_events.csv

`stocks` writes results/ml/cross_stock.json. For each stock, on its own 13:30-15:55 test period:
    imbalance       AUC of L1 imbalance (baseline)
    aapl_model      the AAPL model, applied unchanged (does the pattern transfer?)
    own_model       a model trained on that stock with exactly the AAPL protocol (train / tune / test)

`day2` writes results/ml/second_day.json. Everything about the models was fixed on 2019-01-30: the
trees, the filter thresholds and the pull rates chosen on that day's tuning hour, and the toxicity
models. 2019-12-30 is then scored end to end (9:35-15:55): AUC per label, and the market maker with
each filter at its day-1 settings. Nothing is refit, so this is a true out-of-sample test.
"""

from __future__ import annotations

import argparse
import json
import pickle
import time
from pathlib import Path

import numpy as np
import pandas as pd

from ml_signal import (FEATURES, LAT_LABELS, BASELINE, auc, block_bootstrap_ci, build, clean, event_signal, fit_models,
                       split)
from real_mm import NS, QUOTE_END, QUOTE_START, Strategy, markouts, run

ML = Path(__file__).resolve().parent / "results" / "ml"


def load_models() -> dict:
    with open(ML / "models.pkl", "rb") as fh:
        return pickle.load(fh)


def auc_block(ts, y, scores: dict[str, np.ndarray], n_boot: int) -> dict:
    res = {k: auc(y, v) for k, v in scores.items()}
    ci = block_bootstrap_ci(ts, y, scores, n_boot=n_boot)
    return {"auc": res, "ci95": ci, "n": int(len(y)), "base_rate": float(np.mean(y))}


def stocks(paths: list[str], n_boot: int) -> dict:
    models = load_models()
    out = {}
    for path in paths:
        sym = Path(path).name.split("_")[0]
        t0 = time.perf_counter()
        ev, f, _ = build(path)
        parts = split(f)
        te = parts["test"].dropna(subset=["next_move"])
        if len(te) < 5000:
            print(f"{sym}: only {len(te)} test samples, skipped")
            continue
        X, y = te[FEATURES].to_numpy(float), te["next_move"].to_numpy(int)
        own, info = fit_models(parts, "next_move")
        scores = {"imbalance": te[BASELINE].to_numpy(float),
                  "aapl_model": models["next_move"].predict_proba(X)[:, 1],
                  "own_model": own["gbm"].predict_proba(X)[:, 1]}
        r = auc_block(te["ts_ns"].to_numpy(), y, scores, n_boot)
        r.update({"events": ev.n, "mean_spread_cents": float(f["spread"].mean()),
                  "median_touch_shares": float(np.median(np.expm1(f[["log_bq1", "log_aq1"]].to_numpy()).mean(axis=1))),
                  "own_gbm_params": info["gbm_params"], "seconds": round(time.perf_counter() - t0)})
        out[sym] = r
        a = r["auc"]
        print(f"{sym:<5} n={r['n']:>8,}  spread {r['mean_spread_cents']:.2f}c  AUC imbalance {a['imbalance']:.3f}  "
              f"AAPL model {a['aapl_model']:.3f}  own model {a['own_model']:.3f}  "
              f"(own - imb CI {r['ci95']['own_model_minus_imbalance'][0]:+.3f}..{r['ci95']['own_model_minus_imbalance'][1]:+.3f})",
              flush=True)
    (ML / "cross_stock.json").write_text(json.dumps(clean(out), indent=1, allow_nan=False))
    return out


def day2(path: str, n_boot: int, workers: int, name: str = "second_day") -> dict:
    import multiprocessing as mp

    import ml_backtest as mb
    import ml_toxicity

    models = load_models()
    with open(ML / "selection.pkl", "rb") as fh:
        sel = pickle.load(fh)
    ev, f, idx = build(path)
    window = (QUOTE_START, QUOTE_END)
    day = f[(f["ts_ns"] >= window[0]) & (f["ts_ns"] < window[1])]
    out: dict = {"date": Path(path).parent.name, "events": ev.n, "samples": int(len(day)), "auc": {}, "backtest": []}
    for label in ["next_move", *LAT_LABELS]:
        d = day.dropna(subset=[label])
        Xl = d[FEATURES].to_numpy(float)
        scores = {"imbalance": d[BASELINE].to_numpy(float), "model": models[label].predict_proba(Xl)[:, 1]}
        out["auc"][label] = auc_block(d["ts_ns"].to_numpy(), d[label].to_numpy(int), scores, n_boot)
        a = out["auc"][label]["auc"]
        print(f"{out['date']} {label:<16} AUC imbalance {a['imbalance']:.4f}  day-1 model {a['model']:.4f}  "
              f"(diff CI {out['auc'][label]['ci95']['model_minus_imbalance'][0]:+.4f}..{out['auc'][label]['ci95']['model_minus_imbalance'][1]:+.4f})",
              flush=True)

    # per-event signals from the frozen day-1 models
    Xall = f[FEATURES].to_numpy(float)
    mb._EV = ev
    for label in ["next_move", *LAT_LABELS]:
        mb._ARR[f"p_{label}"] = event_signal(ev.n, idx, models[label].predict_proba(Xall)[:, 1].astype(np.float32))
    for lat, tm in sel["tox_models"].items():
        mb._ARR[f"tox_{lat}_bid"], mb._ARR[f"tox_{lat}_ask"] = ml_toxicity.signals(tm, f, ev.n, idx)
    jobs = []
    for lat in sorted({k[1] for k in sel["thresholds"]}):
        key = {"filter": "none", "pull_rate": 0.0, "threshold": 0.0, "placement": "join", "action": "pull", "latency_us": lat}
        jobs.append((key, mb.strategy("none", 0, "join", "pull", lat), None, None, window, False))
        for (filt, l, action), rate in sel["selected"].items():
            if l != lat:
                continue
            t = sel["thresholds"][(filt, lat, "test")][sel["pull_rates"].index(rate)]
            sig, tox = mb.signal_names(filt, lat, "test")
            key = {"filter": filt, "pull_rate": rate, "threshold": t, "placement": "join", "action": action, "latency_us": lat}
            jobs.append((key, mb.strategy(filt, t, "join", action, lat), sig, tox, window, False))
    results = mb.run_jobs(jobs, workers, out["date"])
    rows = pd.DataFrame([r for r, _ in results]).drop(columns=["seconds"])
    out["backtest"] = rows.to_dict("records")
    rows.to_csv(ML / f"{name}_backtest.csv", index=False, float_format="%.5g")
    print(rows[["latency_us", "filter", "action", "pull_rate", "pnl_with_rebate", "shares", "pnl_per_100_shares", "markout_1s"]]
          .round(3).to_string(index=False))
    (ML / f"{name}.json").write_text(json.dumps(clean(out), indent=1, allow_nan=False))
    return out


def days(paths: list[str], n_boot: int, workers: int) -> dict:
    """Every other day Nasdaq publishes, each scored like `day2`: day-1 models, thresholds and rates frozen.

    Each path is DATE/AAPL_events.csv with DATE in ISO form. Per-day results go to results/ml/days/;
    the summary (one row per day: AUC and each filter's PnL) to results/ml/days.json."""
    (ML / "days").mkdir(exist_ok=True)
    summary = []
    for path in sorted(paths):
        date = Path(path).parent.name
        r = day2(path, n_boot, workers, name=f"days/{date}")
        nm = r["auc"]["next_move"]
        row = {"date": date, "events": r["events"], "auc_model": nm["auc"]["model"], "auc_imbalance": nm["auc"]["imbalance"],
               "auc_gain_ci95": nm["ci95"]["model_minus_imbalance"], "pnl": {}}
        for b in r["backtest"]:
            k = f"{b['filter']}|{b['action']}|{b['latency_us']}"
            row["pnl"][k] = {"pnl_with_rebate": b["pnl_with_rebate"], "shares": b["shares"], "pull_rate": b["pull_rate"]}
        summary.append(row)
        (ML / "days.json").write_text(json.dumps(clean({"days": summary}), indent=1, allow_nan=False))
    return {"days": summary}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["stocks", "day2", "days"])
    ap.add_argument("events", nargs="+")
    ap.add_argument("--boot", type=int, default=200)
    ap.add_argument("--workers", type=int, default=2)
    a = ap.parse_args()
    if a.mode == "stocks":
        stocks(a.events, a.boot)
    elif a.mode == "days":
        days(a.events, a.boot, a.workers)
    else:
        day2(a.events[0], a.boot, a.workers)


if __name__ == "__main__":
    main()
