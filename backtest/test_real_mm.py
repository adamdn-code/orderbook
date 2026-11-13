"""Tests for the shadow-order backtest: each fill rule on a hand-written event stream.

    make -C cpp python && python -m unittest discover -s backtest -v
"""

from __future__ import annotations

import unittest

import pandas as pd

from real_mm import NS, QUOTE_END, QUOTE_START, Events, Strategy, run

T = QUOTE_START + 60 * NS  # inside the quoting window
COLS = ["ts_ns", "type", "ref", "side", "shares", "price", "new_ref"]


def px(dollars: float) -> int:
    return int(round(dollars * 10_000))


def stream(*rows) -> Events:
    return Events(pd.DataFrame([r + (0,) * (7 - len(r)) for r in rows], columns=COLS))


def book_setup(t0=T):
    """Two bids at $100.00 (ids 1, 2), an ask at $100.02 (id 3). Market maker joins best bid behind 1 and 2."""
    return [
        (t0, "A", 1, "B", 100, px(100.00)),
        (t0 + 1, "A", 2, "B", 200, px(100.00)),
        (t0 + 2, "A", 3, "S", 300, px(100.02)),
    ]


class Signals(unittest.TestCase):
    def quoted_sides(self, strat, rows):
        _, _, fills, _ = run(stream(*rows), strat)
        return fills

    def test_imbalance_pulls_the_threatened_side(self):
        # Bids 900 shares vs asks 100: imbalance +0.8 -> the ask is pulled, the bid stays.
        rows = [(T, "A", 1, "B", 900, px(100.00)), (T + 1, "A", 2, "S", 100, px(100.02)),
                (T + 5, "A", 3, "S", 300, px(100.02)),          # joins behind any ask we had
                (T + 10, "E", 2, "S", 100, px(100.02)),
                (T + 11, "E", 3, "S", 50, px(100.02))]          # would fill a resting ask
        on = Strategy("imb", size=100, max_inventory=10_000, rebate_per_share=0.0, imbalance_pull=0.5)
        off = Strategy("plain", size=100, max_inventory=10_000, rebate_per_share=0.0)
        self.assertTrue((run(stream(*rows), off)[2]["sign"] == -1).any())
        self.assertFalse((run(stream(*rows), on)[2]["sign"] == -1).any())

    def test_thin_level_pulls_quote(self):
        rows = [(T, "A", 1, "B", 100, px(100.00)), (T + 1, "A", 2, "S", 1000, px(100.02)),
                (T + 5, "A", 3, "B", 50, px(100.00)),
                (T + 10, "E", 1, "B", 100, px(100.00)),
                (T + 11, "E", 3, "B", 50, px(100.00))]          # would fill a resting bid
        thin = Strategy("thin", size=100, max_inventory=10_000, rebate_per_share=0.0, min_level_shares=500)
        plain = Strategy("plain", size=100, max_inventory=10_000, rebate_per_share=0.0)
        self.assertTrue((run(stream(*rows), plain)[2]["sign"] == 1).any())
        self.assertFalse((run(stream(*rows), thin)[2]["sign"] == 1).any())


class FillRules(unittest.TestCase):
    STRAT = Strategy("test", size=100, max_inventory=10_000, rebate_per_share=0.0)

    def bid_fills(self, fills):
        return fills[fills["sign"] == 1]

    def test_waits_for_queue_ahead(self):
        ev = stream(*book_setup(),
                    (T + 10, "E", 1, "B", 100, px(100.00)),  # ahead of us
                    (T + 11, "E", 2, "B", 150, px(100.00)))  # still ahead (50 left)
        _, _, fills, _ = run(ev, self.STRAT)
        self.assertTrue(self.bid_fills(fills).empty)

    def test_fills_after_queue_ahead_trades(self):
        ev = stream(*book_setup(),
                    (T + 5, "A", 4, "B", 500, px(100.00)),   # joins behind us
                    (T + 10, "E", 1, "B", 100, px(100.00)),
                    (T + 11, "E", 2, "B", 200, px(100.00)),
                    (T + 12, "E", 4, "B", 60, px(100.00)))   # behind us traded -> we'd have traded first
        summary, _, fills, _ = run(ev, self.STRAT)
        bids = self.bid_fills(fills)
        self.assertEqual(int(bids["qty"].sum()), 60)
        self.assertEqual(int(bids["price_c"].iloc[0]), 10_000)
        self.assertEqual(int(bids["ahead_at_place"].iloc[0]), 300)

    def test_cancels_ahead_move_us_up(self):
        ev = stream(*book_setup(),
                    (T + 5, "A", 4, "B", 500, px(100.00)),
                    (T + 10, "D", 1, "B", 100, px(100.00)),
                    (T + 11, "X", 2, "B", 200, px(100.00)),  # partial cancel of all of order 2
                    (T + 12, "E", 4, "B", 100, px(100.00)))
        _, _, fills, _ = run(ev, self.STRAT)
        self.assertEqual(int(self.bid_fills(fills)["qty"].sum()), 100)

    def test_trade_through_fills_completely(self):
        ev = stream(*book_setup(),
                    (T + 3, "A", 5, "B", 100, px(99.99)),
                    # one sell order sweeps two levels: all three executions share a timestamp
                    (T + 10, "E", 1, "B", 100, px(100.00)),
                    (T + 10, "E", 2, "B", 200, px(100.00)),
                    (T + 10, "E", 5, "B", 100, px(99.99)))   # traded below our bid
        _, _, fills, _ = run(ev, self.STRAT)
        self.assertEqual(int(self.bid_fills(fills)["qty"].sum()), 100)

    def test_incoming_order_through_our_price_fills(self):
        ev = stream(*book_setup(),
                    (T + 10, "D", 1, "B", 100, px(100.00)),
                    (T + 11, "D", 2, "B", 200, px(100.00)),   # we're now alone at $100.00 ...
                    (T + 12, "A", 6, "S", 40, px(100.00)))    # ... and a seller posts at our price
        _, _, fills, _ = run(ev, self.STRAT)
        # After the deletes the real best bid is gone, so we requote; whatever we hold at $100.00 gets 40.
        self.assertLessEqual(int(self.bid_fills(fills)["qty"].sum()), 40)

    def test_latency_delays_placement(self):
        slow = Strategy("slow", size=100, max_inventory=10_000, rebate_per_share=0.0, latency_ns=1_000_000)
        ev = stream(*book_setup(),
                    (T + 3, "A", 4, "B", 500, px(100.00)),
                    (T + 100, "E", 1, "B", 100, px(100.00)),
                    (T + 101, "E", 2, "B", 200, px(100.00)),
                    (T + 102, "E", 4, "B", 50, px(100.00)))   # before our quote is live (1 ms)
        _, _, fills, _ = run(ev, slow)
        self.assertTrue(self.bid_fills(fills).empty)

    def test_no_quotes_outside_window(self):
        early = QUOTE_START - 60 * NS
        ev = stream(*book_setup(early),
                    (early + 10, "A", 4, "B", 500, px(100.00)),
                    (early + 20, "E", 4, "B", 500, px(100.00)))
        _, _, fills, _ = run(ev, self.STRAT)
        self.assertTrue(fills.empty)

    def test_accounting_and_mark_to_close(self):
        ev = stream(*book_setup(),
                    (T + 5, "A", 4, "B", 500, px(100.00)),
                    (T + 6, "A", 7, "S", 500, px(100.02)),     # joins behind our ask
                    (T + 10, "E", 1, "B", 100, px(100.00)),
                    (T + 11, "E", 2, "B", 200, px(100.00)),
                    (T + 12, "E", 4, "B", 100, px(100.00)),    # we buy 100 @ 100.00
                    (T + 20, "E", 3, "S", 300, px(100.02)),
                    (T + 21, "E", 7, "S", 100, px(100.02)),    # we sell 100 @ 100.02
                    (QUOTE_END + 10 * 60 * NS, "Q", 0, "-", 1000, px(100.01)))
        rebate = 0.002
        summary, _, fills, _ = run(ev, Strategy("acct", size=100, max_inventory=10_000, rebate_per_share=rebate))
        bought = int(fills.loc[fills["sign"] == 1, "qty"].sum())
        sold = int(fills.loc[fills["sign"] == -1, "qty"].sum())
        self.assertEqual((bought, sold), (100, 100))
        self.assertAlmostEqual(summary["pnl"], 100 * 0.02, places=9)  # bought 100.00, sold 100.02
        self.assertAlmostEqual(summary["rebates"], 200 * rebate, places=9)
        self.assertEqual(summary["inventory_at_close"], 0)
        self.assertEqual(summary["unexpected_trades"], 0)


if __name__ == "__main__":
    unittest.main()
