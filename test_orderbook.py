import unittest

from orderbook import Order, OrderBook, OrderBookError, OrderType, Side

B, S = Side.BUY, Side.SELL
LIM, MKT, IOC = OrderType.LIMIT, OrderType.MARKET, OrderType.IOC


class OrderBookTests(unittest.TestCase):
    def setUp(self):
        self.book = OrderBook()

    def test_non_crossing_orders_rest(self):
        self.book.add(Order(1, B, LIM, 10, 99))
        self.book.add(Order(2, S, LIM, 10, 101))
        self.assertEqual(self.book.best_bid(), 99)
        self.assertEqual(self.book.best_ask(), 101)
        self.assertEqual(self.book.spread(), 2)
        self.assertEqual(len(self.book), 2)

    def test_limit_cross_trades_at_resting_price(self):
        self.book.add(Order(1, S, LIM, 10, 100))
        trades = self.book.add(Order(2, B, LIM, 10, 105))
        self.assertEqual(len(trades), 1)
        self.assertEqual((trades[0].price, trades[0].qty), (100, 10))
        self.assertEqual(len(self.book), 0)

    def test_time_priority_at_same_price(self):
        self.book.add(Order(1, S, LIM, 5, 100))
        self.book.add(Order(2, S, LIM, 5, 100))
        trades = self.book.add(Order(3, B, LIM, 5, 100))
        self.assertEqual(trades[0].sell_id, 1)
        self.assertIsNotNone(self.book.get(2))

    def test_price_priority_and_walking_the_book(self):
        self.book.add(Order(1, S, LIM, 5, 102))
        self.book.add(Order(2, S, LIM, 5, 101))
        trades = self.book.add(Order(3, B, LIM, 8, 102))
        self.assertEqual([(t.sell_id, t.price, t.qty) for t in trades], [(2, 101, 5), (1, 102, 3)])
        self.assertEqual(self.book.get(1).remaining, 2)

    def test_partial_limit_rests_remainder(self):
        self.book.add(Order(1, S, LIM, 4, 100))
        self.book.add(Order(2, B, LIM, 10, 100))
        self.assertEqual(self.book.get(2).remaining, 6)
        self.assertEqual(self.book.best_bid(), 100)
        self.assertIsNone(self.book.best_ask())

    def test_market_order_drops_unfilled(self):
        self.book.add(Order(1, S, LIM, 3, 100))
        trades = self.book.add(Order(2, B, MKT, 10))
        self.assertEqual(sum(t.qty for t in trades), 3)
        self.assertIsNone(self.book.get(2))
        self.assertEqual(len(self.book), 0)

    def test_ioc_respects_price_and_never_rests(self):
        self.book.add(Order(1, S, LIM, 5, 100))
        self.book.add(Order(2, S, LIM, 5, 103))
        trades = self.book.add(Order(3, B, IOC, 10, 101))
        self.assertEqual(sum(t.qty for t in trades), 5)
        self.assertIsNone(self.book.get(3))
        self.assertEqual(self.book.best_ask(), 103)

    def test_cancel(self):
        self.book.add(Order(1, B, LIM, 5, 100))
        self.assertTrue(self.book.cancel(1))
        self.assertFalse(self.book.cancel(1))
        self.assertIsNone(self.book.best_bid())

    def test_cancel_then_best_price_skips_stale_level(self):
        self.book.add(Order(1, B, LIM, 5, 100))
        self.book.add(Order(2, B, LIM, 5, 99))
        self.book.cancel(1)
        self.assertEqual(self.book.best_bid(), 99)

    def test_modify_shrink_keeps_priority(self):
        self.book.add(Order(1, S, LIM, 10, 100))
        self.book.add(Order(2, S, LIM, 10, 100))
        self.book.modify(1, qty=4)
        trades = self.book.add(Order(3, B, LIM, 4, 100))
        self.assertEqual(trades[0].sell_id, 1)

    def test_modify_grow_loses_priority(self):
        self.book.add(Order(1, S, LIM, 10, 100))
        self.book.add(Order(2, S, LIM, 10, 100))
        self.book.modify(1, qty=20)
        trades = self.book.add(Order(3, B, LIM, 5, 100))
        self.assertEqual(trades[0].sell_id, 2)

    def test_modify_price_can_trade(self):
        self.book.add(Order(1, S, LIM, 5, 100))
        self.book.add(Order(2, B, LIM, 5, 95))
        trades = self.book.modify(2, price=100)
        self.assertEqual((trades[0].buy_id, trades[0].qty), (2, 5))

    def test_depth(self):
        self.book.add(Order(1, B, LIM, 5, 100))
        self.book.add(Order(2, B, LIM, 7, 100))
        self.book.add(Order(3, B, LIM, 1, 98))
        self.assertEqual(self.book.depth(B), [(100, 12), (98, 1)])

    def test_validation(self):
        with self.assertRaises(OrderBookError):
            self.book.add(Order(1, B, LIM, 0, 100))
        with self.assertRaises(OrderBookError):
            self.book.add(Order(2, B, LIM, 5))
        with self.assertRaises(OrderBookError):
            self.book.add(Order(3, B, MKT, 5, 100))
        self.book.add(Order(4, B, LIM, 5, 100))
        with self.assertRaises(OrderBookError):
            self.book.add(Order(4, S, LIM, 5, 110))


if __name__ == "__main__":
    unittest.main()
