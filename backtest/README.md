# Market-making backtest on the C++ order book

A market-making strategy, written in Python, trading in a simulated market that runs on the C++ order book through `obfast`, a CPython extension module.

## Run it
```bash
make -C cpp python                         # build obfast (needs: sudo apt install python3-dev)
python backtest/run_backtest.py            # 24-setting sweep, ~20 s; writes backtest/results/
python backtest/bench_bindings.py          # pure-Python book vs C++ book, same orders
python -m unittest discover -s backtest -v # binding + backtest tests
```
Needs `numpy`, `pandas` and `matplotlib`.

## The bindings
`cpp/bindings/obfast.cpp` exposes the C++ book to Python through the CPython C API directly, with no pybind11 or other dependency. The order methods use `METH_FASTCALL`, so a call costs little more than the C++ work, and C++ exceptions become Python exceptions (`obfast.OrderBookError` is a `ValueError`).

```python
import obfast
book = obfast.Book()
book.limit(1, obfast.SELL, 100, 10_050)   # -> []
book.limit(2, obfast.BUY, 40, 10_100)     # -> [(2, 1, 10050, 40, 0)]  (buy_id, sell_id, price, qty, aggressor)
book.mid(), book.depth(obfast.SELL), book.remaining(1)
```

Same 300k random orders, driven from a Python loop:

| Book | Orders/s |
|---|---|
| Pure Python (`orderbook.py`) | 0.37 M |
| C++ via `obfast` | **4.77 M (12.9×)** |

Both books produce identical trades. A randomized test checks this on 20k mixed adds, cancels and modifies, and memory stays flat over millions of calls, so no reference counts leak.

## The market (`sim.py`)
- A hidden **fundamental** price follows a random walk with occasional **news jumps**.
- **Liquidity traders** place limit orders a few ticks from the fundamental. Each one rests for a random lifetime, then cancels.
- **Informed traders** send market orders that lean toward the fundamental: when the book's mid is below it, buys are more likely. They're the reason market making is hard, because the orders that hit your quotes are disproportionately the ones that know where price is going.
- Every strategy sees the same random draws, so differences between settings come from the strategy, not luck.

## The strategy
Every step the market maker cancels its quotes and re-quotes around the mid of everyone else's orders:

```
bid = mid - half_spread - skew * inventory / 100
ask = mid + half_spread - skew * inventory / 100
```

- **Half spread**: wider earns more per trade but trades less.
- **Inventory skew**: when long, shift both quotes down, so selling becomes more likely and buying less. This pulls inventory back toward zero.
- Hard cap of ±2,000 shares.

## Results (50k steps, 24 settings)
![Parameter sweep](results/sweep.png)

![With vs without skew](results/compare.png)

| Setting | PnL | Sharpe* | Max drawdown | Fills |
|---|---|---|---|---|
| Best PnL, no skew (half spread 1) | $6,802 | 6.3 | $591 | 8,831 |
| Best Sharpe (half spread 1.5, skew 1) | $4,330 | **27.8** | **$53** | 6,234 |
| Too tight + too much skew (0.5, skew 1) | −$1,183 | −6.7 | $1,188 | 15,832 |

Over 5 different seeds: no skew made $5,918 ± $1,023 with ~$612 drawdowns. Half spread 1.5 with skew 1 made **$4,278 ± $67** with ~$87 drawdowns.

**What it shows**
- **Inventory risk is the main risk.** Without skew, inventory swings between the ±2,000 limits (middle panel), and PnL rides the price. Skew keeps it near zero. That gives up about 35% of the profit, but drawdowns shrink 7–11×, risk-adjusted return rises about 4×, and results become far more predictable across seeds (±$67 vs ±$1,023).
- **There's a best spread.** Too tight (0.5) and the spread doesn't cover losses to informed traders. Too wide (3–4) and the strategy rarely trades. 1–2 ticks is best here.
- **Skew and spread interact.** With a 0.5-tick spread, a 1-tick skew pushes one quote across the mid, so the strategy keeps paying the spread to dump inventory and loses money.

*Sharpe here is the mean over the standard deviation of PnL across 1,000-step blocks, scaled by √(number of blocks). It's for comparing settings within this simulation, not an annualized number comparable to real strategies.

## Caveats
This is a stylized market: one venue, no fees or rebates, no latency (the strategy re-quotes every step for free), and background traders who don't react to the strategy beyond what the book does. The numbers show how the trade-offs work; they aren't an estimate of real-world profit.

`sweep.csv` also has each setting's average edge at fill (distance from mid when trading) and 100-step markout (where mid went afterwards), for digging into where the PnL comes from.
