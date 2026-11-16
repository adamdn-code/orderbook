// Checks docs/orderbook.wasm: matching, queue order, cancels, errors that leave the book unchanged,
// and the same trades as the native build on a random million-order run.
// usage: node cpp/wasm/test.mjs EXPECTED_TRADES
import fs from "node:fs";
import assert from "node:assert/strict";

const bytes = fs.readFileSync(new URL("../../docs/orderbook.wasm", import.meta.url));
let mem;
const dec = new TextDecoder();
const { instance } = await WebAssembly.instantiate(bytes, { env: { ob_fail: (p) => {
  const u = new Uint8Array(mem.buffer); let e = p; while (u[e]) e++; throw new Error(dec.decode(u.subarray(p, e))); } } });
const X = instance.exports; mem = X.memory;
const out = () => Array.from(new Float64Array(mem.buffer, X.ob_out(), 4096));

X.ob_reset();
assert.equal(X.ob_add(1, 0, 0, 100, 16300), 0);
assert.equal(X.ob_add(2, 0, 0, 50, 16300), 0);
assert.equal(X.ob_add(3, 1, 0, 70, 16302), 0);
assert.equal(X.ob_best_bid(), 16300);
assert.equal(X.ob_best_ask(), 16302);
assert.equal(X.ob_level(0, 16300), 2);
assert.deepEqual(out().slice(0, 4), [1, 100, 2, 50]);                       // time priority
assert.equal(X.ob_add(4, 1, 0, 120, 16300), 2);                             // sell sweeps both, oldest first
assert.deepEqual(out().slice(0, 10), [1, 4, 16300, 100, 1, 2, 4, 16300, 20, 1]);
assert.throws(() => X.ob_add(2, 0, 0, 10, 16000), /duplicate order id 2/); // rejected...
assert.equal(X.ob_resting(), 2);                                            // ...and nothing changed
assert.equal(X.ob_cancel(2), 1);
assert.equal(X.ob_cancel(2), 0);
assert.equal(X.ob_best_bid(), -1);
assert.equal(X.ob_add(5, 0, 1, 1000, 0), 1);                                // market buy takes the ask
assert.deepEqual(out().slice(0, 5), [5, 3, 16302, 70, 0]);
assert.equal(X.ob_resting(), 0);

const expected = Number(process.argv[2]);
const t0 = performance.now();
const trades = X.ob_bench(1000000, 16300);
const ms = performance.now() - t0;
if (expected) assert.equal(trades, expected, "wasm and native builds disagree");
console.log(`wasm ok: ${trades} trades on 1M orders (native: ${expected || "not given"}), ${(1000 / ms).toFixed(2)}M orders/s`);
