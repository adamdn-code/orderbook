"""Accuracy vs. nanoseconds: how small can the next-move model get before it stops beating imbalance?

    make -C cpp ml
    python backtest/model_size_tradeoff.py ~/itch/AAPL_events.csv     # writes results/ml/cpp_tradeoff.json, cpp_tradeoff.png

Trains the next-move model at several sizes (number of trees x depth) with the same split as
ml_signal.py (fit on train + tuning hour, scored on the test period), exports each to C++, and times
it in cpp/build/ml_replay (predict-only loop over every real decision of the day, plus single-call
percentiles). The full model's numbers come from export_model.py --check.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

import numpy as np

from export_model import export
from ml_signal import BASELINE, FEATURES, auc, build, clean, split

HERE = Path(__file__).resolve().parent
ML = HERE / "results" / "ml"
EXE = HERE.parent / "cpp" / "build" / "ml_replay"
SIZES = [  # (trees, max depth, learning rate)
    (10, 4, 0.3), (25, 4, 0.2), (50, 6, 0.1), (100, 6, 0.1), (200, 8, 0.05),
]


def time_model(events: str, model_path: Path) -> dict:
    with tempfile.TemporaryDirectory() as d:
        js = Path(d) / "out.json"
        subprocess.run([str(EXE), events, str(model_path), "--json", str(js), "--repeat", "1"], check=True, capture_output=True)
        return json.loads(js.read_text())


def main() -> None:
    from sklearn.ensemble import HistGradientBoostingClassifier

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("events")
    a = ap.parse_args()
    _, f, _ = build(a.events)
    parts = split(f)
    fit = np.vstack([parts["train"].dropna(subset=["next_move"])[FEATURES].to_numpy(float),
                     parts["val"].dropna(subset=["next_move"])[FEATURES].to_numpy(float)])
    y = np.concatenate([parts["train"]["next_move"].dropna().to_numpy(int), parts["val"]["next_move"].dropna().to_numpy(int)])
    te = parts["test"].dropna(subset=["next_move"])
    Xte, yte = te[FEATURES].to_numpy(float), te["next_move"].to_numpy(int)
    full = json.loads((ML / "cpp_inference.json").read_text())
    metrics = json.loads((ML / "metrics.json").read_text())
    rows = [{"trees": full["trees"], "max_depth": full.get("max_depth"), "label": "full (tuned)",
             "auc": metrics["labels"]["next_move"]["auc"]["gbm"], "predict_ns": full["predict_loop_ns"],
             "p50_ns": full["predict_p50_ns"], "p99_ns": full["predict_p99_ns"]}]
    for trees, depth, lr in SIZES:
        m = HistGradientBoostingClassifier(max_iter=trees, max_depth=depth, max_leaf_nodes=None, learning_rate=lr,
                                           min_samples_leaf=200, l2_regularization=1.0, early_stopping=False,
                                           random_state=0).fit(fit, y)
        r = auc(yte, m.predict_proba(Xte)[:, 1])
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.gbdt"
            export(m, p, len(FEATURES))
            t = time_model(a.events, p)
        rows.append({"trees": trees, "max_depth": depth, "label": f"{trees} trees, depth {depth}", "auc": r,
                     "predict_ns": t["predict_loop_ns"], "p50_ns": t["predict_p50_ns"], "p99_ns": t["predict_p99_ns"]})
        print(f"{trees:>4} trees depth {depth}: test AUC {r:.4f}  predict {t['predict_loop_ns']:.0f} ns "
              f"(p50 {t['predict_p50_ns']:.0f}, p99 {t['predict_p99_ns']:.0f})", flush=True)
    base = auc(yte, te[BASELINE].to_numpy(float))
    out = {"imbalance_auc": base, "rows": rows}
    (ML / "cpp_tradeoff.json").write_text(json.dumps(clean(out), indent=1))
    plot(rows, base)


def plot(rows, base) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    xs = [r["predict_ns"] for r in rows]
    ax.scatter(xs, [r["auc"] for r in rows], color="#1f6f8b", zorder=3)
    for r in rows:
        ax.annotate(r["label"], (r["predict_ns"], r["auc"]), textcoords="offset points", xytext=(6, -3), fontsize=8)
    ax.axhline(base, color="#c0392b", ls="--", lw=1, label=f"imbalance alone (AUC {base:.3f})")
    ax.set_xscale("log")
    ax.set_xlim(min(xs) / 1.5, max(xs) * 3)
    ax.set_xlabel("C++ prediction time (ns, log scale)")
    ax.set_ylabel("test AUC, next mid move")
    ax.set_title("Model size: accuracy vs. nanoseconds")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(ML / "cpp_tradeoff.png", dpi=130)
    print(f"wrote {ML / 'cpp_tradeoff.json'} and cpp_tradeoff.png")


if __name__ == "__main__":
    main()
