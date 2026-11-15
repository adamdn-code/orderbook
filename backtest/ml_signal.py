"""Predict the next mid-price move from order-flow features, and measure it against an imbalance baseline.

    python backtest/ml_signal.py ~/itch/AAPL_events.csv        # writes backtest/results/ml/

The question it answers: can order flow tell us which way the mid moves next, well enough to pull a
quote before it gets picked off? The backtest (real_mm.py) found that adverse selection, not
inventory, is what loses money, and that quotes landing alone at a fresh level are stale on arrival.
A model that sees the move coming is the direct fix, so this script builds one and grades it honestly.

Samples
    One sample per timestamp, taken after *every* event at that timestamp has been applied. ITCH
    reports one aggressive order that sweeps several resting orders as several executions with the
    same timestamp; nobody can act in the middle of that, so features never see half a sweep.
    Only 9:35-15:55 (the quoting window) is used, and the book must have both sides.

Labels (all look strictly forward from the sample's timestamp)
    next_move   1 if the next mid change is up, 0 if down (primary label; drops samples with no
                later change). Standard event-time target in the limit-order-book literature.
    up_100ms    1 if the mid 100 ms later is above the current mid, 0 if below (flat dropped).
    up_1s       same, 1 s later.

Features (all causal: computed only from events at or before the sample's timestamp)
    book shape  spread, L1/L2/top-5 depth imbalance, log sizes at the touch, microprice offset
    order flow  order-flow imbalance (OFI, Cont-Kukanov-Stoikov 2014) over 100 ms / 1 s / 10 s,
                normalised by touch depth; signed traded volume and trade counts over 1 s / 10 s
    price path  mid change over 1 s / 10 s, mid changes in last 10 s, time since the last mid
                change, direction of the last mid change
    Time of day is deliberately left out: the test period is the afternoon (with the 2 pm Fed
    announcement), and a clock feature would just memorise the morning.

Split (chronological, never shuffled)
    train 9:35-12:30 | validation 12:30-13:30 | test 13:30-15:55
    Samples in the 60 s before each boundary are dropped (embargo), so no training label looks
    into the next period. Hyperparameters are picked on validation; the final model is refit on
    train + validation and scored on test exactly once.

Models
    imbalance       L1 imbalance alone (the signal the backtest already tried). AUC of the raw value.
    logistic        logistic regression on all features (standardised)
    gbm             histogram gradient boosting (scikit-learn), depth/learning rate tuned on validation

AUC confidence intervals use a block bootstrap over 1-minute blocks: consecutive samples share
their label (everything before a mid change has the same "next move"), so resampling single rows
would make the intervals far too narrow.
"""

from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import numpy as np
import pandas as pd

from common import BUY, SELL, obfast
from real_mm import NS, QUOTE_END, QUOTE_START, Events

TRAIN_END = int(12.5 * 3600) * NS
VAL_END = int(13.5 * 3600) * NS
EMBARGO_NS = 60 * NS
FEATURES = [
    "spread", "imb_l1", "imb_l2", "imb_l5", "log_bq1", "log_aq1", "micro_offset",
    "ofi_100ms", "ofi_1s", "ofi_10s", "trade_imb_1s", "trade_imb_10s", "trades_1s", "trades_10s",
    "ret_1s", "ret_10s", "mid_changes_10s", "log_since_move", "last_move",
]
BASELINE = "imb_l1"
LABELS = ["next_move", "up_100ms", "up_1s"]


# ------------------------------------------------------------------------- replay -> snapshots

def snapshots(ev: Events) -> tuple[pd.DataFrame, np.ndarray]:
    """Replay the day and take a snapshot of the book after the last event of every timestamp.

    Returns (snapshot frame, event index of each snapshot). Book updates mirror real_mm.run so the
    model sees exactly the book the backtest trades against."""
    book = obfast.Book()
    ts_l, typ_l, ref_l, side_l, sh_l, px_l, nref_l = ev.ts, ev.type, ev.ref, ev.side, ev.shares, ev.price, ev.new_ref
    n = ev.n
    side_of: dict[int, int] = {}

    def add(ref: int, side: int, shares: int, price_c: int) -> None:
        try:
            book.limit(ref, side, shares, price_c)
            side_of[ref] = side
        except obfast.OrderBookError:
            pass

    cols = {k: [] for k in ("ts", "bb", "ba", "bq1", "aq1", "bq2", "aq2", "bq5", "aq5",
                            "cum_ofi", "cum_tv", "cum_tn", "cum_moves", "last_move_ts", "last_move")}
    ev_idx: list[int] = []
    cum_ofi = cum_tv = cum_tn = cum_moves = 0.0
    pb = pa = pbq = paq = None  # previous top of book, for OFI
    last_mid = None
    last_move_ts = 0
    last_move = 0
    depth = book.depth
    for i in range(n):
        typ, ref, side, shares, px = typ_l[i], ref_l[i], side_l[i], sh_l[i], px_l[i]
        price_c = px // 100
        if typ == "A":
            if px % 100 == 0 and side >= 0:
                add(ref, side, shares, price_c)
        elif typ in ("E", "C", "X"):
            rem = book.remaining(ref)
            if rem is not None:
                if typ in ("E", "C"):
                    s = side_of.get(ref, side)
                    q = min(shares, rem)
                    cum_tv += q if s == SELL else -q  # a resting sell executed = buyer-initiated
                    cum_tn += 1
                if shares >= rem:
                    book.cancel(ref)
                else:
                    book.modify(ref, qty=rem - shares)
        elif typ == "D":
            if book.remaining(ref) is not None:
                book.cancel(ref)
        elif typ == "U":
            if book.remaining(ref) is not None:
                s = side_of.get(ref, side)
                book.cancel(ref)
                if px % 100 == 0 and s >= 0:
                    add(nref_l[i], s, shares, price_c)
        if i + 1 < n and ts_l[i + 1] == ts_l[i]:
            continue  # mid-sweep: wait for the rest of this timestamp
        ts = ts_l[i]
        b, a = depth(BUY, 5), depth(SELL, 5)
        if not b or not a:
            continue
        bb, bq1 = b[0]
        ba, aq1 = a[0]
        # OFI increment from the previous top of book to this one
        if pb is not None:
            e = 0.0
            e += bq1 if bb >= pb else 0
            e -= pbq if bb <= pb else 0
            e -= aq1 if ba <= pa else 0
            e += paq if ba >= pa else 0
            cum_ofi += e
        pb, pbq, pa, paq = bb, bq1, ba, aq1
        mid = bb + ba  # in half-cents; only differences and signs are used
        if last_mid is not None and mid != last_mid:
            cum_moves += 1
            last_move = 1 if mid > last_mid else -1
            last_move_ts = ts
        last_mid = mid
        if ts < QUOTE_START - 15 * NS or ts >= QUOTE_END:
            continue  # history before 9:35 still feeds the cumulative counters above
        c = cols
        c["ts"].append(ts); c["bb"].append(bb); c["ba"].append(ba)
        c["bq1"].append(bq1); c["aq1"].append(aq1)
        c["bq2"].append(bq1 + (b[1][1] if len(b) > 1 else 0)); c["aq2"].append(aq1 + (a[1][1] if len(a) > 1 else 0))
        c["bq5"].append(sum(q for _, q in b)); c["aq5"].append(sum(q for _, q in a))
        c["cum_ofi"].append(cum_ofi); c["cum_tv"].append(cum_tv); c["cum_tn"].append(cum_tn)
        c["cum_moves"].append(cum_moves); c["last_move_ts"].append(last_move_ts); c["last_move"].append(last_move)
        ev_idx.append(i)
    snap = pd.DataFrame({k: np.asarray(v) for k, v in cols.items()})
    return snap, np.asarray(ev_idx, dtype=np.int64)


def _lag(ts: np.ndarray, values: np.ndarray, window_ns: int) -> np.ndarray:
    """Value of a cumulative series at the last snapshot at or before ts - window (0 before the start)."""
    j = np.searchsorted(ts, ts - window_ns, side="right") - 1
    return np.where(j >= 0, values[np.clip(j, 0, None)], values[0])


def features_and_labels(snap: pd.DataFrame) -> pd.DataFrame:
    s = snap
    ts = s["ts"].to_numpy()
    bq1, aq1 = s["bq1"].to_numpy(float), s["aq1"].to_numpy(float)
    bb, ba = s["bb"].to_numpy(float), s["ba"].to_numpy(float)
    mid = (bb + ba) / 2
    touch = (bq1 + aq1) / 2
    f = pd.DataFrame({"ts_ns": ts})
    f["spread"] = ba - bb
    f["imb_l1"] = (bq1 - aq1) / (bq1 + aq1)
    f["imb_l2"] = (s["bq2"] - s["aq2"]) / (s["bq2"] + s["aq2"])
    f["imb_l5"] = (s["bq5"] - s["aq5"]) / (s["bq5"] + s["aq5"])
    f["log_bq1"], f["log_aq1"] = np.log1p(bq1), np.log1p(aq1)
    micro = (bb * aq1 + ba * bq1) / (bq1 + aq1)
    f["micro_offset"] = (micro - mid) / f["spread"]
    ofi, tv, tn, mv = (s[k].to_numpy(float) for k in ("cum_ofi", "cum_tv", "cum_tn", "cum_moves"))
    for name, w in (("100ms", NS // 10), ("1s", NS), ("10s", 10 * NS)):
        f[f"ofi_{name}"] = (ofi - _lag(ts, ofi, w)) / touch
    for name, w in (("1s", NS), ("10s", 10 * NS)):
        vol = tv - _lag(ts, tv, w)
        f[f"trade_imb_{name}"] = vol / touch
        f[f"trades_{name}"] = tn - _lag(ts, tn, w)
        f[f"ret_{name}"] = mid - _lag(ts, mid, w)
    f["mid_changes_10s"] = mv - _lag(ts, mv, 10 * NS)
    f["log_since_move"] = np.log10(1 + (ts - s["last_move_ts"].to_numpy()) / 1e3)  # log10 microseconds
    f["last_move"] = s["last_move"].to_numpy()

    # ---- labels: strictly after this timestamp
    change = np.flatnonzero(np.diff(mid) != 0) + 1  # snapshot indices where the mid differs from the one before
    k = np.searchsorted(change, np.arange(len(mid)), side="right")
    has_next = k < len(change)
    nxt = mid[change[np.clip(k, 0, len(change) - 1)]]
    f["next_move"] = np.where(has_next, (nxt > mid).astype(float), np.nan)
    for name, h in (("up_100ms", NS // 10), ("up_1s", NS)):
        j = np.searchsorted(ts, ts + h, side="right") - 1
        later = mid[j]
        lab = np.where(later > mid, 1.0, np.where(later < mid, 0.0, np.nan))
        lab[ts + h > ts[-1]] = np.nan
        f[name] = lab
    return f


# ------------------------------------------------------------------------- evaluation

def split(f: pd.DataFrame) -> dict[str, pd.DataFrame]:
    t = f["ts_ns"]
    return {
        "train": f[(t >= QUOTE_START) & (t < TRAIN_END - EMBARGO_NS)],
        "val": f[(t >= TRAIN_END) & (t < VAL_END - EMBARGO_NS)],
        "test": f[(t >= VAL_END) & (t < QUOTE_END)],
    }


def auc(y: np.ndarray, p: np.ndarray) -> float:
    """Rank-based AUC (ties averaged)."""
    y = np.asarray(y, bool)
    npos, nneg = y.sum(), (~y).sum()
    if npos == 0 or nneg == 0:
        return float("nan")
    r = pd.Series(p).rank(method="average").to_numpy()
    return float((r[y].sum() - npos * (npos + 1) / 2) / (npos * nneg))


def block_bootstrap_ci(ts, y, scores: dict[str, np.ndarray], n_boot=200, block_ns=60 * NS, seed=0):
    """95% intervals for each score's AUC and for every score's AUC minus the first one's."""
    rng = np.random.default_rng(seed)
    blocks = (np.asarray(ts) // block_ns)
    ub, inv = np.unique(blocks, return_inverse=True)
    members = [np.flatnonzero(inv == b) for b in range(len(ub))]
    names = list(scores)
    draws = {n: [] for n in names}
    for _ in range(n_boot):
        pick = rng.integers(len(ub), size=len(ub))
        idx = np.concatenate([members[b] for b in pick])
        for n in names:
            draws[n].append(auc(y[idx], scores[n][idx]))
    base = np.array(draws[names[0]])
    out = {}
    for n in names:
        d = np.array(draws[n])
        out[n] = (float(np.nanpercentile(d, 2.5)), float(np.nanpercentile(d, 97.5)))
        if n != names[0]:
            out[f"{n}_minus_{names[0]}"] = (float(np.nanpercentile(d - base, 2.5)), float(np.nanpercentile(d - base, 97.5)))
    return out


def fit_models(parts: dict[str, pd.DataFrame], label: str, seed: int = 0):
    """Tune on validation, refit on train+val. Returns fitted models and the chosen GBM settings."""
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.linear_model import LogisticRegression
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler

    def xy(d):
        d = d.dropna(subset=[label])
        return d[FEATURES].to_numpy(float), d[label].to_numpy(int)

    Xtr, ytr = xy(parts["train"])
    Xva, yva = xy(parts["val"])
    best = None
    for lr in (0.03, 0.1):
        for leaves in (15, 63):
            m = HistGradientBoostingClassifier(learning_rate=lr, max_leaf_nodes=leaves, max_iter=400,
                                               min_samples_leaf=200, l2_regularization=1.0,
                                               early_stopping=False, random_state=seed)
            m.fit(Xtr, ytr)
            # pick the number of trees on validation as well
            for it, p in enumerate(m.staged_predict_proba(Xva), start=1):
                if it % 25:
                    continue
                a = auc(yva, p[:, 1])
                if best is None or a > best[0]:
                    best = (a, {"learning_rate": lr, "max_leaf_nodes": leaves, "max_iter": it})
    val_auc, params = best
    Xall = np.vstack([Xtr, Xva])
    yall = np.concatenate([ytr, yva])
    gbm = HistGradientBoostingClassifier(min_samples_leaf=200, l2_regularization=1.0, early_stopping=False,
                                         random_state=seed, **params).fit(Xall, yall)
    logit = make_pipeline(StandardScaler(), LogisticRegression(max_iter=2000)).fit(Xall, yall)
    return {"logistic": logit, "gbm": gbm}, {"gbm_params": params, "gbm_val_auc": val_auc}


def evaluate(f: pd.DataFrame, out: Path, n_boot: int = 200) -> dict:
    parts = split(f)
    report: dict = {"samples": {k: int(len(v)) for k, v in parts.items()}, "labels": {}}
    models_by_label = {}
    for label in LABELS:
        t0 = time.perf_counter()
        models, info = fit_models(parts, label)
        models_by_label[label] = models
        te = parts["test"].dropna(subset=[label])
        X, y = te[FEATURES].to_numpy(float), te[label].to_numpy(int)
        scores = {"imbalance": te[BASELINE].to_numpy(float)}
        for name, m in models.items():
            scores[name] = m.predict_proba(X)[:, 1]
        res = {name: auc(y, s) for name, s in scores.items()}
        ci = block_bootstrap_ci(te["ts_ns"].to_numpy(), y, scores, n_boot=n_boot)
        report["labels"][label] = {
            "test_samples": int(len(te)), "base_rate": float(y.mean()),
            "auc": res, "ci95": ci, **info, "fit_seconds": round(time.perf_counter() - t0, 1),
        }
        print(f"  {label:<10} n={len(te):>9,}  AUC imbalance {res['imbalance']:.4f}  logistic {res['logistic']:.4f}  "
              f"gbm {res['gbm']:.4f}  (gbm - imbalance 95% CI {ci['gbm_minus_imbalance'][0]:+.4f} .. "
              f"{ci['gbm_minus_imbalance'][1]:+.4f})")
    return report, models_by_label, parts


def extras(parts, model, out: Path, label: str = "next_move") -> dict:
    """ROC curves, AUC by half hour, calibration and permutation importance for the primary label."""
    from sklearn.inspection import permutation_importance
    from sklearn.metrics import roc_curve

    te = parts["test"].dropna(subset=[label])
    X, y = te[FEATURES].to_numpy(float), te[label].to_numpy(int)
    p = model.predict_proba(X)[:, 1]
    imb = te[BASELINE].to_numpy(float)
    roc = {}
    for name, s in (("gbm", p), ("imbalance", imb)):
        fpr, tpr, _ = roc_curve(y, s)
        keep = np.unique(np.linspace(0, len(fpr) - 1, 200).astype(int))
        roc[name] = {"fpr": fpr[keep].round(4).tolist(), "tpr": tpr[keep].round(4).tolist()}
    half = (te["ts_ns"].to_numpy() // (1800 * NS))
    by_time = []
    for h in np.unique(half):
        m = half == h
        by_time.append({"start": f"{int(h * 1800 // 3600):02d}:{int(h * 1800 % 3600 // 60):02d}",
                        "n": int(m.sum()), "gbm": auc(y[m], p[m]), "imbalance": auc(y[m], imb[m])})
    deciles = pd.qcut(p, 10, labels=False, duplicates="drop")
    calib = [{"mean_pred": float(p[deciles == d].mean()), "frac_up": float(y[deciles == d].mean()),
              "n": int((deciles == d).sum())} for d in np.unique(deciles)]
    rng = np.random.default_rng(0)
    sub = rng.choice(len(y), size=min(len(y), 150_000), replace=False)
    pi = permutation_importance(model, X[sub], y[sub], scoring="roc_auc", n_repeats=3, random_state=0)
    imp = sorted(({"feature": f, "auc_drop": float(m)} for f, m in zip(FEATURES, pi.importances_mean)),
                 key=lambda r: -r["auc_drop"])
    pd.DataFrame(by_time).to_csv(out / "auc_by_time.csv", index=False, float_format="%.4f")
    pd.DataFrame(imp).to_csv(out / "importance.csv", index=False, float_format="%.5f")
    pd.DataFrame(calib).to_csv(out / "calibration.csv", index=False, float_format="%.4f")
    return {"roc": roc, "auc_by_time": by_time, "calibration": calib, "importance": imp}


def charts(report, ex, out: Path) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (a, b) = plt.subplots(1, 2, figsize=(11, 4.4))
    lab = report["labels"]["next_move"]["auc"]
    for name, col in (("gbm", "#1f6f8b"), ("imbalance", "#c0392b")):
        a.plot(ex["roc"][name]["fpr"], ex["roc"][name]["tpr"], color=col, label=f"{name} (AUC {lab[name]:.3f})")
    a.plot([0, 1], [0, 1], color="#999", lw=0.7, ls="--")
    a.set_xlabel("false positive rate"); a.set_ylabel("true positive rate")
    a.set_title("Next mid move up vs down, test period"); a.legend(fontsize=8)
    imp = ex["importance"][:12][::-1]
    b.barh([r["feature"] for r in imp], [r["auc_drop"] for r in imp], color="#1f6f8b")
    b.set_xlabel("AUC lost when the feature is shuffled"); b.set_title("Permutation importance (test)")
    fig.tight_layout(); fig.savefig(out / "roc_importance.png", dpi=130); plt.close(fig)


# ------------------------------------------------------------------------- per-event signal for the backtest

def event_signal(n_events: int, snap_event_idx: np.ndarray, p: np.ndarray) -> np.ndarray:
    """P(next move up) as known at each event: the prediction from the latest *completed* timestamp.

    The last event of a timestamp gets its own snapshot's prediction (the backtest acts on it no
    earlier than 1 ns later); events in the middle of a timestamp see the previous one's."""
    k = np.searchsorted(snap_event_idx, np.arange(n_events), side="right") - 1
    sig = np.where(k >= 0, p[np.clip(k, 0, None)], np.nan)
    return sig.astype(np.float32)


def clean(o):
    """JSON-safe copy: NaN/inf become null (browsers reject NaN in JSON), numpy scalars become floats."""
    if isinstance(o, dict):
        return {k: clean(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [clean(v) for v in o]
    if isinstance(o, (float, np.floating)):
        return float(o) if math.isfinite(o) else None
    if isinstance(o, np.integer):
        return int(o)
    return o


def build(events_path: str):
    t0 = time.perf_counter()
    ev = Events.read(events_path)
    print(f"{ev.n:,} events loaded in {time.perf_counter() - t0:.1f}s")
    t0 = time.perf_counter()
    snap, idx = snapshots(ev)
    f = features_and_labels(snap)
    print(f"{len(f):,} snapshots with features in {time.perf_counter() - t0:.0f}s")
    return ev, f, idx


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("events", help="SYM_events.csv from itch_replay --track SYM --events")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent / "results" / "ml"))
    ap.add_argument("--boot", type=int, default=200, help="block-bootstrap resamples for the AUC intervals")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    ev, f, idx = build(args.events)
    report, models, parts = evaluate(f, out, n_boot=args.boot)
    gbm = models["next_move"]["gbm"]
    ex = extras(parts, gbm, out)
    charts(report, ex, out)
    p_all = gbm.predict_proba(f[FEATURES].to_numpy(float))[:, 1]
    np.savez_compressed(out / "signal.npz", snap_event_idx=idx, ts_ns=f["ts_ns"].to_numpy(),
                        p_up=p_all.astype(np.float32), imb_l1=f["imb_l1"].to_numpy(np.float32), n_events=ev.n)
    report["split"] = {"train": "09:35-12:30", "val": "12:30-13:30", "test": "13:30-15:55", "embargo_s": 60}
    report["features"] = FEATURES
    (out / "metrics.json").write_text(json.dumps(clean({**report, **{k: ex[k] for k in ("auc_by_time", "importance", "calibration", "roc")}}),
                                                 indent=1, allow_nan=False))
    print(f"Wrote {out}/metrics.json, auc_by_time.csv, importance.csv, calibration.csv, roc_importance.png, signal.npz")


if __name__ == "__main__":
    main()
