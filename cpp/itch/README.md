# Replaying a real day of Nasdaq trading

`itch_replay` reads Nasdaq's raw TotalView-ITCH 5.0 feed, every order added, executed, cancelled or replaced on Nasdaq for a full day, and rebuilds every stock's order book with this repo's C++ book. It then checks the rebuilt books against what Nasdaq reports.

## 1. Get a day of data
Nasdaq publishes free sample days at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/. Each file is one full trading day, 3–10 GB gzipped. List the available days:
```bash
curl -s "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/" | grep -o '[0-9]\{8\}\.NASDAQ_ITCH50\.gz' | sort -u
```
Then download one, for example:
```bash
mkdir -p ~/itch && cd ~/itch
wget "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz"
```
No need to unzip it: the tool reads `.gz` directly.

## 2. Build and run
```bash
make -C cpp itch      # needs zlib: sudo apt install zlib1g-dev
./cpp/build/itch_replay ~/itch/01302019.NASDAQ_ITCH50.gz --track AAPL --out ~/itch
python3 cpp/itch/plot_symbol.py AAPL --dir ~/itch     # -> ~/itch/AAPL_day.png
```
Options: `--symbols AAPL,MSFT` rebuilds only those books (faster), `--track SYM` writes that stock's top of book and trades to CSV, and `--sample-ms N` sets how often the top of book is sampled.

## What it checks
Nasdaq did the matching, so ITCH is a log of its book changing. If the rebuilt book is right:
- every execution, cancel, delete and replace names an order the book holds,
- no new order crosses the book (bid ≥ ask),
- each execution hits the order the book says is **first in line at the best price**. That's price-time priority, the rule the book implements.

The tool reports each of these as a count or percentage. It also reports throughput, executions, volume and VWAP for the most traded stocks.

## How it works
- **Reader** (`itch.hpp`) streams the file through zlib in 4 MB chunks and hands each length-prefixed message to the decoder without copying. Fields are big-endian. Prices are fixed point with 4 decimals.
- **Decoder** handles the message types that change a book: add (`A`/`F`), execute (`E`/`C`), partial cancel (`X`), delete (`D`) and replace (`U`). It also handles hidden-order trades (`P`) and opening/closing crosses (`Q`) for volume. Other types are skipped by length, and any message whose length doesn't match the spec is counted as malformed.
- **Books.** Stocks at $1 and up use the flat-array book in whole cents (Reg NMS doesn't allow sub-penny quotes there). Stocks that start under $1 use the map-based book in 1/100-cent ticks. Any order a book can't hold, such as a sub-penny price in a cents book or one far outside the flat array's range, goes to a side table so its later executions and cancels still apply, and is counted.
- **Partial cancels and partial executions keep an order's place in line. Replaces send it to the back,** matching Nasdaq's rules.

## Tests
`tests/itch_tests.cpp` writes synthetic ITCH files byte for byte to the spec and checks:
- every message type,
- price-time priority,
- the queue-position rules,
- sub-dollar stocks,
- side-table orders,
- unknown order references,
- gzip vs plain files,
- truncated and malformed input,
- symbol filtering and the CSV output.

A randomized test generates 60k messages from an independently tracked "true" book. It requires the replay to match that book's best bid and ask at every checkpoint, with zero inconsistencies and 100% of executions at the front of the queue.

## Speed
On a synthetic 40M-message file across 500 stocks (2-core cloud VM), reading and decoding alone runs at 10.5M messages/s. The full replay runs at 2.0–2.5M messages/s, with 2.7M orders resting at once. At that size most book lookups miss the CPU cache, which is where the time goes. A real day is roughly 300–400M messages, so expect a few minutes.
