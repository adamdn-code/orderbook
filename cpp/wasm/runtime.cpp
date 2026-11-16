// The C runtime the freestanding WebAssembly build needs: memory allocation, mem*/str*, and a
// minimal snprintf. Memory comes from wasm linear memory, grown in 64 KiB pages.
#include <cstddef>
#include <cstdint>

extern "C" {

// ---- allocator: power-of-two size classes with free lists; large blocks bump-allocate too ----
// Each block has an 8-byte header holding its size class. Freed blocks go on their class's list.
extern unsigned char __heap_base;
static uintptr_t heap_top = 0, heap_end = 0;
static void* free_lists[48];

static void* bump(size_t n) {
    if (!heap_top) heap_top = heap_end = (reinterpret_cast<uintptr_t>(&__heap_base) + 15) & ~uintptr_t(15);
    if (heap_top + n > heap_end) {
        const size_t pages = (heap_top + n - heap_end + 65535) / 65536;
        if (__builtin_wasm_memory_grow(0, pages) == size_t(-1)) __builtin_trap();
        heap_end += pages * 65536;
    }
    void* p = reinterpret_cast<void*>(heap_top);
    heap_top += n;
    return p;
}

void* malloc(size_t n) {
    unsigned c = 4;  // smallest block: 16 bytes
    while ((size_t(1) << c) < n + 8) ++c;
    void* block = free_lists[c];
    if (block) free_lists[c] = *static_cast<void**>(block);
    else block = bump(size_t(1) << c);
    *static_cast<uint64_t*>(block) = c;
    return static_cast<unsigned char*>(block) + 8;
}

void free(void* p) {
    if (!p) return;
    void* block = static_cast<unsigned char*>(p) - 8;
    const unsigned c = unsigned(*static_cast<uint64_t*>(block));
    *static_cast<void**>(block) = free_lists[c];
    free_lists[c] = block;
}

void* memcpy(void* d, const void* s, size_t n) {
    auto* a = static_cast<unsigned char*>(d); auto* b = static_cast<const unsigned char*>(s);
    while (n--) *a++ = *b++;
    return d;
}
void* memmove(void* d, const void* s, size_t n) {
    auto* a = static_cast<unsigned char*>(d); auto* b = static_cast<const unsigned char*>(s);
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int v, size_t n) {
    auto* a = static_cast<unsigned char*>(d);
    while (n--) *a++ = static_cast<unsigned char>(v);
    return d;
}
int memcmp(const void* x, const void* y, size_t n) {
    auto* a = static_cast<const unsigned char*>(x); auto* b = static_cast<const unsigned char*>(y);
    for (; n--; ++a, ++b) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }

}  // extern "C"

void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }

namespace std {
int snprintf(char* out, size_t cap, const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    size_t n = 0;
    auto put = [&](char c) { if (n + 1 < cap) out[n] = c; ++n; };
    for (const char* f = fmt; *f; ++f) {
        if (*f != '%') { put(*f); continue; }
        int width = 0;
        while (*++f >= '0' && *f <= '9') width = width * 10 + (*f - '0');
        while (*f == 'l') ++f;  // only %lld is used
        long long v = __builtin_va_arg(ap, long long);
        char digits[24]; int k = 0;
        unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
        do { digits[k++] = char('0' + u % 10); u /= 10; } while (u);
        if (v < 0) digits[k++] = '-';
        for (int pad = width - k; pad > 0; --pad) put(' ');
        while (k) put(digits[--k]);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    __builtin_va_end(ap);
    return int(n);
}
}  // namespace std
