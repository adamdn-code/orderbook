"""A model trained on the market maker's own fills: how much will a fill on this side lose?

The direction model (ml_signal.py) answers "which way is the next move?". What the market maker
actually cares about is narrower: "if my bid gets hit right now, what is that fill worth one second
later?" This module learns that directly, from the fills the backtest itself produces.

    data     run the plain market maker (join best + skew, at the given latency) on 9:35-13:30;
             every fill is one example, weighted by shares
    label    1 s markout in cents per share: side * (mid 1 s after the fill - fill price), clipped to
             +-5 cents so a handful of fills during the Fed jump don't dominate the loss
    features the 19 order-flow features at the last moment a cancel could still have beaten the
             fill (fill time - latency), turned to the fill's point of view: for an ask fill every
             directional feature is negated and the bid/ask sizes swap places, so one model serves
             both sides ("pressure toward me", "my queue", "the other queue")
    split    trained on fills from 9:35-12:30, number of trees picked on 12:30-13:30 fills, scored on
             fills from the plain market maker in the 13:30-15:55 test period

At run time the model is applied to every snapshot twice (as a bid and as an ask), giving a
predicted markout per side; the backtest pulls (or shifts) a side while its prediction is below a
threshold.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ml_signal import EMBARGO_NS, FEATURES, TRAIN_END, VAL_END, auc, event_signal
from real_mm import NS, QUOTE_END, QUOTE_START, Events, Strategy, run

DIRECTIONAL = ["imb_l1", "imb_l2", "imb_l5", "micro_offset", "ofi_100ms", "ofi_1s", "ofi_10s",
               "trade_imb_1s", "trade_imb_10s", "ret_1s", "ret_10s", "last_move"]
NEUTRAL = ["spread", "trades_1s", "trades_10s", "mid_changes_10s", "log_since_move"]
SIDE_FEATURES = [f"{c}_toward_me" for c in DIRECTIONAL] + ["log_my_size", "log_other_size"] + NEUTRAL
HORIZON_NS = NS
CLIP_CENTS = 5.0
assert set(DIRECTIONAL + NEUTRAL + ["log_bq1", "log_aq1"]) == set(FEATURES)


def side_matrix(f: pd.DataFrame, side: np.ndarray | int) -> np.ndarray:
    """Features from the point of view of a resting order on `side` (+1 bid, -1 ask)."""
    d = np.broadcast_to(np.asarray(side, float), (len(f),))
    cols = [f[c].to_numpy(float) * d for c in DIRECTIONAL]
    bq, aq = f["log_bq1"].to_numpy(float), f["log_aq1"].to_numpy(float)
    cols += [np.where(d > 0, bq, aq), np.where(d > 0, aq, bq)]
    cols += [f[c].to_numpy(float) for c in NEUTRAL]
    return np.column_stack(cols)


def fill_examples(ev: Events, f: pd.DataFrame, latency_ns: int, window) -> pd.DataFrame:
    """Fills of the plain market maker in `window`, each with its decision-time snapshot and markout."""
    strat = Strategy("tox data", skew_step=200, latency_ns=latency_ns)
    _, _, fills, (mt, mv) = run(ev, strat, window=window, stop_early=window[1] < QUOTE_END)
    if fills.empty:
        return fills.assign(snap=[], markout=[])
    ts = fills["ts_ns"].to_numpy()
    later = mv[np.clip(np.searchsorted(mt, ts + HORIZON_NS, side="right") - 1, 0, len(mv) - 1)]
    fills = fills.assign(markout=fills["sign"].to_numpy() * (later * 100 - fills["price_c"].to_numpy()))
    # last snapshot at or before the moment a cancel had to leave to arrive before the fill
    snap_ts = f["ts_ns"].to_numpy()
    k = np.searchsorted(snap_ts, ts - max(latency_ns, 1), side="right") - 1
    fills = fills.assign(snap=k)
    return fills[k >= 0].reset_index(drop=True)


def train(ev: Events, f: pd.DataFrame, latency_ns: int, seed: int = 0) -> tuple[object, dict]:
    from sklearn.ensemble import HistGradientBoostingRegressor

    fit = fill_examples(ev, f, latency_ns, (QUOTE_START, VAL_END))
    tr = fit[fit["ts_ns"] < TRAIN_END - EMBARGO_NS]
    va = fit[fit["ts_ns"] >= TRAIN_END]

    def xyw(d):
        X = side_matrix(f.iloc[d["snap"].to_numpy()].reset_index(drop=True), d["sign"].to_numpy())
        return X, np.clip(d["markout"].to_numpy(float), -CLIP_CENTS, CLIP_CENTS), d["qty"].to_numpy(float)

    Xtr, ytr, wtr = xyw(tr)
    Xva, yva, wva = xyw(va)
    m = HistGradientBoostingRegressor(learning_rate=0.05, max_leaf_nodes=15, min_samples_leaf=100,
                                      l2_regularization=1.0, max_iter=400, early_stopping=False, random_state=seed)
    m.fit(Xtr, ytr, sample_weight=wtr)
    best_it, best_mse = 1, np.inf
    for it, p in enumerate(m.staged_predict(Xva), start=1):
        mse = float(np.average((p - yva) ** 2, weights=wva))
        if mse < best_mse:
            best_it, best_mse = it, mse
    model = HistGradientBoostingRegressor(learning_rate=0.05, max_leaf_nodes=15, min_samples_leaf=100,
                                          l2_regularization=1.0, max_iter=best_it, early_stopping=False,
                                          random_state=seed).fit(Xtr, ytr, sample_weight=wtr)

    # score on the test afternoon's fills
    te = fill_examples(ev, f, latency_ns, (VAL_END, QUOTE_END))
    Xte, yte, wte = xyw(te)
    pred = model.predict(Xte)
    base = te["sign"].to_numpy() * f["imb_l1"].to_numpy()[te["snap"].to_numpy()]  # imbalance toward me
    bad = yte < 0
    report = {
        "latency_ns": latency_ns, "train_fills": int(len(tr)), "val_fills": int(len(va)), "test_fills": int(len(te)),
        "trees": best_it,
        "test_auc_bad_fill": auc(bad, -pred), "test_auc_bad_fill_imbalance": auc(bad, -base),
        "test_spearman": float(pd.Series(pred).corr(pd.Series(yte), method="spearman")),
        "test_spearman_imbalance": float(pd.Series(base).corr(pd.Series(yte), method="spearman")),
        "test_mean_markout": float(np.average(yte, weights=wte)),
    }
    # how much worse are the fills it flags? (test, by predicted quintile)
    q = pd.qcut(pred, 5, labels=False, duplicates="drop")
    report["test_markout_by_quintile"] = [float(np.average(yte[q == i], weights=wte[q == i])) for i in np.unique(q)]
    return model, report


def signals(model, f: pd.DataFrame, n_events: int, snap_event_idx: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Predicted markout of a bid fill and of an ask fill, as known at each event."""
    pb = model.predict(side_matrix(f, 1)).astype(np.float32)
    pa = model.predict(side_matrix(f, -1)).astype(np.float32)
    return event_signal(n_events, snap_event_idx, pb), event_signal(n_events, snap_event_idx, pa)
