CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -Itests -pthread
SRC = src/orderbook.cpp src/engine.cpp
REF = tests/reference_book.cpp

all: build/tests build/bench

build:
	mkdir -p build

build/tests: tests/tests.cpp $(SRC) $(REF) include/*.hpp tests/*.hpp | build
	$(CXX) $(CXXFLAGS) tests/tests.cpp $(SRC) $(REF) -o $@

build/bench: bench/bench.cpp $(SRC) $(REF) include/*.hpp tests/*.hpp | build
	$(CXX) $(CXXFLAGS) bench/bench.cpp $(SRC) $(REF) -o $@

test: build/tests
	./build/tests

bench: build/bench
	./build/bench

# Run the tests under ThreadSanitizer to catch data races.
tsan: | build
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -Iinclude -Itests -pthread tests/tests.cpp $(SRC) $(REF) -o build/tests_tsan
	./build/tests_tsan

# Run the tests under AddressSanitizer + UBSan to catch memory and arithmetic bugs.
asan: | build
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Iinclude -Itests -pthread tests/tests.cpp $(SRC) $(REF) -o build/tests_asan
	./build/tests_asan

clean:
	rm -rf build

.PHONY: all test bench tsan asan clean
