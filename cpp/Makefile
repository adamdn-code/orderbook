CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -pthread
SRC = src/orderbook.cpp src/engine.cpp

all: build/tests build/bench

build:
	mkdir -p build

build/tests: tests/tests.cpp $(SRC) include/*.hpp | build
	$(CXX) $(CXXFLAGS) tests/tests.cpp $(SRC) -o $@

build/bench: bench/bench.cpp $(SRC) include/*.hpp | build
	$(CXX) $(CXXFLAGS) bench/bench.cpp $(SRC) -o $@

test: build/tests
	./build/tests

bench: build/bench
	./build/bench

# Run the tests under ThreadSanitizer to catch data races.
tsan: | build
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -Iinclude -pthread tests/tests.cpp $(SRC) -o build/tests_tsan
	./build/tests_tsan

clean:
	rm -rf build

.PHONY: all test bench tsan clean
