import random
import threading
import unittest

from engine import EngineStopped, MatchingEngine
from orderbook import Order, OrderBookError, OrderType, Side

B, S = Side.BUY, Side.SELL
LIM, MKT, IOC = OrderType.LIMIT, OrderType.MARKET, OrderType.IOC


class EngineBasics(unittest.TestCase):
    def test_submit_returns_trades(self):
        with MatchingEngine(["X"]) as eng:
            eng.submit("X", Order(1, S, LIM, 10, 100)).result()
            trades = eng.submit("X", Order(2, B, LIM, 4, 100)).result()
            self.assertEqual([(t.buy_id, t.sell_id, t.qty) for t in trades], [(2, 1, 4)])
            self.assertEqual(eng.best_ask("X"), 100)

    def test_commands_from_one_thread_keep_their_order(self):
        with MatchingEngine(["X"]) as eng:
            futs = [eng.submit("X", Order(i, S, LIM, 1, 100)) for i in range(1, 51)]
            buy = eng.submit("X", Order(999, B, LIM, 50, 100))
            trades = buy.result()
            self.assertEqual([t.sell_id for t in trades], list(range(1, 51)))
            for f in futs:
                self.assertEqual(f.result(), [])

    def test_cancel_and_modify(self):
        with MatchingEngine(["X"]) as eng:
            eng.submit("X", Order(1, B, LIM, 5, 99))
            eng.submit("X", Order(2, B, LIM, 5, 98))
            self.assertTrue(eng.cancel("X", 1).result())
            self.assertFalse(eng.cancel("X", 1).result())
            self.assertEqual(eng.best_bid("X"), 98)
            eng.submit("X", Order(3, S, LIM, 5, 100))
            trades = eng.modify("X", 2, price=100).result()
            self.assertEqual(trades[0].qty, 5)

    def test_errors_come_back_through_the_future(self):
        with MatchingEngine(["X"]) as eng:
            fut = eng.submit("X", Order(1, B, LIM, 0, 100))
            with self.assertRaises(OrderBookError):
                fut.result()
            # the matching thread survived the bad order
            self.assertEqual(eng.submit("X", Order(2, B, LIM, 1, 100)).result(), [])

    def test_unknown_symbol(self):
        with MatchingEngine(["X"]) as eng:
            with self.assertRaises(KeyError):
                eng.submit("Y", Order(1, B, LIM, 1, 100))

    def test_submit_after_stop_is_rejected(self):
        eng = MatchingEngine(["X"]).start()
        eng.stop()
        with self.assertRaises(EngineStopped):
            eng.submit("X", Order(1, B, LIM, 1, 100))

    def test_stop_drains_queue(self):
        eng = MatchingEngine(["X"]).start()
        futs = [eng.submit("X", Order(i, B, LIM, 1, 100)) for i in range(1, 1001)]
        eng.stop()
        self.assertTrue(all(f.done() for f in futs))
        self.assertEqual(eng.resting_qty("X"), 1000)

    def test_bad_callback_does_not_kill_matcher(self):
        def boom(symbol, trade):
            raise RuntimeError("callback bug")

        with MatchingEngine(["X"], on_trade=boom) as eng:
            eng.submit("X", Order(1, S, LIM, 1, 100))
            eng.submit("X", Order(2, B, LIM, 1, 100)).result()
            self.assertEqual(eng.submit("X", Order(3, B, LIM, 1, 90)).result(), [])


class EngineStress(unittest.TestCase):
    """Many threads hammer several symbols; check nothing is lost or crossed."""

    SYMBOLS = ["A", "B", "C"]
    THREADS = 8
    PER_THREAD = 2_000

    def test_quantity_is_conserved_and_books_never_cross(self):
        submitted = {s: 0 for s in self.SYMBOLS}
        dropped = {s: 0 for s in self.SYMBOLS}
        lock = threading.Lock()
        callback_trades = {s: 0 for s in self.SYMBOLS}
        crossed = []

        def on_trade(symbol, trade):
            with lock:
                callback_trades[symbol] += 1

        def client(idx, eng):
            rng = random.Random(idx)
            pending = []
            for i in range(self.PER_THREAD):
                oid = idx * 1_000_000 + i
                sym = rng.choice(self.SYMBOLS)
                kind = rng.choices([LIM, MKT, IOC], [6, 2, 2])[0]
                price = None if kind is MKT else rng.randint(95, 105)
                qty = rng.randint(1, 20)
                order = Order(oid, rng.choice([B, S]), kind, qty, price)
                pending.append((sym, kind, qty, eng.submit(sym, order)))
                if i % 200 == 0:
                    bid, ask = eng.top_of_book(sym)
                    if bid is not None and ask is not None and bid >= ask:
                        crossed.append((sym, bid, ask))  # asserts in threads don't fail the test
            local_sub = {s: 0 for s in self.SYMBOLS}
            local_drop = {s: 0 for s in self.SYMBOLS}
            for sym, kind, qty, fut in pending:
                filled = sum(t.qty for t in fut.result())
                local_sub[sym] += qty
                if kind is not LIM:
                    local_drop[sym] += qty - filled
            with lock:
                for s in self.SYMBOLS:
                    submitted[s] += local_sub[s]
                    dropped[s] += local_drop[s]

        with MatchingEngine(self.SYMBOLS, on_trade=on_trade) as eng:
            threads = [threading.Thread(target=client, args=(i, eng)) for i in range(self.THREADS)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()

            self.assertEqual(crossed, [])
            for s in self.SYMBOLS:
                trades = eng.trades(s)
                traded = sum(t.qty for t in trades)
                # every unit submitted is resting, traded (counted once per side), or dropped
                self.assertEqual(submitted[s], eng.resting_qty(s) + 2 * traded + dropped[s], s)
                self.assertEqual(callback_trades[s], len(trades))
                bid, ask = eng.best_bid(s), eng.best_ask(s)
                if bid is not None and ask is not None:
                    self.assertLess(bid, ask)


if __name__ == "__main__":
    unittest.main()
