"""Tests for the obfast bindings and the backtest.

    make -C cpp python && python -m unittest discover -s backtest -v
"""

from __future__ import annotations

import random
import unittest

from common import BUY, SELL, obfast
from orderbook import Order, OrderBook, OrderBookError, OrderType, Side  # pure-Python book
from sim import MarketParams, MMParams, run


class BindingTests(unittest.TestCase):
    def test_basic_trade_and_book_state(self):
        b = obfast.Book()
        self.assertEqual(b.limit(1, SELL, 100, 10_050), [])
        self.assertEqual(b.limit(2, BUY, 40, 10_100), [(2, 1, 10_050, 40, BUY)])
        self.assertEqual(b.best_ask(), 10_050)
        self.assertIsNone(b.best_bid())
        self.assertIsNone(b.mid())
        self.assertEqual(b.remaining(1), 60)
        self.assertEqual(len(b), 1)
        self.assertEqual(b.depth(SELL), [(10_050, 60)])
        b.limit(3, BUY, 10, 10_000)
        self.assertEqual(b.mid(), 10_025.0)
        self.assertEqual(b.spread(), 50)
        self.assertEqual(b.resting_qty(), 70)
        self.assertIn("ASK", repr(b))

    def test_market_ioc_cancel_modify(self):
        b = obfast.Book()
        b.limit(1, SELL, 5, 100)
        b.limit(2, SELL, 5, 103)
        self.assertEqual(sum(t[3] for t in b.ioc(3, BUY, 10, 101)), 5)
        self.assertIsNone(b.remaining(3))
        self.assertEqual(b.market(4, BUY, 2), [(4, 2, 103, 2, BUY)])
        self.assertTrue(b.cancel(2))
        self.assertFalse(b.cancel(2))
        b.limit(5, BUY, 5, 95)
        b.limit(6, SELL, 5, 100)
        self.assertEqual(b.modify(5, price=100), [(5, 6, 100, 5, BUY)])
        b.limit(7, BUY, 9, 90)
        self.assertEqual(b.modify(7, qty=4), [])
        self.assertEqual(b.remaining(7), 4)

    def test_errors(self):
        b = obfast.Book()
        b.limit(1, BUY, 1, 100)
        with self.assertRaises(obfast.OrderBookError):
            b.limit(1, BUY, 1, 100)  # duplicate resting id
        with self.assertRaises(ValueError):  # OrderBookError is a ValueError
            b.limit(2, BUY, 0, 100)
        with self.assertRaises(ValueError):
            b.limit(2, 7, 1, 100)  # bad side
        with self.assertRaises(TypeError):
            b.limit(2, BUY, 1)  # missing price
        with self.assertRaises(OverflowError):
            b.limit(-1, BUY, 1, 100)
        with self.assertRaises(obfast.OrderBookError):
            b.modify(99, qty=1)
        self.assertEqual(len(b), 1)  # nothing above changed the book

    def test_matches_pure_python_book(self):
        """Random adds/cancels/modifies through both books must give identical results."""
        fast, slow = obfast.Book(), OrderBook()
        rng = random.Random(11)
        ids: list[int] = []
        next_id = 1
        for _ in range(20_000):
            r = rng.random()
            if r < 0.6:
                side_py = Side.BUY if rng.random() < 0.5 else Side.SELL
                side_c = BUY if side_py is Side.BUY else SELL
                qty, price = rng.randint(1, 50), rng.randint(95, 105)
                kind = rng.random()
                if kind < 0.8:
                    a = fast.limit(next_id, side_c, qty, price)
                    b = slow.add(Order(next_id, side_py, OrderType.LIMIT, qty, price))
                elif kind < 0.9:
                    a = fast.market(next_id, side_c, qty)
                    b = slow.add(Order(next_id, side_py, OrderType.MARKET, qty))
                else:
                    a = fast.ioc(next_id, side_c, qty, price)
                    b = slow.add(Order(next_id, side_py, OrderType.IOC, qty, price))
                ids.append(next_id)
                next_id += 1
            elif r < 0.85 and ids:
                oid = rng.choice(ids)
                self.assertEqual(fast.cancel(oid), slow.cancel(oid))
                a = b = []
            elif ids:
                oid = rng.choice(ids)
                kw = {"qty": rng.randint(1, 50)} if rng.random() < 0.5 else {"price": rng.randint(95, 105)}
                try:
                    a = fast.modify(oid, **kw)
                except obfast.OrderBookError:
                    a = "error"
                try:
                    b = slow.modify(oid, **kw)
                except OrderBookError:
                    b = "error"
            else:
                continue
            if b != "error":
                b = [(t.buy_id, t.sell_id, t.price, t.qty, BUY if t.aggressor is Side.BUY else SELL) for t in b]
            self.assertEqual(a, b)
            self.assertEqual((fast.best_bid(), fast.best_ask()), (slow.best_bid(), slow.best_ask()))
        self.assertEqual(fast.depth(BUY, 20), slow.depth(Side.BUY, 20))
        self.assertEqual(fast.depth(SELL, 20), slow.depth(Side.SELL, 20))


class BacktestTests(unittest.TestCase):
    MARKET = MarketParams(steps=3_000)

    def test_deterministic(self):
        a, ta, _ = run(self.MARKET, MMParams(half_spread=1.5, skew=0.5))
        b, tb, _ = run(self.MARKET, MMParams(half_spread=1.5, skew=0.5))
        self.assertEqual(a, b)
        self.assertTrue(ta.equals(tb))

    def test_accounting_reconciles(self):
        """Inventory and cash must equal what the fills add up to."""
        summary, timeline, fills = run(self.MARKET, MMParams(half_spread=1.0, skew=0.25))
        self.assertGreater(len(fills), 0)
        inventory = int((fills["sign"] * fills["qty"]).sum())
        cash = int((-fills["sign"] * fills["price"] * fills["qty"]).sum())
        self.assertEqual(inventory, timeline["inventory"].iloc[-1])
        mid = timeline["mid"].iloc[-1]
        self.assertAlmostEqual((cash + inventory * mid) / 100, summary["pnl"], places=6)
        self.assertEqual(summary["volume"], int(fills["qty"].sum()))

    def test_inventory_limit_respected(self):
        _, timeline, _ = run(self.MARKET, MMParams(half_spread=0.5, skew=0.0, max_inventory=500))
        # One quote can fill completely after the check, so allow one quote of overshoot.
        self.assertLessEqual(timeline["inventory"].abs().max(), 500 + 100)

    def test_skew_reduces_inventory(self):
        no_skew, _, _ = run(self.MARKET, MMParams(half_spread=1.5, skew=0.0))
        skew, _, _ = run(self.MARKET, MMParams(half_spread=1.5, skew=1.0))
        self.assertLess(skew["mean_abs_inventory"], no_skew["mean_abs_inventory"])

if __name__ == "__main__":
    unittest.main()
