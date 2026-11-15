# Market-making backtest on the C++ order book

A market-making strategy, written in Python, trading in a simulated market and then on a full day of real Nasdaq order flow for Apple. Both run on the C++ order book through `obfast`, a CPython extension module.

## Run it
```bash
make -C cpp python                         # build obfast (needs: sudo apt install python3-dev)
python backtest/run_backtest.py            # 24-setting sweep, ~20 s; writes backtest/results/
python backtest/bench_bindings.py          # pure-Python book vs C++ book, same orders
python backtest/real_mm.py ~/itch/AAPL_events.csv   # real-data study (see below for getting the file)
python backtest/ml_signal.py ~/itch/AAPL_events.csv     # ML signal (see below), then ml_backtest.py
python -m unittest discover -s backtest -v # binding + backtest + ML tests
```
Needs `numpy`, `pandas` and `matplotlib` (plus `scikit-learn` for the ML signal).

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

## On real data: market making Apple on 2019-01-30 (`real_mm.py`)
The simulation above is stylized. This part runs the strategy against the real thing: every Apple order event Nasdaq reported that day (1.67M events, exported with `itch_replay --track AAPL --events`). The book is rebuilt with the C++ book through `obfast`.

**How fills work.** The market maker's quotes are *shadow orders*: they sit in a virtual queue next to the real orders but don't change what anyone else did. Because ITCH names every order, the backtest knows exactly which real orders are ahead of each quote when it's placed. A quote fills when:
1. the orders ahead of it trade or cancel, and an order behind it then trades at its price (the incoming order would have reached the quote first), or
2. the market trades through its price, or
3. a new order on the other side is priced through it.

Placing and cancelling take effect after a latency. Even at zero latency nothing happens mid-sweep, since one incoming order that takes several levels shows up as several executions with the same timestamp. `test_real_mm.py` tests each rule on hand-written event streams.

| Strategy (100 µs latency, 100-share quotes, 9:35–15:55) | PnL | With rebate* | Shares | Edge at fill | Markout 1 s | Markout 60 s |
|---|---|---|---|---|---|---|
| Join the best bid and ask | −$6,879 | −$4,742 | 1,068,666 | +0.60¢ | −0.43¢ | −0.87¢ |
| Join best + inventory skew | −$6,405 | −$4,597 | 903,776 | +0.60¢ | −0.43¢ | −0.79¢ |
| One cent behind best + skew | −$299 | −$234 | 32,799 | +1.81¢ | +0.21¢ | −0.35¢ |

*Assumes a $0.0020/share rebate for adding liquidity; Nasdaq's actual rate depends on the firm's volume tier.

![PnL and inventory through the day](results/real/day.png)

**What it shows**
- **Adverse selection beats the spread.** Each fill is on average 0.6¢ better than the mid at that moment. Within a tenth of a second the mid has moved about 1¢ against it (markout −0.38¢), and it keeps drifting the wrong way over the next minute. The orders that trade with a resting quote are disproportionately the ones that know where the price is going, so quoting at the best price all day loses money steadily even before inventory risk.
- **Inventory skew halves inventory swings but doesn't fix the problem.** Max inventory falls from ±1,097 to ±507 shares and drawdown shrinks, but the loss per share is about the same, because the losses come from adverse selection, not from holding inventory.
- **Standing one cent back is safer but barely trades** (420 fills), and with so few fills its markouts are noisy.

![Markouts](results/real/markouts.png)

**Latency is money.** The same skewed strategy, varying only how long a place or cancel takes:

| Latency | 0 | 100 µs | 1 ms | 10 ms | 100 ms |
|---|---|---|---|---|---|
| PnL with rebate | −$2,745 | −$4,597 | −$7,059 | −$8,711 | −$10,892 |
| Edge at fill | +0.87¢ | +0.60¢ | +0.34¢ | +0.22¢ | +0.10¢ |
| Markout 1 s | −0.29¢ | −0.43¢ | −0.55¢ | −0.68¢ | −0.82¢ |

Every step slower makes things worse, because a slow cancel leaves a stale quote out for faster traders to hit. That's why market makers spend so much on speed.

![Latency](results/real/latency.png)

**Trying signals: when *not* to quote.** Two signals were added to the skewed strategy, both re-checked on every book event:
- *Imbalance:* with `imb = (bid size − ask size) / (bid size + ask size)` at the top of the book, pull the bid when `imb < −threshold` and the ask when `imb > threshold`. A thin side tends to be the next one to get eaten.
- *Thin level:* pull a quote when its price level has fewer than N real shares on it.

| Variant (100 µs, with rebate) | PnL | Shares | PnL per 100 shares | Markout 1 s |
|---|---|---|---|---|
| Skew, no signal | −$4,597 | 903,776 | −$0.51 | −0.43¢ |
| + imbalance, threshold 0.2 | −$1,470 | 249,562 | −$0.59 | −0.41¢ |
| + thin level, < 200 shares | −$2,059 | 278,040 | −$0.74 | −0.50¢ |

| Imbalance threshold | 0 (off) | 0.2 | 0.4 | 0.6 | 0.8 |
|---|---|---|---|---|---|
| PnL with rebate | −$4,597 | −$1,470 | −$1,739 | −$2,546 | −$3,083 |
| Shares | 904k | 250k | 342k | 466k | 630k |

Both signals cut the day's loss by 55–70%, but **neither made it profitable, and neither improved the fills**: the loss per share and the markouts stay the same or get worse. The signals help by trading less, not by picking better trades. Pulling harder (a thin-level cutoff of 800 shares) gets the loss to about −$170, but only by barely trading (28k shares).

**Where the losses come from.** Splitting the skewed strategy's fills by how many real shares were ahead of the quote when it was placed (`results/real/queue.csv`):

| Shares ahead at placement | 0 (alone) | 1–100 | 101–300 | 301–600 | 601–1500 | >1500 |
|---|---|---|---|---|---|---|
| Shares filled | 155k | 225k | 228k | 109k | 175k | 13k |
| Markout 0.1 s | −0.73¢ | −0.33¢ | −0.34¢ | −0.35¢ | −0.25¢ | −0.57¢ |
| Median wait to fill | 1 ms | 0.37 s | 0.57 s | 0.72 s | 0.80 s | 0.85 s |

The worst fills, at twice the loss of the others, are quotes that landed *alone*: they arrived after their level had already emptied and were filled within a millisecond by the same move that cleared it. That's latency again. The quote is stale on arrival. Fixing it needs either a faster reaction (see the latency table) or a short-horizon price prediction good enough to stay out of the way, which top-of-book signals alone don't give.

**Limitations**
- Shadow orders have no market impact. In reality the market maker's 100 shares would have taken some fills from other orders and could have changed what others did, so fill counts are on the high side.
- One stock, one day. The rebate is an assumption. Inventory left at 15:55 is marked at the closing auction price.
- Fills at the open and close auctions and hidden-order executions aren't modeled.

```bash
./cpp/build/itch_replay ~/itch/01302019.NASDAQ_ITCH50.gz --symbols AAPL --track AAPL --events --out ~/itch
python backtest/real_mm.py ~/itch/AAPL_events.csv     # ~1 min; writes backtest/results/real/
```

## An ML signal: predicting the next move from order flow (`ml_signal.py`, `ml_backtest.py`)

The study above ends on an open problem: quotes get picked off because they're stale, and top-of-book imbalance alone doesn't predict the move well enough to avoid it. So the next step is a model that does, graded two ways: AUC against the imbalance baseline, and then dollars in the backtest.

```bash
python backtest/ml_signal.py   ~/itch/AAPL_events.csv   # features, labels, training, AUC (~6 min); writes results/ml/
python backtest/ml_backtest.py ~/itch/AAPL_events.csv   # 104 backtests with the model as a quote filter (~3 min)
```
Also needs `scikit-learn`. The second script writes `docs/playground_data.json` for the **[interactive playground](https://adamdn-code.github.io/orderbook/playground.html)**.

**Setup**
- *Samples:* one per timestamp after all its events are applied (1.44M), so features never see half of a sweep.
- *Label:* is the next mid-price change up or down? Also checked: up or down 100 ms and 1 s later.
- *Features (19, all causal):* spread; depth imbalance at 1, 2 and 5 levels; log sizes at the touch; microprice offset; order-flow imbalance (Cont, Kukanov & Stoikov) over 100 ms, 1 s and 10 s; signed trade volume and trade counts over 1 s and 10 s; mid change over 1 s and 10 s; time since the last move and its direction. No time-of-day feature, which would just memorise the morning.
- *Split:* train 9:35–12:30, tune 12:30–13:30, test 13:30–15:55, 60 s embargo at each boundary, never shuffled. Settings for the gradient-boosted trees (scikit-learn `HistGradientBoostingClassifier`) are picked on the tuning hour, then the model is refit on train + tune and scored on test once.
- *Intervals:* block bootstrap over 1-minute blocks. Consecutive samples share a label, so a row-level bootstrap would be far too narrow.
- *Leakage tests* (`test_ml.py`): features at time *t* are identical whether the events after *t* are kept, dropped or replaced.

**Test-period AUC** (13:30–15:55, includes the 2 pm Fed announcement)

| Label | Imbalance (baseline) | Logistic, all features | Gradient boosting | Gain over baseline, 95% CI |
|---|---|---|---|---|
| Next mid move (528,778 samples) | 0.603 | 0.609 | **0.652** | +0.048 (+0.041 to +0.054) |
| Up/down in 100 ms | 0.638 | 0.642 | **0.683** | +0.045 (+0.034 to +0.052) |
| Up/down in 1 s | 0.582 | 0.592 | **0.605** | +0.023 (+0.009 to +0.035) |

The model beats the baseline in every half hour of the test period (0.630–0.680 vs 0.590–0.629), and it is calibrated: the top decile predicts 72% up and 73% were. The logistic regression barely beats imbalance, so the gain comes from interactions the trees find, not from just adding features. The most important features are signed trade flow over the last second, the size at the best ask and bid, time since the last move and 100 ms order-flow imbalance.

![ROC and feature importance](results/ml/roc_importance.png)

**Does it make money?** The model becomes a quote filter: pull the ask while P(up) > 0.5 + *t*, and the bid while P(up) < 0.5 − *t*. Pulling quotes always means trading less, which earlier looked like an improvement and wasn't. So each signal's thresholds are set so it pulls a quote the same share of the time (10–60%) on the tuning hour, and the comparison is **loss per share** at equal pull rates. Join best + inventory skew, test period only:

| Latency | No filter | Imbalance, pull 50% | Model, pull 50% |
|---|---|---|---|
| 0 | −$1,147 · −$0.31/100 sh · 1 s markout −0.28¢ | −$841 · −$0.54/100 sh · −0.28¢ | **+$160 · +$0.10/100 sh · −0.14¢** |
| 100 µs | −$2,086 · −$0.51 · −0.42¢ | −$871 · −$0.48 · −0.39¢ | **−$145 · −$0.08 · −0.33¢** |
| 1 ms | −$2,792 · −$0.56 · −0.56¢ | −$1,578 · −$0.53 · −0.49¢ | **−$1,154 · −$0.36 · −0.48¢** |
| 10 ms | −$3,325 · −$0.68 · −0.68¢ | −$2,266 · −$0.71 · −0.65¢ | **−$1,689 · −$0.50 · −0.64¢** |

- The model filter loses less per share than imbalance at **all 24** matched settings (6 pull rates × 4 latencies). At 0 latency it turns the afternoon positive at 50–60% pull, and halves the 1 s adverse selection.
- Imbalance at the same pull rates barely changes loss per share (and sometimes makes it worse). It mostly just trades less, which matches the finding above.
- **The edge decays with latency:** +$0.43 per 100 shares over imbalance at 0, +$0.28 at 100 µs, +$0.16 at 1 ms, +$0.14 at 10 ms. The prediction is about the next few milliseconds, so it's only worth what you can act on in time.
- Quoting 1¢ behind trades only 2k–100k shares in the test period, too few for the differences to mean much. The playground shows those runs, but the conclusions above use the join-best runs.

![Loss per share at matched pull rates](results/ml/filters.png)

**Limitations, on top of the backtest's:** one stock, one test afternoon. The intervals cover noise within that afternoon, not other days, so the next check is a second day held out entirely. The filter's thresholds come from the tuning hour, but the choice of a 50% pull rate for the table is made looking at test results; the playground shows every pull rate so nothing is hidden.

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
