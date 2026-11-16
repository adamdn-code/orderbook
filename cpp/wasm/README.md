# The order book in WebAssembly

`docs/engine.html` runs the book from `../src/orderbook.cpp`, unmodified, in the browser. It is built with plain
clang and `wasm-ld`, with no Emscripten and no libc++ for wasm:

```bash
sudo apt install clang lld   # wasm-ld comes with lld
make -C cpp wasm             # -> docs/orderbook.wasm (about 16 KB)
make -C cpp wasm-test        # needs node: checks it against the native build, trade for trade
```

- `shim/` stands in for the few standard library pieces the book uses: `vector`, `optional`, `pair`, `min`/`max`,
  `reverse`, and a small `string` and `snprintf` for error messages and `to_string()`.
- `runtime.cpp` is the C runtime: a size-class allocator on wasm linear memory, `memcpy` and friends.
- There is no unwinder, so `prelude.hpp` turns `throw X(...)` into a call that hands the message to JavaScript,
  which throws a JS `Error`. Every throw in the book happens while validating an order, before the book changes,
  so a rejected order leaves the book exactly as it was.
- `ob_wasm.cpp` is the C API the page calls: add, cancel, depth, the queue at a price, best prices and a benchmark.

`test.mjs` checks matching, time priority, cancels and rejected orders, then runs the same random million orders
as `native_bench.cpp` and requires the same number of trades (557,474).
