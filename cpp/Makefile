CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -Itests -pthread
SRC = src/orderbook.cpp src/engine.cpp
REF = tests/reference_book.cpp

# Python extension module (needs Python headers: sudo apt install python3-dev)
PYTHON ?= python3
PY_INCLUDE = $(shell $(PYTHON) -c "import sysconfig; print(sysconfig.get_paths()['include'])")
PY_SUFFIX = $(shell $(PYTHON) -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))")
PY_EXT = build/obfast$(PY_SUFFIX)

all: build/tests build/bench

build:
	mkdir -p build

build/tests: tests/tests.cpp $(SRC) $(REF) include/*.hpp tests/*.hpp | build
	$(CXX) $(CXXFLAGS) tests/tests.cpp $(SRC) $(REF) -o $@

build/bench: bench/bench.cpp $(SRC) $(REF) include/*.hpp tests/*.hpp | build
	$(CXX) $(CXXFLAGS) bench/bench.cpp $(SRC) $(REF) -o $@

python: $(PY_EXT)

$(PY_EXT): bindings/obfast.cpp src/orderbook.cpp include/orderbook.hpp | build
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type -shared -fPIC -fvisibility=hidden -Iinclude -I$(PY_INCLUDE) bindings/obfast.cpp src/orderbook.cpp -o $@

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

.PHONY: all python test bench tsan asan clean
