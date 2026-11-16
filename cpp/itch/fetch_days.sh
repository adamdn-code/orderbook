#!/usr/bin/env bash
# Download every other full-day ITCH 5.0 sample Nasdaq publishes and extract Apple's order events.
# Run from the repo root inside WSL/Linux. Needs ~6 GB free at a time (each .gz is deleted after use).
#
#   bash cpp/itch/fetch_days.sh            # all five days
#   bash cpp/itch/fetch_days.sh 03272019   # just one
#
# Output: ~/itch/days/<YYYY-MM-DD>/AAPL_events.csv (plus book/trades CSVs). If /mnt/c/Users/$WINUSER/itch
# exists (WINUSER defaults to the Windows user name), the event files are copied there too.
set -euo pipefail
if [ $# -gt 0 ]; then DAYS=("$@"); else DAYS=(03272019 07302019 08302019 10302019 01302020); fi
OUT="${OUT:-$HOME/itch/days}"
WIN="/mnt/c/Users/${WINUSER:-$(cmd.exe /c 'echo %USERNAME%' 2>/dev/null | tr -d '\r' || true)}/itch/days"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"

make -C cpp itch >/dev/null
for d in "${DAYS[@]}"; do
  iso="${d:4:4}-${d:0:2}-${d:2:2}"
  dir="$OUT/$iso"
  if [ -s "$dir/AAPL_events.csv" ]; then echo "$iso: already done"; continue; fi
  mkdir -p "$dir"
  gz="$OUT/$d.NASDAQ_ITCH50.gz"
  echo "$iso: downloading (3.5-5.6 GB)"
  curl -fL --retry 5 -C - -o "$gz" "$URL/$d.NASDAQ_ITCH50.gz"
  echo "$iso: replaying Apple's book"
  ./cpp/build/itch_replay "$gz" --symbols AAPL --track AAPL --events --out "$dir" | tee "$dir/run.txt"
  rm -f "$gz"
  if [ -d "$(dirname "$WIN")" ]; then mkdir -p "$WIN/$iso" && cp "$dir"/AAPL_*.csv "$WIN/$iso/"; fi
  echo "$iso: done"
done
