"""A single-threaded limit order book with price-time priority.

Prices are integers in ticks (e.g. cents) so comparisons are exact.

Order types:
  LIMIT  - trade at this price or better; any leftover rests in the book.
  MARKET - trade immediately at the best prices available; leftover is dropped.
  IOC    - immediate-or-cancel: like LIMIT, but leftover is dropped, never rests.

Matching rule (price-time priority): the best price trades first; among
orders at the same price, the one that arrived first trades first. Trades
execute at the resting order's price.
"""

from __future__ import annotations

import heapq
import itertools
from collections import deque
from dataclasses import dataclass, field
from enum import Enum


class Side(Enum):
    BUY = "buy"
    SELL = "sell"

    @property
    def opposite(self) -> "Side":
        return Side.SELL if self is Side.BUY else Side.BUY


class OrderType(Enum):
    LIMIT = "limit"
    MARKET = "market"
    IOC = "ioc"


@dataclass
class Order:
    id: int
    side: Side
    type: OrderType
    qty: int
    price: int | None = None
    remaining: int = field(init=False)
    seq: int = field(init=False, default=-1)  # arrival order, set by the book

    def __post_init__(self) -> None:
        self.remaining = self.qty


@dataclass(frozen=True)
class Trade:
    buy_id: int
    sell_id: int
    price: int
    qty: int
    aggressor: Side  # side of the incoming order that caused the trade


class OrderBookError(ValueError):
    pass


class OrderBook:
    def __init__(self) -> None:
        # price -> FIFO queue of resting orders at that price
        self._levels: dict[Side, dict[int, deque[Order]]] = {Side.BUY: {}, Side.SELL: {}}
        # heaps of prices for fast best-price lookup (bids stored negated).
        # Empty levels are removed lazily when they reach the top.
        self._heaps: dict[Side, list[int]] = {Side.BUY: [], Side.SELL: []}
        self._resting: dict[int, Order] = {}
        self._seen_ids: set[int] = set()
        self._seq = itertools.count()
        self.trades: list[Trade] = []

    # ---------- public API ----------

    def add(self, order: Order) -> list[Trade]:
        """Submit an order. Returns the trades it caused."""
        self._validate(order)
        self._seen_ids.add(order.id)
        order.seq = next(self._seq)
        trades = self._match(order)
        if order.type is OrderType.LIMIT and order.remaining > 0:
            self._rest(order)
        return trades

    def cancel(self, order_id: int) -> bool:
        """Remove a resting order. Returns False if it isn't resting."""
        order = self._resting.pop(order_id, None)
        if order is None:
            return False
        level = self._levels[order.side][order.price]
        level.remove(order)
        if not level:
            del self._levels[order.side][order.price]
        return True

    def modify(self, order_id: int, *, qty: int | None = None, price: int | None = None) -> list[Trade]:
        """Change a resting order's quantity and/or price.

        Shrinking the quantity keeps the order's place in line. Changing the
        price or increasing the quantity sends it to the back (it's treated as
        a new order and may trade immediately).
        """
        order = self._resting.get(order_id)
        if order is None:
            raise OrderBookError(f"order {order_id} is not resting")
        new_qty = order.remaining if qty is None else qty
        new_price = order.price if price is None else price
        if new_qty <= 0:
            raise OrderBookError("quantity must be positive; use cancel() to remove")

        if new_price == order.price and new_qty <= order.remaining:
            order.remaining = new_qty
            return []

        self.cancel(order_id)
        replacement = Order(order_id, order.side, OrderType.LIMIT, new_qty, new_price)
        replacement.seq = next(self._seq)
        trades = self._match(replacement)
        if replacement.remaining > 0:
            self._rest(replacement)
        return trades

    def best_bid(self) -> int | None:
        return self._best(Side.BUY)

    def best_ask(self) -> int | None:
        return self._best(Side.SELL)

    def spread(self) -> int | None:
        bid, ask = self.best_bid(), self.best_ask()
        return None if bid is None or ask is None else ask - bid

    def depth(self, side: Side, levels: int = 5) -> list[tuple[int, int]]:
        """Top price levels on one side as (price, total quantity)."""
        prices = sorted(self._levels[side], reverse=side is Side.BUY)[:levels]
        return [(p, sum(o.remaining for o in self._levels[side][p])) for p in prices]

    def get(self, order_id: int) -> Order | None:
        return self._resting.get(order_id)

    def __len__(self) -> int:
        return len(self._resting)

    def __str__(self) -> str:
        asks = list(reversed(self.depth(Side.SELL)))
        bids = self.depth(Side.BUY)
        lines = ["      price    qty"]
        lines += [f"ASK {p:>8} {q:>6}" for p, q in asks]
        lines.append("    " + "-" * 15)
        lines += [f"BID {p:>8} {q:>6}" for p, q in bids]
        return "\n".join(lines)

    # ---------- internals ----------

    def _validate(self, order: Order) -> None:
        if order.id in self._seen_ids:
            raise OrderBookError(f"duplicate order id {order.id}")
        if order.qty <= 0:
            raise OrderBookError("quantity must be positive")
        if order.type is OrderType.MARKET:
            if order.price is not None:
                raise OrderBookError("market orders don't take a price")
        elif order.price is None or order.price <= 0:
            raise OrderBookError(f"{order.type.value} orders need a positive price")

    def _crosses(self, order: Order, best_opposite: int) -> bool:
        if order.type is OrderType.MARKET:
            return True
        if order.side is Side.BUY:
            return best_opposite <= order.price
        return best_opposite >= order.price

    def _match(self, order: Order) -> list[Trade]:
        trades: list[Trade] = []
        opp = order.side.opposite
        while order.remaining > 0:
            best = self._best(opp)
            if best is None or not self._crosses(order, best):
                break
            level = self._levels[opp][best]
            resting = level[0]
            qty = min(order.remaining, resting.remaining)
            order.remaining -= qty
            resting.remaining -= qty
            buy, sell = (order, resting) if order.side is Side.BUY else (resting, order)
            trades.append(Trade(buy.id, sell.id, best, qty, order.side))
            if resting.remaining == 0:
                level.popleft()
                del self._resting[resting.id]
                if not level:
                    del self._levels[opp][best]
        self.trades.extend(trades)
        return trades

    def _rest(self, order: Order) -> None:
        levels = self._levels[order.side]
        if order.price not in levels:
            levels[order.price] = deque()
            key = -order.price if order.side is Side.BUY else order.price
            heapq.heappush(self._heaps[order.side], key)
        levels[order.price].append(order)
        self._resting[order.id] = order

    def _best(self, side: Side) -> int | None:
        heap, levels = self._heaps[side], self._levels[side]
        while heap:
            price = -heap[0] if side is Side.BUY else heap[0]
            if price in levels:
                return price
            heapq.heappop(heap)  # stale: level was emptied
        return None


if __name__ == "__main__":
    book = OrderBook()
    book.add(Order(1, Side.SELL, OrderType.LIMIT, 100, 10_100))
    book.add(Order(2, Side.SELL, OrderType.LIMIT, 50, 10_050))
    book.add(Order(3, Side.BUY, OrderType.LIMIT, 80, 9_950))
    book.add(Order(4, Side.BUY, OrderType.LIMIT, 40, 10_000))
    print("Starting book (prices in cents):")
    print(book)

    print("\nBuy 120 at market:")
    for t in book.add(Order(5, Side.BUY, OrderType.MARKET, 120)):
        print(f"  {t.qty} @ {t.price}  (buyer {t.buy_id}, seller {t.sell_id})")
    print(book)
    print(f"\nBest bid {book.best_bid()}, best ask {book.best_ask()}, spread {book.spread()}")
