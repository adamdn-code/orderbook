"""The whole pipeline end to end on a synthetic day, in about a minute. No data download needed.

    python backtest/demo.py            (or `make demo` from the repo root, which also builds and tests the C++)

1. A synthetic Apple-like day of order events, 9:34 to 15:59, with real but weak predictability in
   the order flow (synth_events.py). The real day comes from `itch_replay --track AAPL --events`.
2. The same causal features and labels as the real study (ml_signal.py), with the same split:
   train 9:35-12:30, tune 12:30-13:30, test 13:30-15:55.
3. One gradient-boosted model of the next mid move, scored on the test afternoon against book imbalance.
4. The market maker on the test afternoon with exact queue position (real_mm.py): no filter, an
   imbalance filter and the model filter, each active 20% of the time by a threshold set on the
   tuning hour.

It writes nothing under results/ or docs/: the real study's outputs stay as they are. Synthetic
numbers are not the paper's numbers; the point is that every stage runs.
"""

from __future__ import annotations

import argparse
import tempfile
import time
from pathlib import Path

import numpy as np
import pandas as pd

from ml_signal import BASELINE, FEATURES, TRAIN_END, VAL_END, auc, build, event_signal, split
from real_mm import QUOTE_END, Strategy, run
from synth_events import generate


def step(msg: str, t0: float) -> float:
    t = time.perf_counter()
    print(f"  {msg} ({t - t0:.1f}s)", flush=True)
    return t


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gap-ms", type=float, default=40.0, help="mean time between synthetic events (15 = AAPL's rate)")
    a = ap.parse_args()
    from sklearn.ensemble import HistGradientBoostingClassifier

    t = time.perf_counter()
    print("Demo: synthetic day -> features -> model -> market maker")
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "SYN_events.csv"
        df = generate(minutes=385, seed=1, gap_ms=a.gap_ms)
        df.to_csv(path, index=False)
        t = step(f"1. synthetic day: {len(df):,} order events", t)
        ev, f, idx = build(str(path))
    t = step(f"2. features and labels: {len(f):,} book snapshots, {len(FEATURES)} features", t)

    parts = split(f)
    fit = pd.concat([parts["train"], parts["val"]]).dropna(subset=["next_move"])
    test = parts["test"].dropna(subset=["next_move"])
    gbm = HistGradientBoostingClassifier(learning_rate=0.1, max_leaf_nodes=15, max_iter=100, min_samples_leaf=200,
                                         l2_regularization=1.0, early_stopping=False, random_state=0)
    gbm.fit(fit[FEATURES].to_numpy(float), fit["next_move"].to_numpy(int))
    y = test["next_move"].to_numpy(int)
    a_model = auc(y, gbm.predict_proba(test[FEATURES].to_numpy(float))[:, 1])
    a_imb = auc(y, test[BASELINE].to_numpy(float))
    t = step(f"3. next-move model on the test afternoon: AUC {a_model:.3f} vs {a_imb:.3f} for book imbalance", t)

    p = gbm.predict_proba(f[FEATURES].to_numpy(float))[:, 1].astype(np.float32)
    tune = (f["ts_ns"] >= TRAIN_END).to_numpy() & (f["ts_ns"] < VAL_END).to_numpy()
    model_thr = float(np.quantile(np.abs(p[tune] - 0.5), 0.8))                 # active 20% of the tuning hour
    imb_thr = float(np.quantile(np.abs(f["imb_l1"].to_numpy()[tune]), 0.8))
    signal = event_signal(ev.n, idx, p)
    window = (VAL_END, QUOTE_END)
    rows = []
    for name, strat, sig in [("no filter", Strategy("none", skew_step=200), None),
                             ("imbalance", Strategy("imbalance", skew_step=200, imbalance_pull=imb_thr), None),
                             ("next-move model", Strategy("model", skew_step=200, model_pull=model_thr), signal)]:
        summary, *_ = run(ev, strat, signal=sig, window=window)
        rows.append((name, summary["pnl_with_rebate"], summary["shares"]))
    step("4. market maker on the test afternoon, filters active 20% of the time (set on the tuning hour)", t)
    print()
    print(f"  {'filter':<18}{'PnL':>10}{'shares':>10}{'per 100 sh':>12}")
    for name, pnl, shares in rows:
        print(f"  {name:<18}{pnl:>10,.0f}{shares:>10,}{100 * pnl / max(shares, 1):>12.3f}")
    print("\n  Synthetic data, not the paper's numbers: here quoting is already profitable, so a filter has little")
    print("  to save. On Apple it is not (backtest/README.md, docs/paper.pdf).")


if __name__ == "__main__":
    main()
