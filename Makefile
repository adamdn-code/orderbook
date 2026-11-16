# One command to see the whole project work, from a fresh clone, in about a minute:
#
#   pip install -r requirements.txt     # numpy, pandas, scikit-learn, matplotlib
#   make demo                           # needs g++, zlib (sudo apt install zlib1g-dev) and python3-dev
#
# 1. builds the C++ book, engine, ITCH replay and Python module, and runs the C++ tests
# 2. writes a synthetic 2M-message, 500-stock ITCH feed and replays it on 1 and 2 threads (same counts;
#    price-time priority is checked against an independent book in cpp/tests/itch_tests.cpp)
# 3. checks the WebAssembly build against the native one, if clang, wasm-ld and node are installed
# 4. runs the ML pipeline end to end on a synthetic day (backtest/demo.py)
#
# The real study needs Nasdaq's 5 GB sample file; see cpp/itch/README.md and backtest/README.md.

PYTHON ?= python3

demo:
	@echo "== 1. build and test the C++ =="
	$(MAKE) -C cpp test python itch build/itch_synth PYTHON=$(PYTHON)
	@echo
	@echo "== 2. replay a synthetic multi-stock ITCH feed =="
	./cpp/build/itch_synth cpp/build/demo.itch 2000000 500
	./cpp/build/itch_replay cpp/build/demo.itch --threads 1 | grep -E "messages/s|naming an order|would have crossed"
	./cpp/build/itch_replay cpp/build/demo.itch --threads 2 | grep -E "messages/s|naming an order|would have crossed"
	@echo
	@echo "== 3. the order book in WebAssembly =="
	@if command -v node >/dev/null && command -v clang++ >/dev/null && (command -v wasm-ld >/dev/null || ls /usr/lib/llvm-*/bin/wasm-ld >/dev/null 2>&1); \
	  then WASM_OUT=$(CURDIR)/cpp/build/orderbook.wasm $(MAKE) -s -C cpp wasm-test; else echo "skipped: needs clang, lld (wasm-ld) and node"; fi
	@echo
	@echo "== 4. features, model and market maker on a synthetic day =="
	$(PYTHON) backtest/demo.py

test:
	$(MAKE) -C cpp test
	$(PYTHON) -m unittest test_orderbook test_engine
	$(PYTHON) -m unittest discover -s backtest

.PHONY: demo test
