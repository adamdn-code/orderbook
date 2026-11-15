"""Does SPY tell us where AAPL goes next? Cross-asset (lead-lag) features for the next-move model.

    python backtest/ml_spy.py ~/itch/AAPL_events.csv ~/itch/SPY_events.csv      # writes results/ml/spy_features.json

SPY (the S&P 500 ETF) trades far more than any one stock and reacts first to market-wide news, so its
order flow is a classic leading signal for single stocks. For every AAPL snapshot at time t, these
SPY features are computed from SPY's book as of t (its last snapshot at or before t, so causal):

    spy_ret_100ms / 1s / 10s    SPY mid change over the window ending at t (cents)
    spy_ofi_100ms / 1s          SPY order-flow imbalance over the window, per share at SPY's touch
    spy_trade_imb_1s            SPY signed traded volume over 1 s, per share at the touch
    spy_imb_l1                  SPY L1 depth imbalance
    spy_move_since_aapl_move    how far SPY's mid has moved since AAPL's mid last changed: the
                                "SPY already moved and AAPL hasn't caught up" feature
    spy_log_since_move          time since SPY's mid last changed (log10 microseconds)

The AAPL model is retrained with its 19 features plus these 9, with exactly the same split and tuning,
and compared with the AAPL-only model on the same test samples. The interval on the difference uses
the same 1-minute block bootstrap.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
import pandas as pd

from ml_signal import (FEATURES, BASELINE, _libm, auc, block_bootstrap_ci, clean, features_and_labels, fit_models,
                       snapshots, split)
from real_mm import NS, Events

ML = Path(__file__).resolve().parent / "results" / "ml"
SPY_FEATURES = ["spy_ret_100ms", "spy_ret_1s", "spy_ret_10s", "spy_ofi_100ms", "spy_ofi_1s", "spy_trade_imb_1s",
                "spy_imb_l1", "spy_move_since_aapl_move", "spy_log_since_move"]


def spy_features(aapl_snap: pd.DataFrame, spy_snap: pd.DataFrame) -> pd.DataFrame:
    t = aapl_snap["ts"].to_numpy()
    s_ts = spy_snap["ts"].to_numpy()
    mid = (spy_snap["bb"].to_numpy(float) + spy_snap["ba"].to_numpy(float)) / 2
    ofi, tv = spy_snap["cum_ofi"].to_numpy(float), spy_snap["cum_tv"].to_numpy(float)
    bq, aq = spy_snap["bq1"].to_numpy(float), spy_snap["aq1"].to_numpy(float)

    def at(x, when):  # SPY value as of `when` (its last snapshot at or before; the first one if none)
        j = np.searchsorted(s_ts, when, side="right") - 1
        return x[np.clip(j, 0, None)]

    touch = (at(bq, t) + at(aq, t)) / 2
    out = pd.DataFrame(index=aapl_snap.index)
    for name, w in (("100ms", NS // 10), ("1s", NS), ("10s", 10 * NS)):
        out[f"spy_ret_{name}"] = at(mid, t) - at(mid, t - w)
    for name, w in (("100ms", NS // 10), ("1s", NS)):
        out[f"spy_ofi_{name}"] = (at(ofi, t) - at(ofi, t - w)) / touch
    out["spy_trade_imb_1s"] = (at(tv, t) - at(tv, t - NS)) / touch
    out["spy_imb_l1"] = (at(bq, t) - at(aq, t)) / (at(bq, t) + at(aq, t))
    out["spy_move_since_aapl_move"] = at(mid, t) - at(mid, aapl_snap["last_move_ts"].to_numpy())
    out["spy_log_since_move"] = _libm(math.log10, 1 + (t - at(spy_snap["last_move_ts"].to_numpy(), t)) / 1e3)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("aapl")
    ap.add_argument("spy")
    ap.add_argument("--boot", type=int, default=200)
    a = ap.parse_args()

    a_snap, _ = snapshots(Events.read(a.aapl))
    s_snap, _ = snapshots(Events.read(a.spy))
    f = features_and_labels(a_snap)
    f = pd.concat([f, spy_features(a_snap, s_snap)], axis=1)
    parts = split(f)
    both = FEATURES + SPY_FEATURES
    out = {"spy_features": SPY_FEATURES, "labels": {}}
    for label in ("next_move", "up_100ms"):
        base, _ = fit_models(parts, label, features=FEATURES)
        plus, info = fit_models(parts, label, features=both)
        te = parts["test"].dropna(subset=[label])
        y = te[label].to_numpy(int)
        scores = {"aapl_only": base["gbm"].predict_proba(te[FEATURES].to_numpy(float))[:, 1],
                  "with_spy": plus["gbm"].predict_proba(te[both].to_numpy(float))[:, 1],
                  "imbalance": te[BASELINE].to_numpy(float)}
        res = {k: auc(y, v) for k, v in scores.items()}
        ci = block_bootstrap_ci(te["ts_ns"].to_numpy(), y, scores, n_boot=a.boot)
        out["labels"][label] = {"n": int(len(te)), "auc": res, "ci95": ci, "gbm_params": info["gbm_params"]}
        print(f"{label:<10} AUC AAPL-only {res['aapl_only']:.4f}  with SPY {res['with_spy']:.4f}  "
              f"(diff CI {ci['with_spy_minus_aapl_only'][0]:+.4f}..{ci['with_spy_minus_aapl_only'][1]:+.4f})", flush=True)
        if label == "next_move":
            from sklearn.inspection import permutation_importance

            rng = np.random.default_rng(0)
            sub = rng.choice(len(te), size=min(len(te), 150_000), replace=False)
            pi = permutation_importance(plus["gbm"], te[both].to_numpy(float)[sub], y[sub], scoring="roc_auc",
                                        n_repeats=3, random_state=0)
            out["importance"] = sorted(({"feature": c, "auc_drop": float(m)} for c, m in zip(both, pi.importances_mean)),
                                       key=lambda r: -r["auc_drop"])
            print("top features:", [(r["feature"], round(r["auc_drop"], 4)) for r in out["importance"][:8]])
    (ML / "spy_features.json").write_text(json.dumps(clean(out), indent=1, allow_nan=False))
    print(f"wrote {ML / 'spy_features.json'}")


if __name__ == "__main__":
    main()
