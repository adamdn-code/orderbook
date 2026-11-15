"""Export the next-move model to the text format cpp/ml/gbdt.hpp reads, then check the C++ against Python.

    python backtest/export_model.py                      # results/ml/models.pkl -> cpp/ml/next_move.gbdt
    make -C cpp ml
    python backtest/export_model.py --check ~/itch/AAPL_events.csv
        # runs cpp/build/ml_replay, compares every feature and probability with the Python pipeline,
        # and writes results/ml/cpp_inference.json (parity + nanoseconds per decision)

Format (gbdt1): a header `gbdt1 n_features n_trees baseline`, then per tree `tree n_nodes` and one
line per node: `feature threshold_or_value left right is_leaf missing_go_left`. Numbers are written
with 17 significant digits, so thresholds round-trip exactly.
"""

from __future__ import annotations

import argparse
import json
import pickle
import subprocess
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ML = HERE / "results" / "ml"
OUT = ROOT / "cpp" / "ml" / "next_move.gbdt"


def export(model, path: Path, n_features: int) -> None:
    if model.n_trees_per_iteration_ != 1:
        raise ValueError("only binary classifiers / single-output models")
    lines = [f"gbdt1 {n_features} {len(model._predictors)} {float(np.ravel(model._baseline_prediction)[0])!r}"]
    for (tree,) in model._predictors:
        nodes = tree.nodes
        if nodes["is_categorical"].any():
            raise ValueError("categorical splits aren't supported")
        lines.append(f"tree {len(nodes)}")
        for n in nodes:
            leaf = int(n["is_leaf"])
            val = float(n["value"]) if leaf else float(n["num_threshold"])
            lines.append(f"{int(n['feature_idx'])} {val!r} {int(n['left'])} {int(n['right'])} {leaf} {int(n['missing_go_to_left'])}")
    path.write_text("\n".join(lines) + "\n")


def check(events: str) -> dict:
    import sys

    sys.path.insert(0, str(HERE))
    from ml_signal import FEATURES, build

    with open(ML / "models.pkl", "rb") as fh:
        saved = pickle.load(fh)
    dump = Path("/tmp/ml_replay_dump.bin")
    js = Path("/tmp/ml_replay.json")
    exe = ROOT / "cpp" / "build" / "ml_replay"
    res = subprocess.run([str(exe), events, str(OUT), "--dump", str(dump), "--json", str(js)], capture_output=True, text=True, check=True)
    print(res.stdout)
    perf = json.loads(js.read_text())
    d = np.fromfile(dump, dtype=np.float64).reshape(-1, len(FEATURES) + 2)
    _, f, _ = build(events)
    py_X = f[FEATURES].to_numpy(float)
    py_p = saved["next_move"].predict_proba(py_X)[:, 1]
    same_rows = len(d) == len(f) and np.array_equal(d[:, 0].astype(np.int64), f["ts_ns"].to_numpy())
    feat_diff = np.nanmax(np.abs(d[:, 1:-1] - py_X), axis=0) if same_rows else None
    p_diff = np.abs(d[:, -1] - py_p) if same_rows else None
    # the C++ model on the *Python* features isolates tree evaluation from feature arithmetic
    out = {
        "rows_cpp": int(len(d)), "rows_python": int(len(f)), "same_timestamps": bool(same_rows),
        "max_abs_feature_diff": {k: float(v) for k, v in zip(FEATURES, feat_diff)} if same_rows else None,
        "max_abs_prob_diff": float(p_diff.max()) if same_rows else None,
        "rows_prob_diff_over_1e-9": int((p_diff > 1e-9).sum()) if same_rows else None,
        **perf,
    }
    (ML / "cpp_inference.json").write_text(json.dumps(out, indent=1))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", metavar="EVENTS_CSV", help="run the C++ replay and compare with Python")
    args = ap.parse_args()
    with open(ML / "models.pkl", "rb") as fh:
        saved = pickle.load(fh)
    if not args.check:
        export(saved["next_move"], OUT, len(saved["features"]))
        print(f"wrote {OUT} ({OUT.stat().st_size / 1e6:.1f} MB, {len(saved['next_move']._predictors)} trees)")
        return
    out = check(args.check)
    print(json.dumps({k: v for k, v in out.items() if k != "max_abs_feature_diff"}, indent=1))
    print("worst feature diffs:", sorted(out["max_abs_feature_diff"].items(), key=lambda kv: -kv[1])[:4])


if __name__ == "__main__":
    main()
