// Force-included before orderbook.cpp in the WebAssembly build. There is no unwinder in this
// freestanding build, so `throw X(...)` becomes a call that hands X's message to JavaScript, which
// throws a JS Error. Every throw in the book happens in validation, before the book is changed.
#pragma once
#include <stdexcept>
extern "C" [[noreturn]] void ob_fail(const char* message);
struct ob_wasm_thrower {
    template <class E> [[noreturn]] void operator<<(const E& e) const { ob_fail(e.what()); }
};
#define throw ::ob_wasm_thrower{} <<
