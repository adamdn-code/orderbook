// Runs ob_bench from ob_wasm.cpp natively against the real standard library, so the WebAssembly
// build can be checked trade for trade against it (see test.mjs).
#include <cstdio>
#include <cstdlib>
extern "C" [[noreturn]] void ob_fail(const char* m) { std::printf("fail: %s\n", m); std::exit(1); }
#define import_module(x) unused
#define import_name(x) unused
#define export_name(x) unused
#include "ob_wasm.cpp"
int main(int argc, char** argv) { std::printf("%.0f\n", ob_bench(argc > 1 ? std::atoi(argv[1]) : 1000000, 16300)); }
