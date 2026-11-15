"""Tests for the ML signal: no look-ahead in features, correct labels, and the quote filter in the backtest.

    make -C cpp python && python -m unittest discover -s backtest -v
"""

from __future__ import annotations

import unittest

import numpy as np
import pandas as pd
from sklearn.metrics import roc_auc_score

from ml_signal import FEATURES, auc, event_signal, features_and_labels, snapshots
from real_mm import NS, QUOTE_START, Events, Strategy, run
from synth_events import generate
from test_real_mm import px, stream

T = QUOTE_START + 60 * NS


def feats(df: pd.DataFrame) -> pd.DataFrame:
    snap, _ = snapshots(Events(df))
    return features_and_labels(snap)


class NoLookAhead(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.df = generate(minutes=6, seed=3, start_ns=QUOTE_START - 60 * NS)

    def test_features_ignore_the_future(self):
        """Features at time t must be identical whether or not anything after t ever happened."""
        cut = QUOTE_START + 3 * 60 * NS
        full = feats(self.df).set_index("ts_ns")
        short = feats(self.df[self.df["ts_ns"] <= cut]).set_index("ts_ns")
        common = short.index[short.index <= cut]
        self.assertGreater(len(common), 1000)
        pd.testing.assert_frame_equal(full.loc[common, FEATURES], short.loc[common, FEATURES])

    def test_future_rewrite_changes_labels_not_features(self):
        cut = QUOTE_START + 3 * 60 * NS
        alt = pd.concat([self.df[self.df["ts_ns"] <= cut],
                         generate(minutes=2, seed=99, start_ns=cut + 1).assign(ref=lambda d: d["ref"] + 10**9)])
        a = feats(self.df).set_index("ts_ns")
        b = feats(alt).set_index("ts_ns")
        common = a.index[a.index <= cut]
        pd.testing.assert_frame_equal(a.loc[common, FEATURES], b.loc[common, FEATURES])


class Labels(unittest.TestCase):
    def test_next_move_and_horizons(self):
        rows = [(T, "A", 1, "B", 100, px(100.00)), (T, "A", 2, "S", 100, px(100.02)),
                (T + NS // 2, "A", 3, "B", 100, px(100.01)),          # bid improves: mid up at +0.5 s
                (T + 2 * NS, "E", 3, "B", 100, px(100.01))]           # bid gone: mid back down at +2 s
        f = features_and_labels(snapshots(stream(*rows))[0])
        f = f.set_index("ts_ns")
        self.assertEqual(f.loc[T, "next_move"], 1.0)
        self.assertEqual(f.loc[T + NS // 2, "next_move"], 0.0)
        self.assertTrue(np.isnan(f.loc[T, "up_100ms"]))              # flat after 100 ms -> dropped
        self.assertEqual(f.loc[T, "up_1s"], 1.0)
        self.assertTrue(np.isnan(f.loc[T + 2 * NS, "next_move"]))    # no later move

    def test_sweep_is_one_sample(self):
        rows = [(T, "A", 1, "B", 100, px(100.00)), (T, "A", 2, "S", 100, px(100.02)),
                (T, "A", 3, "S", 100, px(100.03)),
                (T + NS, "E", 2, "S", 100, px(100.02)), (T + NS, "E", 3, "S", 100, px(100.03)),
                (T + NS, "A", 4, "S", 100, px(100.05))]
        snap, idx = snapshots(stream(*rows))
        self.assertEqual(list(snap["ts"]), [T, T + NS])               # nothing from the middle of the sweep
        self.assertEqual(int(snap["ba"].iloc[-1]), 10005)
        self.assertEqual(list(idx), [2, 5])


class SignalAlignment(unittest.TestCase):
    def test_mid_timestamp_events_see_previous_snapshot(self):
        # events 0..5, snapshots taken after events 1 and 4
        sig = event_signal(6, np.array([1, 4]), np.array([0.2, 0.9], dtype=np.float32))
        self.assertTrue(np.isnan(sig[0]))
        np.testing.assert_allclose(sig[1:], [0.2, 0.2, 0.2, 0.9, 0.9])


class ModelFilter(unittest.TestCase):
    ROWS = [(T, "A", 1, "B", 900, px(100.00)), (T + 1, "A", 2, "S", 100, px(100.02)),
            (T + 5, "A", 3, "S", 300, px(100.02)),
            (T + 10, "E", 2, "S", 100, px(100.02)),
            (T + 11, "E", 3, "S", 50, px(100.02))]          # would fill a resting ask

    def run_with(self, p: float, pull: float):
        ev = stream(*self.ROWS)
        strat = Strategy("m", size=100, max_inventory=10_000, rebate_per_share=0.0, model_pull=pull)
        return run(ev, strat, signal=np.full(ev.n, p, dtype=np.float32))[2]

    def test_confident_up_pulls_ask(self):
        self.assertTrue((self.run_with(0.5, 0.1)["sign"] == -1).any())
        self.assertFalse((self.run_with(0.9, 0.1)["sign"] == -1).any())

    def test_nan_means_no_opinion(self):
        self.assertTrue((self.run_with(float("nan"), 0.1)["sign"] == -1).any())

    def test_needs_signal(self):
        with self.assertRaises(ValueError):
            run(stream(*self.ROWS), Strategy("m", model_pull=0.1))


class LatencyLabels(unittest.TestCase):
    def test_only_moves_after_the_latency_count(self):
        rows = [(T, "A", 1, "B", 100, px(100.00)), (T, "A", 2, "S", 100, px(100.02)),
                (T + 50_000, "A", 3, "B", 100, px(100.01)),       # up 50 us later
                (T + 3_000_000, "E", 3, "B", 100, px(100.01))]    # down 3 ms later
        f = features_and_labels(snapshots(stream(*rows))[0]).set_index("ts_ns")
        self.assertEqual(f.loc[T, "next_move"], 1.0)         # can't dodge a move 50 us away...
        self.assertEqual(f.loc[T, "next_move_100us"], 0.0)   # ...so the 100 us label is the move after it
        self.assertEqual(f.loc[T, "next_move_1ms"], 0.0)
        self.assertTrue(np.isnan(f.loc[T, "next_move_10ms"]))


class CppParity(unittest.TestCase):
    """The C++ features + tree evaluation (cpp/ml) must match the Python pipeline."""

    def test_cpp_matches_python(self):
        import subprocess
        import tempfile
        from pathlib import Path

        from sklearn.ensemble import HistGradientBoostingClassifier

        from export_model import export
        from ml_signal import build

        exe = Path(__file__).resolve().parents[1] / "cpp" / "build" / "ml_replay"
        if not exe.exists():
            self.skipTest("cpp/build/ml_replay not built (make -C cpp ml)")
        with tempfile.TemporaryDirectory() as d:
            csv, model_path, dump = Path(d) / "ev.csv", Path(d) / "m.gbdt", Path(d) / "dump.bin"
            generate(minutes=8, seed=5, start_ns=QUOTE_START - 60 * NS).to_csv(csv, index=False)
            _, f, _ = build(str(csv))
            f = f.dropna(subset=["next_move"])
            X = f[FEATURES].to_numpy(float)
            m = HistGradientBoostingClassifier(max_iter=40, max_leaf_nodes=15, random_state=0).fit(X, f["next_move"].astype(int))
            export(m, model_path, len(FEATURES))
            subprocess.run([str(exe), str(csv), str(model_path), "--dump", str(dump), "--repeat", "1"],
                           check=True, capture_output=True)
            got = np.fromfile(dump, dtype=np.float64).reshape(-1, len(FEATURES) + 2)
            _, full, _ = build(str(csv))
            self.assertEqual(len(got), len(full))
            np.testing.assert_array_equal(got[:, 0].astype(np.int64), full["ts_ns"].to_numpy())
            np.testing.assert_array_equal(got[:, 1:-1], full[FEATURES].to_numpy(float))  # bit-identical
            np.testing.assert_allclose(got[:, -1], m.predict_proba(full[FEATURES].to_numpy(float))[:, 1], rtol=0, atol=1e-12)


class Auc(unittest.TestCase):
    def test_matches_sklearn_with_ties(self):
        rng = np.random.default_rng(0)
        y = rng.integers(0, 2, 5000)
        s = np.round(rng.normal(y * 0.3, 1), 1)
        self.assertAlmostEqual(auc(y, s), roc_auc_score(y, s), places=10)


if __name__ == "__main__":
    unittest.main()
